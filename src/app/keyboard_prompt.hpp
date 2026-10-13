// ps5fwdgen - The look and buttons every on-screen keyboard prompt shares.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/components/input_prompt.hpp"
#include "ui/components/overlay.hpp"
#include "ui/theme.hpp"

namespace fwd
{

// The app theme is translucent glass. Behind a keyboard that lets the page
// show through and clash with the keys, so the prompt gets the same colours
// made opaque and a solid (unfrosted) panel.
inline hui::ui::Theme opaque_prompt_theme(hui::ui::Theme theme)
{
    using hui::ui::opaque_over;
    const hui::gfx::Color surface = opaque_over(theme.page, theme.surface);
    theme.secondary = opaque_over(surface, theme.secondary);
    theme.surface_high = opaque_over(surface, theme.surface_high);
    theme.surface = surface;
    return theme;
}

inline void style_keyboard_prompt(hui::ui::InputPrompt &prompt, const hui::ui::Theme &theme)
{
    prompt.style.theme = opaque_prompt_theme(theme);
    prompt.style.frosted = false;
}

// Standard console bindings, except that R2 confirms (Done) and R1 takes
// over switching to the symbols layout that R2 normally does.
inline void bind_keyboard_prompt(hui::ui::InputPrompt &prompt)
{
    auto bindings = hui::ui::KeyboardBindings::standard();
    bindings.layout = hui::Action::page_next; // R1
    bindings.done = hui::Action::jump_next;   // R2
    prompt.keyboard.style.bindings = bindings;
}

} // namespace fwd
