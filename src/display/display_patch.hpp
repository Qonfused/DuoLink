/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include "control_flow.hpp"
#include "src/patch.hpp"

namespace duolink {
namespace display {

enum class Local : uint8_t {
    Width,
    Height,
    FormatCheck,
    Mode,
    SavedEdi,
    Count
};
constexpr size_t localCount = static_cast<size_t>(Local::Count);

struct Operand {
    Local local;
    uint8_t byteOffset;
    uint8_t displacementSize;
};

struct ModeCheck {
    const uint8_t *original;
    const uint8_t *replacement;
    size_t size;
    const Operand *originalOperands;
    size_t originalOperandCount;
    const Operand *replacementOperands;
    size_t replacementOperandCount;
    uint8_t formatAccessSize;
};

struct Match {
    const ModeCheck *check{};
    size_t offset{};
    int32_t displacements[localCount]{};
};

} // namespace display
} // namespace duolink

#include "display_patch_data.hpp"

namespace duolink {
namespace display {

// Stack displacements may change between builds, but each repeated load must
// still name one local. Access widths come from the matched instructions:
// the mode pointer is eight bytes, while SETcc tests a one-byte format result.
inline bool matchWindow(const uint8_t *data, const uint8_t *pattern,
                        const ModeCheck &check, const Operand *operands,
                        size_t count, int32_t (&displacements)[localCount]) {
    if (!equal(data, pattern, 3))
        return false;
    bool seen[localCount]{};
    size_t operand = 0;
    for (size_t i = 3; i < check.size;) {
        if (operand < count && i == operands[operand].byteOffset) {
            const auto &slot = operands[operand++];
            const auto field = static_cast<size_t>(slot.local);
            const int32_t value = slot.displacementSize == 1
                                      ? static_cast<int8_t>(data[i])
                                      : static_cast<int32_t>(read32(data + i));
            if (seen[field] && displacements[field] != value)
                return false;
            displacements[field] = value;
            seen[field] = true;
            i += slot.displacementSize;
        } else {
            if (data[i] != pattern[i])
                return false;
            ++i;
        }
    }
    const auto accessSize = [&](size_t field) -> int64_t {
        if (field == static_cast<size_t>(Local::Mode))
            return 8;
        if (field == static_cast<size_t>(Local::FormatCheck))
            return check.formatAccessSize;
        return 4;
    };
    for (size_t i = 0; i < localCount; ++i) {
        if (!seen[i])
            continue;
        // These operands address separate locals below RBP. Reject
        // overlap with another local or the saved frame pointer, without
        // assuming a particular frame size or alignment chosen by Apple's
        // compiler.
        const int64_t end =
            static_cast<int64_t>(displacements[i]) + accessSize(i);
        if (end > 0)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (seen[j] &&
                displacements[i] <
                    static_cast<int64_t>(displacements[j]) + accessSize(j) &&
                displacements[j] < end)
                return false;
    }
    return true;
}

inline bool findModeCheck(const uint8_t *data, size_t size, Match &match) {
    match = {};
    if (!data)
        return false;
    for (const auto &check : modeChecks) {
        if (size < check.size)
            continue;
        for (size_t offset = 0; offset <= size - check.size; ++offset) {
            const auto *window = data + offset;
            if (!equal(window, check.original, 3) &&
                !equal(window, check.replacement, 3))
                continue;
            Match candidate{};
            if (!matchWindow(window, check.original, check,
                             check.originalOperands, check.originalOperandCount,
                             candidate.displacements) &&
                !matchWindow(
                    window, check.replacement, check, check.replacementOperands,
                    check.replacementOperandCount, candidate.displacements))
                continue;
            if (match.check) {
                match = {};
                return false;
            }
            candidate.check = &check;
            candidate.offset = offset;
            match = candidate;
        }
    }
    return match.check;
}

inline PatchResult patchPage(uint8_t *page, size_t size, const Match &match) {
    if (!page || size != executablePageSize || !match.check ||
        match.offset > size || match.check->size > size - match.offset)
        return PatchResult::NoMatch;
    const auto &check = *match.check;
    auto *target = page + match.offset;
    int32_t current[localCount]{};
    if (matchWindow(target, check.replacement, check, check.replacementOperands,
                    check.replacementOperandCount, current))
        return PatchResult::AlreadyApplied;
    if (!matchWindow(target, check.original, check, check.originalOperands,
                     check.originalOperandCount, current))
        return PatchResult::Mismatch;
    for (size_t i = 0; i < localCount; ++i)
        if (current[i] != match.displacements[i])
            return PatchResult::Mismatch;
    if (!preservesBranchEntries(page, size, match.offset, check.size))
        return PatchResult::Mismatch;

    uint8_t replacement[maxWindowSize];
    for (size_t i = 0; i < check.size; ++i)
        replacement[i] = check.replacement[i];
    for (size_t i = 0; i < check.replacementOperandCount; ++i) {
        const auto &slot = check.replacementOperands[i];
        const auto value = static_cast<uint32_t>(
            match.displacements[static_cast<size_t>(slot.local)]);
        for (size_t byte = 0; byte < slot.displacementSize; ++byte)
            replacement[slot.byteOffset + byte] =
                static_cast<uint8_t>(value >> (8 * byte));
    }
    for (size_t i = 0; i < check.size; ++i)
        target[i] = replacement[i];
    return PatchResult::Applied;
}

inline bool cachePath(const char *path) {
    if (!path)
        return false;
    const char suffix[] = "/System/Library/dyld/dyld_shared_cache_x86_64h";
    size_t length = 0;
    while (path[length])
        ++length;
    size_t end = length;
    while (end && path[end - 1] >= '0' && path[end - 1] <= '9')
        --end;
    if (end != length) {
        if (!end || path[end - 1] != '.')
            return false;
        --end;
    }
    return end >= sizeof(suffix) - 1 &&
           equal(reinterpret_cast<const uint8_t *>(path + end -
                                                   (sizeof(suffix) - 1)),
                 reinterpret_cast<const uint8_t *>(suffix), sizeof(suffix) - 1);
}

} // namespace display
} // namespace duolink
