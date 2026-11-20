/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "instruction.h"

#include <arch/X86/X86DisassemblerDecoder.h>

static int readByte(const struct reader_info *reader, uint8_t *byte,
                    uint64_t offset) {
    if (offset >= reader->size)
        return -1;
    *byte = reader->code[offset];
    return 0;
}

// Only Capstone's stateless instruction decoder is linked. Its printing,
// allocation, and shared-handle APIs are unnecessary for branch validation.
DuoLinkInstruction duolinkDecodeInstruction(const uint8_t *code, size_t size) {
    struct InternalInstruction instruction = {0};
    const struct reader_info reader = {code, size, 0};
    if (!code ||
        decodeInstruction(&instruction, readByte, &reader, 0, MODE_64BIT))
        return (DuoLinkInstruction){0};

    const uint8_t opcode = instruction.opcode;
    const bool single = instruction.opcodeType == ONEBYTE;
    // Match Capstone's x86-64 rejection of legacy LES/LDS encodings.
    if (single && (opcode == 0xc4 || opcode == 0xc5))
        return (DuoLinkInstruction){0};
    const bool conditional =
        (single && ((opcode >= 0x70 && opcode <= 0x7f) ||
                    (opcode >= 0xe0 && opcode <= 0xe3))) ||
        (instruction.opcodeType == TWOBYTE && opcode >= 0x80 && opcode <= 0x8f);
    const bool jump = single && (opcode == 0xe9 || opcode == 0xeb);
    const bool call = single && opcode == 0xe8;
    const bool abortsTransaction =
        single && opcode == 0xc7 && instruction.orgModRM == 0xf8;
    const bool returns =
        single && (opcode == 0xc2 || opcode == 0xc3 || opcode == 0xca ||
                   opcode == 0xcb || opcode == 0xcf);
    const bool traps = (single && (opcode == 0xcc || opcode == 0xcd ||
                                   opcode == 0xf1 || opcode == 0xf4)) ||
                       (instruction.opcodeType == TWOBYTE &&
                        (opcode == 0x0b || opcode == 0x05 || opcode == 0x07 ||
                         opcode == 0x34 || opcode == 0x35));
    const bool indirectJump = single && opcode == 0xff &&
                              ((instruction.orgModRM >> 3 & 7) == 4 ||
                               (instruction.orgModRM >> 3 & 7) == 5);
    DuoLinkInstruction decoded = {
        (uint8_t)instruction.length,
        !jump && !returns && !indirectJump && !traps,
        conditional || jump || call || abortsTransaction, 0};
    if (decoded.relative) {
        const size_t offset = instruction.immediateOffset;
        const size_t width = instruction.length - offset;
        if (width != 1 && width != 2 && width != 4)
            return (DuoLinkInstruction){0};
        uint32_t value = 0;
        for (size_t i = 0; i < width; ++i)
            value |= (uint32_t)code[offset + i] << (8 * i);
        decoded.displacement = width == 1   ? (int8_t)value
                               : width == 2 ? (int16_t)value
                                            : (int32_t)value;
    }
    return decoded;
}
