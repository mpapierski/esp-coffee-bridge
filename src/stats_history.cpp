#include "stats_history.h"

#include <LittleFS.h>

#include <algorithm>
#include <vector>

#include "history_paging.h"
#include "history_retention.h"
#include "history_storage.h"

namespace stats_history {

namespace {

constexpr char HISTORY_PREFIX[] = "/stathist-";
constexpr char HISTORY_PREFIX_BARE[] = "stathist-";
size_t gBudgetBytes = DEFAULT_HISTORY_BYTES;

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

bool listHistoryPaths(std::vector<String>& pathsOut, String& error, bool includeArtifacts = false) {
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
        if ((path.startsWith(HISTORY_PREFIX) || path.startsWith(HISTORY_PREFIX_BARE)) &&
            (includeArtifacts || path.endsWith(".jsonl"))) {
            pathsOut.push_back(normalizeHistoryPath(path));
        }
        entry = root.openNextFile();
    }
    root.close();
    return true;
}

bool countPhysicalLines(File& file, size_t& lineCountOut, String& error) {
    lineCountOut = 0;
    if (!file.seek(0)) {
        error = "failed to seek stats history";
        return false;
    }

    history_paging::PhysicalLineCounter counter;
    char buffer[256];
    while (file.available()) {
        const size_t readCount = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
        if (readCount == 0) {
            error = "failed to read stats history";
            return false;
        }
        counter.consume(buffer, readCount);
    }
    lineCountOut = counter.lineCount();
    return true;
}

bool collectPageOffsets(File& file,
                        const history_paging::Window& window,
                        history_paging::OffsetCollector& offsetsOut,
                        String& error) {
    if (!file.seek(0)) {
        error = "failed to seek stats history";
        return false;
    }

    history_paging::PhysicalLineCounter counter;
    char buffer[256];
    while (file.available()) {
        const size_t readCount = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
        if (readCount == 0) {
            error = "failed to index stats history";
            return false;
        }
        counter.consume(buffer, readCount);
        offsetsOut.consume(buffer, readCount);
    }
    if (counter.lineCount() != window.totalLines) {
        error = "stats history changed while it was being indexed";
        return false;
    }
    return offsetsOut.count() == window.selectedCount;
}

bool readBoundedLineAt(File& file,
                       size_t offset,
                       String& lineOut,
                       bool& exceededLimitOut,
                       String& error) {
    lineOut = "";
    lineOut.reserve(512);
    exceededLimitOut = false;
    if (offset > 0) {
        if (!file.seek(offset - 1) || file.read() != '\n') {
            error = "stats history changed while its page was being read";
            return false;
        }
    }
    if (!file.seek(offset)) {
        error = "failed to seek stats history entry";
        return false;
    }
    while (file.available()) {
        const int value = file.read();
        if (value < 0) {
            error = "failed to read stats history entry";
            return false;
        }
        if (value == '\n') {
            break;
        }
        if (lineOut.length() < history_storage::MAX_JSON_LINE_BYTES) {
            lineOut += static_cast<char>(value);
        } else {
            exceededLimitOut = true;
        }
    }
    if (lineOut.endsWith("\r")) {
        lineOut.remove(lineOut.length() - 1);
    }
    return true;
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
    if ((lineOut.length() + 1) > budgetBytes()) {
        error = "stats history entry exceeds the configured size limit";
        return false;
    }
    return true;
}

bool loadLatestEntry(const String& serial, DynamicJsonDocument& docOut, bool& foundOut, String& error) {
    error = "";
    foundOut = false;
    docOut.clear();

    const String path = historyPath(serial);
    if (!history_storage::recoverFile(path, error)) {
        return false;
    }
    File file = LittleFS.open(path, "r");
    if (!file) {
        return true;
    }

    size_t lineCount = 0;
    if (!countPhysicalLines(file, lineCount, error)) {
        file.close();
        return false;
    }
    if (lineCount == 0) {
        file.close();
        return true;
    }

    const history_paging::Window window = history_paging::makeWindow(
        lineCount, 0, history_paging::MAX_PAGE_SIZE);
    history_paging::OffsetCollector offsets(window);
    if (!collectPageOffsets(file, window, offsets, error) || offsets.count() == 0) {
        file.close();
        if (error.isEmpty()) {
            error = "stats history changed while reading its latest entry";
        }
        return false;
    }
    // A torn final append must not permanently disable sampling. Walk the
    // bounded newest page backwards and use the newest valid snapshot.
    for (size_t index = offsets.count(); index > 0; --index) {
        String candidate;
        bool exceededLimit = false;
        if (!readBoundedLineAt(file, offsets.offsetAt(index - 1), candidate, exceededLimit, error)) {
            file.close();
            return false;
        }
        if (exceededLimit || candidate.isEmpty()) {
            continue;
        }
        docOut.clear();
        const DeserializationError parseError = deserializeJson(docOut, candidate);
        String validationError;
        if (!parseError && validateEntry(docOut.as<JsonObjectConst>(), validationError)) {
            file.close();
            foundOut = true;
            return true;
        }
    }
    file.close();
    docOut.clear();
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

String formatIso8601Utc(time_t epoch) {
    struct tm utcTime;
    if (gmtime_r(&epoch, &utcTime) == nullptr) {
        return "";
    }
    char buffer[32];
    if (strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utcTime) == 0) {
        return "";
    }
    return String(buffer);
}

bool applyTimestampPatch(JsonObject target, JsonObjectConst patch, String& error) {
    int64_t timeUnix = 0;
    if (!patch["timeUnixMs"].isNull()) {
        const int64_t milliseconds = patch["timeUnixMs"].as<int64_t>();
        if (milliseconds <= 0) {
            error = "timeUnixMs must be positive";
            return false;
        }
        timeUnix = milliseconds / 1000;
    } else if (!patch["timeUnix"].isNull()) {
        timeUnix = patch["timeUnix"].as<int64_t>();
        if (timeUnix <= 0) {
            error = "timeUnix must be positive";
            return false;
        }
    } else {
        error = "timeUnix or timeUnixMs is required";
        return false;
    }

    const String iso = sanitizeText(patch["timeIsoUtc"], 32);
    const String source = sanitizeText(patch["timeSource"], 16);
    target["timeUnix"] = timeUnix;
    target["timeIsoUtc"] = iso.isEmpty() ? formatIso8601Utc(static_cast<time_t>(timeUnix)) : iso;
    target["timeSource"] = source.isEmpty() ? "patched" : source;
    target["timeSynced"] = patch["timeSynced"].isNull() ? false : patch["timeSynced"].as<bool>();
    return true;
}

bool transferPhysicalLine(File& source,
                          File* destination,
                          String* capturedOut,
                          bool& hadLineOut,
                          bool& exceededCaptureLimitOut,
                          String& error) {
    hadLineOut = source.available();
    exceededCaptureLimitOut = false;
    if (!hadLineOut) {
        return true;
    }
    if (capturedOut != nullptr) {
        *capturedOut = "";
        capturedOut->reserve(512);
    }

    uint8_t outputBuffer[256];
    size_t outputLength = 0;
    while (source.available()) {
        const int value = source.read();
        if (value < 0) {
            error = "failed to read stats history during rewrite";
            return false;
        }
        const char ch = static_cast<char>(value);
        if (destination != nullptr) {
            outputBuffer[outputLength++] = static_cast<uint8_t>(ch);
            if (outputLength == sizeof(outputBuffer)) {
                if (destination->write(outputBuffer, outputLength) != outputLength) {
                    error = "failed to write temporary stats history";
                    return false;
                }
                outputLength = 0;
            }
        }
        if (capturedOut != nullptr && ch != '\n') {
            if (capturedOut->length() < history_storage::MAX_JSON_LINE_BYTES) {
                *capturedOut += ch;
            } else {
                exceededCaptureLimitOut = true;
            }
        }
        if (ch == '\n') {
            break;
        }
    }
    if (destination != nullptr && outputLength > 0 &&
        destination->write(outputBuffer, outputLength) != outputLength) {
        error = "failed to write temporary stats history";
        return false;
    }
    if (capturedOut != nullptr && capturedOut->endsWith("\r")) {
        capturedOut->remove(capturedOut->length() - 1);
    }
    return true;
}

void abandonRewrite(File& source, File& temporary, const String& temporaryPath) {
    source.close();
    temporary.close();
    LittleFS.remove(temporaryPath);
}

} // namespace

