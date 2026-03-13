#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

#include "bridge_time.h"

namespace stats_history {

inline constexpr size_t DEFAULT_HISTORY_BYTES = 32 * 1024;

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

String filePath(const String& serial);

bool validateEntry(JsonObjectConst entry, String& error);
bool append(const String& serial, JsonObjectConst entry, String& error);
bool appendSerializedLines(const String& serial, const std::vector<String>& lines, String& error);
bool recordSnapshotIfChanged(const String& serial,
                             JsonObjectConst liveValues,
                             const bridge_time::StatusSnapshot& timeStatus,
                             uint32_t loggedAtMs,
                             const String& source,
                             AppendResult& resultOut,
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
