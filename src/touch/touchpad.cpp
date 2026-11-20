/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "duolink_touch.hpp"
#include "src/input/devices.hpp"
#include "src/input/native_hid.hpp"

#include <IOKit/IOLocks.h>
#include <IOKit/hid/IOHIDElement.h>
#include <IOKit/hid/IOHIDInterface.h>
#include <IOKit/hid/IOHIDUsageTables.h>

namespace duolink {
namespace touchpad {
namespace {
IORecursiveLock *featureLock;
constexpr UInt32 featureReport = 5;
constexpr UInt32 deviceConfiguration = 0x0e;
constexpr UInt32 surfaceSwitch = 0x57;
constexpr UInt32 buttonSwitch = 0x58;
} // namespace

bool initialize() {
    featureLock = IORecursiveLockAlloc();
    return featureLock != nullptr;
}

void releaseUnregisteredResources() {
    if (featureLock) {
        IORecursiveLockFree(featureLock);
        featureLock = nullptr;
    }
}

// Surface Switch and Button Switch enable touch and button reporting.
// Request both after ELAN1207 enters Precision Touchpad mode, following
// VoodooI2CHID PR #82.
// https://github.com/VoodooI2C/VoodooI2CHID/pull/82
void requestFeatures(IOService *driver) {
    auto *provider = driver ? driver->getProvider() : nullptr;
    auto *interface = OSDynamicCast(IOHIDInterface, provider);
    if (!interface ||
        !input::native::hasDigitizerIdentity(interface, input::touchpadVendor,
                                             input::touchpadProduct))
        return;

    IORecursiveLockLock(featureLock);
    auto *elements = interface->createMatchingElements();
    IOHIDElement *surface = nullptr;
    IOHIDElement *buttons = nullptr;
    if (elements) {
        for (unsigned i = 0; i < elements->getCount(); ++i) {
            auto *element = OSDynamicCast(IOHIDElement, elements->getObject(i));
            if (!element || element->getType() != kIOHIDElementTypeFeature ||
                element->getUsagePage() != kHIDPage_Digitizer ||
                element->getReportID() != featureReport ||
                element->getReportSize() != 1)
                continue;

            auto *parent = element->getParentElement();
            auto *configuration = parent ? parent->getParentElement() : nullptr;
            if (!parent || !configuration ||
                parent->getUsagePage() != kHIDPage_Digitizer ||
                parent->getUsage() != kHIDUsage_Dig_Finger ||
                configuration->getUsagePage() != kHIDPage_Digitizer ||
                configuration->getUsage() != deviceConfiguration)
                continue;

            if (element->getUsage() == surfaceSwitch)
                surface = element;
            if (element->getUsage() == buttonSwitch)
                buttons = element;
        }
    }

    if (surface && buttons) {
        const UInt8 one = 1;
        auto *value = OSData::withBytes(&one, sizeof(one));
        if (value) {
            surface->setDataValue(value);
            buttons->setDataValue(value);
            value->release();
            // setDataValue returns void. Record that both requests were sent.
            driver->setProperty("DuoLinkPTPFeatureFields", 2, 32);
            driver->setProperty("DuoLinkPTPStatus", "enable-requested");
        } else {
            driver->setProperty("DuoLinkPTPStatus", "allocation-failed");
        }
    } else {
        driver->setProperty("DuoLinkPTPStatus",
                            elements ? "unexpected-feature-layout"
                                     : "elements-unavailable");
    }

    OSSafeReleaseNULL(elements);
    IORecursiveLockUnlock(featureLock);
}

} // namespace touchpad
} // namespace duolink
