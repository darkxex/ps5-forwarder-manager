// ps5fwdgen - The games SwanStationPS5 (PlayStation) lists in its library.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/psx_library.hpp"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <string_view>
#include <sys/stat.h>

namespace fwd
{

namespace
{

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string extension(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? std::string() : lower(name.substr(dot + 1));
}

std::string stem(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// SwanStationPS5's image_rank: a playlist beats a single disc; 0 is no image.
int image_rank(const std::string &name)
{
    const std::string ext = extension(name);
    if (ext == "m3u")
        return 6;
    if (ext == "pbp")
        return 5;
    if (ext == "chd")
        return 4;
    if (ext == "cue" || ext == "ccd")
        return 3;
    if (ext == "iso" || ext == "img" || ext == "mdf")
        return 2;
    if (ext == "bin")
        return 1; // only when no sheet describes it
    return 0;
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

std::vector<std::string> names_in(const std::string &folder)
{
    std::vector<std::string> names;
    DIR *directory = ::opendir(folder.c_str());
    if (directory == nullptr)
        return names;
    while (const dirent *entry = ::readdir(directory))
        if (entry->d_name[0] != '.')
            names.emplace_back(entry->d_name);
    ::closedir(directory);
    std::sort(names.begin(), names.end());
    return names;
}

// <dir>/<name>.png|jpg|jpeg, the first that exists.
std::string image_named(const std::string &dir, const std::string &name)
{
    if (dir.empty() || name.empty())
        return {};
    for (const char *suffix : {".png", ".jpg", ".jpeg"})
        if (is_file(dir + "/" + name + suffix))
            return dir + "/" + name + suffix;
    return {};
}

class Scanner
{
  public:
    Scanner(const std::string &covers, std::vector<LibraryGame> &games)
        : covers_(covers), games_(games)
    {
    }

    void scan_root(const std::string &root)
    {
        const std::vector<std::string> names = names_in(root);
        bool root_has_sheet = false;
        for (const std::string &name : names)
            root_has_sheet = root_has_sheet || image_rank(name) == 3;
        for (const std::string &name : names)
        {
            const std::string full = root + "/" + name;
            if (is_dir(full))
            {
                scan_folder(full, name);
                continue;
            }
            const int rank = image_rank(name);
            if (rank == 0 || (rank == 1 && root_has_sheet))
                continue;
            add(psx_game_title(name), full, root, stem(name));
        }
    }

  private:
    // One folder is one game, started from its best image.
    void scan_folder(const std::string &dir, const std::string &folder_name)
    {
        std::string best;
        int best_rank = 0;
        bool has_sheet = false;
        for (const std::string &name : names_in(dir))
        {
            const int rank = image_rank(name);
            has_sheet = has_sheet || rank == 3;
            if (rank > best_rank) // names are sorted: the first of the best kind wins
            {
                best_rank = rank;
                best = name;
            }
        }
        if (best_rank == 0 || (best_rank == 1 && has_sheet))
            return;
        add(psx_game_title(folder_name), dir + "/" + best, dir, stem(best));
    }

    void add(const std::string &title, const std::string &path, const std::string &folder,
             const std::string &disc_name)
    {
        if (std::any_of(games_.begin(), games_.end(),
                        [&](const LibraryGame &game) { return game.path == path; }))
            return;
        LibraryGame game;
        game.name = title;
        game.path = path;
        game.location = path;
        game.kind = lower(extension(path));
        std::transform(game.kind.begin(), game.kind.end(), game.kind.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        // Beside the game first, then in the shared covers folder.
        for (const std::string &cover :
             {image_named(folder, "cover"), image_named(folder, disc_name),
              image_named(covers_, title), image_named(covers_, disc_name)})
            if (!cover.empty())
            {
                game.cover = cover;
                break;
            }
        games_.push_back(std::move(game));
    }

    const std::string &covers_;
    std::vector<LibraryGame> &games_;
};

} // namespace

std::vector<std::string> psx_game_paths()
{
    std::vector<std::string> paths = {std::string(kPsxDataRoot) + "/games", "/data/PSXS5/games"};
    for (const char *drive : {"/mnt/usb0", "/mnt/usb1", "/mnt/ext0", "/mnt/ext1"})
        for (const char *name : {"SwanStationPS5", "PSXS5"})
        {
            paths.push_back(std::string(drive) + "/" + name);
            paths.push_back(std::string(drive) + "/" + name + "/games");
        }
    return paths;
}

std::vector<LibraryGame> read_psx_library(const std::vector<std::string> &game_paths,
                                          const std::string &covers)
{
    std::vector<LibraryGame> games;
    Scanner scanner(covers, games);
    for (const std::string &root : game_paths)
        if (is_dir(root))
            scanner.scan_root(root);
    std::stable_sort(games.begin(), games.end(), [](const LibraryGame &a, const LibraryGame &b)
                     { return lower(a.name) < lower(b.name); });
    return games;
}

std::string psx_game_title(const std::string &file_or_folder)
{
    std::string raw = file_or_folder;
    if (image_rank(raw) > 0)
        raw = stem(raw);
    std::string out;
    for (std::size_t i = 0; i < raw.size();)
    {
        if (raw[i] == '[')
        {
            const std::size_t end = raw.find(']', i);
            if (end != std::string::npos)
            {
                i = end + 1;
                continue;
            }
        }
        if (raw[i] == '(' &&
            (raw.compare(i, 5, "(Disc") == 0 || raw.compare(i, 5, "(disc") == 0 ||
             raw.compare(i, 3, "(CD") == 0 || raw.compare(i, 16, "(SwanStationPS5)") == 0))
        {
            const std::size_t end = raw.find(')', i);
            if (end != std::string::npos)
            {
                i = end + 1;
                continue;
            }
        }
        out += raw[i] == '_' ? ' ' : raw[i];
        ++i;
    }
    // Whitespace collapsed to single spaces, none at the ends.
    std::string title;
    bool space = true;
    for (const char c : out)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            if (!space)
                title += ' ';
            space = true;
        }
        else
        {
            title += c;
            space = false;
        }
    }
    while (!title.empty() && title.back() == ' ')
        title.pop_back();
    return title;
}

} // namespace fwd
