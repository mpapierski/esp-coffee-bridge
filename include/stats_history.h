#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

#include "bridge_time.h"

namespace stats_history {

// Keep enough transactional headroom to rewrite/validate the file while
// retaining materially more counter snapshots than the legacy 32 KiB cap.
// The runtime clamps this default to the current filesystem's transaction-safe
// single-file limit.
inline constexpr size_t DEFAULT_HISTORY_BYTES = 192 * 1024;
inline constexpr size_t MIN_HISTORY_BYTES = 2 * 1024;

struct Stats {
    size_t entryCount{0};
    size_t fileBytes{0};
    size_t maxBytes{DEFAULT_HISTORY_BYTES};
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

struct AppendResult {
    bool appended{false};
    bool baseline{false};
    size_t changedMetricCount{0};
    bool hasTotalDelta{false};
    int32_t totalDelta{0};
};

using PageEntryVisitor = bool (*)(JsonObjectConst entry,
                                  size_t entryId,
                                  void* context,
                                  String& error);

String filePath(const String& serial);

size_t clampBudgetBytes(size_t requestedBytes, size_t filesystemBytes);
void configureBudget(size_t requestedBytes,
                     size_t filesystemBytes,
                     size_t preservedFileBytes = 0);
size_t budgetBytes();
size_t budgetUpperBytes();
bool validateEntry(JsonObjectConst entry, String& error);
bool append(const String& serial, JsonObjectConst entry, String& error);
bool appendSerializedLines(const String& serial, const std::vector<String>& lines, String& error);
bool buildImportedLines(JsonVariantConst payload,
                        std::vector<String>& linesOut,
                        size_t& importedCount,
                        String& error);
bool recordSnapshotIfChanged(const String& serial,
                             JsonObjectConst liveValues,
                             const bridge_time::StatusSnapshot& timeStatus,
                             uint32_t loggedAtMs,
                             const String& source,
                             AppendResult& resultOut,
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
bool visitPage(const String& serial,
               size_t offset,
               size_t limit,
               PageEntryVisitor visitor,
               void* visitorContext,
               Stats& statsOut,
               Page& pageOut,
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

} // namespace stats_history
