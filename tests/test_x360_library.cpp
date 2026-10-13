// ps5fwdgen - Host tests for PS5X360's game list and how its forwarders pass
// the game (fwd/x360_library.hpp).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/forwarder.hpp"
#include "fwd/store.hpp"
#include "fwd/x360_library.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string &what)
{
    if (!condition)
    {
        std::fprintf(stderr, "failed: %s\n", what.c_str());
        ++failures;
    }
}

void make_dirs(const std::string &path)
{
    for (std::size_t slash = path.find('/', 1);; slash = path.find('/', slash + 1))
    {
        (void)::mkdir(path.substr(0, slash).c_str(), 0755);
        if (slash == std::string::npos)
            break;
    }
}

void write(const std::string &path, const std::vector<unsigned char> &bytes)
{
    make_dirs(path.substr(0, path.find_last_of('/')));
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        std::abort();
    (void)std::fwrite(bytes.data(), 1, bytes.size(), file);
    (void)std::fclose(file);
}

void write(const std::string &path, const std::string &text)
{
    write(path, std::vector<unsigned char>(text.begin(), text.end()));
}

void put32(std::vector<unsigned char> &bytes, std::size_t at, std::uint32_t value)
{
    bytes[at] = static_cast<unsigned char>(value >> 24);
    bytes[at + 1] = static_cast<unsigned char>(value >> 16);
    bytes[at + 2] = static_cast<unsigned char>(value >> 8);
    bytes[at + 3] = static_cast<unsigned char>(value);
}

// An STFS package header: its kind, display name (UTF-16BE) and thumbnail.
std::vector<unsigned char> package(std::uint32_t type, const std::string &name,
                                   const std::string &thumbnail)
{
    std::vector<unsigned char> bytes(0x971A, 0);
    std::memcpy(bytes.data(), "CON ", 4);
    put32(bytes, 0x344, type);
    for (std::size_t i = 0; i < name.size(); ++i)
        bytes[0x411 + i * 2 + 1] = static_cast<unsigned char>(name[i]);
    put32(bytes, 0x1712, static_cast<std::uint32_t>(thumbnail.size()));
    std::memcpy(&bytes[0x171A], thumbnail.data(), thumbnail.size());
    return bytes;
}

const fwd::LibraryGame *find(const std::vector<fwd::LibraryGame> &games, const std::string &name)
{
    for (const fwd::LibraryGame &game : games)
        if (game.name == name)
            return &game;
    return nullptr;
}

} // namespace

