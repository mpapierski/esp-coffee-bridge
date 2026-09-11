#pragma once

#include <cstddef>

namespace bridge_json {

struct ObjectExtent {
    size_t openingBrace{0};
    size_t closingBrace{0};
    bool hasMembers{false};
};

inline bool jsonWhitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

// Inspect the bounded outer shape of JSON produced by ArduinoJson. This lets
// callers splice generated metadata without parsing or duplicating a resource
// document that may be tens of kilobytes long.
inline bool inspectObject(const char* json, size_t length, ObjectExtent& extentOut) {
    extentOut = {};
    if (json == nullptr || length < 2) {
        return false;
    }
    size_t opening = 0;
    while (opening < length && jsonWhitespace(json[opening])) {
        ++opening;
    }
    size_t closing = length;
    while (closing > opening && jsonWhitespace(json[closing - 1])) {
        --closing;
    }
    if (closing <= opening + 1 || json[opening] != '{' || json[closing - 1] != '}') {
        return false;
    }
    extentOut.openingBrace = opening;
    extentOut.closingBrace = closing - 1;
    for (size_t index = opening + 1; index < extentOut.closingBrace; ++index) {
        if (!jsonWhitespace(json[index])) {
            extentOut.hasMembers = true;
            break;
        }
    }
    return true;
}

} // namespace bridge_json
