#pragma once

#include <algorithm>
#include <cstddef>

namespace history_retention {

// A configured limit may be lowered by a firmware release, but an already
// persisted file is never made expendable by that change.  The preservation
// floor is deliberately allowed to exceed the current writable upper bound:
// the bridge can still serve/export that file and will reject new appends
// instead of deleting old entries.
constexpr size_t effectiveBudget(size_t configuredBytes,
                                 size_t writableUpperBytes,
                                 size_t preservedFileBytes) {
    return std::max(std::min(configuredBytes, writableUpperBytes),
                    preservedFileBytes);
}

constexpr bool appendFits(size_t existingBytes,
                          size_t incomingBytes,
                          size_t budgetBytes) {
    return existingBytes <= budgetBytes &&
        incomingBytes <= budgetBytes - existingBytes;
}

constexpr bool canLowerWithoutDataLoss(size_t requestedBytes,
                                       size_t largestPersistedFileBytes) {
    return requestedBytes >= largestPersistedFileBytes;
}

} // namespace history_retention
