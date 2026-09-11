#include "bridge_multipart.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace bridge_http {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return value;
}

bool quotedParameter(const std::string& value,
                     const std::string& parameter,
                     std::string& result) {
    const std::string needle = lower(parameter) + "=";
    const std::string lowered = lower(value);
    size_t position = lowered.find(needle);
    while (position != std::string::npos) {
        if (position == 0 || value[position - 1] == ';' ||
            std::isspace(static_cast<unsigned char>(value[position - 1]))) {
            position += needle.size();
            while (position < value.size() &&
                   std::isspace(static_cast<unsigned char>(value[position]))) {
                position++;
            }
            if (position >= value.size()) {
                return false;
            }
            if (value[position] != '"') {
                const size_t end = value.find(';', position);
                result = trim(value.substr(position, end - position));
                return !result.empty();
            }
            position++;
            result.clear();
            bool escaped = false;
            for (; position < value.size(); ++position) {
                const char ch = value[position];
                if (escaped) {
                    result += ch;
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    return true;
                } else {
                    result += ch;
                }
            }
            return false;
        }
        position = lowered.find(needle, position + 1);
    }
    return false;
}

} // namespace

MultipartParser::MultipartParser(std::string boundary,
                                 BeginHandler beginHandler,
                                 DataHandler dataHandler,
                                 EndHandler endHandler)
    : boundary_(std::move(boundary)),
      firstBoundary_("--" + boundary_),
      delimiter_("\r\n--" + boundary_),
      beginHandler_(std::move(beginHandler)),
      dataHandler_(std::move(dataHandler)),
      endHandler_(std::move(endHandler)) {
    buffer_.reserve(MAX_HEADER_BYTES + MAX_BOUNDARY_BYTES + 8);
    if (boundary_.empty() || boundary_.size() > MAX_BOUNDARY_BYTES) {
        fail("multipart boundary is missing or too long");
    }
}

bool MultipartParser::feed(const uint8_t* data, size_t size) {
    if (state_ == State::Complete) {
        // RFC 7578 permits a bounded epilogue after the closing boundary. The
        // transport still enforces the total request length.
        return true;
    }
    if (state_ == State::Failed) {
        return false;
    }
    if (size != 0 && data == nullptr) {
        return fail("multipart input is invalid");
    }
    if (size != 0) {
        buffer_.append(reinterpret_cast<const char*>(data), size);
    }
    return process();
}

bool MultipartParser::finish() {
    if (state_ == State::Complete) {
        return true;
    }
    if (state_ != State::Failed) {
        fail("multipart upload ended before its closing boundary");
    }
    return false;
}

bool MultipartParser::complete() const {
    return state_ == State::Complete;
}

const std::string& MultipartParser::error() const {
    return error_;
}

bool MultipartParser::fail(const char* message) {
    if (state_ != State::Failed) {
        error_ = message != nullptr ? message : "multipart parsing failed";
        state_ = State::Failed;
    }
    return false;
}

bool MultipartParser::emit(const char* data, size_t size) {
    if (size == 0) {
        return true;
    }
    if (!dataHandler_ || !dataHandler_(reinterpret_cast<const uint8_t*>(data), size)) {
        return fail("multipart upload consumer rejected payload data");
    }
    return true;
}

bool MultipartParser::parseHeaders(const std::string& headers) {
    MultipartPart part;
    bool dispositionFound = false;
    size_t offset = 0;
    while (offset <= headers.size()) {
        const size_t end = headers.find("\r\n", offset);
        const std::string line = headers.substr(offset, end - offset);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return fail("multipart part contains an invalid header");
        }
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "content-disposition") {
            if (lower(value).find("form-data") == std::string::npos ||
                !quotedParameter(value, "name", part.name)) {
                return fail("multipart content disposition is invalid");
            }
            quotedParameter(value, "filename", part.filename);
            dispositionFound = true;
        } else if (name == "content-type") {
            part.contentType = value;
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 2;
    }
    if (!dispositionFound || part.name.empty() || part.filename.empty()) {
        return fail("multipart upload must contain one named file field");
    }
    if (!beginHandler_ || !beginHandler_(part)) {
        return fail("multipart upload consumer rejected the file");
    }
    partStarted_ = true;
    return true;
}

