/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

namespace duolink {
namespace assr {

constexpr const char *writeAUXSymbol =
    "__"
    "ZN31AppleIntelFramebufferController8WriteAUXEP21AppleIntelFramebufferj"
    "tPvP21AppleIntelDisplayPath";
constexpr const char *readAUXSymbol =
    "__"
    "ZN31AppleIntelFramebufferController7ReadAUXEP21AppleIntelFramebufferjt"
    "PvP21AppleIntelDisplayPath";
constexpr const char *readEDIDSymbol =
    "__"
    "ZN31AppleIntelFramebufferController19DisplayPortReadEDIDEP21AppleIntel"
    "FramebufferhjtPhP21AppleIntelDisplayPath";
constexpr const char *readRegister32Symbol =
    "__ZN31AppleIntelFramebufferController14ReadRegister32Em";

} // namespace assr

void registerASSRPatch();

} // namespace duolink
