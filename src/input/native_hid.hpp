/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include <stddef.h>
#include <stdint.h>

class IOService;
class IOFramebuffer;
class OSObject;

namespace duolink {
namespace input {
struct Report;
namespace native {

template <typename T> inline T readField(const void *object, size_t offset) {
    T value;
    __builtin_memcpy(&value, static_cast<const uint8_t *>(object) + offset,
                     sizeof(value));
    return value;
}
template <typename T>
inline void writeField(void *object, size_t offset, T value) {
    __builtin_memcpy(static_cast<uint8_t *>(object) + offset, &value,
                     sizeof(value));
}
// Clear the field before releasing its reference so callbacks triggered by
// release can't read a stale cache entry.
template <typename Framebuffer>
inline bool invalidateFramebufferCache(void *driver, size_t offset,
                                       Framebuffer *selected) {
    auto *cached = readField<Framebuffer *>(driver, offset);
    if (!cached || (cached == selected && !cached->isInactive()))
        return false;
    writeField<Framebuffer *>(driver, offset, nullptr);
    cached->release();
    return true;
}

// VoodooI2CHID normalizes pen samples by their declared range. A zero or
// negative range cannot describe that input, so pass sample 0 and range 1
// for the native call, then restore both fields.
class RangeGuard {
public:
    RangeGuard(void *object, size_t valueOffset, size_t maximumOffset)
        : object(object), valueOffset(valueOffset),
          maximumOffset(maximumOffset),
          maximum(readField<int32_t>(object, maximumOffset)) {
        if (changed()) {
            value = readField<uint16_t>(object, valueOffset);
            writeField<uint16_t>(object, valueOffset, 0);
            writeField<int32_t>(object, maximumOffset, 1);
        }
    }

    ~RangeGuard() {
        if (changed()) {
            writeField<uint16_t>(object, valueOffset, value);
            writeField<int32_t>(object, maximumOffset, maximum);
        }
    }

    RangeGuard(const RangeGuard &) = delete;
    RangeGuard &operator=(const RangeGuard &) = delete;
    bool changed() const {
        return maximum <= 0;
    }

private:
    void *object;
    size_t valueOffset, maximumOffset;
    int32_t maximum;
    uint16_t value{0};
};

bool hasTouchscreenLayout(IOService *driver);

// Drop the native cache entry if its framebuffer is inactive or differs from
// the companion's selection. The companion retains its framebuffer separately.
bool invalidateFramebuffer(IOService *driver, IOFramebuffer *selected);

// Returns a borrowed transducer only when both coordinates belong to this
// report. Cached pen coordinates in a finger report do not satisfy the check.
OSObject *currentPen(const Report &report, uint64_t timestamp);
bool penInRange(const OSObject *pen);

class PenRanges {
public:
    explicit PenRanges(OSObject *pen);
    unsigned adjusted() const;

private:
    RangeGuard z;
    RangeGuard pressure;
};

bool hasDigitizerIdentity(IOService *provider, uint32_t vendor,
                          uint32_t product);

} // namespace native
} // namespace input
} // namespace duolink
