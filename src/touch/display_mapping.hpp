/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include <stdint.h>

namespace duolink {
namespace touchscreen {

struct Bounds {
    int32_t minx, maxx;
    int32_t miny, maxy;
};

struct Point {
    int32_t x, y;
};

inline bool validBounds(const Bounds &bounds) {
    return bounds.maxx > bounds.minx && bounds.maxy > bounds.miny;
}

inline bool sameBounds(const Bounds &a, const Bounds &b) {
    return a.minx == b.minx && a.maxx == b.maxx && a.miny == b.miny &&
           a.maxy == b.maxy;
}

// Framebuffer bounds exclude their maximum coordinates. Map the largest
// digitizer value to the last pixel inside the display.
inline int32_t mapAxis(int32_t value, int32_t minimum, int32_t maximum) {
    constexpr int32_t digitizerMaximum = 65535;
    if (value <= 0)
        return minimum;
    if (value >= digitizerMaximum)
        return maximum - 1;

    const int64_t span = static_cast<int64_t>(maximum) - minimum - 1;
    const int64_t offset = value * span / digitizerMaximum;
    return static_cast<int32_t>(minimum + offset);
}

inline Point mapPosition(int32_t x, int32_t y, const Bounds &bounds) {
    return {mapAxis(x, bounds.minx, bounds.maxx),
            mapAxis(y, bounds.miny, bounds.maxy)};
}

} // namespace touchscreen
} // namespace duolink