String filePath(const String& serial) {
    return historyPath(serial);
}

void configureBudget(size_t requestedBytes, size_t preservedFileBytes) {
    const size_t configured = std::max(
        MIN_HISTORY_BYTES,
        requestedBytes > 0 ? requestedBytes : DEFAULT_HISTORY_BYTES);
    gBudgetBytes = std::max(configured, preservedFileBytes);
}

size_t budgetBytes() {
    return gBudgetBytes;
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

bool buildImportedLines(JsonVariantConst payload,
                        std::vector<String>& linesOut,
                        size_t& importedCount,
                        String& error) {
    error = "";
    linesOut.clear();
    importedCount = 0;

    auto appendEntry = [&](JsonObjectConst entry, size_t index) -> bool {
        String entryError;
        if (entry.isNull() || !validateEntry(entry, entryError)) {
            error = String("entry ") + String(index + 1) + ": " +
                (entryError.isEmpty() ? String("invalid stats history entry") : entryError);
            return false;
        }
        String line;
        line.reserve(measureJson(entry) + 8);
        if (serializeJson(entry, line) == 0 || line.isEmpty() ||
            line.length() + 1 > history_storage::MAX_JSON_LINE_BYTES) {
            error = String("entry ") + String(index + 1) +
                ": stats history entry exceeds the bounded line limit";
            return false;
        }
        linesOut.push_back(line);
        importedCount++;
        return true;
    };

    const JsonArrayConst entries = payload.as<JsonArrayConst>();
    if (!entries.isNull()) {
        size_t index = 0;
        for (JsonVariantConst item : entries) {
            if (!appendEntry(item.as<JsonObjectConst>(), index++)) {
                return false;
            }
        }
        if (importedCount == 0) {
            error = "stats history import requires at least one entry";
            return false;
        }
        return true;
    }
    return appendEntry(payload.as<JsonObjectConst>(), 0);
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

    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }

    const String path = historyPath(serial);
    if (!history_storage::recoverFile(path, error)) {
        return false;
    }
    bool needsSeparator = false;
    size_t existingBytes = 0;
    if (LittleFS.exists(path)) {
        File existing = LittleFS.open(path, "r");
        if (!existing) {
            error = "failed to inspect stats history tail";
            return false;
        }
        existingBytes = existing.size();
        if (existingBytes > 0) {
            if (!existing.seek(existingBytes - 1)) {
                existing.close();
                error = "failed to seek stats history tail";
                return false;
            }
            needsSeparator = existing.read() != '\n';
        }
        existing.close();
    }
    size_t incomingBytes = needsSeparator ? 1U : 0U;
    for (const String& line : lines) {
        incomingBytes += line.length() + 1;
    }
    if (!history_retention::appendFits(existingBytes, incomingBytes, budgetBytes())) {
        error = "stats history is full; existing entries were preserved";
        return false;
    }
    const size_t filesystemBytes = LittleFS.totalBytes();
    const size_t usedBytes = LittleFS.usedBytes();
    const size_t freeBytes = filesystemBytes > usedBytes ? filesystemBytes - usedBytes : 0;
    if (freeBytes < history_storage::writeReserveBytes(filesystemBytes) + incomingBytes) {
        error = "stats history append would consume transactional filesystem headroom";
        return false;
    }
    File file = LittleFS.open(path, "a");
    if (!file) {
        error = "failed to open stats history for append";
        return false;
    }

    bool wroteAll = !needsSeparator || file.write('\n') == 1;
    for (const String& line : lines) {
        if (file.print(line) != line.length() || file.write('\n') != 1) {
            wroteAll = false;
            break;
        }
    }
    file.close();

    if (!wroteAll) {
        error = "failed to append stats history entry";
        return false;
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

    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }

    DynamicJsonDocument currentDoc(4096);
    JsonObject currentEntry = currentDoc.to<JsonObject>();
    currentEntry["schema"] = 1;
    currentEntry["loggedAtMs"] = loggedAtMs;
    currentEntry["source"] = sanitizeText(source, 24).isEmpty() ? "stats" : sanitizeText(source, 24);
    currentEntry["timeSynced"] = timeStatus.synced;
    if (timeStatus.available) {
        currentEntry["timeUnix"] = static_cast<int64_t>(timeStatus.unixTime);
        currentEntry["timeIsoUtc"] = timeStatus.iso8601Utc;
    }
    if (timeStatus.synced) {
        currentEntry["timeSource"] = "ntp";
    } else if (timeStatus.restored) {
        currentEntry["timeSource"] = "restored";
    } else if (timeStatus.clientSeeded) {
        currentEntry["timeSource"] = "client";
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

bool patchTimestamp(const String& serial,
                    size_t entryId,
                    JsonObjectConst patch,
                    JsonObject updatedOut,
                    String& error) {
    error = "";
    if (serial.isEmpty()) {
        error = "serial is required for stats history";
        return false;
    }

    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }
    const String path = historyPath(serial);
    if (!history_storage::recoverFile(path, error)) {
        return false;
    }
    File source = LittleFS.open(path, "r");
    if (!source) {
        error = "stats history entry not found";
        return false;
    }

    const String temporaryPath = path + ".tmp";
    LittleFS.remove(temporaryPath);
    File temporary = LittleFS.open(temporaryPath, "w");
    if (!temporary) {
        source.close();
        error = "failed to open temporary stats history";
        return false;
    }

    size_t currentEntryId = 0;
    size_t writtenLines = 0;
    bool found = false;
    while (source.available()) {
        if (currentEntryId == entryId) {
            String line;
            bool hadLine = false;
            bool exceededLimit = false;
            if (!transferPhysicalLine(source, nullptr, &line, hadLine, exceededLimit, error)) {
                abandonRewrite(source, temporary, temporaryPath);
                return false;
            }
            if (!hadLine || exceededLimit || line.isEmpty()) {
                abandonRewrite(source, temporary, temporaryPath);
                error = String("failed to parse stats history entry ") + currentEntryId;
                return false;
            }
            DynamicJsonDocument lineDoc(4096);
            const DeserializationError parseError = deserializeJson(lineDoc, line);
            if (parseError) {
                abandonRewrite(source, temporary, temporaryPath);
                error = String("failed to parse stats history entry ") + currentEntryId;
                return false;
            }
            JsonObject entry = lineDoc.as<JsonObject>();
            if (!applyTimestampPatch(entry, patch, error)) {
                abandonRewrite(source, temporary, temporaryPath);
                return false;
            }
            updatedOut.set(entry);
            updatedOut["entryId"] = static_cast<uint32_t>(entryId);
            String serialized;
            if (!serializeEntry(entry, serialized, error) ||
                temporary.print(serialized) != serialized.length() ||
                temporary.write('\n') != 1) {
                if (error.isEmpty()) {
                    error = "failed to write patched stats history entry";
                }
                abandonRewrite(source, temporary, temporaryPath);
                return false;
            }
            found = true;
        } else {
            bool hadLine = false;
            bool exceededLimit = false;
            if (!transferPhysicalLine(source, &temporary, nullptr, hadLine, exceededLimit, error)) {
                abandonRewrite(source, temporary, temporaryPath);
                return false;
            }
        }
        currentEntryId++;
        writtenLines++;
    }
    source.close();
    temporary.close();

    if (!found) {
        LittleFS.remove(temporaryPath);
        error = "stats history entry not found";
        return false;
    }
    return history_storage::commitTemporaryFile(path,
                                                temporaryPath,
                                                budgetBytes(),
                                                writtenLines,
                                                error);
}

bool deleteEntry(const String& serial,
                 size_t entryId,
                 JsonObject deletedOut,
                 String& error) {
    error = "";
    if (serial.isEmpty()) {
        error = "serial is required for stats history";
        return false;
    }

    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }
    const String path = historyPath(serial);
    if (!history_storage::recoverFile(path, error)) {
        return false;
    }
    File source = LittleFS.open(path, "r");
    if (!source) {
        error = "stats history entry not found";
        return false;
    }

    const String temporaryPath = path + ".tmp";
    LittleFS.remove(temporaryPath);
    File temporary = LittleFS.open(temporaryPath, "w");
    if (!temporary) {
        source.close();
        error = "failed to open temporary stats history";
        return false;
    }

    size_t currentEntryId = 0;
    size_t writtenLines = 0;
    bool found = false;
    while (source.available()) {
        if (currentEntryId == entryId) {
            String line;
            bool hadLine = false;
            bool exceededLimit = false;
            if (!transferPhysicalLine(source, nullptr, &line, hadLine, exceededLimit, error)) {
                abandonRewrite(source, temporary, temporaryPath);
                return false;
            }
            if (!hadLine || exceededLimit || line.isEmpty()) {
                abandonRewrite(source, temporary, temporaryPath);
                error = String("failed to parse stats history entry ") + currentEntryId;
                return false;
            }
            DynamicJsonDocument lineDoc(4096);
            const DeserializationError parseError = deserializeJson(lineDoc, line);
            if (parseError) {
                abandonRewrite(source, temporary, temporaryPath);
                error = String("failed to parse stats history entry ") + currentEntryId;
                return false;
            }
            const JsonObject entry = lineDoc.as<JsonObject>();
            deletedOut.set(entry);
            deletedOut["entryId"] = static_cast<uint32_t>(entryId);
            found = true;
        } else {
            bool hadLine = false;
            bool exceededLimit = false;
            if (!transferPhysicalLine(source, &temporary, nullptr, hadLine, exceededLimit, error)) {
                abandonRewrite(source, temporary, temporaryPath);
                return false;
            }
            writtenLines++;
        }
        currentEntryId++;
    }
    source.close();
    temporary.close();

    if (!found) {
        LittleFS.remove(temporaryPath);
        error = "stats history entry not found";
        return false;
    }
    return history_storage::commitTemporaryFile(path,
                                                temporaryPath,
                                                budgetBytes(),
                                                writtenLines,
                                                error);
}

