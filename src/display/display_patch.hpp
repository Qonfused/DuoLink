/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include "control_flow.hpp"
#include "src/patch.hpp"

namespace duolink {
namespace display {

enum class Field : uint8_t {
    Width,
    Height,
    FormatCheck,
    Mode,
    SavedEdi,
    SavedFlags,
    SavedPointer,
    // Stack-slot overlap checks cover only the fields before DisplayBase.
    DisplayBase,
    CopyTarget,
    Thresholds,
    Count
};
constexpr size_t fieldCount = static_cast<size_t>(Field::Count);

struct Operand {
    Field field;
    uint8_t byteOffset;
    uint8_t displacementSize;
    bool relativeToWidth{false};
    int16_t bias{};
};

struct ModeCheck {
    const uint8_t *original;
    const uint8_t *replacement;
    size_t size;
    size_t writeOffset; // Bytes of frame setup before the size check.
    size_t writeSize;
    const Operand *originalOperands;
    size_t originalOperandCount;
    const Operand *replacementOperands;
    size_t replacementOperandCount;
    uint8_t formatAccessSize;
};

struct Match {
    const ModeCheck *check{};
    size_t offset{};
    int32_t displacements[fieldCount]{};
};

} // namespace display
} // namespace duolink

#include "display_patch_data.hpp"

namespace duolink {
namespace display {

// A stack slot can move between builds as long as every instruction that
// uses it resolves to the same offset from RBP.
inline bool matchWindow(const uint8_t *data, const uint8_t *pattern,
                        const ModeCheck &check, const Operand *operands,
                        size_t count, int32_t (&displacements)[fieldCount]) {
    // A valid mode jumps past CoreDisplay's invalid-mode handling, whose
    // length determines the positive displacement at the end of the pattern.
    const size_t jumpDisplacement = check.size - 1;
    if (data[jumpDisplacement] == 0 || data[jumpDisplacement] > INT8_MAX ||
        !equal(data, pattern, 3))
        return false;
    bool seen[fieldCount]{};
    size_t operand = 0;
    for (size_t i = 3; i < jumpDisplacement;) {
        if (operand < count && i == operands[operand].byteOffset) {
            const auto &slot = operands[operand++];
            const auto field = static_cast<size_t>(slot.field);
            int32_t value = slot.displacementSize == 1
                                ? static_cast<int8_t>(data[i])
                                : static_cast<int32_t>(read32(data + i));
            value -= slot.bias;
            if (slot.relativeToWidth) {
                const auto width = static_cast<size_t>(Field::Width);
                if (!seen[width])
                    return false;
                const int64_t absolute =
                    static_cast<int64_t>(displacements[width]) + value;
                if (absolute < INT32_MIN || absolute > INT32_MAX)
                    return false;
                value = static_cast<int32_t>(absolute);
            }
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
    const auto base = static_cast<size_t>(Field::DisplayBase);
    if (seen[base] &&
        static_cast<int64_t>(displacements[base]) - 32 !=
            displacements[static_cast<size_t>(Field::FormatCheck)])
        return false;
    const bool packed = seen[static_cast<size_t>(Field::Thresholds)];
    const auto accessSize = [&](size_t field) -> int64_t {
        if (field == static_cast<size_t>(Field::Width) && packed)
            return 16; // MOVDQA reads four 32-bit lanes.
        if (field == static_cast<size_t>(Field::Mode) ||
            field == static_cast<size_t>(Field::SavedPointer))
            return 8;
        if (field == static_cast<size_t>(Field::FormatCheck))
            return check.formatAccessSize;
        return 4;
    };
    for (size_t i = 0; i < static_cast<size_t>(Field::DisplayBase); ++i) {
        if (!seen[i])
            continue;
        // Each local must fit below the saved frame pointer and occupy
        // separate bytes, including the full vector read by MOVDQA.
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

// Most executable pages contain no mode check. Two byte lookups rule out
// unlikely offsets before comparing the full prefixes of individual layouts.
struct ModeCheckPrefixes {
    bool secondByte[256]{};
    bool thirdByte[256]{};
    constexpr ModeCheckPrefixes() {
        for (const auto &check : modeChecks) {
            secondByte[check.original[1]] = true;
            secondByte[check.replacement[1]] = true;
            thirdByte[check.original[2]] = true;
            thirdByte[check.replacement[2]] = true;
        }
    }
};
constexpr ModeCheckPrefixes modeCheckPrefixes{};

inline bool findModeCheck(const uint8_t *data, size_t size, Match &match) {
    match = {};
    if (!data || size < 3)
        return false;
    for (size_t offset = 0; offset <= size - 3; ++offset) {
        const auto *window = data + offset;
        if (!modeCheckPrefixes.thirdByte[window[2]] ||
            !modeCheckPrefixes.secondByte[window[1]])
            continue;
        for (const auto &check : modeChecks) {
            if (check.size > size - offset)
                continue;
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
    int32_t current[fieldCount]{};
    if (matchWindow(target, check.replacement, check, check.replacementOperands,
                    check.replacementOperandCount, current))
        return PatchResult::AlreadyApplied;
    if (!matchWindow(target, check.original, check, check.originalOperands,
                     check.originalOperandCount, current))
        return PatchResult::Mismatch;
    for (size_t i = 0; i < fieldCount; ++i)
        if (current[i] != match.displacements[i])
            return PatchResult::Mismatch;
    const size_t start = check.writeOffset;
    const size_t end = start + check.writeSize;
    if (!preservesBranchEntries(page, size, match.offset, match.offset + start,
                                check.writeSize))
        return PatchResult::Mismatch;

    uint8_t replacement[maxWriteSize];
    for (size_t i = 0; i < check.writeSize; ++i)
        replacement[i] = check.replacement[start + i];
    for (size_t i = 0; i < check.replacementOperandCount; ++i) {
        const auto &slot = check.replacementOperands[i];
        if (slot.byteOffset < start)
            continue;
        if (slot.byteOffset >= end)
            break;
        int64_t offset = match.displacements[static_cast<size_t>(slot.field)];
        offset += slot.bias;
        if (slot.relativeToWidth) {
            offset -= match.displacements[static_cast<size_t>(Field::Width)];
        }
        if (slot.displacementSize == 1 &&
            (offset < INT8_MIN || offset > INT8_MAX))
            return PatchResult::Mismatch;
        const auto value = static_cast<uint32_t>(offset);
        for (size_t byte = 0; byte < slot.displacementSize; ++byte)
            replacement[slot.byteOffset - start + byte] =
                static_cast<uint8_t>(value >> (8 * byte));
    }
    for (size_t i = 0; i < check.writeSize; ++i)
        target[start + i] = replacement[i];
    return PatchResult::Applied;
}

inline bool cachePath(const char *path) {
    if (!path)
        return false;
    size_t length = 0;
    while (path[length])
        ++length;
    size_t end = length;
    while (end && path[end - 1] >= '0' && path[end - 1] <= '9')
        --end;
    if (end < length && end && path[end - 1] == '.')
        --end;
    else
        end = length;
    if (end && path[end - 1] == 'h')
        --end;
    const auto endsWith = [&](const auto &suffix) {
        const size_t size = sizeof(suffix) - 1;
        return end >= size &&
               equal(reinterpret_cast<const uint8_t *>(path + end - size),
                     reinterpret_cast<const uint8_t *>(suffix), size);
    };
    return endsWith("/System/Library/dyld/dyld_shared_cache_x86_64") ||
           endsWith("/private/var/db/dyld/dyld_shared_cache_x86_64");
}

} // namespace display
} // namespace duolink
