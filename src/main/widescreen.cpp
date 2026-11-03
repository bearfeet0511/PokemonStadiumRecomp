// Widescreen (launcher Settings > aspect_ratio=widescreen).
//
// RT64's Expand mode widens 3D scenes to the window, but Stadium's menus mix
// 2D and 3D drawn for 4:3: full-screen fades only cover the middle, and menu
// models tear at the edges. So widescreen only applies while a battle is on
// screen; everywhere else the window shows the original 4:3 picture with bars.
// Known gap: battle fades still only cover the 4:3 middle. Stretching
// full-screen 2D draws in RT64 also stretched HUD panels and lost the fade's
// blend, so that was dropped for now.
//
// Battle detection reads game memory (pret/pokestadium names, US v1.0):
//   gFragments (0x800A58F0): struct { u32 vaddr; u32 size; } [240], the loaded
//   fragment table; fragment 62 is the battle engine.
//   sMemPool (0x800A6070), mainState at +0x30: the stack of named pool states;
//   battles started from menus push 'FIGT'.
// Renderer-only: nothing here writes game memory, so netplay stays in sync.

#include "widescreen.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <ultramodern/config.hpp>

#include "recompui_launcher.h"

extern "C" uint8_t* recomp_runtime_get_rdram(void);

namespace pkmnstadium::widescreen {
namespace {

constexpr uint32_t kFragments    = 0x800A58F0;
// Fragment ids come from the load address, not the name:
// ((TEXT_START & 0x0FF00000) >> 20) - 0x10. fragment62 runs at 0x84300000.
constexpr uint32_t kBattleFrag   = 0x43 - 0x10;
constexpr uint32_t kPoolState    = 0x800A6070 + 0x30;
constexpr uint32_t kTagFight     = 0x46494754; // 'FIGT'

bool s_enabled = false;
bool s_wide = false;
bool s_trace = false;

bool valid_ptr(uint32_t a) {
    return a >= 0x80000000u && a < 0x80800000u && (a & 3) == 0;
}

uint32_t read32(const uint8_t* rdram, uint32_t addr) {
    // RDRAM is stored as native 32-bit words, so aligned reads are direct.
    return *reinterpret_cast<const uint32_t*>(rdram + (addr - 0x80000000u));
}

bool fight_state_pushed(const uint8_t* rdram) {
    uint32_t state = read32(rdram, kPoolState);
    for (int depth = 0; depth < 32 && valid_ptr(state); ++depth) {
        if (read32(rdram, state - 4) == kTagFight) return true;
        state = read32(rdram, state + 0xC);
    }
    return false;
}

void apply(bool wide) {
    auto conf = ultramodern::renderer::get_graphics_config();
    conf.ar_option = wide ? ultramodern::renderer::AspectRatio::Expand
                          : ultramodern::renderer::AspectRatio::Original;
    ultramodern::renderer::set_graphics_config(conf);
}

} // namespace

bool enabled() {
    return s_enabled;
}

void init() {
    s_enabled = recompui::startup_widescreen();
    s_trace = std::getenv("PSR_WIDESCREEN_TRACE") != nullptr;
}

void poll() {
    if (!s_enabled && !s_trace) return;
    const uint8_t* rdram = recomp_runtime_get_rdram();
    if (rdram == nullptr) return;

    const uint32_t frag = read32(rdram, kFragments + kBattleFrag * 8);
    const bool fight = fight_state_pushed(rdram);
    const bool battle = frag != 0;

    if (s_trace) {
        static FILE* log = std::fopen("widescreen.log", "w");
        static uint32_t last_frag = ~0u;
        static int last_fight = -1;
        static unsigned polls = 0;
        ++polls;
        static std::string last_loaded;
        std::string loaded;
        for (uint32_t id = 0; id < 240; ++id) {
            if (read32(rdram, kFragments + id * 8) != 0) loaded += " " + std::to_string(id);
        }
        if (log && (frag != last_frag || (int)fight != last_fight || loaded != last_loaded)) {
            std::fprintf(log, "poll %u: battle=0x%08X figt=%d loaded:%s\n", polls, frag, (int)fight, loaded.c_str());
            std::fflush(log);
            last_loaded = loaded;
            last_frag = frag;
            last_fight = fight;
        }
    }

    if (s_enabled && battle != s_wide) {
        s_wide = battle;
        apply(battle);
    }
}

} // namespace pkmnstadium::widescreen