bool visitPage(const String& serial,
               size_t offset,
               size_t limit,
               PageEntryVisitor visitor,
               void* visitorContext,
               Stats& statsOut,
               Page& pageOut,
               String& error) {
    error = "";
    statsOut = Stats{};
    pageOut = Page{};
    statsOut.maxBytes = budgetBytes();

    if (serial.isEmpty()) {
        error = "serial is required for stats history";
        return false;
    }

    pageOut.offset = offset;
    pageOut.limit = limit;

    const String path = historyPath(serial);
    {
        history_storage::Guard filesystem;
        if (!filesystem) {
            error = "failed to lock history storage";
            return false;
        }
        if (!history_storage::recoverFile(path, error)) {
            return false;
        }
        File file = LittleFS.open(path, "r");
        if (!file) {
            return true;
        }

        statsOut.fileBytes = file.size();
        if (!countPhysicalLines(file, statsOut.entryCount, error)) {
            file.close();
            return false;
        }
        file.close();
    }

    const history_paging::Window window = history_paging::makeWindow(statsOut.entryCount, offset, limit);
    pageOut.offset = window.offset;
    pageOut.limit = window.limit;
    pageOut.returned = window.selectedCount;
    pageOut.hasOlder = window.hasOlder;
    pageOut.hasNewer = window.hasNewer;
    pageOut.nextOffset = window.nextOffset;
    pageOut.prevOffset = window.prevOffset;

    history_paging::OffsetCollector offsets(window);
    {
        history_storage::Guard filesystem;
        if (!filesystem) {
            error = "failed to lock history storage";
            return false;
        }
        File file = LittleFS.open(path, "r");
        if (!file || file.size() != statsOut.fileBytes) {
            if (file) {
                file.close();
            }
            error = "stats history changed while it was being indexed";
            return false;
        }
        if (!collectPageOffsets(file, window, offsets, error)) {
            file.close();
            if (error.isEmpty()) {
                error = "stats history changed while it was being indexed";
            }
            return false;
        }
        file.close();
    }

    String line;
    line.reserve(512);
    DynamicJsonDocument lineDoc(4096);
    for (size_t selectedIndex = offsets.count(); selectedIndex > 0; --selectedIndex) {
        bool exceededLimit = false;
        bool invalidEntry = false;
        {
            history_storage::Guard filesystem;
            if (!filesystem) {
                error = "failed to lock history storage";
                return false;
            }
            File file = LittleFS.open(path, "r");
            if (!file || file.size() != statsOut.fileBytes) {
                if (file) {
                    file.close();
                }
                error = "stats history changed while its page was being read";
                return false;
            }
            if (!readBoundedLineAt(file, offsets.offsetAt(selectedIndex - 1), line, exceededLimit, error)) {
                file.close();
                return false;
            }
            file.close();

            lineDoc.clear();
            invalidEntry = exceededLimit || line.isEmpty() || deserializeJson(lineDoc, line);
        }
        if (invalidEntry) {
            statsOut.skippedEntries++;
            continue;
        }

        const size_t physicalIndex = window.startInclusive + selectedIndex - 1;
        JsonObject item = lineDoc.as<JsonObject>();
        item["entryId"] = static_cast<uint32_t>(physicalIndex);
        // The visitor may stream over the network. Keep it outside the
        // filesystem guard so slow clients cannot block history writers.
        if (visitor != nullptr && !visitor(item, physicalIndex, visitorContext, error)) {
            if (error.isEmpty()) {
                error = "stats history page visitor stopped";
            }
            return false;
        }
    }
    return true;
}

