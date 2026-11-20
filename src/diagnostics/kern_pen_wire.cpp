/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "diagnostics.hpp"

#include "src/input/devices.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOService.h>
#include <kern/clock.h>

#include "kern_pen_wire.hpp"
#include "pen_wire.hpp"

namespace duolink {
namespace {

const char *corePaths[] = {
    "/Library/Extensions/VoodooI2C.kext/Contents/MacOS/VoodooI2C",
    "/System/Library/Extensions/VoodooI2C.kext/Contents/MacOS/VoodooI2C",
};

KernelPatcher::KextInfo core{
    "com.alexandred.VoodooI2C",        corePaths, 2, {true}, {},
    KernelPatcher::KextInfo::Unloaded,
};

const char *status = "not-registered";
uint32_t hookError;
mach_vm_address_t originalReadI2C;

struct Counters {
    uint32_t reads, readErrors, touch, pen, shiftedPen, fill, invalid, reset,
        other;
    uint32_t lastLength, lastShiftedOffset, lastError;
};

Counters mainPanel{}, screenPad{};
uint64_t nextDiagnosticUpdate{0};

uint32_t increment(uint32_t &value) {
    return __atomic_add_fetch(&value, 1, __ATOMIC_RELAXED);
}

Counters *selectPanel(IORegistryEntry *nub) {
    auto *name = OSDynamicCast(OSData, nub->getProperty("name"));
    if (!name)
        return nullptr;
    const auto *panel =
        input::panelForACPIName(name->getBytesNoCopy(), name->getLength());
    if (!panel)
        return nullptr;
    return panel->digitizerProduct == input::panels[0].digitizerProduct
               ? &mainPanel
               : &screenPad;
}

void requestDiagnosticUpdate() {
    constexpr uint64_t publicationIntervalNs = 250000000;
    AbsoluteTime now;
    clock_get_uptime(&now);
    uint64_t nanoseconds;
    absolutetime_to_nanoseconds(now, &nanoseconds);

    // Only one panel may claim each 250 ms refresh interval.
    auto next = __atomic_load_n(&nextDiagnosticUpdate, __ATOMIC_RELAXED);
    if (nanoseconds >= next &&
        __atomic_compare_exchange_n(&nextDiagnosticUpdate, &next,
                                    nanoseconds + publicationIntervalNs, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        scheduleDiagnostics();
}

IOReturn readHook(IORegistryEntry *nub, uint8_t *buffer, uint16_t length) {
    const IOReturn result =
        FunctionCast(readHook, originalReadI2C)(nub, buffer, length);
    if (!nub || length != pen_wire::inputReadSize)
        return result;

    auto *counters = selectPanel(nub);
    if (!counters)
        return result;

    increment(counters->reads);
    if (result != kIOReturnSuccess) {
        increment(counters->readErrors);
        __atomic_store_n(&counters->lastError, static_cast<uint32_t>(result),
                         __ATOMIC_RELAXED);
        requestDiagnosticUpdate();
        return result;
    }

    const auto frame = pen_wire::inspect(buffer, length);
    using pen_wire::Kind;
    __atomic_store_n(&counters->lastLength, frame.length, __ATOMIC_RELAXED);
    switch (frame.kind) {
    case Kind::Touch:
        increment(counters->touch);
        break;
    case Kind::Pen:
        increment(counters->pen);
        break;
    case Kind::ShiftedPen:
        increment(counters->shiftedPen);
        __atomic_store_n(&counters->lastShiftedOffset, frame.offset,
                         __ATOMIC_RELAXED);
        break;
    case Kind::Fill:
        increment(counters->fill);
        break;
    case Kind::Invalid:
        increment(counters->invalid);
        break;
    case Kind::Reset:
        increment(counters->reset);
        break;
    case Kind::Other:
        increment(counters->other);
        break;
    }
    requestDiagnosticUpdate();
    return result;
}

void coreLoaded(void *, KernelPatcher &patcher, size_t index,
                mach_vm_address_t address, size_t size) {
    if (index != core.loadIndex)
        return;

    constexpr const char *symbol = "__ZN18VoodooI2CDeviceNub7readI2CEPht";
    patcher.clearError();
    const auto entry = patcher.solveSymbol(index, symbol, address, size);
    if (!entry) {
        status = "read-symbol-unavailable";
    } else if (entry < address || size < 15 || entry - address > size - 15 ||
               !pen_wire::safeReadEntry(
                   reinterpret_cast<const uint8_t *>(entry),
                   size - (entry - address))) {
        status = "unsupported-read-entry";
    } else {
        KernelPatcher::RouteRequest request{symbol, readHook, originalReadI2C};
        status = patcher.routeMultipleLong(index, &request, 1, address, size)
                     ? "ready"
                     : "route-failed";
    }

    hookError = static_cast<uint32_t>(patcher.getError());
    patcher.clearError();
    scheduleDiagnostics();
}

void publishPanel(IOService *service, const char *name,
                  const Counters &counters) {
    struct Property {
        const char *name;
        uint32_t value;
    };
    const Property properties[] = {
        {"Reads", __atomic_load_n(&counters.reads, __ATOMIC_RELAXED)},
        {"ReadErrors", __atomic_load_n(&counters.readErrors, __ATOMIC_RELAXED)},
        {"Touch", __atomic_load_n(&counters.touch, __ATOMIC_RELAXED)},
        {"Pen", __atomic_load_n(&counters.pen, __ATOMIC_RELAXED)},
        {"ShiftedPen", __atomic_load_n(&counters.shiftedPen, __ATOMIC_RELAXED)},
        {"Fill", __atomic_load_n(&counters.fill, __ATOMIC_RELAXED)},
        {"Invalid", __atomic_load_n(&counters.invalid, __ATOMIC_RELAXED)},
        {"Reset", __atomic_load_n(&counters.reset, __ATOMIC_RELAXED)},
        {"Other", __atomic_load_n(&counters.other, __ATOMIC_RELAXED)},
        {"LastLength", __atomic_load_n(&counters.lastLength, __ATOMIC_RELAXED)},
        {"LastShiftedOffset",
         __atomic_load_n(&counters.lastShiftedOffset, __ATOMIC_RELAXED)},
        {"LastError", __atomic_load_n(&counters.lastError, __ATOMIC_RELAXED)},
    };
    auto *values =
        OSDictionary::withCapacity(sizeof(properties) / sizeof(properties[0]));
    if (!values)
        return;

    for (const auto &property : properties) {
        auto *value = OSNumber::withNumber(property.value, 32);
        if (value) {
            values->setObject(property.name, value);
            value->release();
        }
    }
    service->setProperty(name, values);
    values->release();
}

} // namespace

void registerPenWireProbe() {
    if (!checkKernelArgument("-duolinkpenwire") ||
        checkKernelArgument("-duolinkpenwireoff")) {
        status = "disabled";
        return;
    }

    const auto error = lilu.onKextLoad(&core, 1, coreLoaded);
    hookError = static_cast<uint32_t>(error);
    status = error == LiluAPI::Error::NoError ? "waiting-for-VoodooI2C"
                                              : "registration-failed";
}

void publishPenWireProbe(IOService *service) {
    service->setProperty("PenWireStatus", status);
    service->setProperty("PenWireHookError", hookError, 32);
    publishPanel(service, "PenWireMain", mainPanel);
    publishPanel(service, "PenWireScreenPad", screenPad);
}

} // namespace duolink
