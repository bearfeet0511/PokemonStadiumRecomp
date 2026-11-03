#pragma once

// Widescreen during battles only; menus stay 4:3. See widescreen.cpp.
namespace pkmnstadium::widescreen {

void init();      // read the launcher setting (before the window is created)
bool enabled();
void poll();      // once per VI from update_gfx; switches the aspect ratio

} // namespace pkmnstadium::widescreen
