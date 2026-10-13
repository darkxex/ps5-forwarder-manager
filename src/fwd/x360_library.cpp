// ps5fwdgen - The games PS5X360 (Xbox 360) lists in its library.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/x360_library.hpp"

#include "core/json.hpp"
#include "fwd/rom_name.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <set>
#include <string_view>
#include <sys/stat.h>

namespace fwd
{

namespace
{

// PS5X360's DefaultGamePaths() (include/xbox360ps5/game_paths.hpp).
const char *const kDefaultGamePaths[] = {
    "/app0/assets/roms", "/data/xbox360",     "/mnt/usb0/xbox360",
    "/mnt/usb1/xbox360", "/mnt/ext0/xbox360", "/mnt/ext1/xbox360",
};

// How deep below a game folder PS5X360 still looks.
constexpr int kMaxDepth = 6;

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// The extension with its dot, in lower case; empty when there is none.
std::string extension(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? std::string() : lower(name.substr(dot));
}

std::string base_name(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string without_extension(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    return dot == std::string::npos || (slash != std::string::npos && dot <= slash + 1)
               ? path
               : path.substr(0, dot);
}

bool is_dir(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

struct Entry
{
    std::string name;
    bool dir = false;
};

std::vector<Entry> list(const std::string &folder)
{
    std::vector<Entry> entries;
    DIR *directory = ::opendir(folder.c_str());
    if (directory == nullptr)
        return entries;
    while (const dirent *entry = ::readdir(directory))
    {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        entries.push_back({name, is_dir(folder + "/" + name)});
    }
    ::closedir(directory);
    // A stable order, so a folder with several .xex always gives the same answer.
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.name < b.name; });
    return entries;
}

// default.xex, or the only .xex among the entries; empty otherwise.
std::string executable_in(const std::string &folder, const std::vector<Entry> &entries)
{
    std::string executable;
    int executables = 0;
    for (const Entry &entry : entries)
    {
        if (entry.dir || extension(entry.name) != ".xex")
            continue;
        if (lower(entry.name) == "default.xex")
            return folder + "/" + entry.name;
        ++executables;
        executable = folder + "/" + entry.name;
    }
    return executables == 1 ? executable : std::string();
}

// PS5X360's NormalizeGamePath, minus what a plain string cannot hold: an
// absolute path without trailing slashes; empty for anything else.
std::string normalize(std::string path)
{
    const std::size_t first = path.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    path = path.substr(first, path.find_last_not_of(" \t\r\n") - first + 1);
    if (path.size() > 1024 || path.front() != '/' || path.find('\\') != std::string::npos)
        return {};
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path == "/" ? std::string() : path;
}

std::string utf16be(const unsigned char *data, std::size_t characters)
{
    std::string text;
    for (std::size_t i = 0; i < characters; ++i)
    {
        const unsigned code = static_cast<unsigned>(data[i * 2]) << 8 | data[i * 2 + 1];
        if (code == 0)
            break;
        if (code < 0x80)
            text += static_cast<char>(code);
        else if (code < 0x800)
        {
            text += static_cast<char>(0xC0 | code >> 6);
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
        else
        {
            text += static_cast<char>(0xE0 | code >> 12);
            text += static_cast<char>(0x80 | (code >> 6 & 0x3F));
            text += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return text;
}

std::uint32_t be32(const unsigned char *p)
{
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

// The STFS header of a package, as PS5X360's ReadPackage reads it.
constexpr std::size_t kHeaderSize = 0x971A;

std::vector<unsigned char> package_header(const std::string &location)
{
    std::vector<unsigned char> header(kHeaderSize);
    std::FILE *file = std::fopen(location.c_str(), "rb");
    if (file == nullptr)
        return {};
    header.resize(std::fread(header.data(), 1, header.size(), file));
    (void)std::fclose(file);
    if (header.size() < 0x1800 ||
        (std::memcmp(header.data(), "LIVE", 4) != 0 && std::memcmp(header.data(), "PIRS", 4) != 0 &&
         std::memcmp(header.data(), "CON ", 4) != 0))
        return {};
    return header;
}

// A game package's display name; false for anything that is not one (a save,
// downloadable content, an update, or no package at all).
bool read_package(const std::string &location, std::string &name)
{
    const std::vector<unsigned char> header = package_header(location);
    if (header.empty())
        return false;
    // Arcade, demos, Games on Demand, installed discs, community games...
    const std::uint32_t type = be32(&header[0x344]);
    const bool game = type == 0x000D0000 || type == 0x00080000 || type == 0x00007000 ||
                      type == 0x00004000 || type == 0x00005000 || type == 0x02000000 ||
                      type == 0x00060000 || type == 0x000C0000;
    if (!game)
        return false;
    name = utf16be(&header[0x411], 0x40);
    if (name.empty())
        name = utf16be(&header[0x1691], 0x40);
    return true;
}

// A whole small text file; empty when it is missing or larger than 64 KiB.
std::string read_text(const std::string &path)
{
    std::string text;
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return text;
    char block[4096];
    std::size_t got = 0;
    while ((got = std::fread(block, 1, sizeof(block), file)) > 0 && text.size() <= 65536)
        text.append(block, got);
    (void)std::fclose(file);
    return text.size() > 65536 ? std::string() : text;
}

std::string cover_beside(const std::string &location, const std::string &kind)
{
    const std::string stem = kind == "XEX"
                                 ? location.substr(0, location.find_last_of('/')) + "/cover"
                                 : without_extension(location);
    for (const char *suffix : {".png", ".jpg", ".jpeg"})
        if (is_file(stem + suffix))
            return stem + suffix;
    return {};
}

class Scanner
{
  public:
    Scanner(const std::string &install, std::vector<X360Game> &games)
        : install_(install), games_(games)
    {
    }

    void scan(const std::string &folder, int depth, const std::string &root)
    {
        const std::vector<Entry> entries = list(folder);
        const std::string executable = executable_in(folder, entries);
        if (!executable.empty())
        {
            add(folder == root ? base_name(root) : base_name(folder), executable, "XEX");
            if (folder != root)
                return; // nothing below a game is another game
        }
        for (const Entry &entry : entries)
        {
            const std::string path = folder + "/" + entry.name;
            const std::string type = extension(entry.name);
            if (entry.dir)
            {
                if (depth < kMaxDepth && type != ".data") // a package's data
                    scan(path, depth + 1, root);
            }
            else if (type == ".iso")
                add(without_extension(entry.name), path, "ISO");
            else if (type.empty() || type == ".xbla" || type == ".god")
            {
                std::string name;
                if (read_package(path, name))
                    add(name.empty() ? entry.name : name, path, "GOD/STFS");
            }
        }
    }

  private:
    void add(const std::string &name, const std::string &location, const char *kind)
    {
        if (!seen_.insert(location).second)
            return;
        X360Game game;
        game.name = name;
        game.location = location;
        game.path = x360_path_for(install_, location);
        game.kind = kind;
        game.cover = cover_beside(location, game.kind);
        games_.push_back(std::move(game));
    }

    const std::string &install_;
    std::vector<X360Game> &games_;
    std::set<std::string> seen_;
};

} // namespace

std::string find_x360_install(const std::string &forwarders_root)
{
    const auto is_x360 = [](const std::string &folder)
    {
        const std::string text = read_text(folder + "/sce_sys/param.json");
        json::Value document;
        return !text.empty() && json::parse(text, document) && document.is_object() &&
               document.get("titleId").as_string() == kX360TitleId;
    };
    const std::string usual = forwarders_root + "/" + kX360TitleId;
    if (is_x360(usual))
        return usual;
    for (const Entry &entry : list(forwarders_root))
        if (entry.dir && is_x360(forwarders_root + "/" + entry.name))
            return forwarders_root + "/" + entry.name;
    return {};
}

std::vector<std::string> x360_game_paths(const std::string &install)
{
    std::vector<std::string> paths;
    const auto add = [&paths](const std::string &line)
    {
        const std::string path = normalize(line);
        if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end())
            paths.push_back(path);
    };
    for (const char *path : kDefaultGamePaths)
        add(path);
    // What a fresh PS5X360 starts with instead of the defaults, when its
    // install carries a list.
    if (!install.empty())
    {
        const std::string text = read_text(install + "/assets/game_paths.txt");
        std::size_t start = 0;
        while (start < text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            add(text.substr(start, end - start));
            start = end + 1;
        }
    }
    return paths;
}

std::vector<X360Game> read_x360_library(const std::string &install,
                                        const std::vector<std::string> &game_paths)
{
    std::vector<X360Game> games;
    Scanner scanner(install, games);
    for (const std::string &configured : game_paths)
    {
        std::string root = configured;
        if (root == "/app0" || root.rfind("/app0/", 0) == 0)
        {
            if (install.empty())
                continue;
            root = install + root.substr(5);
        }
        if (is_dir(root))
            scanner.scan(root, 0, root);
    }
    std::stable_sort(games.begin(), games.end(), [](const X360Game &a, const X360Game &b)
                     { return lower(a.name) < lower(b.name); });
    return games;
}

std::string x360_folder_executable(const std::string &folder)
{
    std::string root = folder;
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    return executable_in(root, list(root));
}

std::string x360_path_for(const std::string &install, const std::string &location)
{
    if (!install.empty() && location.size() > install.size() &&
        location.compare(0, install.size(), install) == 0 && location[install.size()] == '/')
        return "/app0" + location.substr(install.size());
    return location;
}

std::string x360_game_name(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    if (extension(path) == ".xex" && slash != std::string::npos && slash > 0)
        return name_from_rom(path.substr(0, slash) + "/");
    return name_from_rom(path);
}

std::vector<unsigned char> x360_package_thumbnail(const std::string &location)
{
    const std::vector<unsigned char> header = package_header(location);
    if (header.empty())
        return {};
    // The title's own thumbnail first, then the package's.
    const std::uint32_t thumbnail = be32(&header[0x1712]);
    const std::uint32_t title_thumbnail = be32(&header[0x1716]);
    if (title_thumbnail != 0 && title_thumbnail <= 0x4000 &&
        header.size() >= 0x571A + title_thumbnail)
        return {header.begin() + 0x571A, header.begin() + 0x571A + title_thumbnail};
    if (thumbnail != 0 && thumbnail <= 0x4000 && header.size() >= 0x171A + thumbnail)
        return {header.begin() + 0x171A, header.begin() + 0x171A + thumbnail};
    return {};
}

} // namespace fwd
