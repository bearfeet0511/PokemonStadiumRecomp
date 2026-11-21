// Battle info for a second-screen panel: the player's team with HP, status, moves
// and PP, and which Pokemon is out. Read-only; it never writes game memory, so it
// can't affect the game or netplay sync. Addresses are for US v1.0 and were found
// by diffing RDRAM between turns (STADIUM.md, "Battle memory").

#include "battle_panel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "app_paths.h"

extern "C" uint8_t* recomp_runtime_get_rdram(void);

namespace pkmnstadium::battle_panel {
namespace {

constexpr uint32_t kFragments = 0x800A58F0;  // gFragments: { vaddr, size } per id
constexpr uint32_t kBattleFrag = 0x43 - 0x10; // fragment62, the battle engine
constexpr uint32_t kTeamCount = 0x800AE546;   // how many records take part (3 or 6)
constexpr uint32_t kTeam = 0x800AE561;        // player's records, in pick order
constexpr uint32_t kRecordSize = 0x54;
constexpr uint32_t kActivePtr = 0x800FCB18;   // -> battle copy of the active Pokemon

uint8_t read8(const uint8_t* rdram, uint32_t addr) {
    // RDRAM holds native 32-bit words; byte addresses are XOR 3.
    return rdram[(addr - 0x80000000u) ^ 3];
}

uint16_t read16(const uint8_t* rdram, uint32_t addr) {
    return static_cast<uint16_t>((read8(rdram, addr) << 8) | read8(rdram, addr + 1));
}

uint32_t read32(const uint8_t* rdram, uint32_t addr) {
    return *reinterpret_cast<const uint32_t*>(rdram + (addr - 0x80000000u));
}

bool in_rdram(uint32_t addr, uint32_t size) {
    return addr >= 0x80000000u && addr + size <= 0x80800000u;
}

// Move names come from the player's own ROM (a NUL-separated list in move-ID
// order), so no game text is copied into this project.
const std::vector<std::string>& move_names() {
    static std::vector<std::string> names;
    static std::once_flag loaded;
    std::call_once(loaded, [] {
        std::filesystem::path rom;
        if (std::ifstream cfg(exe_dir() / "rom.cfg"); cfg) {
            std::string line;
            if (std::getline(cfg, line)) rom = line;
        }
        if (rom.empty() || !std::filesystem::exists(rom)) rom = exe_dir() / "baserom.z64";
        std::ifstream file(rom, std::ios::binary);
        const std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        static const char kFirst[] = "POUND\0KARATE CHOP";
        const auto it = std::search(data.begin(), data.end(), kFirst, kFirst + sizeof(kFirst) - 1);
        if (it == data.end()) return;
        names.emplace_back(); // move 0: empty slot
        for (auto p = it; p != data.end() && names.size() < 166;) {
            auto end = std::find(p, data.end(), '\0');
            names.emplace_back(p, end);
            if (end == data.end()) break;
            p = end + 1;
        }
    });
    return names;
}

std::string status_text(uint8_t status) {
    if (status & 0x07) return "SLP";
    if (status & 0x08) return "PSN";
    if (status & 0x10) return "BRN";
    if (status & 0x20) return "FRZ";
    if (status & 0x40) return "PAR";
    return "";
}

std::string clean(std::string text) {
    for (char& c : text) {
        if (c == '\t' || c == '\n') c = ' ';
    }
    return text;
}

} // namespace

// Tapping a move: the C-buttons only pick a move once the move list is open, so
// a tap first checks which battle menu has control. From pret's decomp of the
// battle engine (fragment62_2F74E0.c, func_8430F0C0 / func_8430EF4C):
// D_84390010[2] (fragment + 0x90010) points to each side's battle state, whose
// menu block at +0x654 has a mode at +0x04 (0 top menu, 1 Fight, 2 Pokemon,
// 3 Run), a step at +0x10 (0 waiting at the top menu, 1 in a submenu, 3 chosen)
// and the side's controller at +0xC8. A at the top menu sets mode 1; the next
// frame sets step 1, and only then do the C-buttons pick a move. Checked against
// RDRAM dumps at both menus (Thor, 2026-11-21).
constexpr uint32_t kSideStates = 0x90010;     // fragment offset of D_84390010[2]
constexpr uint32_t kMenuBlock = 0x654;
constexpr uint32_t kControllers = 0x800A7320; // gControllers; Player 1 is the first
constexpr uint16_t kButtonA = 0x8000;
constexpr int64_t kHoldMs = 100;      // long enough for a couple of controller polls
constexpr int64_t kGiveUpMs = 1500;   // the move list never came up

enum class TapPhase { Idle, Start, PressA, WaitMoves, PressMove };

static std::mutex s_tap_mutex;
static TapPhase s_phase = TapPhase::Idle;
static uint16_t s_move_button = 0;
static int64_t s_phase_start = 0;
static int64_t s_tap_start = 0;

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

enum class Menu { Unknown, First, Moves };

static Menu battle_menu(const uint8_t* rdram) {
    if (rdram == nullptr) return Menu::Unknown;
    const uint32_t frag = read32(rdram, kFragments + kBattleFrag * 8);
    if (frag == 0 || !in_rdram(frag + kSideStates, 8)) return Menu::Unknown;
    for (uint32_t side = 0; side < 2; ++side) {
        const uint32_t state = read32(rdram, frag + kSideStates + side * 4);
        if (!in_rdram(state, kMenuBlock + 0xCC)) continue;
        const uint32_t menu = state + kMenuBlock;
        if (read32(rdram, menu + 0xC8) != kControllers) continue; // not Player 1's side
        const uint32_t mode = read32(rdram, menu + 0x04);
        const uint32_t step = read32(rdram, menu + 0x10);
        if (mode == 0 && step == 0) return Menu::First;
        if (mode == 1 && step == 1) return Menu::Moves;
        return Menu::Unknown;
    }
    return Menu::Unknown;
}

static const char* menu_name(Menu menu) {
    return menu == Menu::Moves ? "moves" : menu == Menu::First ? "first" : "unknown";
}

void press(uint16_t buttons) {
    std::lock_guard lock(s_tap_mutex);
    s_move_button = buttons;
    s_phase = TapPhase::Start;
    s_tap_start = s_phase_start = now_ms();
}

// Called on every controller poll for Player 1, on the game thread. Walks the
// tap through: (first menu) press A, wait for the move list, press the move.
uint16_t tapped_buttons() {
    std::lock_guard lock(s_tap_mutex);
    if (s_phase == TapPhase::Idle) return 0;
    const int64_t now = now_ms();
    const Menu menu = battle_menu(recomp_runtime_get_rdram());

    if (s_phase == TapPhase::Start) {
        std::fprintf(stderr, "[battle-panel] tap %#06x at menu %s\n", s_move_button, menu_name(menu));
        if (menu == Menu::Moves) {
            s_phase = TapPhase::PressMove;
        } else if (menu == Menu::First) {
            s_phase = TapPhase::PressA;
        } else {
            s_phase = TapPhase::Idle; // not a menu we know: press nothing
            return 0;
        }
        s_phase_start = now;
    }

    switch (s_phase) {
        case TapPhase::PressA:
            if (now - s_phase_start < kHoldMs) return kButtonA;
            s_phase = TapPhase::WaitMoves;
            s_phase_start = now;
            return 0;
        case TapPhase::WaitMoves:
            if (menu == Menu::Moves) {
                s_phase = TapPhase::PressMove;
                s_phase_start = now;
                return s_move_button;
            }
            if (now - s_tap_start > kGiveUpMs) {
                std::fprintf(stderr, "[battle-panel] move list didn't open (menu %s)\n", menu_name(menu));
                s_phase = TapPhase::Idle;
            }
            return 0;
        case TapPhase::PressMove:
            if (now - s_phase_start < kHoldMs) return s_move_button;
            s_phase = TapPhase::Idle;
            return 0;
        default:
            return 0;
    }
}

// Research aid for platforms without the debug server: creating `rdram_dump.request`
// next to the game makes the next snapshot write all 8 MB of RDRAM, in N64 byte
// order, to `rdram_dump.bin` (then the request is removed).
static void dump_rdram_if_requested(const uint8_t* rdram) {
    const auto request = exe_dir() / "rdram_dump.request";
    std::error_code ec;
    if (rdram == nullptr || !std::filesystem::exists(request, ec)) return;
    std::filesystem::remove(request, ec);
    std::vector<char> data(0x800000);
    for (uint32_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(rdram[i ^ 3]);
    std::ofstream(exe_dir() / "rdram_dump.bin", std::ios::binary).write(data.data(), data.size());
}

std::string snapshot() {
    const uint8_t* rdram = recomp_runtime_get_rdram();
    dump_rdram_if_requested(rdram);
    if (rdram == nullptr || read32(rdram, kFragments + kBattleFrag * 8) == 0) {
        return "battle\t0\n";
    }
    const auto& names = move_names();

    // The active Pokemon's battle copy has live HP/PP; find which record it is.
    const uint32_t active = read32(rdram, kActivePtr);
    const bool have_active = in_rdram(active, 0x40);

    // Pick-3 battles keep the picks first, in pick order, with the count beside them.
    uint32_t count = read8(rdram, kTeamCount);
    if (count < 1 || count > 6) count = 6;

    std::string out = "battle\t1\n";
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t rec = kTeam + i * kRecordSize;
        std::array<uint8_t, 4> moves{};
        for (int m = 0; m < 4; ++m) moves[m] = read8(rdram, rec + 8 + m);
        if (moves[0] == 0) continue; // empty slot

        const uint8_t level = read8(rdram, rec + 0x23);
        bool is_active = have_active && read8(rdram, active + 0x26) == level;
        for (int m = 0; m < 4 && is_active; ++m) {
            is_active = read8(rdram, active + 0x1F + m) == moves[m];
        }

        std::string name;
        for (uint32_t c = 0; c < 11; ++c) {
            const char ch = static_cast<char>(read8(rdram, rec + 0x2F + c));
            if (ch == '\0') break;
            name += ch;
        }

        const uint16_t hp = is_active ? read16(rdram, active + 0x0C) : read16(rdram, rec + 1);
        out += "mon\t" + clean(name) + "\t" + std::to_string(level) + "\t" + std::to_string(hp) + "\t" +
               std::to_string(read16(rdram, rec + 0x25)) + "\t" + status_text(read8(rdram, rec + 4)) + "\t" +
               (is_active ? "1" : "0") + "\t" + (read8(rdram, rec + 0x24) != 0 || hp == 0 ? "1" : "0");
        for (int m = 0; m < 4; ++m) {
            const uint8_t pp = (is_active ? read8(rdram, active + 0x32 + m) : read8(rdram, rec + 0x1F + m)) & 0x3F;
            const std::string move = moves[m] != 0 && moves[m] < names.size() ? names[moves[m]] : "";
            out += "\t" + clean(move) + "\t" + (move.empty() ? "" : std::to_string(pp));
        }
        out += "\n";
    }
    return out;
}

} // namespace pkmnstadium::battle_panel

#if defined(__ANDROID__)
#include <jni.h>

// Called from BattlePanel.java on the UI thread a few times a second.
extern "C" JNIEXPORT jstring JNICALL
Java_io_github_bearfeet0511_stadiumrecomp_BattlePanel_nativeSnapshot(JNIEnv* env, jclass) {
    return env->NewStringUTF(pkmnstadium::battle_panel::snapshot().c_str());
}

// Called from BattlePanel.java when a move is tapped.
extern "C" JNIEXPORT void JNICALL
Java_io_github_bearfeet0511_stadiumrecomp_BattlePanel_nativePress(JNIEnv*, jclass, jint buttons) {
    pkmnstadium::battle_panel::press(static_cast<uint16_t>(buttons));
}
#endif
