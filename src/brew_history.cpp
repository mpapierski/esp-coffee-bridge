#include "brew_history.h"

#include <LittleFS.h>

#include <vector>

#include "history_paging.h"
#include "history_retention.h"
#include "history_storage.h"

namespace brew_history {

String sanitizeMetadataText(JsonVariantConst value, size_t maxLength);

namespace {

constexpr char HISTORY_PREFIX[] = "/brewhist-";
constexpr char HISTORY_PREFIX_BARE[] = "brewhist-";
size_t gBudgetUpperBytes = DEFAULT_HISTORY_BYTES;
size_t gBudgetBytes = DEFAULT_HISTORY_BYTES;

size_t effectiveBudgetUpperBytes(size_t upperBytes) {
    const size_t filesystemBytes = upperBytes > 0 ? upperBytes : DEFAULT_HISTORY_BYTES;
    return history_storage::writableHistoryLimit(filesystemBytes);
}

size_t effectiveBudgetMinBytes(size_t upperBytes) {
    return std::min(MIN_HISTORY_BYTES, upperBytes);
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
        error = "failed to open brew history root";
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
        error = "failed to seek brew history";
        return false;
    }

    history_paging::PhysicalLineCounter counter;
    char buffer[256];
    while (file.available()) {
        const size_t readCount = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
        if (readCount == 0) {
            error = "failed to read brew history";
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
        error = "failed to seek brew history";
        return false;
    }

    history_paging::PhysicalLineCounter counter;
    char buffer[256];
    while (file.available()) {
        const size_t readCount = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
        if (readCount == 0) {
            error = "failed to index brew history";
            return false;
        }
        counter.consume(buffer, readCount);
        offsetsOut.consume(buffer, readCount);
    }
    if (counter.lineCount() != window.totalLines) {
        error = "brew history changed while it was being indexed";
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
            error = "brew history changed while its page was being read";
            return false;
        }
    }
    if (!file.seek(offset)) {
        error = "failed to seek brew history entry";
        return false;
    }

