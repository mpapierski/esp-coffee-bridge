#pragma once

#include <Arduino.h>
#include <esp_http_server.h>
#include <http_parser.h>

#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "bridge_event_changes.h"

typedef enum http_method HTTPMethod;

#ifndef HTTP_ANY
#define HTTP_ANY (HTTPMethod)(255)
#endif

#ifndef HTTP_UPLOAD_BUFLEN
#define HTTP_UPLOAD_BUFLEN 1436
#endif

#ifndef CONTENT_LENGTH_UNKNOWN
#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)
#endif

enum HTTPUploadStatus {
    UPLOAD_FILE_START,
    UPLOAD_FILE_WRITE,
    UPLOAD_FILE_END,
    UPLOAD_FILE_ABORTED,
};

struct HTTPUpload {
    HTTPUploadStatus status{UPLOAD_FILE_ABORTED};
    String filename;
    String name;
    String type;
    size_t totalSize{0};
    size_t currentSize{0};
    uint8_t buf[HTTP_UPLOAD_BUFLEN]{};
};

namespace bridge_http {

enum class EventJobLookup : uint8_t {
    Found,
    Missing,
    Busy,
};

class BridgeHttpServer;

class BridgeHttpClient {
public:
    explicit BridgeHttpClient(BridgeHttpServer* server = nullptr) : server_(server) {}
    void stop();

private:
    BridgeHttpServer* server_{nullptr};
};

class BridgeHttpServer {
public:
    using Handler = std::function<void()>;
    using BeforeRequest = bool (*)(void* context);
    using AfterRequest = void (*)(uint32_t durationUs, void* context);
    using StatusRenderer = bool (*)(char* jsonOut,
                                    size_t capacity,
                                    size_t& lengthOut,
                                    void* context);
    using JobRenderer = EventJobLookup (*)(const String& id,
                                           String& jsonOut,
                                           bool& terminalOut,
                                           void* context);

    explicit BridgeHttpServer(uint16_t port = 80);
    ~BridgeHttpServer();

    void on(const char* uri, HTTPMethod method, Handler handler);
    void on(const char* uri, HTTPMethod method, Handler handler, Handler uploadHandler);
    void on(const char* uri,
            HTTPMethod method,
            Handler handler,
            Handler uploadHandler,
            size_t maximumMultipartRequestBytes);
    void onNotFound(Handler handler);
    void collectHeaders(const char*[], size_t) {}

    void setRequestLifecycle(BeforeRequest before,
                             AfterRequest after,
                             void* context = nullptr);
    void configureEvents(uint32_t bootNonce,
                         StatusRenderer statusRenderer,
                         JobRenderer jobRenderer,
                         void* context = nullptr);

    bool begin();
    void stop();

    String uri() const;
    HTTPMethod method() const;
    bool hasArg(const String& name) const;
    String arg(const String& name) const;
    bool hasHeader(const String& name) const;
    String header(const String& name) const;
    bool headerEquals(const char* name, const char* expected) const;
    HTTPUpload& upload();

    void sendHeader(const String& name, const String& value, bool first = false);
    void setContentLength(size_t length);
    void send(int code, const char* contentType, const String& body);
    void send(int code, const char* contentType, const char* body);
    void send(int code, const char* contentType, const char* body, size_t length);
    void send_P(int code,
                const char* contentType,
                PGM_P body,
                size_t length = 0);
    // Returns false once the response can no longer be written. Streaming
    // handlers must stop producing data immediately in that case.
    bool sendContent(const String& content);
    bool sendContent(const char* content);
    bool sendContent(const char* content, size_t length);
    BridgeHttpClient client();

    void notifyJobChanged(const char* id);
    void markStatusChanged();
    bool sendStatusSnapshot();
    void tickEvents(uint32_t nowMs, bool active);
    size_t websocketClientCount() const;
    bool running() const;
    uint32_t taskHeartbeatAtMs() const;
    uint32_t taskHeartbeatAgeMs(uint32_t nowMs) const;
    uint32_t taskStackHighWaterBytes() const;
    bool healthProbePending() const;
    uint32_t healthProbeQueueFailures() const;
    // Long synchronous handlers call this while making bounded progress. The
    // queued health probe runs on this same task and cannot do so for them.
    void markTaskProgress();

private:
    friend class BridgeHttpClient;

    struct Route {
        String uri;
        HTTPMethod method{HTTP_GET};
        Handler handler;
        Handler uploadHandler;
        size_t maximumMultipartRequestBytes{0};
    };

