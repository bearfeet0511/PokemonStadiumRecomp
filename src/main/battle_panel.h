#pragma once

#include <string>

// Battle info for a second-screen panel, read from game memory (see battle_panel.cpp
// and STADIUM.md "Battle memory").
namespace pkmnstadium::battle_panel {

// One line per row, fields separated by tabs:
//   "battle\t0"                     no battle on screen
//   "battle\t1"                     followed by one "mon" line per Pokemon in the
//                                   battle, in pick order:
//   "mon\t<name>\t<level>\t<hp>\t<max hp>\t<status>\t<active 0/1>\t<fainted 0/1>"
//   "\t<move>\t<pp>" x4             (empty move name for an empty slot)
std::string snapshot();

} // namespace pkmnstadium::battle_panel
