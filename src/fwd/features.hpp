// ps5fwdgen - Feature flags: parts of the app that can be switched off.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every flag is decided at build time. Its default is set below; a build
// overrides it with FEATURES, e.g. `make FEATURES="PAYLOAD_TARGET=1"`, which
// the Makefile passes on as FWD_FEATURE_PAYLOAD_TARGET=1.
//
// Adding a flag: a Feature value, its default macro, a case in enabled() and
// one in feature_name().
#pragma once

#include <cstdint>

#ifndef FWD_FEATURE_PAYLOAD_TARGET
#define FWD_FEATURE_PAYLOAD_TARGET 0 // the "Payload" (PPSA00000) target app
#endif

namespace fwd
{

enum class Feature : std::uint8_t
{
    none, // not behind a flag: always on
    payload_target,
    count,
};

constexpr bool enabled(Feature feature)
{
    switch (feature)
    {
    case Feature::none:
        return true;
    case Feature::payload_target:
        return FWD_FEATURE_PAYLOAD_TARGET != 0;
    case Feature::count:
        break;
    }
    return false;
}

// The name FEATURES uses for the flag (logged at start).
constexpr const char *feature_name(Feature feature)
{
    switch (feature)
    {
    case Feature::none:
        return "NONE";
    case Feature::payload_target:
        return "PAYLOAD_TARGET";
    case Feature::count:
        break;
    }
    return "?";
}

} // namespace fwd
