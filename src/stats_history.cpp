#include "stats_history.h"

#include <LittleFS.h>

#include <algorithm>
#include <vector>

namespace stats_history {

namespace {

constexpr char HISTORY_PREFIX[] = "/stathist-";
constexpr char HISTORY_PREFIX_BARE[] = "stathist-";

String sanitizeText(const String& value, size_t maxLength) {
    String out = value;
    out.trim();
    out.replace('\r', ' ');
    out.replace('\n', ' ');
    out.replace('\t', ' ');
    if (out.length() > maxLength) {
        out.remove(maxLength);
        out.trim();
    }
    return out;
}

String sanitizeText(JsonVariantConst value, size_t maxLength) {
    if (value.isNull()) {
        return "";
    }
    return sanitizeText(value.as<String>(), maxLength);
}

String safeToken(const String& value) {
    String out;
    out.reserve(value.length());
    for (size_t index = 0; index < value.length(); ++index) {
        const char ch = value.charAt(index);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
            out += ch;
        } else {
            out += '_';
        }
    }
    return out;
}

String historyPath(const String& serial) {
    return String(HISTORY_PREFIX) + safeToken(serial) + ".jsonl";
}

String normalizeHistoryPath(const String& path) {
    if (path.startsWith("/")) {
        return path;
    }
    return String("/") + path;
}

bool listHistoryPaths(std::vector<String>& pathsOut, String& error) {
    error = "";
    pathsOut.clear();

    File root = LittleFS.open("/");
    if (!root) {
        error = "failed to open stats history root";
        return false;
    }

    File entry = root.openNextFile();
    while (entry) {
        const String path = entry.name();
        entry.close();
        if (path.startsWith(HISTORY_PREFIX) || path.startsWith(HISTORY_PREFIX_BARE)) {
            pathsOut.push_back(normalizeHistoryPath(path));
        }
        entry = root.openNextFile();
    }
    root.close();
    return true;
}

bool readLine(File& file, String& lineOut) {
    while (file.available()) {
        lineOut = file.readStringUntil('\n');
        if (lineOut.endsWith("\r")) {
            lineOut.remove(lineOut.length() - 1);
        }
        if (!lineOut.isEmpty()) {
            return true;
        }
    }
    lineOut = "";
    return false;
}

bool serializeEntry(JsonObjectConst entry, String& lineOut, String& error) {
    if (!validateEntry(entry, error)) {
        return false;
    }
    lineOut = "";
    lineOut.reserve(measureJson(entry) + 8);
    if (serializeJson(entry, lineOut) == 0 || lineOut.isEmpty()) {
        error = "failed to serialize stats history entry";
        return false;
    }
    if ((lineOut.length() + 1) > DEFAULT_HISTORY_BYTES) {
        error = "stats history entry exceeds the configured size limit";
        return false;
    }
    return true;
}

bool writeLines(const String& path, const std::vector<String>& lines, String& error) {
    File file = LittleFS.open(path, "w");
    if (!file) {
        error = "failed to open stats history for writing";
        return false;
    }
    for (const String& line : lines) {
        if (file.print(line) != line.length() || file.write('\n') != 1) {
            file.close();
            error = "failed to write stats history";
            return false;
        }
    }
    file.close();
    return true;
}

bool compact(const String& path, String& error) {
    File file = LittleFS.open(path, "r");
    if (!file) {
        error = "failed to open stats history for compaction";
        return false;
    }

    std::vector<String> retained;
    size_t retainedBytes = 0;
    String line;
    while (readLine(file, line)) {
        const size_t lineBytes = line.length() + 1;
        if (lineBytes > DEFAULT_HISTORY_BYTES) {
            continue;
        }
        retained.push_back(line);
        retainedBytes += lineBytes;
        while (!retained.empty() && retainedBytes > DEFAULT_HISTORY_BYTES) {
            retainedBytes -= retained.front().length() + 1;
            retained.erase(retained.begin());
        }
    }
    file.close();
    return writeLines(path, retained, error);
}

bool loadLatestEntry(const String& serial, DynamicJsonDocument& docOut, bool& foundOut, String& error) {
    error = "";
    foundOut = false;
    docOut.clear();

    File file = LittleFS.open(historyPath(serial), "r");
    if (!file) {
        return true;
    }

    String line;
    String lastLine;
    while (readLine(file, line)) {
        lastLine = line;
    }
    file.close();

    if (lastLine.isEmpty()) {
        return true;
    }

    const DeserializationError parseError = deserializeJson(docOut, lastLine);
    if (parseError) {
        error = String("failed to parse latest stats history entry: ") + parseError.c_str();
        return false;
    }
    foundOut = true;
    return true;
}

void buildCompactValues(JsonObject target, JsonObjectConst liveValues) {
    std::vector<std::pair<String, int32_t>> items;
    for (JsonPairConst item : liveValues) {
        const JsonObjectConst valueItem = item.value().as<JsonObjectConst>();
        if (valueItem.isNull() || valueItem["rawValue"].isNull()) {
            continue;
        }
        items.emplace_back(String(item.key().c_str()), valueItem["rawValue"].as<int32_t>());
    }

    std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    for (const auto& item : items) {
        target[item.first] = item.second;
    }
}

bool valuesEqual(JsonObjectConst left, JsonObjectConst right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (JsonPairConst item : left) {
        const JsonVariantConst other = right[item.key()];
        if (other.isNull()) {
            return false;
        }
        if (item.value().as<int32_t>() != other.as<int32_t>()) {
            return false;
        }
    }
    return true;
}

void buildDelta(JsonObjectConst currentValues,
                JsonObjectConst previousValues,
                JsonObject target,
                JsonArray changedKeysOut,
                AppendResult& resultOut) {
    for (JsonPairConst item : currentValues) {
        const String key = String(item.key().c_str());
        const int32_t currentValue = item.value().as<int32_t>();
        const JsonVariantConst previousVariant = previousValues[key];
        const bool hadPrevious = !previousVariant.isNull();
        const int32_t previousValue = hadPrevious ? previousVariant.as<int32_t>() : 0;
        if (hadPrevious && currentValue == previousValue) {
            continue;
        }
        const int32_t delta = currentValue - previousValue;
        target[key] = delta;
        changedKeysOut.add(key);
        resultOut.changedMetricCount++;
        if (key == "total_beverages") {
            resultOut.hasTotalDelta = true;
            resultOut.totalDelta = delta;
        }
    }
}

} // namespace

