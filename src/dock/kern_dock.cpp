/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "kern_dock.hpp"
#include "dock_patch.hpp"
#include "src/diagnostics/diagnostics.hpp"
#include "transform_hook.hpp"
#include <Headers/kern_patcher.hpp>
#include <sys/proc.h>

namespace duolink {
static transform::Slot originalTransformSlot;
static bool dockEnabled;

static void patchDockPage(uint8_t *page) {
    const auto result = dock::patchPage(page, PAGE_SIZE);
    if (result != PatchResult::Applied && result != PatchResult::AlreadyApplied)
        return;
    if (result == PatchResult::Applied) {
        __atomic_add_fetch(&diagnostics.dockReplacements, 1, __ATOMIC_RELAXED);
        SYSLOG("duolink", "kept Dock placement on the primary display");
    }
    __atomic_store_n(&diagnostics.dockStatus, "patch-applied",
                     __ATOMIC_RELAXED);
    scheduleDiagnostics();
}

void patchValidatedDockPage(vnode_t vnode, const void *data) {
    if (!__atomic_load_n(&dockEnabled, __ATOMIC_ACQUIRE) || !data ||
        !dock::placementBranch(static_cast<const uint8_t *>(data), PAGE_SIZE))
        return;
    char path[PATH_MAX];
    int length = sizeof(path);
    if (vn_getpath(vnode, path, &length) || !dock::pathMatches(path))
        return;
    patchDockPage(const_cast<uint8_t *>(static_cast<const uint8_t *>(data)));
}

// Protected executables reach the matcher after DSMOS decodes their pages.
// This callback has no vnode, so restrict it to Dock's own faults.
static int transformPage(const void *from, void *to, unsigned long long offset,
                         void *ops) {
    const auto slot = __atomic_load_n(&originalTransformSlot, __ATOMIC_ACQUIRE);
    const int error = transform::forward(slot, from, to, offset, ops);
    if (error || !__atomic_load_n(&dockEnabled, __ATOMIC_ACQUIRE) || !to ||
        reinterpret_cast<uintptr_t>(ops) != 0x2e69cf40 ||
        !dock::placementBranch(static_cast<const uint8_t *>(to), PAGE_SIZE))
        return error;
    char name[6]{};
    proc_selfname(name, sizeof(name));
    if (!strcmp(name, "Dock"))
        patchDockPage(static_cast<uint8_t *>(to));
    return error;
}

void registerDockPatch(KernelPatcher &patcher, bool validationHookReady) {
    patcher.clearError();
    const auto address =
        patcher.solveSymbol(KernelPatcher::KernelID, "_dsmos_page_transform");
    const auto slot =
        address ? transform::resolveSlot(
                      reinterpret_cast<const uint8_t *>(address), 19, address)
                : 0;
    bool routed = false;
    if (slot) {
        __atomic_store_n(&originalTransformSlot,
                         reinterpret_cast<transform::Slot>(slot),
                         __ATOMIC_RELEASE);
        KernelPatcher::RouteRequest request{"_dsmos_page_transform",
                                            transformPage};
        patcher.clearError();
        routed =
            patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1);
    }
    __atomic_store_n(&diagnostics.dockHookError,
                     static_cast<uint32_t>(patcher.getError()),
                     __ATOMIC_RELAXED);
    patcher.clearError();
    __atomic_store_n(&dockEnabled, validationHookReady || routed,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&diagnostics.dockStatus,
                     validationHookReady || routed ? "waiting-for-Dock-pages"
                                                   : "hook-failed",
                     __ATOMIC_RELAXED);
    SYSLOG("duolink", "Dock validation hook %s, decode hook %s",
           validationHookReady ? "registered" : "unavailable",
           routed ? "registered" : "unavailable");
}
} // namespace duolink
