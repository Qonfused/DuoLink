/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t length;
    bool fallsThrough;
    bool relative;
    int32_t displacement;
} DuoLinkInstruction;

#ifdef __cplusplus
extern "C" {
#endif

DuoLinkInstruction duolinkDecodeInstruction(const uint8_t *code, size_t size);

#ifdef __cplusplus
}
#endif
