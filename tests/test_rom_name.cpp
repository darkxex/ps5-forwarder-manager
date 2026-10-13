// ps5fwdgen - Host tests for the display name suggested by a ROM file.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/rom_name.hpp"

#include <cstdio>
#include <string>

namespace
{

int failures = 0;

void expect(const char *input, const std::string &actual, const char *expected)
{
    if (actual != expected)
    {
        std::fprintf(stderr, "name_from_rom(\"%s\"): \"%s\", expected \"%s\"\n", input,
                     actual.c_str(), expected);
        ++failures;
    }
}

void check(const char *input, const char *expected)
{
    expect(input, fwd::name_from_rom(input), expected);
}

} // namespace

int main()
{
    check("Super Game.nsp", "Super Game");
    check("/data/roms/Super Game.nsp", "Super Game");
    check("game.tar.gz", "game.tar");
    check("no-extension", "no-extension");
    check(".nsp", ".nsp");
    check("  spaced name  .iso", "spaced name");
    check("Pokémon Edição.xci", "Pokémon Edição");
    check("", "");
    // A game kept as a folder (a trailing slash) keeps its dots.
    check("/data/x360/Halo 3/", "Halo 3");
    check("/data/x360/Halo 3 v1.1/", "Halo 3 v1.1");
    check("Gears.of.War//", "Gears.of.War");
    check("/", "");
    // Cut at 60 characters, never inside a multi-byte one.
    std::string long_name;
    for (int i = 0; i < 70; ++i)
        long_name += "é";
    std::string sixty;
    for (int i = 0; i < 60; ++i)
        sixty += "é";
    expect("70 x é", fwd::name_from_rom(long_name + ".bin"), sixty.c_str());
    expect("short limit", fwd::name_from_rom("abcdef.bin", 3), "abc");
    expect("trailing space at the cut", fwd::name_from_rom("ab cd.bin", 3), "ab");

    if (failures != 0)
        return 1;
    std::puts("ROM display name checks passed.");
    return 0;
}
