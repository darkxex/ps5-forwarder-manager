// ps5fwdgen - Host tests for where settings live and that they round-trip.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/settings.hpp"
#include "platform/app_paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace
{

int failures = 0;

void expect(const char *what, const std::string &actual, const std::string &expected)
{
    if (actual != expected)
    {
        std::fprintf(stderr, "%s: \"%s\", expected \"%s\"\n", what, actual.c_str(),
                     expected.c_str());
        ++failures;
    }
}

void check_download_root(const char *app_root, const char *expected)
{
    expect(app_root, paths::download_root_for(app_root), expected);
}

} // namespace

int main()
{
    // Sandboxed, and after elevation (the title's sandbox mount).
    check_download_root("/app0", "/download0");
    check_download_root("/mnt/sandbox/PPSA99930_000/app0", "/mnt/sandbox/PPSA99930_000/download0");
    // Anything else keeps the sandbox path.
    check_download_root("/data/homebrew/PPSA99930", "/download0");
    check_download_root("", "/download0");
    check_download_root("/mnt/sandbox/PPSA99930_000/app01", "/download0");

    // A custom forwarders folder survives a save and a fresh load.
    char directory[] = "/tmp/fwd-settings-XXXXXX";
    if (::mkdtemp(directory) == nullptr)
    {
        std::perror("mkdtemp");
        return 1;
    }
    const std::string path = std::string(directory) + "/settings.txt";
    expect("default root", fwd::Settings::load(path).forwarders_root, "/data/homebrew");
    fwd::Settings settings;
    settings.forwarders_root = "/data/forwarders";
    settings.sounds = false;
    if (!settings.save(path).empty())
    {
        std::fprintf(stderr, "save failed: %s\n", path.c_str());
        ++failures;
    }
    const fwd::Settings loaded = fwd::Settings::load(path);
    expect("saved root", loaded.forwarders_root, "/data/forwarders");
    expect("saved sounds", loaded.sounds ? "1" : "0", "0");
    ::unlink(path.c_str());
    ::rmdir(directory);

    if (failures != 0)
    {
        std::fprintf(stderr, "test_settings: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_settings: ok\n");
    return 0;
}
