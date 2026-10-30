#pragma once

#include <filesystem>

// Lets a Transfer Pak cart use a Gen1Recomp save slot
// (%APPDATA%\pokemon-love2d\saves\<game>\slotN.lua) instead of a .sav.
//
// At boot the slot is exported to a working .sav with Gen1Recomp's own
// converter; at quit, whatever Stadium changed on that cart is merged back
// into the slot (tools/gen1recomp/merge.lua). Both steps run LuaJIT against the
// pinned Gen1Recomp sources shipped next to the exe in gen1recomp\.
//
// Gen1Recomp: Copyright (C) 2026 BOIS CLUB GAMES, LLC, Pokemon Gen 1
// Recompilation Project (https://github.com/bryanthaboi/gen1recomp).
namespace pkmnstadium::gen1recomp {
    // True for a Gen1Recomp slot path (*.lua).
    bool is_slot(const std::filesystem::path& path);

    // Merges any session a crash left unmerged for this port, backs the slot
    // up, and exports it to a working cart. Returns the cart's .sav path, or
    // an empty path if the slot can't be used (the reason is shown to the user).
    std::filesystem::path prepare_cart(int port, const std::filesystem::path& slot);

    // Merges every prepared cart's changes back into its slot. Call after the
    // Transfer Pak carts have been flushed, before the process exits.
    void finish_all();
}
