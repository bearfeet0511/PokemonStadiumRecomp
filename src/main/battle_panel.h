#pragma once

#include <cstdint>
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
//   "\t<slot>\t<send-out button>"   team slot and its CONT_* mask (0 if unknown)
//   "pick\t1"                       the pre-battle pick screen instead, followed by
//   "slot\t<slot>\t<name>\t<level>" one per team slot
std::string snapshot();

// Holds buttons briefly with no menu checks (the pick screen's buttons).
void hold(uint16_t buttons);

// Taps on the panel: press() uses a move (its C-button mask), send_out() switches
// to a team slot. Each opens the right battle submenu first when needed; the
// input code ORs tapped_buttons() into Player 1's buttons.
void press(uint16_t buttons);
void send_out(int slot);
uint16_t tapped_buttons();

} // namespace pkmnstadium::battle_panel