bool loadPage(const String& serial,
              size_t offset,
              size_t limit,
              JsonArray entriesOut,
              Stats& statsOut,
              Page& pageOut,
              String& error) {
    const auto appendEntry = [](JsonObjectConst entry,
                                size_t,
                                void* context,
                                String&) -> bool {
        auto* entries = static_cast<JsonArray*>(context);
        JsonObject item = entries->createNestedObject();
        item.set(entry);
        return true;
    };
    return visitPage(serial,
                     offset,
                     limit,
                     appendEntry,
                     &entriesOut,
                     statsOut,
                     pageOut,
                     error);
}

bool clear(const String& serial, String& error) {
    error = "";
    if (serial.isEmpty()) {
        error = "serial is required for stats history";
        return false;
    }
    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }
    const String path = historyPath(serial);
    if (!history_storage::recoverFile(path, error)) {
        return false;
    }
    if (LittleFS.exists(path) && !LittleFS.remove(path)) {
        error = "failed to remove stats history";
        return false;
    }
    const String temporaryPath = path + ".tmp";
    const String backupPath = path + ".bak";
    if ((LittleFS.exists(temporaryPath) && !LittleFS.remove(temporaryPath)) ||
        (LittleFS.exists(backupPath) && !LittleFS.remove(backupPath))) {
        error = "failed to remove stats history rewrite artifact";
        return false;
    }
    return true;
}

bool clearAll(String& error) {
    error = "";
    history_storage::Guard filesystem;
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }
    std::vector<String> paths;
    if (!listHistoryPaths(paths, error, true)) {
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
