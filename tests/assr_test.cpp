/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "src/screenpad/assr.hpp"

#include <cassert>
#include <cstring>

using namespace duolink::assr;

struct Panel {
    uint8_t capabilities = 0x0b; // BOE087F capability byte on the UX481FL.
    uint8_t configuration = 0;
    bool hasScreenPadIdentity = true;
    bool edidReadable = true;
    bool validChecksum = true;
    bool acceptsWrite = true;
    bool retainsWrite = true;
    bool readbackFails = false;
    uint32_t unreadableRegister = UINT32_MAX;
    unsigned writes = 0;

    bool read(uint32_t address, uint8_t &byte) const {
        if (address == unreadableRegister)
            return false;
        if (address == 0x00d) {
            byte = capabilities;
            return true;
        }
        assert(address == 0x10a);
        if (writes && readbackFails)
            return false;
        byte = configuration;
        return true;
    }

    bool readEDID(uint8_t (&edid)[128]) {
        if (!edidReadable)
            return false;
        constexpr uint8_t identity[] = {0,   255, 255,  255,  255,  255,
                                        255, 0,   0x09, 0xe5, 0x7f, 0x08};
        std::memcpy(edid, identity, sizeof(identity));
        if (!hasScreenPadIdentity)
            edid[10] ^= 1;
        for (size_t i = 0; i < 127; ++i)
            edid[127] -= edid[i];
        if (!validChecksum)
            ++edid[127];
        return true;
    }

    bool write(uint32_t address, uint8_t byte) {
        assert(address == 0x10a);
        ++writes;
        if (!acceptsWrite)
            return false;
        if (retainsWrite)
            configuration = byte;
        return true;
    }
};

static void testPanelRetraining() {
    Panel panel;
    assert(enablePanelASSR(panel) == Result::Enabled);
    assert(panel.configuration == 0x01 && panel.writes == 1);
    assert(enablePanelASSR(panel) == Result::AlreadyEnabled);
    assert(panel.writes == 1);

    // The panel loses the enable bit between link trainings. The remaining
    // configuration bits belong to the panel and must survive the new write.
    panel.configuration = 0xfe;
    assert(enablePanelASSR(panel) == Result::Enabled);
    assert(panel.configuration == 0xff && panel.writes == 2);
}

static void testPanelIdentification() {
    Panel unsupported;
    unsupported.capabilities = 0x0a;
    assert(enablePanelASSR(unsupported) == Result::UnsupportedPanel);
    assert(unsupported.writes == 0);

    Panel different;
    different.hasScreenPadIdentity = false;
    assert(enablePanelASSR(different) == Result::DifferentPanel);
    assert(different.writes == 0);

    Panel corrupt;
    corrupt.validChecksum = false;
    assert(enablePanelASSR(corrupt) == Result::DifferentPanel);
    assert(corrupt.writes == 0);
}

static void testAUXFailures() {
    Panel panel;
    panel.unreadableRegister = 0x00d;
    assert(enablePanelASSR(panel) == Result::CapabilitiesReadFailed);
    panel.unreadableRegister = 0x10a;
    assert(enablePanelASSR(panel) == Result::ConfigReadFailed);
    panel.unreadableRegister = UINT32_MAX;
    panel.edidReadable = false;
    assert(enablePanelASSR(panel) == Result::EDIDReadFailed);
    assert(panel.writes == 0);

    Panel rejected;
    rejected.acceptsWrite = false;
    assert(enablePanelASSR(rejected) == Result::WriteFailed);
    assert(rejected.writes == 1 && rejected.configuration == 0);

    Panel unchanged;
    unchanged.retainsWrite = false;
    assert(enablePanelASSR(unchanged) == Result::ReadbackFailed);

    Panel unreadable;
    unreadable.readbackFails = true;
    assert(enablePanelASSR(unreadable) == Result::ReadbackFailed);
}

int main() {
    testPanelRetraining();
    testPanelIdentification();
    testAUXFailures();
}
