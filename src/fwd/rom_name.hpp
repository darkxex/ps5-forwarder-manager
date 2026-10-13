// ps5fwdgen - A display name suggested by a ROM file's name.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fwd
{

// The file name without its folder and last extension, trimmed and cut to
// at most max_chars UTF-8 characters: "/data/roms/Super Game.nsp" ->
// "Super Game". A leading dot is part of the name (".nsp" stays ".nsp").
inline std::string name_from_rom(std::string_view path, std::size_t max_chars = 60)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0)
        path = path.substr(0, dot);
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t'))
        path.remove_prefix(1);
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t'))
        path.remove_suffix(1);

    std::size_t chars = 0;
    std::size_t end = 0;
    while (end < path.size())
    {
        // Continuation bytes (10xxxxxx) belong to the character before them.
        std::size_t next = end + 1;
        while (next < path.size() && (static_cast<unsigned char>(path[next]) & 0xC0) == 0x80)
            ++next;
        if (++chars > max_chars)
            break;
        end = next;
    }
    path = path.substr(0, end);
    while (!path.empty() && path.back() == ' ')
        path.remove_suffix(1);
    return std::string(path);
}

} // namespace fwd
