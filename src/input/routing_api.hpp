/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <stddef.h>
class IOFramebuffer;
class OSArray;
namespace duolink {
namespace input {

// VoodooI2CHID 2.9.1 passes this 16-byte report in registers. Its transducer
// array is borrowed. Adding a field can change how the report is passed.
struct Report {
    uint8_t contact_count;
    OSArray *transducers;
};
static_assert(sizeof(Report) == 16, "Unexpected stock report ABI");
static_assert(offsetof(Report, transducers) == 8, "Unexpected report padding");

using Dispatch = void (*)(IOService *, AbsoluteTime, UInt32, UInt32, bool,
                          UInt32, IOFixed, IOFixed, IOFixed, IOFixed, IOFixed,
                          IOFixed, IOFixed, IOFixed, IOOptionBits);

// Check available() before calling through this table from outside a native
// hook. A hook can forward to its own trampoline during route installation.
struct Originals {
    IOFramebuffer *(*framebuffer)(IOService *){nullptr};
    bool (*stylus)(IOService *, AbsoluteTime, Report){nullptr};
    void (*lift)(IOService *){nullptr};
    void (*report)(IOService *, Report, AbsoluteTime){nullptr};
    bool (*start)(IOService *, IOService *){nullptr};
    void (*stop)(IOService *, IOService *){nullptr};
    void (*ptp)(IOService *){nullptr};
    Dispatch dispatch{nullptr};
    bool ready{false};
};

constexpr uint32_t callbackVersion = 2;

// The callback table must remain valid until reboot because installed
// hooks can call its functions after the companion service stops.
struct Callbacks {
    uint32_t version, size;
    bool (*matches)(IOService *);
    // Return a borrowed framebuffer. VoodooI2CHID retains its cache entry.
    IOFramebuffer *(*framebuffer)(IOService *);
    bool (*stylus)(IOService *, AbsoluteTime, Report);
    void (*lift)(IOService *);
    void (*report)(IOService *, Report, AbsoluteTime);
    void (*started)(IOService *, IOService *, bool);
    void (*stopped)(IOService *);
    void (*ptp)(IOService *);
    Dispatch dispatch;
};

inline bool available(const Originals *value) {
    return value && __atomic_load_n(&value->ready, __ATOMIC_ACQUIRE);
}
} // namespace input
} // namespace duolink
extern "C" const duolink::input::Originals *
DuoLinkRegisterTouchCallbacks(const duolink::input::Callbacks *callbacks);
