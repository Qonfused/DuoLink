/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "diagnostics.hpp"
#include "../input/kern_input.hpp"
#include "kern_pen_wire.hpp"
#include <Headers/kern_api.hpp>
#include <kern/thread_call.h>
#include <IOKit/IOLocks.h>

// Lilu's default service declines to attach if bootstrap fails. Use a
// separate service so that failure still appears in IORegistry.
class EXPORT DuoLink : public IOService {
    OSDeclareDefaultStructors(DuoLink);

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(DuoLink, IOService);

namespace duolink {

Diagnostics diagnostics;
static IOService *diagnosticService{nullptr};
static thread_call_t diagnosticCall{nullptr};

// Service stop clears diagnosticCall under this lock before freeing it.
// Keep the lock allocated so later hook calls can safely check for work.
static IOSimpleLock *schedulerLock{nullptr};

void scheduleDiagnostics() {
    auto *lock = __atomic_load_n(&schedulerLock, __ATOMIC_ACQUIRE);
    if (!lock)
        return;

    const auto interrupts = IOSimpleLockLockDisableInterrupt(lock);
    if (diagnosticCall)
        thread_call_enter(diagnosticCall);
    IOSimpleLockUnlockEnableInterrupt(lock, interrupts);
}

static void publishDiagnostics() {
    if (!diagnosticService || !lilu_get_interrupts_enabled())
        return;

    const auto &d = diagnostics;
    diagnosticService->setProperty("BootstrapSucceeded", ADDPR(startSuccess));
    diagnosticService->setProperty("Status", d.status);
    diagnosticService->setProperty("DisplayStatus", d.displayStatus);

    diagnosticService->setProperty(
        "DockStatus", __atomic_load_n(&d.dockStatus, __ATOMIC_RELAXED));
    diagnosticService->setProperty(
        "DockReplacementCount",
        __atomic_load_n(&d.dockReplacements, __ATOMIC_RELAXED), 32);
    diagnosticService->setProperty(
        "DockHookError", __atomic_load_n(&d.dockHookError, __ATOMIC_RELAXED),
        32);

    diagnosticService->setProperty("DisplayReplacementCount",
                                   d.displayReplacements, 32);
    diagnosticService->setProperty("DisplayHookError", d.displayHookError, 32);
    diagnosticService->setProperty("DriverCallbackCount", d.callbacks, 32);

    diagnosticService->setProperty("ASSRTrainingCalls", d.ASSRTrainingCalls,
                                   32);
    diagnosticService->setProperty("ASSRFB1TrainingCalls",
                                   d.ASSRFB1TrainingCalls, 32);
    diagnosticService->setProperty("ASSRVerifiedWrites", d.ASSRVerifiedWrites,
                                   32);
    diagnosticService->setProperty("ASSRLastTxControl", d.ASSRLastTXControl,
                                   32);
    diagnosticService->setProperty("ASSRLastAuxError", d.ASSRLastAUXError, 32);
    diagnosticService->setProperty("RegistrationError", d.registrationError,
                                   32);
    diagnosticService->setProperty("PatchError", d.patchError, 32);
    diagnosticService->setProperty("DriverImageMagic", d.imageMagic, 32);
    diagnosticService->setProperty("DriverImageSize", d.imageSize, 64);

    publishPenWireProbe(diagnosticService);
    publishInputRoutes(diagnosticService);
}

static void refreshDiagnostics(thread_call_param_t, thread_call_param_t) {
    publishDiagnostics();
}

void setStatus(const char *status) {
    diagnostics.status = status;
    scheduleDiagnostics();
}

} // namespace duolink

bool DuoLink::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;

    if (!duolink::schedulerLock) {
        auto *lock = IOSimpleLockAlloc();
        if (!lock) {
            IOLog("DuoLink: diagnostic scheduler lock allocation failed\n");
            return false;
        }
        __atomic_store_n(&duolink::schedulerLock, lock, __ATOMIC_RELEASE);
    }
    auto call = thread_call_allocate(duolink::refreshDiagnostics, nullptr);
    if (!call) {
        IOLog("DuoLink: diagnostic callback allocation failed\n");
        return false;
    }

    duolink::diagnosticService = this;
    const auto interrupts =
        IOSimpleLockLockDisableInterrupt(duolink::schedulerLock);
    duolink::diagnosticCall = call;
    IOSimpleLockUnlockEnableInterrupt(duolink::schedulerLock, interrupts);

    setProperty("VersionInfo", KEXT_VERSION);
    if (!ADDPR(startSuccess)) {
        duolink::setStatus(checkKernelArgument("-duolinkoff")
                               ? "disabled"
                               : "Lilu-bootstrap-rejected");
    } else {
        duolink::publishDiagnostics();
    }
    registerService();
    return true;
}

void DuoLink::stop(IOService *provider) {
    thread_call_t call = nullptr;
    if (duolink::schedulerLock) {
        const auto interrupts =
            IOSimpleLockLockDisableInterrupt(duolink::schedulerLock);
        call = duolink::diagnosticCall;
        duolink::diagnosticCall = nullptr;
        IOSimpleLockUnlockEnableInterrupt(duolink::schedulerLock, interrupts);
    }

    // Wait for any running IORegistry update before clearing diagnosticService.
    if (call) {
        thread_call_cancel_wait(call);
        thread_call_free(call);
    }
    duolink::diagnosticService = nullptr;
    IOService::stop(provider);
}
