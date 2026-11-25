#pragma once
// Text in another language (Spanish / French), taken from the player's own
// copy of that version of Stadium. The game code stays the US version; only
// its text archive ("textdata") is swapped, as the 3DS port does.

#include <cstdint>
#include <string>
#include <vector>

namespace pkmnstadium::language {

// After the US ROM is loaded and before the game starts: if launcher.cfg asks
// for another language and names a matching ROM, the game reads its text from
// that ROM. Logs and keeps English when anything doesn't fit.
void apply();

// The strings of one section of the text archive the game is using (English
// unless apply() switched it), as raw Latin-1 bytes. Empty if unavailable.
std::vector<std::string> text_section(int section);

} // namespace pkmnstadium::language