int main()
{
    char pattern[] = "/tmp/x360-library-XXXXXX";
    const char *made = ::mkdtemp(pattern);
    if (made == nullptr)
        return 1;
    const std::string root = made;
    const std::string library = root + "/xbox360";
    const std::string homebrew = root + "/homebrew";
    const std::string install = homebrew + "/X360"; // not named after its title ID

    // An extracted game with a cover, and something below it that is not a game.
    write(library + "/Halo 3 v1.1/default.xex", "x");
    write(library + "/Halo 3 v1.1/other.xex", "x");
    write(library + "/Halo 3 v1.1/cover.jpg", "jpeg");
    write(library + "/Halo 3 v1.1/dlc/Inner/default.xex", "x");
    // A folder with a single executable of another name, one with two, and one
    // too deep to be looked at.
    write(library + "/Single/game.xex", "x");
    write(library + "/Two/a.xex", "x");
    write(library + "/Two/b.xex", "x");
    write(library + "/1/2/3/4/5/6/Deep/default.xex", "x");
    write(library + "/1/2/3/4/5/Shallow/default.xex", "x");
    // A disc image with a cover beside it, and one in a package's data folder.
    write(library + "/Disc Game.iso", "iso");
    write(library + "/Disc Game.png", "png");
    write(library + "/Pack.data/Hidden.iso", "iso");
    // Packages: a game (no extension), a save (skipped), and not a package.
    write(library + "/ARCADE01", package(0x000D0000, "Arcade Hit", "PNGDATA"));
    write(library + "/SAVE01", package(0x00000001, "A save", ""));
    write(library + "/notes", "just text");
    // PS5X360's install, with a game in its own roms folder and an extra path.
    write(install + "/sce_sys/param.json", "{\"titleId\": \"PPSA50011\"}");
    write(install + "/assets/roms/Rom Game/default.xex", "x");
    write(install + "/assets/game_paths.txt", " " + root + "/extra/ \n\nnot-absolute\n");
    write(homebrew + "/Other/sce_sys/param.json", "{\"titleId\": \"PPSA99008\"}");

    expect(fwd::find_x360_install(homebrew) == install, "install found by its title ID");
    expect(fwd::find_x360_install(root + "/missing").empty(), "no install");

    const std::vector<std::string> paths = fwd::x360_game_paths(install);
    expect(paths.size() == 7 && paths[0] == "/app0/assets/roms" && paths[1] == "/data/xbox360",
           "defaults first");
    expect(paths.back() == root + "/extra", "the install's extra path, trimmed");
    expect(fwd::x360_game_paths("").size() == 6, "defaults only without an install");

    const std::vector<fwd::LibraryGame> games =
        fwd::read_x360_library(install, {"/app0/assets/roms", library, root + "/missing"});
    expect(games.size() == 6, "six games");

    const fwd::LibraryGame *halo = find(games, "Halo 3 v1.1");
    expect(halo != nullptr && halo->kind == "XEX" &&
               halo->path == library + "/Halo 3 v1.1/default.xex" &&
               halo->cover == library + "/Halo 3 v1.1/cover.jpg",
           "extracted game: default.xex and its cover");
    expect(find(games, "Inner") == nullptr, "nothing below a game is another game");
    const fwd::LibraryGame *single = find(games, "Single");
    expect(single != nullptr && single->path == library + "/Single/game.xex" &&
               single->cover.empty(),
           "a single .xex of any name");
    expect(find(games, "Two") == nullptr, "two executables: not a game");
    expect(find(games, "Shallow") != nullptr && find(games, "Deep") == nullptr,
           "six levels down and no further");
    const fwd::LibraryGame *disc = find(games, "Disc Game");
    expect(disc != nullptr && disc->kind == "ISO" && disc->cover == library + "/Disc Game.png",
           "disc image and its cover");
    expect(find(games, "Hidden") == nullptr, "a package's data folder is skipped");
    const fwd::LibraryGame *arcade = find(games, "Arcade Hit");
    expect(arcade != nullptr && arcade->kind == "GOD/STFS" && arcade->path == library + "/ARCADE01",
           "a game package by its own name");
    expect(find(games, "A save") == nullptr, "a save is not a game");
    const std::vector<unsigned char> thumbnail = fwd::x360_package_thumbnail(library + "/ARCADE01");
    expect(std::string(thumbnail.begin(), thumbnail.end()) == "PNGDATA", "package thumbnail");
    const fwd::LibraryGame *rom = find(games, "Rom Game");
    expect(rom != nullptr && rom->path == "/app0/assets/roms/Rom Game/default.xex" &&
               rom->location == install + "/assets/roms/Rom Game/default.xex",
           "a game in the install is given as /app0");
    expect(fwd::read_x360_library("", {"/app0/assets/roms"}).empty(), "no /app0 without install");
    expect(games.front().name == "Arcade Hit" && games.back().name == "Single", "sorted by name");

    expect(fwd::x360_folder_executable(library + "/Halo 3 v1.1/") ==
               library + "/Halo 3 v1.1/default.xex",
           "a folder's default.xex");
    expect(fwd::x360_folder_executable(library + "/Single") == library + "/Single/game.xex",
           "a folder's only .xex");
    expect(fwd::x360_folder_executable(library + "/Two").empty(), "two executables: none");
    expect(fwd::x360_path_for(install, install + "/assets/x.iso") == "/app0/assets/x.iso" &&
               fwd::x360_path_for(install, install + "X/y.iso") == install + "X/y.iso" &&
               fwd::x360_path_for("", "/data/y.iso") == "/data/y.iso",
           "/app0 only for the install itself");
    expect(fwd::x360_game_name("/data/xbox360/Halo 3 v1.1/default.xex") == "Halo 3 v1.1" &&
               fwd::x360_game_name("/data/xbox360/Disc Game.iso") == "Disc Game",
           "names from paths");

    // The forwarder gives PS5X360 the path alone, and reads it back.
    expect(fwd::takes_game_path("PPSA50011") && !fwd::takes_game_path("PPSA99008"),
           "only PS5X360 takes a game path");
    fwd::Forwarder forwarder;
    forwarder.title_id = "PPSA99777";
    forwarder.display_name = "Halo 3";
    forwarder.target = "PPSA50011";
    forwarder.rom = halo != nullptr ? halo->path : "";
    const std::vector<fwd::Argument> args = forwarder.resolved_args();
    expect(args.size() == 1 && args[0].key == forwarder.rom && args[0].value.empty(),
           "the path is the only argument");
    write(root + "/template/eboot.bin", "eboot");
    write(root + "/template/sce_module/libc.prx", "libc");
    make_dirs(root + "/forwarders");
    fwd::Assets assets;
    assets.icon_png = {0x89, 'P', 'N', 'G'}; // any bytes: the writer only copies them
    const fwd::WriteResult written =
        fwd::write_forwarder(root + "/forwarders", root + "/template", forwarder, assets);
    expect(written.ok, "forwarder written: " + written.error);
    fwd::Forwarder loaded;
    expect(fwd::load_forwarder(root + "/forwarders/PPSA99777", loaded) &&
               loaded.rom == forwarder.rom && loaded.extra_args.empty(),
           "the path reads back as the game");
    fwd::Forwarder eden = forwarder;
    eden.target = "PPSA99008";
    eden.rom = "game.nsp";
    expect(eden.resolved_args().size() == 1 && eden.resolved_args()[0].key == "--rom",
           "other emulators keep --rom");

    (void)std::system(("rm -rf '" + root + "'").c_str());
    if (failures != 0)
        return 1;
    std::puts("PS5X360 library checks passed.");
    return 0;
}
