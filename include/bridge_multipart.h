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

// A single-file multipart request adds an opening boundary, bounded headers,
// and a closing boundary around the payload. Keep the transport allowance
// explicit so endpoint payload limits can be converted to request limits
// without silently falling back to the generic upload ceiling.
inline constexpr size_t MULTIPART_REQUEST_ENVELOPE_ALLOWANCE_BYTES = 2 * 1024;

inline constexpr size_t multipartRequestLimit(size_t payloadBytes) {
    return payloadBytes > SIZE_MAX - MULTIPART_REQUEST_ENVELOPE_ALLOWANCE_BYTES
        ? SIZE_MAX
        : payloadBytes + MULTIPART_REQUEST_ENVELOPE_ALLOWANCE_BYTES;
}

static_assert(
    MULTIPART_REQUEST_ENVELOPE_ALLOWANCE_BYTES >=
        MultipartParser::MAX_HEADER_BYTES + 2 * MultipartParser::MAX_BOUNDARY_BYTES + 16,
    "Multipart request allowance must cover the parser's bounded envelope");

bool extractMultipartBoundary(const std::string& contentType,
                              std::string& boundaryOut,
                              std::string& errorOut);

} // namespace bridge_http
