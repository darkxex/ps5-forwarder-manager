// ps5fwdgen - The games SwanStationPS5 (PlayStation) lists in its library.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SwanStationPS5 (github.com/darkxex/SwanStationPS5) looks for games the way
// this does (its src/main.c and src/library.c):
//
//   - the folders: /data/SwanStationPS5/games, /data/PSXS5/games, and on each
//     of /mnt/usb0, usb1, ext0 and ext1 both <drive>/SwanStationPS5 and
//     <drive>/SwanStationPS5/games, and the same for PSXS5;
//   - in each, a folder is one game, started from its best image (.m3u, then
//     .pbp, .chd, .cue or .ccd, .iso, .img or .mdf, and a .bin only when no
//     sheet describes it); a loose image is a game of its own;
//   - a cover: cover.png or .jpg in the game's folder, an image named like the
//     disc file, or one named like the game in /data/SwanStationPS5/covers.
//
// A forwarder gives it the game as --rom <full path>.
#pragma once

#include "fwd/library_game.hpp"

#include <string>
#include <vector>

namespace fwd
{

inline constexpr char kPsxTitleId[] = "PPSA98510";
inline constexpr char kPsxDataRoot[] = "/data/SwanStationPS5";

// The folders SwanStationPS5 scans.
std::vector<std::string> psx_game_paths();

// Every game in those folders, by name. covers is the shared covers folder.
std::vector<LibraryGame> read_psx_library(const std::vector<std::string> &game_paths,
                                          const std::string &covers);

// A game's name as SwanStationPS5 shows it: without the image's extension,
// [tags] and disc numbers ("Final Fantasy VII (Disc 1).cue" -> "Final Fantasy VII").
std::string psx_game_title(const std::string &file_or_folder);

} // namespace fwd
