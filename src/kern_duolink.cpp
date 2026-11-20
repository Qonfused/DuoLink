/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <libkern/version.h>
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>

#include "diagnostics/diagnostics.hpp"
#include "diagnostics/kern_pen_wire.hpp"
#include "display/kern_display.hpp"
#include "input/kern_input.hpp"
#include "screenpad/kern_assr.hpp"

static void pluginStart() {
    duolink::registerPenWireProbe();
    duolink::registerInputRoutes();
    const auto displayError =
        lilu.onPatcherLoad(duolink::installDisplayAndDockHooks);
    if (displayError != LiluAPI::Error::NoError) {
        duolink::diagnostics.displayStatus = "callback-registration-failed";
        duolink::diagnostics.displayHookError =
            static_cast<uint32_t>(displayError);
    }
    duolink::registerASSRPatch();
}

static const char *disableArgs[] = {"-duolinkoff"};
static const char *debugArgs[] = {"-duolinkdbg"};

PluginConfiguration ADDPR(config){
    "DuoLink",
    parseModuleVersion(KEXT_VERSION),
    LiluAPI::AllowNormal,
    disableArgs,
    1,
    debugArgs,
    1,
    nullptr,
    0,
    KernelVersion::Catalina,
    // Darwin 25 is Tahoe; the pinned Lilu headers end at Ventura.
    static_cast<KernelVersion>(25),
    pluginStart,
};
