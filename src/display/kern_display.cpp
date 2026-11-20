/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <libkern/version.h>
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <libkern/OSAtomic.h>
#include <sys/vnode.h>

#include "display_patch.hpp"
#include "kern_display.hpp"
#include "src/diagnostics/diagnostics.hpp"
#include "src/dock/kern_dock.hpp"

namespace duolink {

static mach_vm_address_t originalValidatePage;
static bool displayEnabled;

// Let XNU authenticate the original executable page before modifying it.
// The mode-check pattern is independent of its location in the dyld cache.
static void validatePage(vnode_t vnode, memory_object_t pager,
                         memory_object_offset_t offset, const void *data,
                         int *validated, int *tainted, int *nx) {
    FunctionCast(validatePage, originalValidatePage)(vnode, pager, offset, data,
                                                     validated, tainted, nx);
    if (!validated || !*validated || !tainted || *tainted || !nx || *nx ||
        !data)
        return;

    patchValidatedDockPage(vnode, data);
    display::Match match;
    if (!displayEnabled ||
        !display::findModeCheck(static_cast<const uint8_t *>(data), PAGE_SIZE,
                                match))
        return;

    char path[PATH_MAX];
    int length = sizeof(path);
    if (vn_getpath(vnode, path, &length) || !display::cachePath(path))
        return;

    const auto result = display::patchPage(
        const_cast<uint8_t *>(static_cast<const uint8_t *>(data)), PAGE_SIZE,
        match);
    if (result == PatchResult::Mismatch) {
        diagnostics.displayStatus = "unsupported-CoreDisplay-control-flow";
        scheduleDiagnostics();
    }
    if (result == PatchResult::Applied) {
        OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(
            &diagnostics.displayReplacements));
        SYSLOG("duolink", "applied CoreDisplay native 1920x515 mode exception");
    }
    if (result == PatchResult::Applied ||
        result == PatchResult::AlreadyApplied) {
        diagnostics.displayStatus = "patch-applied";
        scheduleDiagnostics();
    }
}

void installDisplayAndDockHooks(void *, KernelPatcher &patcher) {
    displayEnabled = !checkKernelArgument("-duolinkdisplayoff");
    const bool dockOff = checkKernelArgument("-duolinkdockoff");
    if (!displayEnabled)
        diagnostics.displayStatus = "disabled";
    if (dockOff)
        __atomic_store_n(&diagnostics.dockStatus, "disabled", __ATOMIC_RELAXED);
    if (!displayEnabled && dockOff)
        return;

    KernelPatcher::RouteRequest request{"_cs_validate_page", validatePage,
                                        originalValidatePage};
    patcher.clearError();
    const auto entry =
        patcher.solveSymbol(KernelPatcher::KernelID, "_cs_validate_page");
    const bool success =
        entry &&
        canCopyKernelPrologue(reinterpret_cast<const uint8_t *>(entry), 21) &&
        patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1);
    diagnostics.displayHookError = static_cast<uint32_t>(patcher.getError());
    patcher.clearError();
    if (displayEnabled)
        diagnostics.displayStatus =
            success ? "waiting-for-CoreDisplay-page" : "hook-failed";
    if (!dockOff)
        registerDockPatch(patcher, success);
    SYSLOG("duolink", "CoreDisplay page hook %s",
           success ? "registered" : "failed");
}

} // namespace duolink