String filePath(const String& serial) {
    return historyPath(serial);
}

bool validateEntry(JsonObjectConst entry, String& error) {
    error = "";
    if ((entry["schema"] | 0U) != 1U) {
        error = "stats history entry schema must be 1";
        return false;
    }

    const JsonObjectConst values = entry["values"].as<JsonObjectConst>();
    if (values.isNull() || values.size() == 0) {
        error = "stats history entry values are required";
        return false;
    }

    for (JsonPairConst item : values) {
        if (item.value().isNull()) {
            error = "stats history entry values must be numeric";
            return false;
        }
        (void)item.value().as<int32_t>();
    }
    return true;
}

bool append(const String& serial, JsonObjectConst entry, String& error) {
    String line;
    if (!serializeEntry(entry, line, error)) {
        return false;
    }
    return appendSerializedLines(serial, std::vector<String>{line}, error);
}

bool appendSerializedLines(const String& serial, const std::vector<String>& lines, String& error) {
    error = "";
    if (serial.isEmpty()) {
        error = "serial is required for stats history";
        return false;
    }
    if (lines.empty()) {
        return true;
    }

    const String path = historyPath(serial);
    File file = LittleFS.open(path, "a");
    if (!file) {
        error = "failed to open stats history for append";
        return false;
    }

    bool wroteAll = true;
    for (const String& line : lines) {
        if (file.print(line) != line.length() || file.write('\n') != 1) {
            wroteAll = false;
            break;
        }
    }
    const size_t finalSize = file.size();
    file.close();

    if (!wroteAll) {
        error = "failed to append stats history entry";
        return false;
    }
    if (finalSize > DEFAULT_HISTORY_BYTES) {
        return compact(path, error);
    }
    return true;
}