    while (file.available()) {
        const int value = file.read();
        if (value < 0) {
            error = "failed to read brew history entry";
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

bool serializeEntryWithLimit(JsonObjectConst entry,
                             size_t maximumEntryBytes,
                             String& lineOut,
                             String& error) {
    lineOut = "";
    lineOut.reserve(measureJson(entry) + 8);
    if (serializeJson(entry, lineOut) == 0 || lineOut.isEmpty()) {
        error = "failed to serialize brew history entry";
        return false;
    }
    if ((lineOut.length() + 1) > maximumEntryBytes) {
        error = "brew history entry exceeds the configured size limit";
        return false;
    }
    return true;
}

bool serializeEntry(JsonObjectConst entry, String& lineOut, String& error) {
    return serializeEntryWithLimit(entry, budgetBytes(), lineOut, error);
}

bool readHistoryFileSize(const String& serial, size_t& fileBytesOut, String& error) {
    error = "";
    fileBytesOut = 0;
    const String path = historyPath(serial);
    if (!LittleFS.exists(path)) {
        return true;
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
        error = "failed to open brew history for sizing";
        return false;
    }
    fileBytesOut = file.size();
    file.close();
    return true;
}

} // namespace

size_t clampBudgetBytes(size_t requestedBytes, size_t upperBytes) {
    const size_t effectiveUpper = effectiveBudgetUpperBytes(upperBytes);
    const size_t effectiveMin = effectiveBudgetMinBytes(effectiveUpper);
    const size_t requested = requestedBytes > 0 ? requestedBytes : DEFAULT_HISTORY_BYTES;
    return std::max(effectiveMin, std::min(requested, effectiveUpper));
}

void configureBudget(size_t requestedBytes,
                     size_t upperBytes,
                     size_t preservedFileBytes) {
    gBudgetUpperBytes = effectiveBudgetUpperBytes(upperBytes);
    const size_t effectiveMin = effectiveBudgetMinBytes(gBudgetUpperBytes);
    const size_t requested = requestedBytes > 0 ? requestedBytes : DEFAULT_HISTORY_BYTES;
    const size_t configured = std::max(effectiveMin,
                                       std::min(requested, gBudgetUpperBytes));
    gBudgetBytes = history_retention::effectiveBudget(
        configured, gBudgetUpperBytes, preservedFileBytes);
}

size_t budgetBytes() {
    return gBudgetBytes;
}

size_t budgetMinBytes() {
    return effectiveBudgetMinBytes(gBudgetUpperBytes);
}

size_t budgetUpperBytes() {
    return gBudgetUpperBytes;
}

String filePath(const String& serial) {
    return historyPath(serial);
}

bool append(const String& serial, JsonObjectConst entry, String& error) {
    String line;
    if (!serializeEntry(entry, line, error)) {
        return false;
    }
    return appendSerializedLines(serial, std::vector<String>{line}, error);
}

bool canAppendWithoutCompaction(const String& serial,
                                JsonObjectConst entry,
                                size_t reserveBytes,
                                CapacityCheck& checkOut,
                                String& error) {
    error = "";
    checkOut = CapacityCheck{};
    checkOut.reserveBytes = reserveBytes;
    if (serial.isEmpty()) {
        error = "serial is required for brew history";
        return false;
    }

    history_storage::Guard filesystem("brew_history_capacity");
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }
    if (!history_storage::recoverFile(historyPath(serial), error)) {
        return false;
    }

    String line;
    if (!serializeEntry(entry, line, error)) {
        return false;
    }
    checkOut.entryBytes = line.length() + 1;
    checkOut.maxBytes = budgetBytes();
    if (!readHistoryFileSize(serial, checkOut.fileBytes, error)) {
        return false;
    }

    checkOut.projectedBytes = checkOut.fileBytes + checkOut.entryBytes + reserveBytes;
    return checkOut.projectedBytes <= checkOut.maxBytes;
}

bool appendSerializedLines(const String& serial, const std::vector<String>& lines, String& error) {
    error = "";
    if (serial.isEmpty()) {
        error = "serial is required for brew history";
        return false;
    }
    if (lines.empty()) {
        return true;
    }

    history_storage::Guard filesystem("brew_history_append");
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
            error = "failed to inspect brew history tail";
            return false;
        }
        existingBytes = existing.size();
        if (existingBytes > 0) {
            if (!existing.seek(existingBytes - 1)) {
                existing.close();
                error = "failed to seek brew history tail";
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
        error = "brew history is full; existing entries were preserved";
        return false;
    }
    const size_t filesystemBytes = LittleFS.totalBytes();
    const size_t usedBytes = LittleFS.usedBytes();
    const size_t freeBytes = filesystemBytes > usedBytes ? filesystemBytes - usedBytes : 0;
    if (freeBytes < history_storage::writeReserveBytes(filesystemBytes) + incomingBytes) {
        error = "brew history append would consume transactional filesystem headroom";
        return false;
    }
    File file = LittleFS.open(path, "a");
    if (!file) {
        error = "failed to open brew history for append";
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
        error = "failed to append brew history entry";
        return false;
    }

    return true;
}

bool collectStorageStats(StorageStats& statsOut, String& error) {
    error = "";
    statsOut = StorageStats{};
    statsOut.budgetBytes = budgetBytes();
    statsOut.budgetMinBytes = budgetMinBytes();
    statsOut.budgetUpperBytes = budgetUpperBytes();

    history_storage::Guard filesystem("brew_history_stats");
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }

    std::vector<String> paths;
    if (!listHistoryPaths(paths, error)) {
        return false;
    }

