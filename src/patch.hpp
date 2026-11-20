/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace duolink {

enum class PatchResult { NoMatch, Applied, AlreadyApplied, Mismatch };

inline bool equal(const uint8_t *left, const uint8_t *right, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (left[i] != right[i])
            return false;
    return true;
}

inline uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}

// When no address slot is available, Lilu needs enough whole instructions
// to make room for a 14-byte jump. Accept only the position-independent
// frame setup, pushes, stack allocation, and register moves recognized below.
inline bool canCopyKernelPrologue(const uint8_t *code, size_t length) {
    constexpr uint8_t frame[] = {0x55, 0x48, 0x89,
                                 0xe5}; // push rbp; mov rbp,rsp
    constexpr size_t jumpSize = 14;
    if (!code || length < jumpSize || !equal(code, frame, sizeof(frame)))
        return false;
    size_t cursor = sizeof(frame);
    while (cursor < jumpSize) {
        const auto *p = code + cursor;
        const size_t left = length - cursor;
        if (*p == 0x53 || *p == 0x56 || *p == 0x57) {
            ++cursor;
        } else if (left >= 2 && p[0] == 0x41 && p[1] >= 0x54 && p[1] <= 0x57) {
            cursor += 2;
        } else if (left >= 4 && p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xec) {
            cursor += 4;
        } else if (left >= 7 && p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xec) {
            cursor += 7;
        } else if (left >= 3 && p[0] >= 0x48 && p[0] <= 0x4f && p[1] == 0x89 &&
                   (p[2] & 0xc0) == 0xc0) {
            cursor += 3;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace duolink
