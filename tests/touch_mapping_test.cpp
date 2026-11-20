/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "src/touch/display_mapping.hpp"

#include <cassert>
#include <cstdint>

using namespace duolink::touchscreen;

static void testVerticallyAdjacentPanels() {
    const Bounds main = {0, 1920, 0, 1080};
    const Bounds screenPad = {0, 1920, 1080, 1595};
    assert(validBounds(main) && validBounds(screenPad));
    assert(sameBounds(main, main) && !sameBounds(main, screenPad));

    assert(mapPosition(0, 65535, main).y == 1079);
    assert(mapPosition(0, 0, screenPad).y == 1080);
    assert(mapPosition(32768, 32768, screenPad).y == 1337);
    const Point last = mapPosition(65535, 65535, screenPad);
    assert(last.x == 1919 && last.y == 1594);

    const Point clamped = mapPosition(-100, 100000, screenPad);
    assert(clamped.x == 0 && clamped.y == 1594);
}

static void testFramebufferBounds() {
    assert(!validBounds({0, 0, 0, 1}));
    assert(!validBounds({0, 1, 1, 1}));
    assert(!validBounds({10, 0, 0, 1}));

    const Bounds layouts[] = {
        {0, 1920, 0, 1080}, {0, 1920, 1080, 1595}, {-1920, 0, -515, 0},
        {0, 1080, 0, 1920}, {100, 101, 100, 101},
    };
    const int32_t inputs[] = {-100, 0, 1, 32767, 65534, 65535, 100000};
    for (const auto &bounds : layouts) {
        assert(validBounds(bounds));
        for (const auto x : inputs) {
            for (const auto y : inputs) {
                const Point point = mapPosition(x, y, bounds);
                assert(point.x >= bounds.minx && point.x < bounds.maxx);
                assert(point.y >= bounds.miny && point.y < bounds.maxy);
            }
        }
    }

    // A valid signed desktop coordinate may have an offset from the origin
    // larger than INT32_MAX. Narrow only after adding the origin back.
    assert(mapAxis(65534, INT32_MIN, INT32_MAX) == INT32_MAX - 65538);
}

int main() {
    testVerticallyAdjacentPanels();
    testFramebufferBounds();
}
