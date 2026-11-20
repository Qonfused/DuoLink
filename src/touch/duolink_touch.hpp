/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include "src/input/routing_api.hpp"

// Registers for matching notifications to discover drivers that have already
// started. Native hooks can still call the companion after this service stops.
class __attribute__((visibility("default"))) DuoLinkTouch : public IOService {
    OSDeclareDefaultStructors(DuoLinkTouch);

public:
    static bool registerInputCallbacks();
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

private:
    IONotifier *touchscreenNotifier{nullptr};
    IONotifier *touchpadNotifier{nullptr};
};

namespace duolink {
namespace touch {
// Set when DuoLink accepts the companion's callback table.
extern const input::Originals *originals;
} // namespace touch

namespace touchscreen {

bool initialize();

// Use during startup failure. Once registration succeeds, hooks can still
// take these locks after the service stops.
void releaseUnregisteredResources();

bool matches(IOService *driver);
void attach(IOService *driver, IOService *provider, bool success);
void detach(IOService *driver);
IOFramebuffer *framebuffer(IOService *driver);
bool stylus(IOService *driver, AbsoluteTime timestamp, input::Report report);
void lift(IOService *driver);
void report(IOService *driver, input::Report report, AbsoluteTime timestamp);
void dispatch(IOService *driver, AbsoluteTime timestamp, UInt32 id, UInt32 type,
              bool inRange, UInt32 buttons, IOFixed x, IOFixed y, IOFixed z,
              IOFixed pressure, IOFixed auxiliary, IOFixed twist, IOFixed tiltX,
              IOFixed tiltY, IOOptionBits flags);

} // namespace touchscreen

namespace touchpad {
bool initialize();
void releaseUnregisteredResources();
void requestFeatures(IOService *driver);
} // namespace touchpad
} // namespace duolink