    statsOut.fileCount = paths.size();
    for (const String& path : paths) {
        File file = LittleFS.open(path, "r");
        if (!file) {
            error = String("failed to open brew history file ") + path;
            return false;
        }
        statsOut.totalBytes += file.size();
        file.close();
    }
    return true;
}

String sanitizeMetadataText(const String& value, size_t maxLength) {
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

void copyIntField(JsonObject target, JsonObjectConst source, const char* key) {
    const JsonVariantConst value = source[key];
    if (!value.isNull()) {
        target[key] = value.as<int32_t>();
    }
}

void copyUInt32Field(JsonObject target, JsonObjectConst source, const char* key) {
    const JsonVariantConst value = source[key];
    if (!value.isNull()) {
        target[key] = value.as<uint32_t>();
    }
}

void copyInt64Field(JsonObject target, JsonObjectConst source, const char* key) {
    const JsonVariantConst value = source[key];
    if (!value.isNull()) {
        target[key] = static_cast<int64_t>(value.as<int64_t>());
    }
}

void copyBoolField(JsonObject target, JsonObjectConst source, const char* key) {
    const JsonVariantConst value = source[key];
    if (!value.isNull()) {
        target[key] = value.as<bool>();
    }
}

void copySanitizedStringField(JsonObject target, JsonObjectConst source, const char* key, size_t maxLength) {
    const String value = sanitizeMetadataText(source[key], maxLength);
    if (!value.isEmpty()) {
        target[key] = value;
    }
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
    statsOut.maxBytes = budgetBytes();
    pageOut = Page{};

    if (serial.isEmpty()) {
        error = "serial is required for brew history";
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
            error = "brew history changed while it was being indexed";
            return false;
        }
        if (!collectPageOffsets(file, window, offsets, error)) {
            file.close();
            if (error.isEmpty()) {
                error = "brew history changed while it was being indexed";
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
                error = "brew history changed while its page was being read";
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
                error = "brew history page visitor stopped";
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
        error = "serial is required for brew history";
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
        error = "failed to remove brew history";
        return false;
    }
    const String temporaryPath = path + ".tmp";
    const String backupPath = path + ".bak";
    if ((LittleFS.exists(temporaryPath) && !LittleFS.remove(temporaryPath)) ||
        (LittleFS.exists(backupPath) && !LittleFS.remove(backupPath))) {
        error = "failed to remove brew history rewrite artifact";
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
            error = String("failed to remove brew history file ") + path;
            return false;
        }
    }
    return true;
}

bool findNewestByStringField(const String& serial,
                             const char* field,
                             const String& value,
                             JsonObject entryOut,
                             String& error,
                             ProgressCallback progress,
                             void* progressContext) {
    error = "";
    if (serial.isEmpty() || field == nullptr || field[0] == '\0' || value.isEmpty()) {
        error = "history lookup requires serial, field, and value";
        return false;
    }
    history_storage::Guard filesystem("brew_history_lookup");
    if (!filesystem) {
        error = "failed to lock history storage";
        return false;
    }
    const String path = historyPath(serial);
    if (!history_storage::recoverFile(path, error)) return false;
    File file = LittleFS.open(path, "r");
    if (!file) return false;

    bool found = false;
    String line;
    line.reserve(512);
    DynamicJsonDocument document(4096);
    size_t bytesSinceProgress = 0;
    while (file.available()) {
        line = file.readStringUntil('\n');
        bytesSinceProgress += line.length() + 1;
        if (progress != nullptr && bytesSinceProgress >= 32 * 1024) {
            progress(progressContext);
            bytesSinceProgress = 0;
        }
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        if (line.isEmpty() || line.length() > history_storage::MAX_JSON_LINE_BYTES) continue;
        document.clear();
        if (deserializeJson(document, line)) continue;
        JsonObjectConst candidate = document.as<JsonObjectConst>();
        if ((candidate[field] | String("")) == value) {
            if (!entryOut.set(candidate)) {
                file.close();
                error = "matching history entry exceeds the response capacity";
                return false;
            }
            found = true;
        }
    }
    file.close();
    if (progress != nullptr) progress(progressContext);
    return found;
}

String sanitizeMetadataText(JsonVariantConst value, size_t maxLength) {
    if (value.isNull()) {
        return "";
    }

    return sanitizeMetadataText(value.as<String>(), maxLength);
}

void copyRecipeField(JsonObject target, JsonObjectConst recipe, const char* key) {
    const JsonVariantConst value = recipe[key];
    if (!value.isNull()) {
        target[key] = value;
    }
}

String fingerprintHex(const String& value) {
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < value.length(); ++index) {
        hash ^= static_cast<uint8_t>(value[index]);
        hash *= 16777619u;
    }

    char buffer[9];
    snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(hash));
    return String(buffer);
}

