#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace bridge_http {

struct MultipartPart {
    std::string name;
    std::string filename;
    std::string contentType;
};

// A bounded, streaming parser for the single-file multipart requests used by
// OTA and backup restore. Payload bytes are emitted as soon as it is safe to
// rule out a boundary split, so memory use is independent of upload size.
class MultipartParser {
public:
    using BeginHandler = std::function<bool(const MultipartPart&)>;
    using DataHandler = std::function<bool(const uint8_t*, size_t)>;
    using EndHandler = std::function<bool()>;

    MultipartParser(std::string boundary,
                    BeginHandler beginHandler,
                    DataHandler dataHandler,
                    EndHandler endHandler);

    bool feed(const uint8_t* data, size_t size);
    bool finish();
    bool complete() const;
    const std::string& error() const;

    static constexpr size_t MAX_HEADER_BYTES = 1024;
    static constexpr size_t MAX_BOUNDARY_BYTES = 128;

private:
    enum class State : uint8_t {
        FirstBoundary,
        Headers,
        Data,
        Complete,
        Failed,
    };

    bool process();
    bool parseHeaders(const std::string& headers);
    bool emit(const char* data, size_t size);
    bool fail(const char* message);

    State state_{State::FirstBoundary};
    std::string boundary_;
    std::string firstBoundary_;
    std::string delimiter_;
    std::string buffer_;
    BeginHandler beginHandler_;
    DataHandler dataHandler_;
    EndHandler endHandler_;
    std::string error_;
    bool partStarted_{false};
};

bool extractMultipartBoundary(const std::string& contentType,
                              std::string& boundaryOut,
                              std::string& errorOut);

} // namespace bridge_http
