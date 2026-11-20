/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "src/display/display_patch.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>

using namespace duolink;

static std::vector<uint8_t> readFile(const char *path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {(std::istreambuf_iterator<char>(input)), {}};
}

static PatchResult applyPage(uint8_t *page, size_t size) {
    display::Match match;
    if (!display::findModeCheck(page, size, match))
        return PatchResult::NoMatch;
    return display::patchPage(page, size, match);
}

int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    const auto original = readFile(argv[1]);
    if (argc == 2) {
        std::array<uint8_t, 4096> page{};
        assert(original.size() < page.size());
        std::copy(original.begin(), original.end(), page.begin());
        const auto saved = page;
        assert(applyPage(page.data(), page.size()) == PatchResult::NoMatch);
        assert(page == saved);
        return 0;
    }
    const auto replacement = readFile(argv[2]);
    assert(original.size() == replacement.size());
    if (original.size() == 4096) {
        auto page = original;
        const auto result = applyPage(page.data(), page.size());
        assert(result == PatchResult::Applied ||
               result == PatchResult::AlreadyApplied);
        assert(page == replacement);
        return 0;
    }
    const display::ModeCheck *check = nullptr;
    for (const auto &candidate : display::modeChecks)
        if (candidate.size == original.size())
            check = &candidate;
    assert(check);

    for (const size_t offset :
         {size_t{0}, size_t{1}, size_t{37}, 4096 - original.size()}) {
        std::array<uint8_t, 4096> page;
        page.fill(0x90);
        std::copy(original.begin(), original.end(), page.begin() + offset);
        const auto before = page;
        auto expected = page;
        std::copy(replacement.begin(), replacement.end(),
                  expected.begin() + offset);
        display::Match match;
        assert(display::findModeCheck(page.data(), page.size(), match));
        assert(match.offset == offset);
        assert(applyPage(page.data(), page.size()) == PatchResult::Applied);
        assert(page == expected);
        assert(applyPage(page.data(), page.size()) ==
               PatchResult::AlreadyApplied);
        assert(page == expected);

        // A second valid block makes the page ambiguous, even if patched.
        if (offset == 0) {
            for (const auto &second : {original, replacement}) {
                auto duplicate = before;
                std::copy(second.begin(), second.end(),
                          duplicate.begin() + 512);
                const auto saved = duplicate;
                assert(applyPage(duplicate.data(), duplicate.size()) ==
                       PatchResult::NoMatch);
                assert(duplicate == saved);
            }
        }
    }

    // All relative branch forms must be recognized, including those after
    // returns or inline data that cannot be reached by fallthrough decoding.
    std::vector<std::vector<uint8_t>> branches = {
        {0xe9, 0, 0, 0, 0}, {0xe8, 0, 0, 0, 0},       {0xeb, 0},
        {0x75, 0},          {0x0f, 0x85, 0, 0, 0, 0}, {0xe2, 0},
    };
    for (uint8_t opcode = 0x70; opcode <= 0x7f; ++opcode)
        branches.push_back({opcode, 0});
    for (uint8_t opcode = 0x80; opcode <= 0x8f; ++opcode)
        branches.push_back({0x0f, opcode, 0, 0, 0, 0});
    for (uint8_t opcode = 0xe0; opcode <= 0xe3; ++opcode)
        branches.push_back({opcode, 0});
    branches.push_back({0xc7, 0xf8, 0, 0, 0, 0}); // XBEGIN abort target.
    for (const size_t source : {size_t{192}, 256 + original.size() + 16}) {
        for (auto branch : branches) {
            std::array<uint8_t, 4096> page;
            page.fill(0x90);
            std::copy(original.begin(), original.end(), page.begin() + 256);
            page[256 + original.size()] = 0xc3;
            const size_t target =
                source < 256 ? 256 + 12 : 256 + original.size() - 12;
            const int32_t displacement = target - (source + branch.size());
            const size_t immediate = branch.size() > 2 ? branch.size() - 4 : 1;
            for (size_t i = immediate; i < branch.size(); ++i)
                branch[i] = static_cast<uint32_t>(displacement) >>
                            (8 * (i - immediate));
            std::copy(branch.begin(), branch.end(), page.begin() + source);
            const auto saved = page;
            assert(applyPage(page.data(), page.size()) ==
                   PatchResult::Mismatch);
            assert(page == saved);
        }
    }

    const uint8_t roundss[] = {0x66, 0x0f, 0x3a, 0x0a, 0xcb, 0x0b};
    assert(duolinkDecodeInstruction(roundss, sizeof(roundss)).length == 6);
    for (size_t available = 0; available < sizeof(roundss); ++available)
        assert(!duolinkDecodeInstruction(roundss, available).length);
    const uint8_t vex[] = {0xc5, 0xf8, 0x57, 0xc0};
    assert(duolinkDecodeInstruction(vex, sizeof(vex)).length == sizeof(vex));

    for (const std::vector<uint8_t> &code : {std::vector<uint8_t>{0xc3},
                                             {0xc2, 0, 0},
                                             {0xff, 0xe0},
                                             {0x0f, 0x0b},
                                             {0xcc},
                                             {0xf4}}) {
        const auto decoded = duolinkDecodeInstruction(code.data(), code.size());
        assert(decoded.length == code.size() && !decoded.fallsThrough);
    }

    // A stale match may recheck its window, but must not patch changed code.
    std::array<uint8_t, 4096> changed;
    changed.fill(0x90);
    std::copy(original.begin(), original.end(), changed.begin());
    display::Match match;
    assert(display::findModeCheck(changed.data(), changed.size(), match));
    changed[4] ^= 1;
    const auto changedCopy = changed;
    assert(display::patchPage(changed.data(), changed.size(), match) ==
           PatchResult::Mismatch);
    assert(changed == changedCopy);

    std::vector<bool> operandBytes(original.size(), false);
    for (size_t i = 0; i < check->originalOperandCount; ++i)
        for (size_t byte = 0;
             byte < check->originalOperands[i].displacementSize; ++byte)
            operandBytes[check->originalOperands[i].byteOffset + byte] = true;
    for (size_t changed = 0; changed < original.size(); ++changed) {
        if (operandBytes[changed])
            continue;
        std::array<uint8_t, 4096> page{};
        std::copy(original.begin(), original.end(), page.begin());
        page[changed] ^= 1;
        const auto saved = page;
        assert(applyPage(page.data(), page.size()) == PatchResult::NoMatch);
        assert(page == saved);
    }

    // Repeated loads must refer to the same stack slot.
    std::array<uint8_t, 4096> inconsistent{};
    std::copy(original.begin(), original.end(), inconsistent.begin());
    for (size_t i = 0; i < check->originalOperandCount; ++i) {
        if (check->originalOperands[i].local == display::Local::Mode) {
            inconsistent[check->originalOperands[i].byteOffset] ^= 4;
            break;
        }
    }
    const auto saved = inconsistent;
    assert(applyPage(inconsistent.data(), inconsistent.size()) ==
           PatchResult::NoMatch);
    assert(inconsistent == saved);

    for (size_t length = 0; length < original.size(); ++length) {
        display::Match match;
        assert(!display::findModeCheck(original.data(), length, match));
    }
    assert(applyPage(nullptr, 4096) == PatchResult::NoMatch);
    assert(applyPage(inconsistent.data(), 4095) == PatchResult::NoMatch);
    assert(
        display::cachePath("/System/Library/dyld/dyld_shared_cache_x86_64h"));
    assert(display::cachePath("/System/Cryptexes/OS/System/Library/dyld/"
                              "dyld_shared_cache_x86_64h.01"));
    assert(!display::cachePath(
        "/System/Library/dyld/dyld_shared_cache_x86_64h.symbols"));
    assert(
        !display::cachePath("/System/Library/dyld/dyld_shared_cache_arm64e"));
}
