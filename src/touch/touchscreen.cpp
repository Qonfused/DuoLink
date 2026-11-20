/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "duolink_touch.hpp"
#include "display_mapping.hpp"
#include "src/input/devices.hpp"
#include "src/input/native_hid.hpp"

#include <IOKit/IOLocks.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/hid/AppleHIDUsageTables.h>
#include <IOKit/hid/IOHIDEvent.h>
#include <IOKit/hid/IOHIDEventService.h>
#include <IOKit/hidsystem/IOHIDSystem.h>
#include <IOKit/hidsystem/IOLLEvent.h>

namespace duolink {
namespace touchscreen {
namespace {
using namespace input;
using touch::originals;

struct PanelState {
    // Borrowed until the native stop callback.
    IOService *driver{nullptr};
    IORecursiveLock *lock{nullptr};
    // Retained independently of native HID.
    IOFramebuffer *framebuffer{nullptr};
    const PanelProfile *panel{nullptr};
    bool positionPending{true};
    bool penInRange{false};
    bool releasingFinger{false};
    Bounds bounds{};
    UInt64 repositions{0};
    UInt64 rangeRepairs{0};
    UInt64 penReports{0};
    UInt64 touchReports{0};
    Point lastPosition{};
};

PanelState panelStates[panelCount];

PanelState *findPanel(IOService *driver) {
    for (auto &state : panelStates)
        if (__atomic_load_n(&state.driver, __ATOMIC_ACQUIRE) == driver)
            return &state;
    return nullptr;
}

// Native report handling can reenter through the framebuffer or digitizer
// hooks. Use a recursive lock and check for detach after taking it.
struct PanelGuard {
    PanelState *state;
    explicit PanelGuard(IOService *driver) : state(findPanel(driver)) {
        if (state) {
            IORecursiveLockLock(state->lock);
            if (state->driver != driver) {
                IORecursiveLockUnlock(state->lock);
                state = nullptr;
            }
        }
    }
    ~PanelGuard() {
        if (state)
            IORecursiveLockUnlock(state->lock);
    }
    PanelGuard(const PanelGuard &) = delete;
    PanelGuard &operator=(const PanelGuard &) = delete;
};

IOFramebuffer *locateDisplay(PanelState &state) {
    auto *match = IOService::serviceMatching("IODisplay");
    if (!match)
        return nullptr;

    auto *iterator = IOService::getMatchingServices(match);
    match->release();
    if (!iterator)
        return nullptr;

    IOFramebuffer *result = nullptr;
    while (auto *display =
               OSDynamicCast(IORegistryEntry, iterator->getNextObject())) {
        auto *vendor =
            OSDynamicCast(OSNumber, display->getProperty("DisplayVendorID"));
        auto *product =
            OSDynamicCast(OSNumber, display->getProperty("DisplayProductID"));
        if (!vendor || !product ||
            vendor->unsigned32BitValue() != state.panel->displayVendor ||
            product->unsigned32BitValue() != state.panel->displayProduct)
            continue;

        auto *entry = display;
        IOFramebuffer *candidate = nullptr;
        for (unsigned i = 0; entry && i < 8; ++i) {
            candidate =
                static_cast<IOFramebuffer *>(entry->metaCast("IOFramebuffer"));
            if (candidate)
                break;
            entry = entry->getParentEntry(gIOServicePlane);
        }
        if (!candidate || candidate->isInactive() || candidate == result)
            continue;

        // Multiple display entries can refer to one framebuffer, but an EDID
        // match across two framebuffers is ambiguous.
        if (result) {
            result->release();
            result = nullptr;
            state.driver->setProperty("TouchMappingStatus",
                                      "ambiguous-display");
            break;
        }
        candidate->retain();
        result = candidate;
    }
    iterator->release();
    return result;
}

IOFramebuffer *mappedFramebuffer(PanelState &state) {
    auto *cached = state.framebuffer;
    if (cached && cached->isInactive()) {
        // A callback during release can look up the framebuffer again.
        state.framebuffer = nullptr;
        cached->release();
    }

    if (!state.framebuffer) {
        state.framebuffer = locateDisplay(state);
        state.positionPending = true;
        if (state.framebuffer)
            state.driver->setProperty("TouchMappingFramebuffer",
                                      state.framebuffer->getRegistryEntryID(),
                                      64);
    }
    return state.framebuffer;
}

void synchronizeFramebuffer(PanelState &state) {
    if (native::invalidateFramebuffer(state.driver, mappedFramebuffer(state)))
        state.positionPending = true;
}
} // namespace

bool initialize() {
    for (size_t i = 0; i < panelCount; ++i) {
        panelStates[i].panel = &panels[i];
        panelStates[i].lock = IORecursiveLockAlloc();
        if (!panelStates[i].lock) {
            releaseUnregisteredResources();
            return false;
        }
    }
    return true;
}

void releaseUnregisteredResources() {
    for (auto &state : panelStates) {
        if (state.lock) {
            IORecursiveLockFree(state.lock);
            state.lock = nullptr;
        }
    }
}

bool matches(IOService *driver) {
    return driver && findPanel(driver);
}

void attach(IOService *driver, IOService *provider, bool success) {
    if (!available(__atomic_load_n(&originals, __ATOMIC_ACQUIRE)) || !success ||
        !native::hasTouchscreenLayout(driver))
        return;

    PanelState *state = nullptr;
    for (auto &candidate : panelStates) {
        if (native::hasDigitizerIdentity(provider,
                                         candidate.panel->digitizerVendor,
                                         candidate.panel->digitizerProduct)) {
            state = &candidate;
            break;
        }
    }
    if (!state)
        return;

    IORecursiveLockLock(state->lock);
    if (!state->driver || state->driver == driver) {
        __atomic_store_n(&state->driver, driver, __ATOMIC_RELEASE);
        driver->setProperty("TouchMappingRole", state->panel->role);
        driver->setProperty("TouchMappingBackend", "stock-native-routes");
        driver->setProperty("TouchMappingRevision", 4, 32);
        synchronizeFramebuffer(*state);
    }
    IORecursiveLockUnlock(state->lock);
}

void detach(IOService *driver) {
    PanelGuard guard(driver);
    if (!guard.state)
        return;

    auto &state = *guard.state;
    auto *framebuffer = state.framebuffer;
    state.framebuffer = nullptr;
    __atomic_store_n(&state.driver, static_cast<IOService *>(nullptr),
                     __ATOMIC_RELEASE);
    state.positionPending = true;
    state.penInRange = false;
    state.releasingFinger = false;
    state.bounds = {};
    state.repositions = 0;
    state.rangeRepairs = 0;
    state.penReports = 0;
    state.touchReports = 0;
    state.lastPosition = {};

    if (framebuffer)
        framebuffer->release();
}

IOFramebuffer *framebuffer(IOService *driver) {
    PanelGuard guard(driver);
    return guard.state ? mappedFramebuffer(*guard.state)
                       : originals->framebuffer(driver);
}

void report(IOService *driver, Report event, AbsoluteTime timestamp) {
    PanelGuard guard(driver);
    if (!guard.state) {
        originals->report(driver, event, timestamp);
        return;
    }

    auto &state = *guard.state;
    synchronizeFramebuffer(state);
    if (native::currentPen(event, timestamp)) {
        ++state.penReports;
        // Hover reports can have zero contacts. Set one on this copy so
        // VoodooI2CHID takes its single-contact pen path.
        event.contact_count = 1;
    } else {
        ++state.touchReports;
        if (!event.contact_count)
            state.positionPending = true;
    }
    originals->report(driver, event, timestamp);

    const auto total = state.penReports + state.touchReports;
    if (total == 1 || !(total & 63)) {
        driver->setProperty("TouchMappingPenReports", state.penReports, 64);
        driver->setProperty("TouchMappingTouchReports", state.touchReports, 64);
        driver->setProperty("TouchMappingRangeRepairs", state.rangeRepairs, 64);
        driver->setProperty("TouchMappingRepositions", state.repositions, 64);
        driver->setProperty("TouchMappingLastX",
                            static_cast<UInt32>(state.lastPosition.x), 32);
        driver->setProperty("TouchMappingLastY",
                            static_cast<UInt32>(state.lastPosition.y), 32);
    }
}

bool stylus(IOService *driver, AbsoluteTime timestamp, Report event) {
    PanelGuard guard(driver);
    if (!guard.state)
        return originals->stylus(driver, timestamp, event);
    auto *pen = native::currentPen(event, timestamp);
    if (!pen)
        return false;

    auto &state = *guard.state;
    const bool inRange = native::penInRange(pen);
    if (!inRange || !state.penInRange)
        state.positionPending = true;
    state.penInRange = inRange;
    if (!inRange)
        return true;

    native::PenRanges ranges(pen);
    state.rangeRepairs += ranges.adjusted();
    return originals->stylus(driver, timestamp, event);
}

void lift(IOService *driver) {
    PanelGuard guard(driver);
    if (!guard.state) {
        originals->lift(driver);
        return;
    }

    // Native finger release can dispatch old coordinates. Suppress cursor
    // repositioning during that call so lifting a finger cannot move it
    // back to the previous contact; the next contact positions it anew.
    guard.state->releasingFinger = true;
    originals->lift(driver);
    guard.state->releasingFinger = false;
    guard.state->positionPending = true;
}

namespace {
struct EventAccess : IOHIDEventService {
    using IOHIDEventService::dispatchEvent;
};

void position(PanelState &state, AbsoluteTime timestamp, IOFixed x, IOFixed y) {
    auto *frame = mappedFramebuffer(state);
    if (!frame || frame->isInactive())
        return;
    IOGBounds *rectangle = nullptr;
    frame->getBoundingRect(&rectangle);
    if (!rectangle) {
        state.positionPending = true;
        return;
    }
    const Bounds bounds{rectangle->minx, rectangle->maxx, rectangle->miny,
                        rectangle->maxy};
    if (!validBounds(bounds))
        return;

    if (!sameBounds(state.bounds, bounds)) {
        state.bounds = bounds;
        state.positionPending = true;
        state.driver->setProperty("TouchMappingMinX",
                                  static_cast<UInt32>(bounds.minx), 32);
        state.driver->setProperty("TouchMappingMaxX",
                                  static_cast<UInt32>(bounds.maxx), 32);
        state.driver->setProperty("TouchMappingMinY",
                                  static_cast<UInt32>(bounds.miny), 32);
        state.driver->setProperty("TouchMappingMaxY",
                                  static_cast<UInt32>(bounds.maxy), 32);
    }
    if (!state.positionPending || state.releasingFinger)
        return;
    auto *system = IOHIDSystem::instance();
    if (!system)
        return;

    const auto point = mapPosition(x, y, bounds);
    NXEventExt native{};
    static_assert(sizeof(native) == 124, "Unexpected native NX event layout");
    native.payload.type = NX_MOUSEMOVED;
    native.payload.location.x = point.x;
    native.payload.location.y = point.y;
    UInt64 nanoseconds = 0;
    absolutetime_to_nanoseconds(timestamp, &nanoseconds);
    native.payload.time = nanoseconds;
    native.payload.service_id = state.driver->getRegistryEntryID();
    native.payload.flags =
        static_cast<SInt32>(system->IOHIDSystem::eventFlags());
    auto *event = IOHIDEvent::vendorDefinedEvent(
        timestamp, kHIDPage_AppleVendor, kHIDUsage_AppleVendor_NXEvent, 0,
        reinterpret_cast<UInt8 *>(&native), sizeof(native));
    if (!event)
        return;
    // Name the protected method through EventAccess and call it on the
    // existing IOHIDEventService instance.
    void (IOHIDEventService::*send)(IOHIDEvent *, IOOptionBits) =
        &EventAccess::dispatchEvent;
    (static_cast<IOHIDEventService *>(state.driver)->*send)(event, 0);
    event->release();
    state.positionPending = false;
    state.lastPosition = point;
    ++state.repositions;
    state.driver->setProperty("TouchMappingStatus", "positioned");
}
} // namespace

void dispatch(IOService *driver, AbsoluteTime time, UInt32 id, UInt32 type,
              bool range, UInt32 buttons, IOFixed x, IOFixed y, IOFixed z,
              IOFixed pressure, IOFixed auxiliary, IOFixed twist, IOFixed tx,
              IOFixed ty, IOOptionBits flags) {
    PanelGuard guard(driver);
    if (guard.state)
        position(*guard.state, time, x, y);

    // macOS uses the cursor's display for native digitizer dispatch.
    originals->dispatch(driver, time, id, type, range, buttons, x, y, z,
                        pressure, auxiliary, twist, tx, ty, flags);
}

} // namespace touchscreen
} // namespace duolink