bool MultipartParser::process() {
    while (state_ != State::Complete && state_ != State::Failed) {
        if (state_ == State::FirstBoundary) {
            const size_t required = firstBoundary_.size() + 2;
            if (buffer_.size() < required) {
                return true;
            }
            if (buffer_.compare(0, firstBoundary_.size(), firstBoundary_) != 0 ||
                buffer_.compare(firstBoundary_.size(), 2, "\r\n") != 0) {
                return fail("multipart upload has an invalid opening boundary");
            }
            buffer_.erase(0, required);
            state_ = State::Headers;
            continue;
        }

        if (state_ == State::Headers) {
            const size_t end = buffer_.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (buffer_.size() > MAX_HEADER_BYTES) {
                    return fail("multipart headers exceed the bounded limit");
                }
                return true;
            }
            if (end > MAX_HEADER_BYTES || !parseHeaders(buffer_.substr(0, end))) {
                return false;
            }
            buffer_.erase(0, end + 4);
            state_ = State::Data;
            continue;
        }

        if (state_ == State::Data) {
            size_t searchAt = 0;
            size_t boundaryAt = std::string::npos;
            bool finalBoundary = false;
            while (true) {
                const size_t candidate = buffer_.find(delimiter_, searchAt);
                if (candidate == std::string::npos) {
                    break;
                }
                const size_t suffixAt = candidate + delimiter_.size();
                if (buffer_.size() < suffixAt + 2) {
                    boundaryAt = candidate;
                    break;
                }
                if (buffer_.compare(suffixAt, 2, "--") == 0) {
                    boundaryAt = candidate;
                    finalBoundary = true;
                    break;
                }
                if (buffer_.compare(suffixAt, 2, "\r\n") == 0) {
                    return fail("multipart upload contains more than one part");
                }
                searchAt = candidate + 2;
            }

            if (boundaryAt != std::string::npos) {
                if (!finalBoundary) {
                    if (boundaryAt > 0 && !emit(buffer_.data(), boundaryAt)) {
                        return false;
                    }
                    buffer_.erase(0, boundaryAt);
                    return true;
                }
                if (!emit(buffer_.data(), boundaryAt)) {
                    return false;
                }
                const size_t consumed = boundaryAt + delimiter_.size() + 2;
                buffer_.erase(0, consumed);
                if (buffer_.size() >= 2 && buffer_.compare(0, 2, "\r\n") == 0) {
                    buffer_.erase(0, 2);
                }
                if (!partStarted_ || !endHandler_ || !endHandler_()) {
                    return fail("multipart upload consumer could not finish the file");
                }
                state_ = State::Complete;
                return true;
            }

            // Retain enough look-behind for a boundary and its required two
            // suffix bytes. Everything before it is unambiguously payload.
            const size_t retain = delimiter_.size() + 2;
            if (buffer_.size() > retain) {
                const size_t emitBytes = buffer_.size() - retain;
                if (!emit(buffer_.data(), emitBytes)) {
                    return false;
                }
                buffer_.erase(0, emitBytes);
            }
            return true;
        }
    }
    return state_ == State::Complete;
}

bool extractMultipartBoundary(const std::string& contentType,
                              std::string& boundaryOut,
                              std::string& errorOut) {
    boundaryOut.clear();
    errorOut.clear();
    const std::string lowered = lower(contentType);
    if (lowered.find("multipart/form-data") == std::string::npos) {
        errorOut = "request must use multipart/form-data";
        return false;
    }
    if (!quotedParameter(contentType, "boundary", boundaryOut)) {
        errorOut = "multipart boundary is missing";
        return false;
    }
    if (boundaryOut.empty() || boundaryOut.size() > MultipartParser::MAX_BOUNDARY_BYTES) {
        errorOut = "multipart boundary is missing or too long";
        boundaryOut.clear();
        return false;
    }
    return true;
}

} // namespace bridge_http
