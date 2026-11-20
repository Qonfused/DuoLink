/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once
#include "src/patch.hpp"

namespace duolink {
namespace dock {
constexpr size_t pageSize = 4096;

// Match the query's failure, current-display, and empty-result checks.
// All three must lead to the same fallback before changing its first branch.
inline const uint8_t *matchPlacement(const uint8_t *code, size_t size) {
    // The longest combination of the instruction forms below is 35 bytes.
    if (size < 35 || code[0] != 0xe8)
        return nullptr;
    size_t cursor = 5;
    const uint8_t stackCleanup[] = {0x48, 0x83, 0xc4, 0x20};
    if (equal(code + cursor, stackCleanup, sizeof(stackCleanup)))
        cursor += sizeof(stackCleanup);
    if (code[cursor] != 0x85 || code[cursor + 1] != 0xc0)
        return nullptr;
    cursor += 2;
    const auto branch = cursor;
    if ((code[cursor] != 0x75 && code[cursor] != 0xeb) ||
        code[cursor + 1] >= 0x80)
        return nullptr;
    const auto fallback = cursor + 2 + code[cursor + 1];
    if (fallback >= size)
        return nullptr;
    cursor += 2;

    uint8_t rex = code[cursor] == 0x44 ? code[cursor++] : 0;
    if (code[cursor] != 0x8b || (code[cursor + 1] & 0xc7) != 0x45)
        return nullptr;
    const auto displayReg = ((code[cursor + 1] >> 3) & 7) + (rex ? 8 : 0);
    const auto displaySlot = code[cursor + 2];
    cursor += 3;
    const bool queryMainDisplay = code[cursor] == 0xe8;
    if (queryMainDisplay)
        cursor += 5;

    rex = (code[cursor] & 0xf8) == 0x40 ? code[cursor++] : 0;
    const auto modrm = code[cursor + 1];
    const auto reg = ((modrm >> 3) & 7) + ((rex & 4) ? 8 : 0);
    const auto operand = (modrm & 7) + ((rex & 1) ? 8 : 0);
    if (code[cursor] == 0x3b && (modrm & 0xc7) == 0x45 && reg == displayReg &&
        code[cursor + 2] != displaySlot && !queryMainDisplay && !(rex & 1)) {
        cursor += 3;
    } else if (code[cursor] == 0x39 && (modrm & 0xc0) == 0xc0 &&
               operand == displayReg && reg != displayReg &&
               (!queryMainDisplay || reg == 0)) {
        cursor += 2;
    } else {
        return nullptr;
    }
    if (code[cursor] != 0x74 || cursor + 2 + code[cursor + 1] != fallback)
        return nullptr;
    cursor += 2;

    uint8_t countSlot;
    if (code[cursor] == 0x83 && code[cursor + 1] == 0x7d &&
        code[cursor + 3] == 0) {
        countSlot = code[cursor + 2];
        cursor += 4;
    } else if (code[cursor] == 0x8b && code[cursor + 1] == 0x45 &&
               code[cursor + 3] == 0x85 && code[cursor + 4] == 0xc0) {
        countSlot = code[cursor + 2];
        cursor += 5;
    } else {
        return nullptr;
    }
    if (countSlot == displaySlot || code[cursor] != 0x74 ||
        cursor + 2 + code[cursor + 1] != fallback || cursor + 2 >= fallback)
        return nullptr;
    return code + branch;
}

inline const uint8_t *placementBranch(const uint8_t *data, size_t size) {
    if (!data || size < 35)
        return nullptr;
    const uint8_t *match = nullptr;
    for (size_t i = 0; i <= size - 35; ++i) {
        const auto candidate = matchPlacement(data + i, size - i);
        if (candidate) {
            if (match)
                return nullptr;
            match = candidate;
        }
    }
    return match;
}

inline bool pathMatches(const char *path) {
    if (!path)
        return false;
    const char suffix[] =
        "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock";
    size_t length = 0;
    while (path[length])
        ++length;
    return length >= sizeof(suffix) - 1 &&
           equal(reinterpret_cast<const uint8_t *>(path + length -
                                                   (sizeof(suffix) - 1)),
                 reinterpret_cast<const uint8_t *>(suffix), sizeof(suffix) - 1);
}

inline PatchResult patchPage(uint8_t *page, size_t size) {
    if (size != pageSize)
        return PatchResult::NoMatch;
    auto branch = const_cast<uint8_t *>(placementBranch(page, size));
    if (!branch)
        return PatchResult::NoMatch;
    if (*branch == 0xeb)
        return PatchResult::AlreadyApplied;
    // Taking the query-failure branch keeps Dock on the primary display
    // instead of following an adjacent screen along its configured edge.
    *branch = 0xeb;
    return PatchResult::Applied;
}
} // namespace dock
} // namespace duolink
