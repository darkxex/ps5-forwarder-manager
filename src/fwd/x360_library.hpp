// ps5fwdgen - The games PS5X360 (Xbox 360) lists in its library.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PS5X360 (github.com/BrinooTk/PS5X360) publishes nothing for other apps: its
// settings and caches live in its own /download0, which only it can open. So
// this looks where it looks, the way it does (its src/launcher.cpp, Scan()):
//
//   - the game folders: its defaults (game_paths.hpp), plus the list its
//     install carries in assets/game_paths.txt;
//   - in each, up to six levels down: a folder with default.xex (or a single
//     .xex) is an extracted game and nothing below it is another game; an
//     .iso is a disc image; a file with no extension, .xbla or .god holding
//     an STFS package of a game (arcade, Games on Demand, installed disc...)
//     is a package; a *.data folder (a package's data) is skipped.
//
// PS5X360 starts the game whose full path is its first argument (argv[1]; the
// system leaves argv[0] empty), and only when that path is a file
// (src/canary_game_main.cpp): the .xex for an extracted game, the image, or
// the package. Its own install is /app0 to it.
// Folders added in its settings are not visible here; the edit screen offers
// a file or folder pick for those.
#pragma once

#include "fwd/library_game.hpp"

#include <string>
#include <vector>

namespace fwd
{

inline constexpr char kX360TitleId[] = "PPSA50011";

// Where PS5X360 is installed (its /app0); empty when it is not found. Looks
// in forwarders_root/PPSA50011, then in every folder of forwarders_root.
std::string find_x360_install(const std::string &forwarders_root);

// The folders PS5X360 scans, as it names them (/app0/... for its install).
std::vector<std::string> x360_game_paths(const std::string &install);

// Every game in those folders, by name. install maps /app0; empty skips it.
std::vector<LibraryGame> read_x360_library(const std::string &install,
                                           const std::vector<std::string> &game_paths);

// The executable of a game kept as a folder: default.xex, or the only .xex in
// it. Empty when the folder holds none or several.
std::string x360_folder_executable(const std::string &folder);

// The path PS5X360 is given for a file this app sees at `location`: under the
// install it is /app0/..., anywhere else the same path.
std::string x360_path_for(const std::string &install, const std::string &location);

// A game's name from its path: an extracted game's folder ("Halo 3" for
// ".../Halo 3/default.xex"), an image's or a package's file name.
std::string x360_game_name(const std::string &path);

// A package's thumbnail (PNG), for a game without a cover beside it.
std::vector<unsigned char> x360_package_thumbnail(const std::string &location);

} // namespace fwd
