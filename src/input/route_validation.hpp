/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include "src/patch.hpp"

namespace duolink {
namespace input {

inline bool imageMatches(const uint8_t *image, size_t size,
                         const uint8_t (&uuid)[16]) {
    if (!image || size < 32 || read32(image) != 0xfeedfacf)
        return false;
    const auto count = read32(image + 16);
    const auto bytes = read32(image + 20);
    if (bytes > size - 32 || count > bytes / 8)
        return false;
    const size_t end = 32 + bytes;
    size_t offset = 32;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (end - offset < 8)
            return false;
        const auto command = read32(image + offset);
        const auto length = read32(image + offset + 4);
        if (length < 8 || length > end - offset)
            return false;
        if (command == 0x1b) { // LC_UUID
            if (found || length != 24)
                return false;
            for (size_t j = 0; j < 16; ++j)
                if (image[offset + 8 + j] != uuid[j])
                    return false;
            found = true;
        }
        offset += length;
    }
    return found && offset == end;
}

} // namespace input
} // namespace duolink
