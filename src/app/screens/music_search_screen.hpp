// ps5fwdgen - Search YouTube for a game's theme music.
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

// on_at9 receives the chosen track already converted to ATRAC9 (snd0.at9
// bytes). The screen pops itself.
std::unique_ptr<Screen>
make_music_search_screen(Context &context, std::string query,
                         std::function<void(std::vector<unsigned char>)> on_at9);
} // namespace fwd
