/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once
#include "src/patch.hpp"

namespace duolink {
namespace transform {
using Function = int (*)(const void *, void *, unsigned long long, void *);
using Slot = Function const volatile *;

// Copying the dispatcher's RIP-relative load into a trampoline changes the
// address it reads. Resolve the slot at the original address and read it on
// each call, since DSMOS may register its implementation later in boot.
inline uintptr_t resolveSlot(const uint8_t *code, size_t size,
                             uintptr_t address) {
    const uint8_t prefix[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x05};
    const uint8_t suffix[] = {0x48, 0x85, 0xc0, 0x74, 0x03, 0x5d, 0xff, 0xe0};
    if (!code || size < 19 || !equal(code, prefix, 7) ||
        !equal(code + 11, suffix, 8))
        return 0;
    const int32_t displacement = static_cast<int32_t>(read32(code + 7));
    if (displacement < -0x4000000 || displacement > 0x4000000 ||
        address > UINTPTR_MAX - 11)
        return 0;
    const uintptr_t next = address + 11;
    const uintptr_t magnitude = displacement < 0
                                    ? uintptr_t(-int64_t(displacement))
                                    : uintptr_t(displacement);
    if (displacement < 0 ? next < magnitude : next > UINTPTR_MAX - magnitude)
        return 0;
    const uintptr_t slot =
        displacement < 0 ? next - magnitude : next + magnitude;
    return slot && !(slot & (sizeof(uintptr_t) - 1)) ? slot : 0;
}

inline int forward(Slot slot, const void *from, void *to,
                   unsigned long long offset, void *ops) {
    const auto function = slot ? *slot : nullptr;
    // XNU returns KERN_ABORTED while the DSMOS implementation is absent.
    return function ? function(from, to, offset, ops) : 14;
}
} // namespace transform
} // namespace duolink
