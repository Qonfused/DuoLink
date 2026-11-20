/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "duolink_touch.hpp"
#include <libkern/c++/OSString.h>

OSDefineMetaClassAndStructors(DuoLinkTouch, IOService);

namespace duolink {
namespace touch {
const input::Originals *originals;
} // namespace touch
} // namespace duolink

namespace {
using namespace duolink;
using touch::originals;

void initializeTouchpad(IOService *driver) {
    if (input::available(__atomic_load_n(&originals, __ATOMIC_ACQUIRE)))
        touchpad::requestFeatures(driver);
}

bool touchscreenMatched(void *, void *, IOService *driver, IONotifier *) {
    touchscreen::attach(driver, driver->getProvider(), true);
    return true;
}

bool touchpadMatched(void *, void *, IOService *driver, IONotifier *) {
    auto *status =
        OSDynamicCast(OSString, driver->getProperty("DuoLinkPTPStatus"));
    if (!status || !status->isEqualTo("enable-requested"))
        initializeTouchpad(driver);
    return true;
}

IONotifier *
watchForDriverMatches(const char *className,
                      IOServiceMatchingNotificationHandler callback) {
    auto *matching = IOService::serviceMatching(className);
    if (!matching)
        return nullptr;
    auto *notifier = IOService::addMatchingNotification(
        gIOFirstMatchNotification, matching, callback, nullptr, nullptr);
    // The notifier retains its own reference to the matching dictionary.
    matching->release();
    return notifier;
}
} // namespace

bool DuoLinkTouch::registerInputCallbacks() {
    if (!touchscreen::initialize()) {
        IOLog("DuoLinkTouch: panel lock allocation failed\n");
        return false;
    }
    if (!touchpad::initialize()) {
        IOLog("DuoLinkTouch: touchpad lock allocation failed\n");
        touchscreen::releaseUnregisteredResources();
        return false;
    }

    // Routes remain installed when the resource service stops, so callbacks
    // and their locks must stay valid for the rest of the boot.
    static const input::Callbacks callbacks{
        input::callbackVersion, sizeof(input::Callbacks),
        touchscreen::matches,   touchscreen::framebuffer,
        touchscreen::stylus,    touchscreen::lift,
        touchscreen::report,    touchscreen::attach,
        touchscreen::detach,    initializeTouchpad,
        touchscreen::dispatch,
    };
    const auto *routes = DuoLinkRegisterTouchCallbacks(&callbacks);
    if (!routes) {
        IOLog("DuoLinkTouch: callback registration rejected\n");
        touchpad::releaseUnregisteredResources();
        touchscreen::releaseUnregisteredResources();
        return false;
    }
    __atomic_store_n(&originals, routes, __ATOMIC_RELEASE);
    return true;
}

bool DuoLinkTouch::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;

    setProperty("VersionInfo", KEXT_VERSION);
    setProperty("TouchMappingBackend", "stock-native-routes");
    setProperty("CallbacksRegistered", originals != nullptr);

    // First-match notifications also report drivers that matched before
    // callback registration.
    touchscreenNotifier = watchForDriverMatches(
        "VoodooI2CTouchscreenHIDEventDriver", touchscreenMatched);
    touchpadNotifier = watchForDriverMatches(
        "VoodooI2CPrecisionTouchpadHIDEventDriver", touchpadMatched);
    setProperty("TouchscreenNotifierRegistered",
                touchscreenNotifier != nullptr);
    setProperty("TouchpadNotifierRegistered", touchpadNotifier != nullptr);
    const bool complete = touchscreenNotifier && touchpadNotifier;
    setProperty("NotificationStatus",
                complete ? "registered" : "registration-failed");
    if (!complete)
        IOLog("DuoLinkTouch: native driver notification registration failed\n");

    registerService();
    return true;
}

void DuoLinkTouch::stop(IOService *provider) {
    if (touchscreenNotifier) {
        touchscreenNotifier->remove();
        touchscreenNotifier = nullptr;
    }
    if (touchpadNotifier) {
        touchpadNotifier->remove();
        touchpadNotifier = nullptr;
    }

    IOService::stop(provider);
}
