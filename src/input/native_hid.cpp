/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "native_hid.hpp"
#include "routing_api.hpp"
#include "stock_input_profile.hpp"

#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

namespace duolink {
namespace input {
namespace native {

bool hasTouchscreenLayout(IOService *driver) {
    return driver && driver->metaCast("VoodooI2CTouchscreenHIDEventDriver") &&
           driver->getMetaClass()->getClassSize() == profile::driver_size;
}

bool invalidateFramebuffer(IOService *driver, IOFramebuffer *selected) {
    return invalidateFramebufferCache<IOService>(
        driver, profile::framebuffer_offset, selected);
}

OSObject *currentPen(const Report &report, AbsoluteTime timestamp) {
    if (!report.transducers)
        return nullptr;
    for (unsigned i = 0; i < report.transducers->getCount(); ++i) {
        auto *pen = report.transducers->getObject(i);
        if (pen && pen->metaCast("VoodooI2CDigitiserStylus") &&
            pen->getMetaClass()->getClassSize() == profile::stylus_size &&
            readField<AbsoluteTime>(pen, profile::x_timestamp) == timestamp &&
            readField<AbsoluteTime>(pen, profile::y_timestamp) == timestamp)
            return pen;
    }
    return nullptr;
}

bool penInRange(const OSObject *pen) {
    return readField<bool>(pen, profile::in_range);
}

PenRanges::PenRanges(OSObject *pen)
    : z(pen, profile::z_value, profile::z_maximum),
      pressure(pen, profile::pressure_value, profile::pressure_maximum) {}

unsigned PenRanges::adjusted() const {
    return static_cast<unsigned>(z.changed()) + pressure.changed();
}

bool hasDigitizerIdentity(IOService *provider, uint32_t vendor,
                          uint32_t product) {
    if (!provider)
        return false;
    auto *vendorID = OSDynamicCast(OSNumber, provider->getProperty("VendorID"));
    auto *productID =
        OSDynamicCast(OSNumber, provider->getProperty("ProductID"));
    auto *transport =
        OSDynamicCast(OSString, provider->getProperty("Transport"));
    return vendorID && productID && transport && transport->isEqualTo("I2C") &&
           vendorID->unsigned32BitValue() == vendor &&
           productID->unsigned32BitValue() == product;
}

} // namespace native
} // namespace input
} // namespace duolink