    struct RequestContext {
        httpd_req_t* request{nullptr};
        String uri;
        HTTPMethod method{HTTP_GET};
        String body;
        std::vector<std::pair<String, String>> args;
        std::vector<std::pair<String, String>> responseHeaders;
        size_t contentLength{0};
        size_t streamedBytes{0};
        bool contentLengthSet{false};
        bool chunked{false};
        bool responseStarted{false};
        bool responseFinished{false};
        bool closeRequested{false};
    };

    struct EventClient {
        int fd{-1};
        size_t watchCount{0};
        std::array<std::array<char, EVENT_JOB_ID_BYTES>, 8> watched{};
    };

    static constexpr size_t MAX_JSON_BODY_BYTES = 8192;
    static constexpr size_t DEFAULT_MAX_MULTIPART_REQUEST_BYTES = 2 * 1024 * 1024;
    static constexpr size_t MAX_EVENT_CLIENTS = 4;
    static constexpr uint32_t EVENT_DIRTY_MIN_INTERVAL_MS = 200;
    static constexpr uint32_t EVENT_ACTIVE_STATUS_INTERVAL_MS = 1000;
    static constexpr uint32_t EVENT_IDLE_STATUS_INTERVAL_MS = 5000;
    static constexpr uint32_t HEALTH_PROBE_INTERVAL_MS = 5000;
    static constexpr size_t STATUS_MESSAGE_CAPACITY = 5120;

    static esp_err_t dispatchThunk(httpd_req_t* request);
    static esp_err_t websocketThunk(httpd_req_t* request);
    static void eventWorkThunk(void* context);
    static void healthProbeThunk(void* context);
    static void closeSessionThunk(httpd_handle_t handle, int fd);
    static void ignoreGlobalContextFree(void*) {}

    esp_err_t dispatch(httpd_req_t* request);
    esp_err_t handleWebsocket(httpd_req_t* request);
    bool readBody(httpd_req_t* request, String& bodyOut);
    bool processMultipart(httpd_req_t* request,
                          const Handler& uploadHandler,
                          size_t maximumRequestBytes);
    Route* findRoute(const String& uri, HTTPMethod method);
    void parseQuery(httpd_req_t* request, RequestContext& context);
    void applyResponseMetadata(RequestContext& context, int code, const char* contentType);
    bool finishChunked(RequestContext& context);
    void closeCurrentClient();

    EventClient* findEventClient(int fd);
    EventClient* addEventClient(int fd);
    void removeEventClient(int fd);
    bool watchJob(EventClient& client, const char* id);
    void unwatchJob(EventClient& client, const char* id);
    bool isWatching(const EventClient& client, const char* id) const;
    bool validateOrigin(httpd_req_t* request);
    void sendHello(EventClient& client);
    void sendStatusToAll(const char* type = "status");
    void sendJob(EventClient& client, const char* id);
    void sendJobToWatchers(const char* id);
    void sendErrorEvent(EventClient& client,
                        const char* code,
                        const char* message,
                        const char* jobId = nullptr);
    bool sendEvent(EventClient& client, const String& payload);
    bool sendEvent(EventClient& client, const char* payload, size_t length);
    bool renderStatusEventMessage(const char* type,
                                  bool hello,
                                  size_t& lengthOut);
    String nextEventSequence();
    void drainEventWork();
    void requestEventWork();
    void updateTaskHeartbeat();
    void tickHealthProbe(uint32_t nowMs);

    uint16_t port_{80};
    httpd_handle_t handle_{nullptr};
    std::vector<Route> routes_;
    Handler notFoundHandler_;
    RequestContext* current_{nullptr};
    HTTPUpload upload_;
    BeforeRequest beforeRequest_{nullptr};
    AfterRequest afterRequest_{nullptr};
    void* requestLifecycleContext_{nullptr};

    uint32_t eventBootNonce_{0};
    uint32_t eventCounter_{1};
    StatusRenderer statusRenderer_{nullptr};
    JobRenderer jobRenderer_{nullptr};
    void* eventContext_{nullptr};
    std::array<EventClient, MAX_EVENT_CLIENTS> eventClients_{};
    std::atomic<size_t> eventClientCount_{0};
    std::array<char, STATUS_MESSAGE_CAPACITY> statusMessage_{};
    portMUX_TYPE eventChangesMutex_ = portMUX_INITIALIZER_UNLOCKED;
    EventChanges eventChanges_;
    bool eventWorkQueued_{false};
    uint32_t lastEventDispatchAtMs_{0};
    std::atomic<uint32_t> lastStatusEventAtMs_{0};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> taskHeartbeatAtMs_{0};
    std::atomic<uint32_t> taskStackHighWaterBytes_{0};
    std::atomic<bool> healthProbePending_{false};
    std::atomic<uint32_t> healthProbeQueueFailures_{0};
    std::atomic<uint32_t> lastHealthProbeQueuedAtMs_{0};
};

} // namespace bridge_http
