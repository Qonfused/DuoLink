/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace duolink {
namespace input {

struct PanelProfile {
    const char *role;
    const char *acpiName;
    uint16_t digitizerVendor;
    uint16_t digitizerProduct;
    uint16_t displayVendor;
    uint16_t displayProduct;
};

constexpr PanelProfile panels[] = {
    {"main", "ELAN9008", 0x04f3, 0x2b6a, 0x0dae, 0x14d5},
    {"ScreenPad", "ELAN9009", 0x04f3, 0x29de, 0x09e5, 0x087f},
};
constexpr size_t panelCount = sizeof(panels) / sizeof(panels[0]);
constexpr uint16_t touchpadVendor = 0x04f3;
constexpr uint16_t touchpadProduct = 0x310e;

inline const PanelProfile *panelForACPIName(const void *bytes, size_t size) {
    if (!bytes || size != sizeof("ELAN9008"))
        return nullptr;
    for (const auto &panel : panels) {
        if (!memcmp(bytes, panel.acpiName, size))
            return &panel;
    }
    return nullptr;
}

} // namespace input
} // namespace duolink
