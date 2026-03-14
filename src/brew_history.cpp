#include "brew_history.h"

#include <LittleFS.h>

#include <vector>

namespace brew_history {

String sanitizeMetadataText(JsonVariantConst value, size_t maxLength);

namespace {

constexpr char HISTORY_PREFIX[] = "/brewhist-";
constexpr char HISTORY_PREFIX_BARE[] = "brewhist-";
size_t gBudgetUpperBytes = DEFAULT_HISTORY_BYTES;
size_t gBudgetBytes = DEFAULT_HISTORY_BYTES;

size_t effectiveBudgetUpperBytes(size_t upperBytes) {
    return upperBytes > 0 ? upperBytes : DEFAULT_HISTORY_BYTES;
}

size_t effectiveBudgetMinBytes(size_t upperBytes) {
    const size_t effectiveUpper = effectiveBudgetUpperBytes(upperBytes);
    return std::min(MIN_HISTORY_BYTES, effectiveUpper);
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
        error = "failed to open brew history root";
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

bool serializeEntry(JsonObjectConst entry, String& lineOut, String& error) {
    lineOut = "";
    lineOut.reserve(measureJson(entry) + 8);
    if (serializeJson(entry, lineOut) == 0 || lineOut.isEmpty()) {
        error = "failed to serialize brew history entry";
        return false;
    }
    if ((lineOut.length() + 1) > budgetBytes()) {
        error = "brew history entry exceeds the configured size limit";
        return false;
    }
    return true;
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

bool writeLines(const String& path, const std::vector<String>& lines, String& error) {
    File file = LittleFS.open(path, "w");
    if (!file) {
        error = "failed to open brew history for writing";
        return false;
    }

    for (const String& line : lines) {
        if (file.print(line) != line.length() || file.write('\n') != 1) {
            file.close();
            error = "failed to write brew history";
            return false;
        }
    }
    file.close();
    return true;
}

bool compact(const String& path, String& error) {
    File file = LittleFS.open(path, "r");
    if (!file) {
        error = "failed to open brew history for compaction";
        return false;
    }

    std::vector<String> retained;
    size_t retainedBytes = 0;
    String line;
    const size_t maxBytes = budgetBytes();
    while (readLine(file, line)) {
        const size_t lineBytes = line.length() + 1;
        if (lineBytes > maxBytes) {
            continue;
        }
        retained.push_back(line);
        retainedBytes += lineBytes;
        while (!retained.empty() && retainedBytes > maxBytes) {
            retainedBytes -= retained.front().length() + 1;
            retained.erase(retained.begin());
        }
    }
    file.close();

    return writeLines(path, retained, error);
}

} // namespace

size_t clampBudgetBytes(size_t requestedBytes, size_t upperBytes) {
    const size_t effectiveUpper = effectiveBudgetUpperBytes(upperBytes);
    const size_t effectiveMin = effectiveBudgetMinBytes(effectiveUpper);
    const size_t requested = requestedBytes > 0 ? requestedBytes : DEFAULT_HISTORY_BYTES;
    return std::max(effectiveMin, std::min(requested, effectiveUpper));
}

void configureBudget(size_t requestedBytes, size_t upperBytes) {
    gBudgetUpperBytes = effectiveBudgetUpperBytes(upperBytes);
    gBudgetBytes = clampBudgetBytes(requestedBytes, gBudgetUpperBytes);
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

    const String path = historyPath(serial);
    File file = LittleFS.open(path, "a");
    if (!file) {
        error = "failed to open brew history for append";
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
        error = "failed to append brew history entry";
        return false;
    }

    if (finalSize > budgetBytes()) {
        return compact(path, error);
    }
    return true;
}

bool collectStorageStats(StorageStats& statsOut, String& error) {
    error = "";
    statsOut = StorageStats{};
    statsOut.budgetBytes = budgetBytes();
    statsOut.budgetMinBytes = budgetMinBytes();
    statsOut.budgetUpperBytes = budgetUpperBytes();

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

bool enforceBudget(String& error) {
    error = "";
    std::vector<String> paths;
    if (!listHistoryPaths(paths, error)) {
        return false;
    }

    const size_t maxBytes = budgetBytes();
    for (const String& path : paths) {
        File file = LittleFS.open(path, "r");
        if (!file) {
            error = String("failed to open brew history file ") + path;
            return false;
        }
        const size_t fileBytes = file.size();
        file.close();
        if (fileBytes > maxBytes && !compact(path, error)) {
            return false;
        }
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

bool loadPage(const String& serial,
              size_t offset,
              size_t limit,
              JsonArray entriesOut,
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
        DeserializationError parseError = deserializeJson(lineDoc, allLines[index - 1]);
        if (parseError) {
            statsOut.skippedEntries++;
            continue;
        }
        JsonObject item = entriesOut.createNestedObject();
        item.set(lineDoc.as<JsonObjectConst>());
        item["entryId"] = static_cast<uint32_t>(index - 1);
    }
    return true;
}

bool clear(const String& serial, String& error) {
    error = "";
    if (serial.isEmpty()) {
        error = "serial is required for brew history";
        return false;
    }

    const String path = historyPath(serial);
    if (!LittleFS.exists(path)) {
        return true;
    }
    if (!LittleFS.remove(path)) {
        error = "failed to remove brew history";
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
            error = String("failed to remove brew history file ") + path;
            return false;
        }
    }
    return true;
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

    const String path = historyPath(serial);
    File file = LittleFS.open(path, "r");
    if (!file) {
        error = "brew history entry not found";
        return false;
    }

    std::vector<String> rewrittenLines;
    rewrittenLines.reserve(64);
    size_t totalBytes = 0;
    size_t currentEntryId = 0;
    bool found = false;
    String line;
    while (readLine(file, line)) {
        DynamicJsonDocument lineDoc(4096);
        DeserializationError parseError = deserializeJson(lineDoc, line);
        if (parseError) {
            file.close();
            error = String("failed to parse brew history entry ") + currentEntryId;
            return false;
        }

        JsonObject entry = lineDoc.as<JsonObject>();
        if (currentEntryId == entryId) {
            if (!applyTimestampPatch(entry, patch, error)) {
                file.close();
                return false;
            }
            updatedOut.set(entry);
            updatedOut["entryId"] = static_cast<uint32_t>(entryId);
            found = true;
        }

        String serialized;
        if (!serializeEntry(entry, serialized, error)) {
            file.close();
            return false;
        }
        rewrittenLines.push_back(serialized);
        totalBytes += serialized.length() + 1;
        currentEntryId++;
    }
    file.close();

    if (!found) {
        error = "brew history entry not found";
        return false;
    }
    if (totalBytes > budgetBytes()) {
        error = "patched brew history exceeds the configured size limit";
        return false;
    }
    return writeLines(path, rewrittenLines, error);
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

    const String path = historyPath(serial);
    File file = LittleFS.open(path, "r");
    if (!file) {
        error = "brew history entry not found";
        return false;
    }

    std::vector<String> rewrittenLines;
    rewrittenLines.reserve(64);
    size_t totalBytes = 0;
    size_t currentEntryId = 0;
    bool found = false;
    String line;
    while (readLine(file, line)) {
        DynamicJsonDocument lineDoc(4096);
        DeserializationError parseError = deserializeJson(lineDoc, line);
        if (parseError) {
            file.close();
            error = String("failed to parse brew history entry ") + currentEntryId;
            return false;
        }

        JsonObject entry = lineDoc.as<JsonObject>();
        if (currentEntryId == entryId) {
            deletedOut.set(entry);
            deletedOut["entryId"] = static_cast<uint32_t>(entryId);
            found = true;
            currentEntryId++;
            continue;
        }

        String serialized;
        if (!serializeEntry(entry, serialized, error)) {
            file.close();
            return false;
        }
        rewrittenLines.push_back(serialized);
        totalBytes += serialized.length() + 1;
        currentEntryId++;
    }
    file.close();

    if (!found) {
        error = "brew history entry not found";
        return false;
    }
    if (totalBytes > budgetBytes()) {
        error = "rewritten brew history exceeds the configured size limit";
        return false;
    }
    return writeLines(path, rewrittenLines, error);
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

    target["schema"] = 1;
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
                        String& error) {
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
        if (!serializeEntry(entry, line, entryError)) {
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
