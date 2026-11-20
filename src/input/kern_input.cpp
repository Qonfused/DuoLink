/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "kern_input.hpp"
#include "../diagnostics/diagnostics.hpp"
#include "devices.hpp"
#include "route_validation.hpp"
#include "routing_api.hpp"
#include "stock_input_profile.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>

namespace duolink {
namespace input {
namespace {
const char *hidPaths[] = {
    "/Library/Extensions/VoodooI2CHID.kext/Contents/MacOS/VoodooI2CHID",
    "/System/Library/Extensions/VoodooI2CHID.kext/Contents/MacOS/VoodooI2CHID",
};
const char *corePaths[] = {
    "/Library/Extensions/VoodooI2C.kext/Contents/MacOS/VoodooI2C",
    "/System/Library/Extensions/VoodooI2C.kext/Contents/MacOS/VoodooI2C",
};
KernelPatcher::KextInfo hid{
    "com.alexandred.VoodooI2CHID",    hidPaths, 2, {true}, {},
    KernelPatcher::KextInfo::Unloaded};
KernelPatcher::KextInfo core{
    "com.alexandred.VoodooI2C",       corePaths, 2, {true}, {},
    KernelPatcher::KextInfo::Unloaded};
Originals originals;
const Callbacks *registeredCallbacks;
const char *status = "not-registered";
const char *coreStatus = "not-registered";
uint32_t hookError;
uint32_t coreError;
uint32_t irqCorrections;
mach_vm_address_t originalInterruptType;
bool coreReady;

const Originals *bindCallbacks(const Callbacks *table) {
    if (!table || table->version != callbackVersion ||
        table->size != sizeof(Callbacks) || !table->matches ||
        !table->framebuffer || !table->stylus || !table->lift ||
        !table->report || !table->started || !table->stopped || !table->ptp ||
        !table->dispatch)
        return nullptr;

    const Callbacks *previous = nullptr;
    if (!__atomic_compare_exchange_n(&registeredCallbacks, &previous, table,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE) &&
        previous != table)
        return nullptr;
    return &originals;
}

const Callbacks *activeCallbacks() {
    return available(&originals)
               ? __atomic_load_n(&registeredCallbacks, __ATOMIC_ACQUIRE)
               : nullptr;
}

const Callbacks *callbacksFor(IOService *service) {
    const auto *callbacks = activeCallbacks();
    return callbacks && callbacks->matches(service) ? callbacks : nullptr;
}

IOFramebuffer *framebufferHook(IOService *service) {
    const auto *callbacks = callbacksFor(service);
    const auto framebuffer =
        callbacks ? callbacks->framebuffer : originals.framebuffer;
    return framebuffer(service);
}

bool stylusHook(IOService *service, AbsoluteTime timestamp, Report report) {
    const auto *callbacks = callbacksFor(service);
    const auto stylus = callbacks ? callbacks->stylus : originals.stylus;
    return stylus(service, timestamp, report);
}

void liftHook(IOService *service) {
    const auto *callbacks = callbacksFor(service);
    const auto lift = callbacks ? callbacks->lift : originals.lift;
    lift(service);
}

void reportHook(IOService *service, Report report, AbsoluteTime timestamp) {
    const auto *callbacks = callbacksFor(service);
    const auto forward = callbacks ? callbacks->report : originals.report;
    forward(service, report, timestamp);
}

bool startHook(IOService *service, IOService *provider) {
    const bool started = originals.start(service, provider);
    // The companion attaches only after native initialization succeeds.
    if (const auto *callbacks = activeCallbacks())
        callbacks->started(service, provider, started);
    return started;
}

void stopHook(IOService *service, IOService *provider) {
    originals.stop(service, provider);
    if (const auto *callbacks = activeCallbacks())
        callbacks->stopped(service);
}

void ptpHook(IOService *service) {
    originals.ptp(service);
    if (const auto *callbacks = activeCallbacks())
        callbacks->ptp(service);
}

void dispatchHook(IOService *service, AbsoluteTime timestamp, UInt32 id,
                  UInt32 type, bool range, UInt32 buttons, IOFixed x, IOFixed y,
                  IOFixed z, IOFixed pressure, IOFixed auxiliary, IOFixed twist,
                  IOFixed tx, IOFixed ty, IOOptionBits flags) {
    const auto *callbacks = callbacksFor(service);
    const auto dispatch = callbacks ? callbacks->dispatch : originals.dispatch;
    dispatch(service, timestamp, id, type, range, buttons, x, y, z, pressure,
             auxiliary, twist, tx, ty, flags);
}

bool touchscreenNub(IOService *nub) {
    auto *data = OSDynamicCast(OSData, nub->getProperty("name"));
    return data && panelForACPIName(data->getBytesNoCopy(), data->getLength());
}

IOReturn interruptTypeHook(IOService *nub, int source, int *type) {
    const auto result = FunctionCast(interruptTypeHook,
                                     originalInterruptType)(nub, source, type);
    if (result != kIOReturnSuccess || !type || !touchscreenNub(nub))
        return result;

    auto *irq = OSDynamicCast(OSNumber, nub->getProperty("gpioIRQ"));
    if (!irq)
        return result;

    // IOInterruptEventSource queries the touchscreen's trigger type before
    // VoodooGPIO fills its registration cache. Returning the ACPI type lets
    // macOS mask a level-triggered line during report reads, preventing
    // repeated IRQ callbacks from queuing redundant reads.
    constexpr uint32_t levelTriggerFlags = 0x0c;
    const int expected = (irq->unsigned32BitValue() & levelTriggerFlags)
                             ? kIOInterruptTypeLevel
                             : kIOInterruptTypeEdge;
    if (*type != expected) {
        *type = expected;
        __atomic_add_fetch(&irqCorrections, 1, __ATOMIC_RELAXED);
        scheduleDiagnostics();
    }
    return result;
}

// OSDynamicCast references VoodooGPIO's metaclass across kexts, where an
// invalid relocation can cause a page fault. Runtime metaCast avoids that
// symbol reference, following VoodooI2C PR #589.
// https://github.com/VoodooI2C/VoodooI2C/pull/589
IOService *gpioLookupHook(IOService *) {
    auto *match = IOService::serviceMatching("VoodooGPIO");
    if (!match)
        return nullptr;

    auto *candidate = IOService::waitForMatchingService(match, 1000000000);
    match->release();
    auto *result =
        candidate ? static_cast<IOService *>(candidate->metaCast("VoodooGPIO"))
                  : nullptr;
    OSSafeReleaseNULL(candidate);
    return result;
}

bool entryMatches(mach_vm_address_t entry, mach_vm_address_t base, size_t size,
                  const profile::Method &method) {
    const auto length = sizeof(method.entry);
    return entry >= base && size >= length && entry - base <= size - length &&
           !memcmp(reinterpret_cast<const void *>(entry), method.entry, length);
}

void coreLoaded(void *, KernelPatcher &patcher, size_t index,
                mach_vm_address_t base, size_t size) {
    if (index != core.loadIndex)
        return;

    if (!imageMatches(reinterpret_cast<const uint8_t *>(base), size,
                      profile::core_uuid)) {
        coreStatus = "unsupported-core-build";
        scheduleDiagnostics();
        return;
    }

    patcher.clearError();
    const auto interrupt =
        patcher.solveSymbol(index, profile::interruptType.symbol, base, size);
    const auto lookup =
        patcher.solveSymbol(index, profile::gpioLookup.symbol, base, size);
    if (entryMatches(interrupt, base, size, profile::interruptType) &&
        entryMatches(lookup, base, size, profile::gpioLookup)) {
        KernelPatcher::RouteRequest requests[] = {
            {profile::interruptType.symbol, interruptTypeHook,
             originalInterruptType},
            {profile::gpioLookup.symbol, gpioLookupHook},
        };
        coreReady = patcher.routeMultipleLong(index, requests,
                                              arrsize(requests), base, size);
        coreStatus = coreReady ? "stock-core-ready" : "IRQ-route-failed";
    } else {
        coreStatus = "unsupported-IRQ-entry";
    }

    coreError = static_cast<uint32_t>(patcher.getError());
    patcher.clearError();
    scheduleDiagnostics();
}

void hidLoaded(void *, KernelPatcher &patcher, size_t index,
               mach_vm_address_t base, size_t size) {
    if (index != hid.loadIndex)
        return;

    if (!coreReady) {
        status = "stock-core-not-ready";
        scheduleDiagnostics();
        return;
    }
    if (!imageMatches(reinterpret_cast<const uint8_t *>(base), size,
                      profile::hid_uuid)) {
        status = "unsupported-HID-build";
        scheduleDiagnostics();
        return;
    }

    patcher.clearError();
    const auto resolveRoute = [&](const profile::Method &method, auto hook,
                                  auto &original) {
        KernelPatcher::RouteRequest request{method.symbol, hook, original};
        const auto entry =
            patcher.solveSymbol(index, method.symbol, base, size);
        if (entryMatches(entry, base, size, method))
            request.from = entry;
        return request;
    };

    const auto report =
        resolveRoute(profile::forwardReport, reportHook, originals.report);
    const auto start = resolveRoute(profile::start, startHook, originals.start);
    const auto stop = resolveRoute(profile::stop, stopHook, originals.stop);
    KernelPatcher::RouteRequest requests[] = {
        resolveRoute(profile::framebuffer, framebufferHook,
                     originals.framebuffer),
        resolveRoute(profile::stylus, stylusHook, originals.stylus),
        resolveRoute(profile::fingerLift, liftHook, originals.lift),
        report,
        start,
        stop,
        resolveRoute(profile::precisionTouchpadMode, ptpHook, originals.ptp),
    };

    // A UUID match alone cannot tell us if a method has already been patched.
    for (const auto &request : requests) {
        if (!request.from) {
            status = "unsupported-HID-entry";
            hookError = static_cast<uint32_t>(patcher.getError());
            patcher.clearError();
            scheduleDiagnostics();
            return;
        }
    }

    const auto tableAddress =
        patcher.solveSymbol(index, profile::vtable_symbol, base, size);
    constexpr size_t tableBytes = profile::vtable_size;
    if (tableAddress < base || size < tableBytes ||
        tableAddress - base > size - tableBytes) {
        status = "unsupported-HID-vtable";
        patcher.clearError();
        scheduleDiagnostics();
        return;
    }
    // The vtable symbol starts with the offset-to-top and RTTI pointer.
    auto *table = reinterpret_cast<mach_vm_address_t *>(tableAddress) + 2;
    if (table[profile::start_slot] != start.from ||
        table[profile::stop_slot] != stop.from ||
        table[profile::report_slot] != report.from ||
        table[profile::dispatch_slot] < 0xffff000000000000ULL) {
        status = "unsupported-HID-vtable-layout";
        patcher.clearError();
        scheduleDiagnostics();
        return;
    }

    originals.dispatch =
        reinterpret_cast<Dispatch>(table[profile::dispatch_slot]);
    bool routed = patcher.routeMultipleLong(index, requests, arrsize(requests),
                                            base, size);
    hookError = static_cast<uint32_t>(patcher.getError());
    patcher.clearError();
    if (routed) {
        routed = MachInfo::setKernelWriting(
                     true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS;
        if (routed) {
            table[profile::dispatch_slot] =
                reinterpret_cast<mach_vm_address_t>(dispatchHook);
            routed = MachInfo::setKernelWriting(
                         false, KernelPatcher::kernelWriteLock) == KERN_SUCCESS;
        }
    }

    // Lilu can leave earlier routes installed when a later route fails.
    // Those hooks call their native trampolines until all routes are ready.
    __atomic_store_n(&originals.ready, routed, __ATOMIC_RELEASE);
    status = routed ? "stock-HID-ready" : "route-failed";
    scheduleDiagnostics();
}
} // namespace
} // namespace input

void registerInputRoutes() {
    auto error = lilu.onKextLoad(&input::core, 1, input::coreLoaded);
    input::coreStatus = error == LiluAPI::Error::NoError
                            ? "waiting-for-stock-core"
                            : "registration-failed";
    if (checkKernelArgument("-duolinktouchoff")) {
        input::status = "disabled";
        return;
    }
    error = lilu.onKextLoad(&input::hid, 1, input::hidLoaded);
    input::status = error == LiluAPI::Error::NoError ? "waiting-for-stock-HID"
                                                     : "registration-failed";
}

void publishInputRoutes(IOService *service) {
    service->setProperty("TouchRouteStatus", input::status);
    service->setProperty("TouchRouteError", input::hookError, 32);
    service->setProperty("TouchRoutesReady",
                         input::available(&input::originals));
    service->setProperty("TouchCallbacksRegistered",
                         __atomic_load_n(&input::registeredCallbacks,
                                         __ATOMIC_ACQUIRE) != nullptr);
    service->setProperty("InputCoreStatus", input::coreStatus);
    service->setProperty("InputCoreError", input::coreError, 32);
    service->setProperty(
        "GPIOTypeCorrections",
        __atomic_load_n(&input::irqCorrections, __ATOMIC_RELAXED), 32);
    service->setProperty("StockInputVersion", "2.9.1 RELEASE");
}

} // namespace duolink

extern "C" const duolink::input::Originals *
DuoLinkRegisterTouchCallbacks(const duolink::input::Callbacks *callbacks) {
    const auto *originals = duolink::input::bindCallbacks(callbacks);
    duolink::scheduleDiagnostics();
    return originals;
}
