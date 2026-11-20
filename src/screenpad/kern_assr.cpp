/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <libkern/version.h>
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IORegistryEntry.h>
#include <libkern/OSAtomic.h>

#include "assr.hpp"
#include "kern_assr.hpp"
#include "src/diagnostics/diagnostics.hpp"

static const char *driverPaths[] = {
    "/System/Library/Extensions/AppleIntelCFLGraphicsFramebuffer.kext/Contents/"
    "MacOS/AppleIntelCFLGraphicsFramebuffer",
};

static KernelPatcher::KextInfo framebuffer{
    "com.apple.driver.AppleIntelCFLGraphicsFramebuffer",
    driverPaths,
    1,
    {true},
    {},
    KernelPatcher::KextInfo::Unloaded,
};

namespace {

using TransferAUX = IOReturn (*)(void *, IORegistryEntry *, uint32_t, uint16_t,
                                 void *, void *);
using ReadEDID = IOReturn (*)(void *, IORegistryEntry *, uint8_t, uint32_t,
                              uint16_t, uint8_t *, void *);
using ReadRegister32 = uint32_t (*)(void *, unsigned long);

TransferAUX readAUX;
mach_vm_address_t originalWriteAUX;
ReadEDID readEDID;
ReadRegister32 readRegister32;

struct PanelAUX {
    void *controller;
    IORegistryEntry *framebuffer;
    void *path;
    IOReturn lastError{};

    bool read(uint32_t address, uint8_t &byte) {
        lastError = readAUX(controller, framebuffer, address, 1, &byte, path);
        return lastError == kIOReturnSuccess;
    }

    bool readEDID(uint8_t (&edid)[128]) {
        // Match fbGetDDCBlock's DisplayPort EDID call for base block zero.
        lastError = ::readEDID(controller, framebuffer, 0x50, 0, sizeof(edid),
                               edid, path);
        return lastError == kIOReturnSuccess;
    }

    bool write(uint32_t address, uint8_t byte) {
        lastError = reinterpret_cast<TransferAUX>(originalWriteAUX)(
            controller, framebuffer, address, 1, &byte, path);
        return lastError == kIOReturnSuccess;
    }
};

const char *panelStatus(duolink::assr::Result result) {
    using duolink::assr::Result;
    switch (result) {
    case Result::CapabilitiesReadFailed:
        return "ASSR-capability-read-failed";
    case Result::UnsupportedPanel:
        return "ASSR-not-supported-by-panel";
    case Result::ConfigReadFailed:
        return "ASSR-config-read-failed";
    case Result::AlreadyEnabled:
        return "ASSR-already-enabled";
    case Result::EDIDReadFailed:
        return "ASSR-EDID-read-failed";
    case Result::DifferentPanel:
        return "ASSR-panel-ID-mismatch";
    case Result::WriteFailed:
        return "ASSR-write-failed";
    case Result::ReadbackFailed:
        return "ASSR-readback-failed";
    case Result::Enabled:
        return "ASSR-enabled";
    }
    return "ASSR-unknown-result";
}

IOReturn writeAUXHook(void *controller, IORegistryEntry *fb, uint32_t address,
                      uint16_t length, void *buffer, void *path) {
    const bool startsTraining =
        duolink::assr::startsLinkTraining(address, length, buffer);
    const auto original = reinterpret_cast<TransferAUX>(originalWriteAUX);
    const IOReturn result =
        original(controller, fb, address, length, buffer, path);
    if (!startsTraining || result != kIOReturnSuccess || !fb || !path ||
        !controller)
        return result;

    // FB1 drives ScreenPad Plus on DDI B. Check the transmitter before
    // changing the receiver's scrambler setting.
    auto &d = duolink::diagnostics;
    OSIncrementAtomic(
        reinterpret_cast<volatile SInt32 *>(&d.ASSRTrainingCalls));
    const auto index =
        OSDynamicCast(OSNumber, fb->getProperty("IOFBDependentIndex"));
    if (!index || index->unsigned32BitValue() != 1) {
        duolink::scheduleDiagnostics();
        return result;
    }

    OSIncrementAtomic(
        reinterpret_cast<volatile SInt32 *>(&d.ASSRFB1TrainingCalls));
    const uint32_t txControl =
        readRegister32(controller, duolink::assr::ddiBTransportControl);
    d.ASSRLastTXControl = txControl;
    if (!duolink::assr::sourceUsesASSR(txControl)) {
        d.status = "ASSR-source-disabled";
        duolink::scheduleDiagnostics();
        return result;
    }

    PanelAUX aux{controller, fb, path};
    const auto panel = duolink::assr::enablePanelASSR(aux);
    d.ASSRLastAUXError = static_cast<uint32_t>(aux.lastError);
    d.status = panelStatus(panel);
    if (panel == duolink::assr::Result::Enabled) {
        OSIncrementAtomic(
            reinterpret_cast<volatile SInt32 *>(&d.ASSRVerifiedWrites));
        SYSLOG("duolink", "enabled ASSR on FB1/BOE087F, readback verified");
    }
    duolink::scheduleDiagnostics();
    return result;
}

} // namespace

static void driverLoaded(void *, KernelPatcher &patcher, size_t index,
                         mach_vm_address_t address, size_t size) {
    if (index != framebuffer.loadIndex)
        return;

    auto &d = duolink::diagnostics;
    ++d.callbacks;
    d.imageSize = size;
    d.imageMagic =
        address && size >= 4
            ? duolink::read32(reinterpret_cast<const uint8_t *>(address))
            : 0;

    using namespace duolink::assr;

    patcher.clearError();
    readAUX =
        patcher.solveSymbol<TransferAUX>(index, readAUXSymbol, address, size);
    readEDID =
        patcher.solveSymbol<ReadEDID>(index, readEDIDSymbol, address, size);
    readRegister32 = patcher.solveSymbol<ReadRegister32>(
        index, readRegister32Symbol, address, size);
    const auto entry =
        patcher.solveSymbol(index, writeAUXSymbol, address, size);
    d.patchError = static_cast<uint32_t>(patcher.getError());
    patcher.clearError();
    if (!readAUX || !readEDID || !readRegister32 || !entry) {
        duolink::setStatus("ASSR-symbol-unavailable");
        return;
    }

    if (!duolink::canCopyKernelPrologue(
            reinterpret_cast<const uint8_t *>(entry),
            size - (entry - address))) {
        duolink::setStatus("ASSR-unsupported-WriteAUX-entry");
        return;
    }

    KernelPatcher::RouteRequest route{writeAUXSymbol, writeAUXHook,
                                      originalWriteAUX};
    const bool routed =
        patcher.routeMultipleLong(index, &route, 1, address, size);
    d.patchError = static_cast<uint32_t>(patcher.getError());
    patcher.clearError();
    duolink::setStatus(routed ? "ASSR-hook-ready" : "ASSR-hook-failed");
}

void duolink::registerASSRPatch() {
    if (checkKernelArgument("-duolinkassroff")) {
        duolink::setStatus("ASSR-disabled");
        return;
    }
    const auto error = lilu.onKextLoad(&framebuffer, 1, driverLoaded);
    duolink::diagnostics.registrationError = static_cast<uint32_t>(error);
    if (error != LiluAPI::Error::NoError) {
        duolink::setStatus("callback-registration-failed");
        SYSLOG("duolink", "failed to register framebuffer callback: %d",
               static_cast<int>(error));
        return;
    }
    duolink::setStatus("waiting-for-framebuffer");
    SYSLOG("duolink", "registered CFL framebuffer callback (FB1 ASSR)");
}
