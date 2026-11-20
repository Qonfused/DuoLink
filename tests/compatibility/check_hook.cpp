/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "src/patch.hpp"
#include "src/screenpad/kern_assr.hpp"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    using namespace duolink::assr;

    if (argc == 2 && std::strcmp(argv[1], "--symbols") == 0) {
        std::puts(writeAUXSymbol);
        std::puts(readAUXSymbol);
        std::puts(readEDIDSymbol);
        std::puts(readRegister32Symbol);
        return 0;
    }

    uint8_t entry[32];
    const auto size = std::fread(entry, 1, sizeof(entry), stdin);
    return duolink::canCopyKernelPrologue(entry, size) ? 0 : 1;
}
