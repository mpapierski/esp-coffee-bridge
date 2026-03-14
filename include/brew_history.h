#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

#include "bridge_time.h"
#include "nivona.h"

namespace brew_history {

inline constexpr size_t DEFAULT_HISTORY_BYTES = 64 * 1024;
inline constexpr size_t MIN_HISTORY_BYTES = 8 * 1024;

struct Stats {
    size_t entryCount{0};
    size_t fileBytes{0};
    size_t maxBytes{0};
    size_t skippedEntries{0};
};

struct Page {
    size_t offset{0};
    size_t limit{0};
    size_t returned{0};
    bool hasOlder{false};
    bool hasNewer{false};
    size_t nextOffset{0};
    size_t prevOffset{0};
};

struct CapacityCheck {
    size_t fileBytes{0};
    size_t entryBytes{0};
    size_t reserveBytes{0};
    size_t projectedBytes{0};
    size_t maxBytes{0};
};

struct StorageStats {
    size_t fileCount{0};
    size_t totalBytes{0};
    size_t budgetBytes{0};
    size_t budgetMinBytes{0};
    size_t budgetUpperBytes{0};
    size_t defaultBudgetBytes{DEFAULT_HISTORY_BYTES};
};

size_t clampBudgetBytes(size_t requestedBytes, size_t upperBytes);
void configureBudget(size_t requestedBytes, size_t upperBytes);
size_t budgetBytes();
size_t budgetMinBytes();
size_t budgetUpperBytes();
String filePath(const String& serial);

bool append(const String& serial, JsonObjectConst entry, String& error);
bool appendSerializedLines(const String& serial, const std::vector<String>& lines, String& error);
bool canAppendWithoutCompaction(const String& serial,
                                JsonObjectConst entry,
                                size_t reserveBytes,
                                CapacityCheck& checkOut,
                                String& error);
bool collectStorageStats(StorageStats& statsOut, String& error);
bool enforceBudget(String& error);
bool buildImportedLines(JsonVariantConst payload,
                        std::vector<String>& linesOut,
                        size_t& importedCount,
                        String& error);
bool patchTimestamp(const String& serial,
                    size_t entryId,
                    JsonObjectConst patch,
                    JsonObject updatedOut,
                    String& error);
bool deleteEntry(const String& serial,
                 size_t entryId,
                 JsonObject deletedOut,
                 String& error);
bool loadPage(const String& serial,
              size_t offset,
              size_t limit,
              JsonArray entriesOut,
              Stats& statsOut,
              Page& pageOut,
              String& error);
bool clear(const String& serial, String& error);
bool clearAll(String& error);
void buildAcceptedEntry(JsonVariantConst request,
                        JsonObjectConst recipe,
                        const nivona::ProcessStatus& processStatus,
                        const String& processError,
                        const bridge_time::StatusSnapshot& timeStatus,
                        uint32_t loggedAtMs,
                        JsonObject target);

} // namespace brew_history
