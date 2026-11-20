/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include "src/patch.hpp"

namespace duolink {
namespace pen_wire {

constexpr size_t inputReadSize = 67;
constexpr uint16_t penWireLength = 13;
constexpr uint8_t penReport = 7;
constexpr uint8_t touchReport = 1;

// Verified readI2C() entry in the deployed VoodooI2C driver. Lilu's 6- and
// 14-byte routes end in complete position-independent instructions here. The
// later RIP-relative load of readI2CGated stays at its original address.
inline bool safeReadEntry(const uint8_t *code, size_t size) {
    constexpr uint8_t prologue[] = {
        0x55, 0x48, 0x89, 0xe5, // push rbp; mov rbp,rsp
        0x48, 0x83, 0xec, 0x10, // sub rsp,16
        0x49, 0x89, 0xf0,       // mov r8,rsi
        0x48, 0x8d, 0x4d, 0xfe, // lea rcx,[rbp-2]
    };
    return code && size >= sizeof(prologue) &&
           equal(code, prologue, sizeof(prologue));
}

enum class Kind { Invalid, Reset, Touch, Pen, Other, Fill, ShiftedPen };

struct Frame {
    Kind kind;
    uint16_t length;
    uint8_t offset;
};

inline uint16_t wireLength(const uint8_t *buffer) {
    return static_cast<uint16_t>(buffer[0] | (uint16_t(buffer[1]) << 8));
}

inline Frame inspect(const uint8_t *buffer, size_t size) {
    if (!buffer || size != inputReadSize)
        return {Kind::Invalid, 0, 0};
    const auto length = wireLength(buffer);
    if (length == 0)
        return {Kind::Reset, 0, 0};
    if (length >= 3 && length <= size) {
        const auto id = buffer[2];
        if (id == touchReport)
            return {Kind::Touch, length, 0};
        if (id == penReport) {
            const auto kind =
                length == penWireLength ? Kind::Pen : Kind::Invalid;
            return {kind, length, 0};
        }
        return {Kind::Other, length, 0};
    }
    // Only look after leading 0xff bytes, never in a valid report's payload.
    size_t offset = 0;
    while (offset < size && buffer[offset] == 0xff)
        ++offset;
    if (offset == size)
        return {Kind::Fill, length, 0};
    if (offset && size - offset >= penWireLength &&
        wireLength(buffer + offset) == penWireLength &&
        buffer[offset + 2] == penReport)
        return {Kind::ShiftedPen, length, static_cast<uint8_t>(offset)};
    return {Kind::Invalid, length, 0};
}

} // namespace pen_wire
} // namespace duolink
