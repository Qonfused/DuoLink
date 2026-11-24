/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include "instruction.h"

namespace duolink {
namespace display {

constexpr size_t executablePageSize = 4096;
constexpr size_t pageBitmapWords = executablePageSize / 64;

inline void setBit(uint64_t *bits, size_t offset) {
    bits[offset / 64] |= uint64_t{1} << (offset % 64);
}

inline bool hasBit(const uint64_t *bits, size_t offset) {
    return bits[offset / 64] & (uint64_t{1} << (offset % 64));
}

// Try the first 15 possible boundaries because a page may begin
// mid-instruction. Exclude a byte as a branch start only when every decoding
// that reaches the mode check places that byte inside another instruction.
inline bool markPrefixInteriors(const uint8_t *page, size_t start,
                                uint64_t *interiors) {
    bool found = false;
    for (size_t first = 0; first < 15 && first <= start; ++first) {
        uint64_t candidate[pageBitmapWords]{};
        size_t offset = first;
        while (offset < start) {
            const auto instruction =
                duolinkDecodeInstruction(page + offset, start - offset);
            if (!instruction.length)
                break;
            for (size_t i = 1; i < instruction.length; ++i)
                setBit(candidate, offset + i);
            offset += instruction.length;
        }
        if (offset != start)
            continue;
        for (size_t i = 0; i < pageBitmapWords; ++i)
            interiors[i] = found ? interiors[i] & candidate[i] : candidate[i];
        found = true;
    }
    return found;
}

// Follow execution from the end of the size check to identify bytes inside
// instructions. A jump from another part of the page can still reach code
// beyond a return, so the branch scan also examines bytes left unmarked.
inline bool markReachableInteriors(const uint8_t *page, size_t start,
                                   size_t end, uint64_t *interiors) {
    uint64_t pending[pageBitmapWords]{};
    uint64_t visited[pageBitmapWords]{};
    if (end < executablePageSize)
        setBit(pending, end);
    for (;;) {
        size_t word = 0;
        while (word < pageBitmapWords && !pending[word])
            ++word;
        if (word == pageBitmapWords)
            return true;
        const size_t offset =
            word * 64 + static_cast<size_t>(__builtin_ctzll(pending[word]));
        pending[word] &= pending[word] - 1;
        if (hasBit(visited, offset) || (offset >= start && offset < end))
            continue;
        if (hasBit(interiors, offset))
            return false;
        setBit(visited, offset);
        const auto instruction = duolinkDecodeInstruction(
            page + offset, executablePageSize - offset);
        if (!instruction.length)
            continue;
        for (size_t i = 1; i < instruction.length; ++i) {
            if (hasBit(visited, offset + i) ||
                (offset + i >= start && offset + i < end))
                return false;
            setBit(interiors, offset + i);
        }
        const size_t next = offset + instruction.length;
        if (instruction.fallsThrough && next < executablePageSize)
            setBit(pending, next);
        const int64_t target =
            static_cast<int64_t>(next) + instruction.displacement;
        if (instruction.relative && target >= 0 &&
            target < static_cast<int64_t>(executablePageSize))
            setBit(pending, static_cast<size_t>(target));
    }
}

// A branch into the middle of the size check could land in a different
// instruction after patching. When entry precedes start, branching past
// entry could also skip the LEA that sets up RBX for the format check.
inline bool preservesBranchEntries(const uint8_t *page, size_t size,
                                   size_t entry, size_t start, size_t length) {
    if (entry > start || !page || size != executablePageSize || start > size ||
        length > size - start)
        return false;
    uint64_t interiors[pageBitmapWords]{};
    if (!markPrefixInteriors(page, start, interiors))
        return false;
    const size_t end = start + length;
    if (!markReachableInteriors(page, start, end, interiors))
        return false;
    for (size_t offset = 0; offset < size; ++offset) {
        if ((offset >= start && offset < end) || hasBit(interiors, offset))
            continue;
        const auto instruction =
            duolinkDecodeInstruction(page + offset, size - offset);
        const int64_t target =
            static_cast<int64_t>(offset + instruction.length) +
            instruction.displacement;
        if (instruction.relative && target > static_cast<int64_t>(entry) &&
            target < static_cast<int64_t>(end))
            return false;
    }
    return true;
}

} // namespace display
} // namespace duolink
