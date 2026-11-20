/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include <libkern/version.h>
#include <Headers/plugin_start.hpp>

namespace duolink {

struct Diagnostics {
    const char *status{"bootstrap-not-completed"};
    const char *displayStatus{"not-registered"};
    const char *dockStatus{"not-registered"};
    uint32_t dockReplacements{0};
    uint32_t dockHookError{0};
    uint32_t displayReplacements{0};
    uint32_t displayHookError{0};
    uint32_t callbacks{0};
    uint32_t ASSRTrainingCalls{0};
    uint32_t ASSRFB1TrainingCalls{0};
    uint32_t ASSRVerifiedWrites{0};
    uint32_t ASSRLastTXControl{0};
    uint32_t ASSRLastAUXError{0};
    uint32_t registrationError{0};
    uint32_t patchError{0};
    uint32_t imageMagic{0};
    uint64_t imageSize{0};
};

extern Diagnostics diagnostics;
void scheduleDiagnostics();
void setStatus(const char *status);

} // namespace duolink