String recipeFingerprint(JsonObjectConst recipe) {
    String canonicalRecipe;
    canonicalRecipe.reserve(measureJson(recipe) + 8);
    serializeJson(recipe, canonicalRecipe);
    return fingerprintHex(canonicalRecipe);
}

bool applyTimestampPatch(JsonObject target, JsonObjectConst patch, String& error) {
    error = "";

    int64_t timeUnix = 0;
    if (!patch["timeUnixMs"].isNull()) {
        const int64_t timeUnixMs = patch["timeUnixMs"].as<int64_t>();
        if (timeUnixMs <= 0) {
            error = "timeUnixMs must be positive";
            return false;
        }
        timeUnix = timeUnixMs / 1000;
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

    const String timeSource = sanitizeMetadataText(patch["timeSource"], 16);
    const String timeIsoUtc = sanitizeMetadataText(patch["timeIsoUtc"], 32);

    target["timeUnix"] = timeUnix;
    target["timeIsoUtc"] = !timeIsoUtc.isEmpty() ? timeIsoUtc : formatIso8601Utc(static_cast<time_t>(timeUnix));
    target["timeSource"] = timeSource.isEmpty() ? "patched" : timeSource;
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
            error = "failed to read brew history during rewrite";
            return false;
        }
        const char ch = static_cast<char>(value);
        if (destination != nullptr) {
            outputBuffer[outputLength++] = static_cast<uint8_t>(ch);
            if (outputLength == sizeof(outputBuffer)) {
                if (destination->write(outputBuffer, outputLength) != outputLength) {
                    error = "failed to write temporary brew history";
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
        error = "failed to write temporary brew history";
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

bool patchTimestamp(const String& serial,
                    size_t entryId,
                    JsonObjectConst patch,
                    JsonObject updatedOut,
                    String& error) {
    error = "";
    if (serial.isEmpty()) {
        error = "serial is required for brew history";
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
        error = "brew history entry not found";
        return false;
    }

    const String temporaryPath = path + ".tmp";
    LittleFS.remove(temporaryPath);
    File temporary = LittleFS.open(temporaryPath, "w");
    if (!temporary) {
        source.close();
        error = "failed to open temporary brew history";
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
                error = String("failed to parse brew history entry ") + currentEntryId;
                return false;
            }
            DynamicJsonDocument lineDoc(4096);
            const DeserializationError parseError = deserializeJson(lineDoc, line);
            if (parseError) {
                abandonRewrite(source, temporary, temporaryPath);
                error = String("failed to parse brew history entry ") + currentEntryId;
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
                    error = "failed to write patched brew history entry";
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
        error = "brew history entry not found";
        return false;
    }
    File rewritten = LittleFS.open(temporaryPath, "r");
    const size_t totalBytes = rewritten ? rewritten.size() : budgetBytes() + 1;
    rewritten.close();
    if (totalBytes > budgetBytes()) {
        LittleFS.remove(temporaryPath);
        error = "patched brew history exceeds the configured size limit";
        return false;
    }
    if (!history_storage::commitTemporaryFile(path,
                                              temporaryPath,
                                              budgetBytes(),
                                              writtenLines,
                                              error)) {
        return false;
    }
    return true;
}

bool deleteEntry(const String& serial,
                 size_t entryId,
                 JsonObject deletedOut,
                 String& error) {
    error = "";
    if (serial.isEmpty()) {
        error = "serial is required for brew history";
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
        error = "brew history entry not found";
        return false;
    }

    const String temporaryPath = path + ".tmp";
    LittleFS.remove(temporaryPath);
    File temporary = LittleFS.open(temporaryPath, "w");
    if (!temporary) {
        source.close();
        error = "failed to open temporary brew history";
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
                error = String("failed to parse brew history entry ") + currentEntryId;
                return false;
            }
            DynamicJsonDocument lineDoc(4096);
            const DeserializationError parseError = deserializeJson(lineDoc, line);
            if (parseError) {
                abandonRewrite(source, temporary, temporaryPath);
                error = String("failed to parse brew history entry ") + currentEntryId;
                return false;
            }
            JsonObject entry = lineDoc.as<JsonObject>();
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
        error = "brew history entry not found";
        return false;
    }
    return history_storage::commitTemporaryFile(path,
                                                temporaryPath,
                                                budgetBytes(),
                                                writtenLines,
                                                error);
}

void appendCompactRecipe(JsonObject target, JsonObjectConst recipe) {
    static const char* FIELDS[] = {
        "selector",
        "name",
        "title",
        "iconKey",
        "iconName",
        "typeSelector",
        "typeName",
        "strength",
        "strengthBeans",
        "aroma",
        "aromaLabel",
        "temperature",
        "temperatureLabel",
        "coffeeTemperature",
        "coffeeTemperatureLabel",
        "waterTemperature",
        "waterTemperatureLabel",
        "milkTemperature",
        "milkTemperatureLabel",
        "milkFoamTemperature",
        "milkFoamTemperatureLabel",
        "overallTemperature",
        "overallTemperatureLabel",
        "twoCups",
        "preparation",
        "coffeeAmountMl",
        "waterAmountMl",
        "milkAmountMl",
        "milkFoamAmountMl",
    };

    for (const char* field : FIELDS) {
        copyRecipeField(target, recipe, field);
    }
}

bool buildImportedEntry(JsonObjectConst request, JsonObject target, String& error) {
    error = "";
    const JsonObjectConst recipeInput = request["recipe"].as<JsonObjectConst>();
    if (recipeInput.isNull()) {
        error = "history entry recipe is required";
        return false;
    }

    const uint32_t schema = request["schema"] | 1U;
    target["schema"] = std::min<uint32_t>(schema, 2U);
    if (!request["loggedAtMs"].isNull()) {
        copyUInt32Field(target, request, "loggedAtMs");
    } else {
        target["loggedAtMs"] = 0;
    }

    const String result = sanitizeMetadataText(request["result"], 24);
    target["result"] = result.isEmpty() ? "accepted" : result;

    const String source = sanitizeMetadataText(request["source"], 24);
    target["source"] = source.isEmpty() ? "api-import" : source;

    bool timeSynced = request["timeSynced"] | false;
    const bool hasTimeUnix = !request["timeUnix"].isNull();
    const String timeIsoUtc = sanitizeMetadataText(request["timeIsoUtc"], 32);
    if ((hasTimeUnix || !timeIsoUtc.isEmpty()) && request["timeSynced"].isNull()) {
        timeSynced = true;
    }
    target["timeSynced"] = timeSynced;
    if (hasTimeUnix) {
        copyInt64Field(target, request, "timeUnix");
    }
    if (!timeIsoUtc.isEmpty()) {
        target["timeIsoUtc"] = timeIsoUtc;
    }
    copySanitizedStringField(target, request, "timeSource", 16);

    copySanitizedStringField(target, request, "actor", 48);
    copySanitizedStringField(target, request, "label", 80);
    copySanitizedStringField(target, request, "note", 160);
    copySanitizedStringField(target, request, "correlationId", 64);
    copySanitizedStringField(target, request, "brewId", 31);
    copySanitizedStringField(target, request, "requestHash", 64);
    copyBoolField(target, request, "commandAccepted");
    copyBoolField(target, request, "commandMayHaveBeenSent");
    copyUInt32Field(target, request, "createdAtMs");
    copyUInt32Field(target, request, "acceptedAtMs");
    copyUInt32Field(target, request, "preparingAtMs");
    copyUInt32Field(target, request, "completedAtMs");
    copySanitizedStringField(target, request, "errorCode", 48);
    const JsonObjectConst evidenceInput = request["completionEvidence"].as<JsonObjectConst>();
    if (!evidenceInput.isNull()) {
        JsonObject evidence = target.createNestedObject("completionEvidence");
        copyBoolField(evidence, evidenceInput, "preparationObserved");
        copyIntField(evidence, evidenceInput, "readyObservations");
        copyUInt32Field(evidence, evidenceInput, "readySeparationMs");
    }

    JsonObject compactRecipe = target.createNestedObject("recipe");
    appendCompactRecipe(compactRecipe, recipeInput);
    if (compactRecipe.size() == 0) {
        error = "history entry recipe must include at least one supported field";
        return false;
    }

    if (!compactRecipe["selector"].isNull()) {
        target["selector"] = compactRecipe["selector"].as<int32_t>();
    } else {
        copyIntField(target, request, "selector");
    }

    JsonVariantConst recipeNameValue = compactRecipe["name"];
    if (recipeNameValue.isNull()) {
        recipeNameValue = request["recipeName"];
    }
    const String recipeName = sanitizeMetadataText(recipeNameValue, 48);
    if (!recipeName.isEmpty()) {
        target["recipeName"] = recipeName;
    }
    JsonVariantConst recipeTitleValue = compactRecipe["title"];
    if (recipeTitleValue.isNull()) {
        recipeTitleValue = request["recipeTitle"];
    }
    const String recipeTitle = sanitizeMetadataText(recipeTitleValue, 64);
    if (!recipeTitle.isEmpty()) {
        target["recipeTitle"] = recipeTitle;
    }

    copySanitizedStringField(target, request, "statusSummary", 64);
    copyIntField(target, request, "process");
    copySanitizedStringField(target, request, "processLabel", 64);
    copyIntField(target, request, "message");
    copySanitizedStringField(target, request, "messageLabel", 64);
    copyIntField(target, request, "progress");
    copyBoolField(target, request, "hostConfirmSuggested");
    copySanitizedStringField(target, request, "statusError", 96);

    target["recipeFingerprint"] = recipeFingerprint(compactRecipe);
    return true;
}

bool buildImportedLines(JsonVariantConst payload,
                        std::vector<String>& linesOut,
                        size_t& importedCount,
                        String& error,
                        size_t maximumEntryBytes) {
    error = "";
    importedCount = 0;
    linesOut.clear();

    auto appendImportedLine = [&](JsonObjectConst entryRequest, size_t index) -> bool {
        if (entryRequest.isNull()) {
            error = String("entry ") + String(index + 1) + " must be an object";
            return false;
        }

        DynamicJsonDocument entryDoc(4096);
        JsonObject entry = entryDoc.to<JsonObject>();
        String entryError;
        if (!buildImportedEntry(entryRequest, entry, entryError)) {
            error = String("entry ") + String(index + 1) + ": " + entryError;
            return false;
        }

        String line;
        const size_t entryLimit = maximumEntryBytes > 0
            ? maximumEntryBytes
            : budgetBytes();
        if (!serializeEntryWithLimit(entry, entryLimit, line, entryError)) {
            error = String("entry ") + String(index + 1) + ": " + entryError;
            return false;
        }

        linesOut.push_back(line);
        importedCount++;
        return true;
    };

    const JsonArrayConst arrayPayload = payload.as<JsonArrayConst>();
    if (!arrayPayload.isNull()) {
        size_t index = 0;
        for (JsonVariantConst item : arrayPayload) {
            if (!appendImportedLine(item.as<JsonObjectConst>(), index)) {
                return false;
            }
            index++;
        }
        if (importedCount == 0) {
            error = "history import requires at least one entry";
            return false;
        }
        return true;
    }

    const JsonObjectConst objectPayload = payload.as<JsonObjectConst>();
    if (objectPayload.isNull()) {
        error = "history import requires an entry object or an entries array";
        return false;
    }

    const JsonArrayConst entries = objectPayload["entries"].as<JsonArrayConst>();
    if (!entries.isNull()) {
        size_t index = 0;
        for (JsonVariantConst item : entries) {
            if (!appendImportedLine(item.as<JsonObjectConst>(), index)) {
                return false;
            }
            index++;
        }
        if (importedCount == 0) {
            error = "history import requires at least one entry";
            return false;
        }
        return true;
    }

    JsonObjectConst entry = objectPayload["entry"].as<JsonObjectConst>();
    if (!entry.isNull()) {
        return appendImportedLine(entry, 0);
    }

    if (!objectPayload["recipe"].isNull()) {
        return appendImportedLine(objectPayload, 0);
    }

    error = "history import requires an entry object, an entries array, or a single entry body";
    return false;
}

void buildAcceptedEntry(JsonVariantConst request,
                        JsonObjectConst recipe,
                        const nivona::ProcessStatus& processStatus,
                        const String& processError,
                        const bridge_time::StatusSnapshot& timeStatus,
                        uint32_t loggedAtMs,
                        JsonObject target) {
    const String source = sanitizeMetadataText(request["source"], 24);
    const String actor = sanitizeMetadataText(request["actor"], 48);
    const String label = sanitizeMetadataText(request["label"], 80);
    const String note = sanitizeMetadataText(request["note"], 160);
    const String correlationId = sanitizeMetadataText(request["correlationId"], 64);

    target["schema"] = 1;
    target["loggedAtMs"] = loggedAtMs;
    target["result"] = "accepted";
    target["source"] = source.isEmpty() ? "api" : source;
    target["timeSynced"] = timeStatus.synced;
    if (timeStatus.available) {
        target["timeUnix"] = static_cast<int64_t>(timeStatus.unixTime);
        target["timeIsoUtc"] = timeStatus.iso8601Utc;
    }
    if (timeStatus.synced) {
        target["timeSource"] = "ntp";
    } else if (timeStatus.restored) {
        target["timeSource"] = "restored";
    } else if (timeStatus.clientSeeded) {
        target["timeSource"] = "client";
    }

    if (!actor.isEmpty()) {
        target["actor"] = actor;
    }
    if (!label.isEmpty()) {
        target["label"] = label;
    }
    if (!note.isEmpty()) {
        target["note"] = note;
    }
    if (!correlationId.isEmpty()) {
        target["correlationId"] = correlationId;
    }

    target["selector"] = recipe["selector"] | 0;
    target["recipeName"] = recipe["name"] | "";
    target["recipeTitle"] = recipe["title"] | "";

    if (processStatus.ok) {
        const String statusSummary = sanitizeMetadataText(processStatus.summary, 64);
        const String processLabel = sanitizeMetadataText(processStatus.processLabel, 64);
        const String messageLabel = sanitizeMetadataText(processStatus.messageLabel, 64);
        if (!statusSummary.isEmpty()) {
            target["statusSummary"] = statusSummary;
        }
        target["process"] = processStatus.process;
        if (!processLabel.isEmpty()) {
            target["processLabel"] = processLabel;
        }
        target["message"] = processStatus.message;
        if (!messageLabel.isEmpty()) {
            target["messageLabel"] = messageLabel;
        }
        target["progress"] = processStatus.progress;
        target["hostConfirmSuggested"] = processStatus.hostConfirmSuggested;
    }
    const String historyProcessError = sanitizeMetadataText(processError, 96);
    if (!historyProcessError.isEmpty()) {
        target["statusError"] = historyProcessError;
    }

    JsonObject compactRecipe = target.createNestedObject("recipe");
    appendCompactRecipe(compactRecipe, recipe);

    String canonicalRecipe;
    serializeJson(compactRecipe, canonicalRecipe);
    target["recipeFingerprint"] = fingerprintHex(canonicalRecipe);
}

} // namespace brew_history
