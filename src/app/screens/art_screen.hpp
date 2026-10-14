// ps5fwdgen - Choose a tile icon, background or music: from a file, or online.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/screen.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fwd
{
struct Context;

enum class ArtKind
{
    icon,       // -> 512x512 PNG
    background, // -> 3840x2160 BC7 DDS
    music,      // -> snd0.at9 (ATRAC9)
};

// on_ready receives the already-encoded bytes (PNG for icon, DDS for
// background), then the screen pops itself.
std::unique_ptr<Screen> make_art_screen(Context &context, ArtKind kind, std::string suggested_query,
                                        std::function<void(std::vector<unsigned char>)> on_ready);

// The same choice for the selection music: theme music found online
// (on YouTube, as SDH-GameThemeMusic finds it) arrives already
// converted, as AT9 bytes, through on_at9; an audio file picked on the
// console arrives as its path through on_file (the caller converts it).
std::unique_ptr<Screen>
make_music_source_screen(Context &context, std::string suggested_query,
                         std::function<void(std::string)> on_file,
                         std::function<void(std::vector<unsigned char>)> on_at9);
} // namespace fwd