bool recordSnapshotIfChanged(const String& serial,
                             JsonObjectConst liveValues,
                             const bridge_time::StatusSnapshot& timeStatus,
                             uint32_t loggedAtMs,
                             const String& source,
                             AppendResult& resultOut,
                             String& error) {
    error = "";
    resultOut = AppendResult{};
    if (serial.isEmpty()) {
        error = "serial is required for stats history";
        return false;
    }

    DynamicJsonDocument currentDoc(4096);
    JsonObject currentEntry = currentDoc.to<JsonObject>();
    currentEntry["schema"] = 1;
    currentEntry["loggedAtMs"] = loggedAtMs;
    currentEntry["source"] = sanitizeText(source, 24).isEmpty() ? "stats" : sanitizeText(source, 24);
    currentEntry["timeSynced"] = timeStatus.synced;
    if (timeStatus.synced) {
        currentEntry["timeUnix"] = static_cast<int64_t>(timeStatus.unixTime);
        currentEntry["timeIsoUtc"] = timeStatus.iso8601Utc;
    }
    JsonObject currentValues = currentEntry.createNestedObject("values");
    buildCompactValues(currentValues, liveValues);
    if (currentValues.size() == 0) {
        error = "stats history snapshot requires at least one metric";
        return false;
    }

    DynamicJsonDocument previousDoc(4096);
    bool previousFound = false;
    if (!loadLatestEntry(serial, previousDoc, previousFound, error)) {
        return false;
    }

    if (!previousFound) {
        currentEntry["baseline"] = true;
        resultOut.baseline = true;
    } else {
        const JsonObjectConst previousValues = previousDoc["values"].as<JsonObjectConst>();
        if (!previousValues.isNull() && valuesEqual(currentValues, previousValues)) {
            return true;
        }
        JsonObject delta = currentEntry.createNestedObject("delta");
        JsonArray changedKeys = currentEntry.createNestedArray("changedKeys");
        buildDelta(currentValues, previousValues, delta, changedKeys, resultOut);
        currentEntry["changedCount"] = resultOut.changedMetricCount;
    }

    if (!append(serial, currentEntry, error)) {
        return false;
    }
    resultOut.appended = true;
    return true;
}

bool loadPage(const String& serial,
              size_t offset,
              size_t limit,
              JsonArray entriesOut,
              Stats& statsOut,
              Page& pageOut,
              String& error) {
    error = "";
    statsOut = Stats{};
    pageOut = Page{};
    statsOut.maxBytes = DEFAULT_HISTORY_BYTES;

    if (serial.isEmpty()) {
        error = "serial is required for stats history";
        return false;
    }

    pageOut.offset = offset;
    pageOut.limit = limit;

    File file = LittleFS.open(historyPath(serial), "r");
    if (!file) {
        return true;
    }

    statsOut.fileBytes = file.size();
    std::vector<String> allLines;
    String line;
    while (readLine(file, line)) {
        statsOut.entryCount++;
        allLines.push_back(line);
    }
    file.close();

    const size_t effectiveOffset = std::min(offset, statsOut.entryCount);
    const size_t endExclusive = statsOut.entryCount > effectiveOffset ? statsOut.entryCount - effectiveOffset : 0;
    const size_t startInclusive = limit > 0 && endExclusive > limit ? endExclusive - limit : 0;

    pageOut.offset = effectiveOffset;
    pageOut.returned = endExclusive - startInclusive;
    pageOut.hasOlder = startInclusive > 0;
    pageOut.hasNewer = effectiveOffset > 0;
    pageOut.nextOffset = pageOut.hasOlder ? effectiveOffset + limit : effectiveOffset;
    pageOut.prevOffset = pageOut.hasNewer ? (effectiveOffset > limit ? effectiveOffset - limit : 0) : 0;

    for (size_t index = endExclusive; index > startInclusive; --index) {
        DynamicJsonDocument lineDoc(4096);
        const DeserializationError parseError = deserializeJson(lineDoc, allLines[index - 1]);
        if (parseError) {
            statsOut.skippedEntries++;
            continue;
        }
        JsonObject item = entriesOut.createNestedObject();
        item.set(lineDoc.as<JsonObjectConst>());
    }
    return true;
}

bool clear(const String& serial, String& error) {
    error = "";
    if (serial.isEmpty()) {
        error = "serial is required for stats history";
        return false;
    }
    const String path = historyPath(serial);
    if (!LittleFS.exists(path)) {
        return true;
    }
    if (!LittleFS.remove(path)) {
        error = "failed to remove stats history";
        return false;
    }
    return true;
}

bool clearAll(String& error) {
    error = "";
    std::vector<String> paths;
    if (!listHistoryPaths(paths, error)) {
        return false;
    }
    for (const String& path : paths) {
        if (!LittleFS.remove(path)) {
            error = String("failed to remove stats history file ") + path;
            return false;
        }
    }
    return true;
}

} // namespace stats_history
