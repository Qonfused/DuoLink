/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "src/display/display_patch.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
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
    if ((argc == 4 || argc == 5) && std::strcmp(argv[1], "--page") == 0) {
        const auto original = readFile(argv[2]);
        assert(original.size() == display::executablePageSize);
        auto page = original;
        const auto site = std::strtoul(argv[3], nullptr, 10);
        display::Match match;
        if (!display::findModeCheck(page.data(), page.size(), match)) {
            std::puts("no match");
            return 1;
        }
        const auto begin = match.offset + match.check->writeOffset;
        const auto end = begin + match.check->writeSize;
        if (site < begin || site >= end) {
            std::puts("matched outside the mode check");
            return 1;
        }
        if (display::patchPage(page.data(), page.size(), match) !=
            PatchResult::Applied) {
            assert(page == original);
            std::puts("page rejected");
            return 1;
        }
        assert(page != original);
        assert(
            std::equal(page.begin(), page.begin() + begin, original.begin()));
        assert(
            std::equal(page.begin() + end, page.end(), original.begin() + end));
        if (argc == 5) {
            // Both paths rejoin at the JNZ target. Export through the last
            // instruction before it so the test can execute either path.
            const auto gateEnd = match.offset + match.check->size;
            const auto continuation = gateEnd + original[gateEnd - 1];
            assert(continuation <= page.size());
            const auto directory = std::string(argv[4]);
            const auto writeBlock = [&](const char *name,
                                        const std::vector<uint8_t> &bytes) {
                std::ofstream output(directory + "/" + name, std::ios::binary);
                output.write(
                    reinterpret_cast<const char *>(bytes.data() + match.offset),
                    continuation - match.offset);
                assert(output.good());
            };
            writeBlock("original.bin", original);
            writeBlock("patched.bin", page);
        }
        const auto patched = page;
        assert(applyPage(page.data(), page.size()) ==
               PatchResult::AlreadyApplied);
        assert(page == patched);
        std::puts("applied");
        return 0;
    }
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
    display::Match captured;
    assert(display::findModeCheck(original.data(), original.size(), captured));
    assert(captured.offset == 0);
    const auto *check = captured.check;

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

    // A jump before or after the match can enter the bytes DuoLink rewrites,
    // even when a return separates the jump from the mode check.
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
                source < 256 ? 256 + check->writeOffset + 12
                             : 256 + check->writeOffset + check->writeSize - 12;
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

    if (check->writeOffset) {
        // This jump skips the LEA that sets RBX. A load from RBX - 32
        // would then read from an unknown address when testing the format.
        std::array<uint8_t, 4096> page;
        page.fill(0x90);
        std::copy(original.begin(), original.end(), page.begin() + 256);
        page[254] = 0xeb;
        page[255] = static_cast<uint8_t>(check->writeOffset);
        const auto saved = page;
        assert(applyPage(page.data(), page.size()) == PatchResult::Mismatch);
        assert(page == saved);
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
    for (size_t changed = 0; changed < check->size - 1; ++changed) {
        if (operandBytes[changed])
            continue;
        std::array<uint8_t, 4096> page{};
        std::copy(original.begin(), original.end(), page.begin());
        page[changed] ^= 1;
        const auto saved = page;
        assert(applyPage(page.data(), page.size()) == PatchResult::NoMatch);
        assert(page == saved);
    }

    // A positive displacement skips Apple's invalid-mode handling. Zero
    // falls through to it, and a negative value branches back into the code.
    for (const uint8_t skip : {0x00, 0x01, 0x7f, 0x80, 0xff}) {
        std::array<uint8_t, 4096> page;
        page.fill(0x90);
        std::copy(original.begin(), original.begin() + check->size,
                  page.begin());
        page[check->size - 1] = skip;
        const auto before = page;
        if (skip > 0 && skip < 0x80) {
            assert(applyPage(page.data(), page.size()) == PatchResult::Applied);
            assert(std::equal(
                page.begin() + check->writeOffset + check->writeSize,
                page.end(),
                before.begin() + check->writeOffset + check->writeSize));
            assert(applyPage(page.data(), page.size()) ==
                   PatchResult::AlreadyApplied);
        } else {
            assert(applyPage(page.data(), page.size()) == PatchResult::NoMatch);
            assert(page == before);
        }
    }

    // If two instructions load the same local, changing just one of them
    // must invalidate the match.
    bool seen[display::fieldCount]{};
    for (size_t i = 0; i < check->originalOperandCount; ++i) {
        const auto &slot = check->originalOperands[i];
        const auto field = static_cast<size_t>(slot.field);
        if (seen[field]) {
            std::array<uint8_t, 4096> inconsistent{};
            std::copy(original.begin(), original.end(), inconsistent.begin());
            inconsistent[slot.byteOffset] ^= 4;
            const auto saved = inconsistent;
            assert(applyPage(inconsistent.data(), inconsistent.size()) ==
                   PatchResult::NoMatch);
            assert(inconsistent == saved);
            break;
        }
        seen[field] = true;
    }

    for (size_t length = 0; length < check->size; ++length) {
        display::Match match;
        assert(!display::findModeCheck(original.data(), length, match));
    }
    assert(applyPage(nullptr, 4096) == PatchResult::NoMatch);
    assert(applyPage(changed.data(), 4095) == PatchResult::NoMatch);
    assert(
        display::cachePath("/private/var/db/dyld/dyld_shared_cache_x86_64h"));
    assert(!display::cachePath("/tmp/dyld_shared_cache_x86_64h"));
    assert(!display::cachePath(
        "/System/Library/dyld/dyld_shared_cache_x86_64h01"));
    assert(display::cachePath("/System/Library/dyld/dyld_shared_cache_x86_64"));
    assert(
        display::cachePath("/System/Library/dyld/dyld_shared_cache_x86_64.01"));
    assert(
        display::cachePath("/System/Library/dyld/dyld_shared_cache_x86_64h"));
    assert(display::cachePath("/System/Cryptexes/OS/System/Library/dyld/"
                              "dyld_shared_cache_x86_64h.01"));
    assert(!display::cachePath(
        "/System/Library/dyld/dyld_shared_cache_x86_64h.symbols"));
    assert(
        !display::cachePath("/System/Library/dyld/dyld_shared_cache_arm64e"));
}
