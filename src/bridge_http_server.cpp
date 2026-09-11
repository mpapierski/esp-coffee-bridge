#include "bridge_http_server.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <unistd.h>

#include "bridge_multipart.h"

namespace bridge_http {
namespace {

const char* statusText(int code) {
    switch (code) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 409: return "409 Conflict";
        case 411: return "411 Length Required";
        case 413: return "413 Payload Too Large";
        case 425: return "425 Too Early";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        case 507: return "507 Insufficient Storage";
        default: return code >= 400 ? "500 Internal Server Error" : "200 OK";
    }
}

int hexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

String urlDecode(const char* value, size_t length) {
    String decoded;
    decoded.reserve(length);
    for (size_t index = 0; index < length; ++index) {
        const char ch = value[index];
        if (ch == '+') {
            decoded += ' ';
            continue;
        }
        if (ch == '%' && index + 2 < length) {
            const int high = hexDigit(value[index + 1]);
            const int low = hexDigit(value[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded += static_cast<char>((high << 4) | low);
                index += 2;
                continue;
            }
        }
        decoded += ch;
    }
    return decoded;
}

bool elapsedAtLeast(uint32_t nowMs, uint32_t thenMs, uint32_t durationMs) {
    return static_cast<uint32_t>(nowMs - thenMs) >= durationMs;
}

} // namespace

void BridgeHttpClient::stop() {
    if (server_ != nullptr) {
        server_->closeCurrentClient();
    }
}

BridgeHttpServer::BridgeHttpServer(uint16_t port) : port_(port) {
    routes_.reserve(48);
}

BridgeHttpServer::~BridgeHttpServer() {
    stop();
}

void BridgeHttpServer::on(const char* uri, HTTPMethod method, Handler handler) {
    on(uri, method, std::move(handler), {});
}

void BridgeHttpServer::on(const char* uri,
                          HTTPMethod method,
                          Handler handler,
                          Handler uploadHandler) {
    routes_.push_back({String(uri), method, std::move(handler), std::move(uploadHandler)});
}

void BridgeHttpServer::onNotFound(Handler handler) {
    notFoundHandler_ = std::move(handler);
}

void BridgeHttpServer::setRequestLifecycle(BeforeRequest before,
                                           AfterRequest after,
                                           void* context) {
    beforeRequest_ = before;
    afterRequest_ = after;
    requestLifecycleContext_ = context;
}

void BridgeHttpServer::configureEvents(uint32_t bootNonce,
                                       StatusRenderer statusRenderer,
                                       JobRenderer jobRenderer,
                                       void* context) {
    eventBootNonce_ = bootNonce;
    eventCounter_ = 1;
    statusRenderer_ = statusRenderer;
    jobRenderer_ = jobRenderer;
    eventContext_ = context;
}

bool BridgeHttpServer::begin() {
#if !CONFIG_HTTPD_WS_SUPPORT
#error "Bridge WebSocket events require CONFIG_HTTPD_WS_SUPPORT"
#endif
    if (handle_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port_;
    config.ctrl_port = static_cast<uint16_t>(port_ + 32768U);
    config.task_priority = 3;
    config.stack_size = 12 * 1024;
    config.core_id = 1;
    config.max_open_sockets = 10;
    config.max_uri_handlers = 8;
    config.max_resp_headers = 8;
    config.backlog_conn = 6;
    config.lru_purge_enable = false;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 2;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.global_user_ctx = this;
    config.global_user_ctx_free_fn = ignoreGlobalContextFree;
    config.close_fn = closeSessionThunk;

    if (httpd_start(&handle_, &config) != ESP_OK) {
        handle_ = nullptr;
        return false;
    }

    httpd_uri_t events{};
    events.uri = "/api/events";
    events.method = HTTP_GET;
    events.handler = websocketThunk;
    events.user_ctx = this;
    events.is_websocket = true;
    events.handle_ws_control_frames = false;
    if (httpd_register_uri_handler(handle_, &events) != ESP_OK) {
        stop();
        return false;
    }

    const HTTPMethod methods[] = {
        HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_PATCH, HTTP_DELETE, HTTP_OPTIONS,
    };
    for (HTTPMethod method : methods) {
        httpd_uri_t route{};
        route.uri = "*";
        route.method = method;
        route.handler = dispatchThunk;
        route.user_ctx = this;
        route.is_websocket = false;
        if (httpd_register_uri_handler(handle_, &route) != ESP_OK) {
            stop();
            return false;
        }
    }
    return true;
}

void BridgeHttpServer::stop() {
    if (handle_ != nullptr) {
        httpd_stop(handle_);
        handle_ = nullptr;
    }
    for (EventClient& client : eventClients_) {
        client = {};
    }
    eventClientCount_.store(0, std::memory_order_release);
}

BridgeHttpServer::Route* BridgeHttpServer::findRoute(const String& uri, HTTPMethod method) {
    for (Route& route : routes_) {
        if (route.method == method && route.uri == uri) {
            return &route;
        }
    }
    return nullptr;
}

esp_err_t BridgeHttpServer::dispatchThunk(httpd_req_t* request) {
    auto* server = static_cast<BridgeHttpServer*>(request->user_ctx);
    return server != nullptr ? server->dispatch(request) : ESP_FAIL;
}

esp_err_t BridgeHttpServer::websocketThunk(httpd_req_t* request) {
    auto* server = static_cast<BridgeHttpServer*>(request->user_ctx);
    return server != nullptr ? server->handleWebsocket(request) : ESP_FAIL;
}

void BridgeHttpServer::eventWorkThunk(void* context) {
    auto* server = static_cast<BridgeHttpServer*>(context);
    if (server != nullptr) {
        server->drainEventWork();
    }
}

void BridgeHttpServer::closeSessionThunk(httpd_handle_t handle, int fd) {
    auto* server = static_cast<BridgeHttpServer*>(httpd_get_global_user_ctx(handle));
    if (server != nullptr) {
        server->removeEventClient(fd);
    }
    // esp_http_server does not close the socket when a custom close callback
    // is installed; the callback owns both application cleanup and close().
    // Omitting this leaks one descriptor per completed HTTP/WS session until
    // the server reaches max_open_sockets and rejects every new request.
    ::close(fd);
}

void BridgeHttpServer::parseQuery(httpd_req_t* request, RequestContext& context) {
    const size_t queryLength = httpd_req_get_url_query_len(request);
    if (queryLength == 0 || queryLength > 1024) {
        return;
    }
    std::unique_ptr<char[]> query(new (std::nothrow) char[queryLength + 1]);
    if (!query || httpd_req_get_url_query_str(request, query.get(), queryLength + 1) != ESP_OK) {
        return;
    }
    const char* cursor = query.get();
    while (*cursor != '\0') {
        const char* end = std::strchr(cursor, '&');
        const size_t itemLength = end != nullptr ? static_cast<size_t>(end - cursor) : std::strlen(cursor);
        const char* equals = static_cast<const char*>(std::memchr(cursor, '=', itemLength));
        const size_t nameLength = equals != nullptr ? static_cast<size_t>(equals - cursor) : itemLength;
        const size_t valueLength = equals != nullptr ? itemLength - nameLength - 1 : 0;
        context.args.push_back({
            urlDecode(cursor, nameLength),
            equals != nullptr ? urlDecode(equals + 1, valueLength) : String("")
        });
        if (end == nullptr) {
            break;
        }
        cursor = end + 1;
    }
}

bool BridgeHttpServer::readBody(httpd_req_t* request, String& bodyOut) {
    bodyOut = "";
    if (request->content_len == 0) {
        return true;
    }
    if (request->content_len > MAX_JSON_BODY_BYTES) {
        return false;
    }
    if (!bodyOut.reserve(request->content_len)) {
        return false;
    }
    size_t received = 0;
    char buffer[512];
    while (received < request->content_len) {
        const size_t wanted = std::min(sizeof(buffer), request->content_len - received);
        const int count = httpd_req_recv(request, buffer, wanted);
        if (count <= 0) {
            return false;
        }
        if (!bodyOut.concat(buffer, static_cast<unsigned int>(count))) {
            return false;
        }
        received += static_cast<size_t>(count);
    }
    return true;
}

bool BridgeHttpServer::processMultipart(httpd_req_t* request, const Handler& uploadHandler) {
    const size_t contentTypeLength = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (contentTypeLength == 0 || contentTypeLength > 512 ||
        request->content_len == 0 || request->content_len > MAX_MULTIPART_REQUEST_BYTES) {
        return false;
    }
    std::unique_ptr<char[]> contentType(new (std::nothrow) char[contentTypeLength + 1]);
    if (!contentType ||
        httpd_req_get_hdr_value_str(request, "Content-Type", contentType.get(), contentTypeLength + 1) != ESP_OK) {
        return false;
    }
    std::string boundary;
    std::string parseError;
    if (!extractMultipartBoundary(contentType.get(), boundary, parseError)) {
        return false;
    }

    upload_ = {};
    bool started = false;
    MultipartParser parser(
        std::move(boundary),
        [&](const MultipartPart& part) {
            upload_.status = UPLOAD_FILE_START;
            upload_.name = part.name.c_str();
            upload_.filename = part.filename.c_str();
            upload_.type = part.contentType.c_str();
            upload_.currentSize = 0;
            upload_.totalSize = 0;
            started = true;
            uploadHandler();
            return true;
        },
        [&](const uint8_t* bytes, size_t size) {
            size_t offset = 0;
            while (offset < size) {
                const size_t count = std::min<size_t>(HTTP_UPLOAD_BUFLEN, size - offset);
                std::memcpy(upload_.buf, bytes + offset, count);
                upload_.status = UPLOAD_FILE_WRITE;
                upload_.currentSize = count;
                uploadHandler();
                upload_.totalSize += count;
                offset += count;
            }
            return true;
        },
        [&]() {
            upload_.status = UPLOAD_FILE_END;
            upload_.currentSize = 0;
            uploadHandler();
            return true;
        });

    size_t received = 0;
    uint8_t buffer[1024];
    while (received < request->content_len) {
        const size_t wanted = std::min(sizeof(buffer), request->content_len - received);
        const int count = httpd_req_recv(request, reinterpret_cast<char*>(buffer), wanted);
        if (count <= 0 || !parser.feed(buffer, static_cast<size_t>(count))) {
            if (started) {
                upload_.status = UPLOAD_FILE_ABORTED;
                upload_.currentSize = 0;
                uploadHandler();
            }
            return false;
        }
        received += static_cast<size_t>(count);
    }
    if (!parser.finish()) {
        if (started) {
            upload_.status = UPLOAD_FILE_ABORTED;
            upload_.currentSize = 0;
            uploadHandler();
        }
        return false;
    }
    return true;
}

esp_err_t BridgeHttpServer::dispatch(httpd_req_t* request) {
    const uint32_t startedAtUs = micros();
    bool lifecycleEntered = beforeRequest_ == nullptr || beforeRequest_(requestLifecycleContext_);
    if (!lifecycleEntered) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Retry-After", "1");
        httpd_resp_sendstr(request, "{\"ok\":false,\"error\":\"machine registry is busy\"}");
        return ESP_OK;
    }

    RequestContext context;
    context.request = request;
    context.method = static_cast<HTTPMethod>(request->method);
    context.uri = request->uri;
    const int queryAt = context.uri.indexOf('?');
    if (queryAt >= 0) {
        context.uri.remove(queryAt);
    }
    context.args.reserve(8);
    context.responseHeaders.reserve(8);
    parseQuery(request, context);
    current_ = &context;

    Route* route = findRoute(context.uri, context.method);
    bool requestValid = true;
    if (route != nullptr && route->uploadHandler) {
        requestValid = processMultipart(request, route->uploadHandler);
        if (requestValid) {
            route->handler();
        }
    } else {
        requestValid = readBody(request, context.body);
        if (requestValid) {
            if (route != nullptr) {
                route->handler();
            } else if (notFoundHandler_) {
                notFoundHandler_();
            }
        }
    }

    if (!requestValid && !context.responseStarted) {
        httpd_resp_set_status(request,
                              request->content_len > MAX_MULTIPART_REQUEST_BYTES ||
                                      ((route == nullptr || !route->uploadHandler) &&
                                       request->content_len > MAX_JSON_BODY_BYTES)
                                  ? "413 Payload Too Large"
                                  : "400 Bad Request");
        httpd_resp_set_type(request, "application/json");
        httpd_resp_sendstr(request, "{\"ok\":false,\"error\":\"invalid or oversized request body\"}");
        context.responseStarted = true;
        context.responseFinished = true;
    }
    if (context.chunked && !context.responseFinished && !context.closeRequested) {
        finishChunked(context);
    }

    current_ = nullptr;
    if (afterRequest_ != nullptr) {
        afterRequest_(micros() - startedAtUs, requestLifecycleContext_);
    }
    if (context.closeRequested) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

String BridgeHttpServer::uri() const {
    return current_ != nullptr ? current_->uri : String("");
}

HTTPMethod BridgeHttpServer::method() const {
    return current_ != nullptr ? current_->method : HTTP_ANY;
}

bool BridgeHttpServer::hasArg(const String& name) const {
    if (current_ == nullptr) return false;
    if (name == "plain") return current_->body.length() != 0;
    for (const auto& argument : current_->args) {
        if (argument.first == name) return true;
    }
    return false;
}

String BridgeHttpServer::arg(const String& name) const {
    if (current_ == nullptr) return "";
    if (name == "plain") return current_->body;
    for (const auto& argument : current_->args) {
        if (argument.first == name) return argument.second;
    }
    return "";
}

bool BridgeHttpServer::hasHeader(const String& name) const {
    return current_ != nullptr && current_->request != nullptr &&
        httpd_req_get_hdr_value_len(current_->request, name.c_str()) != 0;
}

String BridgeHttpServer::header(const String& name) const {
    if (current_ == nullptr || current_->request == nullptr) return "";
    const size_t length = httpd_req_get_hdr_value_len(current_->request, name.c_str());
    if (length == 0 || length > 1024) return "";
    std::unique_ptr<char[]> value(new (std::nothrow) char[length + 1]);
    if (!value || httpd_req_get_hdr_value_str(
                      current_->request, name.c_str(), value.get(), length + 1) != ESP_OK) {
        return "";
    }
    return String(value.get());
}

HTTPUpload& BridgeHttpServer::upload() {
    return upload_;
}

void BridgeHttpServer::sendHeader(const String& name, const String& value, bool first) {
    if (current_ == nullptr || current_->responseStarted) return;
    if (first) {
        current_->responseHeaders.insert(current_->responseHeaders.begin(), {name, value});
    } else {
        current_->responseHeaders.push_back({name, value});
    }
}

void BridgeHttpServer::setContentLength(size_t length) {
    if (current_ == nullptr || current_->responseStarted) return;
    current_->contentLength = length;
    current_->contentLengthSet = true;
}

void BridgeHttpServer::applyResponseMetadata(RequestContext& context,
                                             int code,
                                             const char* contentType) {
    context.responseStatus = statusText(code);
    context.responseType = contentType != nullptr ? contentType : "text/plain";
    httpd_resp_set_status(context.request, context.responseStatus.c_str());
    httpd_resp_set_type(context.request, context.responseType.c_str());
    for (const auto& header : context.responseHeaders) {
        httpd_resp_set_hdr(context.request, header.first.c_str(), header.second.c_str());
    }
}

void BridgeHttpServer::send(int code, const char* contentType, const String& body) {
    if (current_ == nullptr || current_->responseFinished) return;
    applyResponseMetadata(*current_, code, contentType);
    if (body.length() == 0 && current_->contentLengthSet) {
        current_->chunked = true;
        current_->responseStarted = true;
        return;
    }
    current_->responseStarted = true;
    current_->responseFinished = true;
    httpd_resp_send(current_->request, body.c_str(), body.length());
}

void BridgeHttpServer::send(int code, const char* contentType, const char* body) {
    send(code, contentType, String(body != nullptr ? body : ""));
}

void BridgeHttpServer::send_P(int code,
                              const char* contentType,
                              PGM_P body,
                              size_t length) {
    if (current_ == nullptr || current_->responseFinished) return;
    if (length == 0 && body != nullptr) {
        length = std::strlen(body);
    }
    applyResponseMetadata(*current_, code, contentType);
    current_->responseStarted = true;
    current_->responseFinished = true;
    httpd_resp_send(current_->request, body, length);
}

void BridgeHttpServer::sendContent(const String& content) {
    sendContent(content.c_str(), content.length());
}

void BridgeHttpServer::sendContent(const char* content) {
    sendContent(content, content != nullptr ? std::strlen(content) : 0);
}

void BridgeHttpServer::sendContent(const char* content, size_t length) {
    if (current_ == nullptr || current_->responseFinished || !current_->responseStarted) return;
    if (!current_->chunked) return;
    if (length == 0) {
        finishChunked(*current_);
        return;
    }
    if (httpd_resp_send_chunk(current_->request, content, length) != ESP_OK) {
        current_->closeRequested = true;
        return;
    }
    current_->streamedBytes += length;
    if (current_->contentLength != CONTENT_LENGTH_UNKNOWN &&
        current_->streamedBytes >= current_->contentLength) {
        finishChunked(*current_);
    }
}

void BridgeHttpServer::finishChunked(RequestContext& context) {
    if (!context.responseFinished) {
        httpd_resp_send_chunk(context.request, nullptr, 0);
        context.responseFinished = true;
    }
}

BridgeHttpClient BridgeHttpServer::client() {
    return BridgeHttpClient(this);
}

void BridgeHttpServer::closeCurrentClient() {
    if (current_ != nullptr) {
        current_->closeRequested = true;
    }
}

BridgeHttpServer::EventClient* BridgeHttpServer::findEventClient(int fd) {
    for (EventClient& client : eventClients_) {
        if (client.fd == fd) return &client;
    }
    return nullptr;
}

BridgeHttpServer::EventClient* BridgeHttpServer::addEventClient(int fd) {
    if (EventClient* existing = findEventClient(fd); existing != nullptr) {
        return existing;
    }
    for (EventClient& client : eventClients_) {
        if (client.fd < 0) {
            client = {};
            client.fd = fd;
            eventClientCount_.fetch_add(1, std::memory_order_acq_rel);
            return &client;
        }
    }
    return nullptr;
}

void BridgeHttpServer::removeEventClient(int fd) {
    if (EventClient* client = findEventClient(fd); client != nullptr) {
        *client = {};
        eventClientCount_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

bool BridgeHttpServer::isWatching(const EventClient& client, const char* id) const {
    for (size_t index = 0; index < client.watchCount; ++index) {
        if (std::strcmp(client.watched[index].data(), id) == 0) return true;
    }
    return false;
}

bool BridgeHttpServer::watchJob(EventClient& client, const char* id) {
    if (id == nullptr || id[0] == '\0' || std::strlen(id) >= EVENT_JOB_ID_BYTES) return false;
    if (isWatching(client, id)) return true;
    if (client.watchCount == client.watched.size()) return false;
    std::strncpy(client.watched[client.watchCount].data(), id, EVENT_JOB_ID_BYTES - 1);
    client.watched[client.watchCount][EVENT_JOB_ID_BYTES - 1] = '\0';
    client.watchCount++;
    return true;
}

void BridgeHttpServer::unwatchJob(EventClient& client, const char* id) {
    for (size_t index = 0; index < client.watchCount; ++index) {
        if (std::strcmp(client.watched[index].data(), id) != 0) continue;
        for (size_t move = index + 1; move < client.watchCount; ++move) {
            client.watched[move - 1] = client.watched[move];
        }
        client.watchCount--;
        client.watched[client.watchCount][0] = '\0';
        return;
    }
}

bool BridgeHttpServer::validateOrigin(httpd_req_t* request) {
    const size_t originLength = httpd_req_get_hdr_value_len(request, "Origin");
    if (originLength == 0) return true;
    const size_t hostLength = httpd_req_get_hdr_value_len(request, "Host");
    if (originLength > 255 || hostLength == 0 || hostLength > 192) return false;
    std::unique_ptr<char[]> origin(new (std::nothrow) char[originLength + 1]);
    std::unique_ptr<char[]> host(new (std::nothrow) char[hostLength + 1]);
    if (!origin || !host ||
        httpd_req_get_hdr_value_str(request, "Origin", origin.get(), originLength + 1) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "Host", host.get(), hostLength + 1) != ESP_OK) {
        return false;
    }
    String expected = String("http://") + host.get();
    String actual = origin.get();
    if (actual.endsWith("/")) actual.remove(actual.length() - 1);
    return actual.equalsIgnoreCase(expected);
}

String BridgeHttpServer::nextEventSequence() {
    char sequence[40];
    std::snprintf(sequence,
                  sizeof(sequence),
                  "%08lx-%lu",
                  static_cast<unsigned long>(eventBootNonce_),
                  static_cast<unsigned long>(eventCounter_++));
    return String(sequence);
}

bool BridgeHttpServer::sendEvent(EventClient& client, const String& payload) {
    if (handle_ == nullptr || client.fd < 0 ||
        httpd_ws_get_fd_info(handle_, client.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        const int fd = client.fd;
        removeEventClient(fd);
        return false;
    }
    httpd_ws_frame_t frame{};
    frame.final = true;
    frame.fragmented = false;
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(payload.c_str()));
    frame.len = payload.length();
    if (httpd_ws_send_frame_async(handle_, client.fd, &frame) != ESP_OK) {
        const int fd = client.fd;
        httpd_sess_trigger_close(handle_, fd);
        removeEventClient(fd);
        return false;
    }
    return true;
}

void BridgeHttpServer::sendErrorEvent(EventClient& client,
                                      const char* code,
                                      const char* message,
                                      const char* jobId) {
    DynamicJsonDocument doc(768);
    doc["type"] = "error";
    doc["sequence"] = nextEventSequence();
    doc["code"] = code;
    doc["message"] = message;
    if (jobId != nullptr && jobId[0] != '\0') doc["jobId"] = jobId;
    String payload;
    serializeJson(doc, payload);
    sendEvent(client, payload);
}

void BridgeHttpServer::sendHello(EventClient& client) {
    String status;
    if (statusRenderer_ == nullptr || !statusRenderer_(status, eventContext_)) {
        sendErrorEvent(client, "status_unavailable", "bridge status is temporarily unavailable");
        return;
    }
    DynamicJsonDocument event(512);
    event["type"] = "hello";
    event["eventProtocolVersion"] = 1;
    event["apiVersion"] = 2;
    event["sequence"] = nextEventSequence();
    event["status"] = serialized(status.c_str(), status.length());
    String payload;
    payload.reserve(status.length() + 160);
    if (event.overflowed() || serializeJson(event, payload) == 0) {
        sendErrorEvent(client, "status_unavailable", "bridge status event could not be serialized");
        return;
    }
    sendEvent(client, payload);
}

void BridgeHttpServer::sendStatusToAll(const char* type) {
    if (eventClientCount_.load(std::memory_order_acquire) == 0 || statusRenderer_ == nullptr) return;
    String status;
    if (!statusRenderer_(status, eventContext_)) return;
    DynamicJsonDocument event(512);
    event["type"] = type;
    event["sequence"] = nextEventSequence();
    event["status"] = serialized(status.c_str(), status.length());
    if (std::strcmp(type, "resync") == 0) {
        event["reason"] = "event_overflow";
    }
    String payload;
    payload.reserve(status.length() + 128);
    if (event.overflowed() || serializeJson(event, payload) == 0) return;
    for (EventClient& client : eventClients_) {
        if (client.fd >= 0) sendEvent(client, payload);
    }
    lastStatusEventAtMs_.store(millis(), std::memory_order_release);
}

void BridgeHttpServer::sendJob(EventClient& client, const char* id) {
    if (jobRenderer_ == nullptr) {
        sendErrorEvent(client, "job_unavailable", "job status is unavailable", id);
        return;
    }
    String job;
    bool terminal = false;
    const EventJobLookup lookup = jobRenderer_(String(id), job, terminal, eventContext_);
    if (lookup == EventJobLookup::Busy) {
        sendErrorEvent(client, "job_busy", "job registry is temporarily busy", id);
        return;
    }
    if (lookup == EventJobLookup::Missing) {
        sendErrorEvent(client, "job_not_found", "job not found or expired", id);
        unwatchJob(client, id);
        return;
    }
    DynamicJsonDocument event(384);
    event["type"] = "job";
    event["sequence"] = nextEventSequence();
    event["job"] = serialized(job.c_str(), job.length());
    String payload;
    payload.reserve(job.length() + 112);
    if (event.overflowed() || serializeJson(event, payload) == 0) {
        sendErrorEvent(client, "job_unavailable", "job event could not be serialized", id);
        return;
    }
    sendEvent(client, payload);
    if (terminal && client.fd >= 0) unwatchJob(client, id);
}

void BridgeHttpServer::sendJobToWatchers(const char* id) {
    for (EventClient& client : eventClients_) {
        if (client.fd >= 0 && isWatching(client, id)) sendJob(client, id);
    }
}

esp_err_t BridgeHttpServer::handleWebsocket(httpd_req_t* request) {
    const int fd = httpd_req_to_sockfd(request);
    if (request->method == HTTP_GET) {
        if (!validateOrigin(request)) {
            return ESP_FAIL;
        }
        EventClient* client = addEventClient(fd);
        if (client == nullptr) {
            return ESP_FAIL;
        }
        sendHello(*client);
        return ESP_OK;
    }

    EventClient* client = findEventClient(fd);
    if (client == nullptr) return ESP_FAIL;
    httpd_ws_frame_t frame{};
    if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK || frame.len > 512) {
        return ESP_FAIL;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT || !frame.final || frame.fragmented) {
        sendErrorEvent(*client, "invalid_frame", "only unfragmented text messages are accepted");
        return ESP_OK;
    }
    std::array<uint8_t, 513> payload{};
    frame.payload = payload.data();
    if (frame.len != 0 && httpd_ws_recv_frame(request, &frame, frame.len) != ESP_OK) {
        return ESP_FAIL;
    }
    payload[frame.len] = '\0';
    DynamicJsonDocument command(768);
    if (deserializeJson(command, payload.data(), frame.len)) {
        sendErrorEvent(*client, "invalid_command", "event command must be valid JSON");
        return ESP_OK;
    }
    const String type = command["type"] | "";
    if (type == "watch_job") {
        const String id = command["jobId"] | "";
        if (!watchJob(*client, id.c_str())) {
            sendErrorEvent(*client, "watch_limit", "job watch is invalid or the watch limit was reached", id.c_str());
        } else {
            sendJob(*client, id.c_str());
        }
        return ESP_OK;
    }
    if (type == "unwatch_job") {
        const String id = command["jobId"] | "";
        unwatchJob(*client, id.c_str());
        return ESP_OK;
    }
    if (type == "ping") {
        DynamicJsonDocument pong(512);
        pong["type"] = "pong";
        pong["sequence"] = nextEventSequence();
        pong["nonce"] = command["nonce"] | "";
        String response;
        serializeJson(pong, response);
        sendEvent(*client, response);
        return ESP_OK;
    }
    sendErrorEvent(*client, "unknown_command", "unsupported event command");
    return ESP_OK;
}

void BridgeHttpServer::notifyJobChanged(const char* id) {
    portENTER_CRITICAL(&eventChangesMutex_);
    eventChanges_.markJob(id);
    portEXIT_CRITICAL(&eventChangesMutex_);
}

void BridgeHttpServer::markStatusChanged() {
    portENTER_CRITICAL(&eventChangesMutex_);
    eventChanges_.markStatus();
    portEXIT_CRITICAL(&eventChangesMutex_);
}

void BridgeHttpServer::requestEventWork() {
    bool queue = false;
    portENTER_CRITICAL(&eventChangesMutex_);
    if (!eventWorkQueued_) {
        eventWorkQueued_ = true;
        queue = true;
    }
    portEXIT_CRITICAL(&eventChangesMutex_);
    if (queue && (handle_ == nullptr || httpd_queue_work(handle_, eventWorkThunk, this) != ESP_OK)) {
        portENTER_CRITICAL(&eventChangesMutex_);
        eventWorkQueued_ = false;
        portEXIT_CRITICAL(&eventChangesMutex_);
    }
}

void BridgeHttpServer::tickEvents(uint32_t nowMs, bool active) {
    if (eventClientCount_.load(std::memory_order_acquire) == 0) {
        portENTER_CRITICAL(&eventChangesMutex_);
        eventChanges_.take();
        portEXIT_CRITICAL(&eventChangesMutex_);
        return;
    }
    const uint32_t statusInterval = active
        ? EVENT_ACTIVE_STATUS_INTERVAL_MS
        : EVENT_IDLE_STATUS_INTERVAL_MS;
    if (elapsedAtLeast(nowMs,
                       lastStatusEventAtMs_.load(std::memory_order_acquire),
                       statusInterval)) {
        markStatusChanged();
    }
    bool pending = false;
    portENTER_CRITICAL(&eventChangesMutex_);
    pending = eventChanges_.pending() &&
        elapsedAtLeast(nowMs, lastEventDispatchAtMs_, EVENT_DIRTY_MIN_INTERVAL_MS);
    portEXIT_CRITICAL(&eventChangesMutex_);
    if (pending) requestEventWork();
}

void BridgeHttpServer::drainEventWork() {
    EventChangeBatch batch;
    portENTER_CRITICAL(&eventChangesMutex_);
    batch = eventChanges_.take();
    eventWorkQueued_ = false;
    lastEventDispatchAtMs_ = millis();
    portEXIT_CRITICAL(&eventChangesMutex_);

    if (batch.resync) {
        sendStatusToAll("resync");
        return;
    }
    if (batch.statusDirty) sendStatusToAll();
    for (size_t index = 0; index < batch.jobCount; ++index) {
        sendJobToWatchers(batch.jobIds[index].data());
    }
}

size_t BridgeHttpServer::websocketClientCount() const {
    return eventClientCount_.load(std::memory_order_acquire);
}

} // namespace bridge_http
