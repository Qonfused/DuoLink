/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "src/dock/dock_patch.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace duolink;

int main(int argc, char **argv) {
    if (argc != 5)
        return 1;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), {});
    const auto textOffset = std::strtoull(argv[2], nullptr, 0);
    const auto textSize = std::strtoull(argv[3], nullptr, 0);
    if (!input || textOffset > data.size() ||
        textSize > data.size() - textOffset)
        return 1;
    const auto branch =
        dock::placementBranch(data.data() + textOffset, textSize);
    if (!branch || *branch != 0x75) {
        std::cerr << "Expected exactly one original Dock placement branch\n";
        return 1;
    }
    const auto offset = static_cast<size_t>(branch - data.data());
    const auto pageOffset = offset & ~(dock::pageSize - 1);
    if (dock::pageSize > data.size() - pageOffset)
        return 1;
    std::array<uint8_t, dock::pageSize> page;
    std::copy_n(data.data() + pageOffset, page.size(), page.data());
    const auto original = page;
    if (dock::patchPage(page.data(), page.size()) != PatchResult::Applied)
        return 1;
    auto expected = original;
    expected[offset - pageOffset] = 0xeb;
    if (page != expected ||
        dock::patchPage(page.data(), page.size()) !=
            PatchResult::AlreadyApplied ||
        page != expected)
        return 1;

    // Changing the error branch's target breaks the shared fallback.
    // The matcher must reject this candidate without changing its bytes.
    auto altered = original;
    altered[offset - pageOffset + 1] ^= 1;
    const auto invalid = altered;
    if (dock::patchPage(altered.data(), altered.size()) !=
            PatchResult::NoMatch ||
        altered != invalid)
        return 1;

    // Two placements in one buffer are ambiguous, even with valid branches.
    std::vector<uint8_t> duplicate(original.begin(), original.end());
    duplicate.insert(duplicate.end(), original.begin(), original.end());
    if (dock::placementBranch(duplicate.data(), duplicate.size()))
        return 1;
    for (size_t length = 0; length < 35; ++length) {
        if (dock::placementBranch(original.data(), length))
            return 1;
    }
    if (dock::patchPage(nullptr, dock::pageSize) != PatchResult::NoMatch)
        return 1;

    std::copy(page.begin(), page.end(), data.begin() + pageOffset);
    std::ofstream output(argv[4], std::ios::binary);
    output.write(reinterpret_cast<const char *>(data.data()), data.size());
    if (!output)
        return 1;
    std::cout << offset << "\n";
    return 0;
}
