/*
 * Copyright (c) 2026, Cory Bennett. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <stdio.h>
#include "blowfish.h"

static uint32_t read_be32(const uint8_t *data) {
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
           (uint32_t)data[2] << 8 | data[3];
}

static void write_be32(uint8_t *data, uint32_t value) {
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

int main(void) {
    uint8_t key[] =
        "ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc";
    BLOWFISH_CTX context;
    Blowfish_Init(&context, key, sizeof(key) - 1);
    uint8_t page[4096];
    size_t size;
    while ((size = fread(page, 1, sizeof(page), stdin)) != 0) {
        if (size != sizeof(page))
            return 1;
        // DSMOS uses a 64-byte key and resets CBC chaining for each page.
        uint32_t previous_left = 0, previous_right = 0;
        for (size_t i = 0; i < sizeof(page); i += 8) {
            uint32_t left = read_be32(page + i);
            uint32_t right = read_be32(page + i + 4);
            const uint32_t next_left = left, next_right = right;
            Blowfish_Decrypt(&context, &left, &right);
            write_be32(page + i, left ^ previous_left);
            write_be32(page + i + 4, right ^ previous_right);
            previous_left = next_left;
            previous_right = next_right;
        }
        if (fwrite(page, 1, sizeof(page), stdout) != sizeof(page))
            return 1;
    }
    return ferror(stdin) || fflush(stdout) != 0;
}
