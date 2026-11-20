/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include "src/patch.hpp"

namespace duolink {
namespace assr {

// DisplayPort Configuration Data (DPCD) registers on the receiving panel.
constexpr uint32_t edpConfigurationCap = 0x00d;
constexpr uint32_t trainingPatternSet = 0x102;
constexpr uint32_t edpConfigurationSet = 0x10a;
constexpr uint8_t alternateScramblerResetCap = 1U << 0;
constexpr uint8_t alternateScramblerResetEnable = 1U << 0;

inline bool startsLinkTraining(uint32_t address, uint16_t length,
                               const void *data) {
    // The CFL driver writes pattern 1 with link scrambling disabled during
    // training.
    constexpr uint8_t patternOne = 0x01;
    constexpr uint8_t scramblingDisabled = 1U << 5;
    return address == trainingPatternSet && length == 1 && data &&
           *static_cast<const uint8_t *>(data) ==
               (patternOne | scramblingDisabled);
}

// On the UX481, AppleIntelFramebuffer 1 drives the built-in ScreenPad on DDI B.
constexpr unsigned long ddiBTransportControl = 0x64140;
constexpr uint32_t transportEnabled = 1U << 31;
constexpr uint32_t transportASSR = 1U << 6;

inline bool sourceUsesASSR(uint32_t control) {
    constexpr uint32_t required = transportEnabled | transportASSR;
    return control != UINT32_MAX && (control & required) == required;
}

inline bool isScreenPad(const uint8_t (&edid)[128]) {
    constexpr uint8_t identity[] = {0,    0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0,    0x09, 0xe5, 0x7f, 0x08};
    if (!equal(edid, identity, sizeof(identity)))
        return false;
    unsigned sum = 0;
    for (auto byte : edid)
        sum += byte;
    return (sum & 0xff) == 0;
}

enum class Result {
    CapabilitiesReadFailed,
    UnsupportedPanel,
    ConfigReadFailed,
    AlreadyEnabled,
    EDIDReadFailed,
    DifferentPanel,
    WriteFailed,
    ReadbackFailed,
    Enabled
};

// Called after framebuffer 1 starts link training with ASSR enabled on the
// transmitter.
template <typename AUX> Result enablePanelASSR(AUX &aux) {
    uint8_t capabilities = 0;
    if (!aux.read(edpConfigurationCap, capabilities))
        return Result::CapabilitiesReadFailed;
    if (!(capabilities & alternateScramblerResetCap))
        return Result::UnsupportedPanel;

    uint8_t config = 0;
    if (!aux.read(edpConfigurationSet, config))
        return Result::ConfigReadFailed;
    if (config & alternateScramblerResetEnable)
        return Result::AlreadyEnabled;

    uint8_t edid[128]{};
    if (!aux.readEDID(edid))
        return Result::EDIDReadFailed;
    if (!isScreenPad(edid))
        return Result::DifferentPanel;

    const auto desired =
        static_cast<uint8_t>(config | alternateScramblerResetEnable);
    if (!aux.write(edpConfigurationSet, desired))
        return Result::WriteFailed;
    uint8_t readback = 0;
    if (!aux.read(edpConfigurationSet, readback) || readback != desired)
        return Result::ReadbackFailed;
    return Result::Enabled;
}

} // namespace assr
} // namespace duolink
