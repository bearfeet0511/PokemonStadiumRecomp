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
constexpr uint32_t kPickFrag = 0x48 - 0x10;   // fragment64, the pre-battle pick screen
constexpr uint32_t kTeamBlock = 0x800AE544;   // player's team block (D_800AE540 + 4; pret: unk_D_800AE540_0004)
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

std::filesystem::path rom_path() {
    std::filesystem::path rom;
    if (std::ifstream cfg(exe_dir() / "rom.cfg"); cfg) {
        std::string line;
        if (std::getline(cfg, line)) rom = line;
    }
    if (rom.empty() || !std::filesystem::exists(rom)) rom = exe_dir() / "baserom.z64";
    return rom;
}

uint32_t be32(const std::vector<char>& d, size_t at) {
    return (uint32_t(uint8_t(d[at])) << 24) | (uint32_t(uint8_t(d[at + 1])) << 16) |
           (uint32_t(uint8_t(d[at + 2])) << 8) | uint32_t(uint8_t(d[at + 3]));
}

std::vector<char> read_rom(std::ifstream& file, uint32_t offset, uint32_t size) {
    std::vector<char> d(size);
    file.seekg(offset);
    if (!file.read(d.data(), size)) d.clear();
    return d;
}

} // namespace

// Yay0 decompression (the format inside Stadium's PERS-SZP assets).
std::vector<uint8_t> yay0(const std::vector<char>& y) {
    if (y.size() < 16 || std::string(y.data(), 4) != "Yay0") return {};
    const uint32_t size = be32(y, 4);
    uint32_t links = be32(y, 8), chunks = be32(y, 12), masks = 16;
    std::vector<uint8_t> d;
    d.reserve(size);
    uint32_t mask = 0;
    int bits = 0;
    while (d.size() < size) {
        if (bits == 0) {
            if (masks + 4 > y.size()) return {};
            mask = be32(y, masks);
            masks += 4;
            bits = 32;
        }
        if (mask & 0x80000000u) {
            if (chunks >= y.size()) return {};
            d.push_back(uint8_t(y[chunks++]));
        } else {
            if (links + 2 > y.size()) return {};
            const uint32_t link = (uint32_t(uint8_t(y[links])) << 8) | uint8_t(y[links + 1]);
            links += 2;
            const uint32_t dist = (link & 0xFFF) + 1;
            uint32_t count = link >> 12;
            if (count == 0) {
                if (chunks >= y.size()) return {};
                count = uint8_t(y[chunks++]) + 18;
            } else {
                count += 2;
            }
            if (dist > d.size()) return {};
            for (uint32_t i = 0; i < count && d.size() < size; ++i) d.push_back(d[d.size() - dist]);
        }
        mask <<= 1;
        --bits;
    }
    return d;
}

// The Stadium logo for the idle bottom screen, read from the player's ROM like
// the move names. The minigame demos draw it from the kids_club_game_ui asset
// (US v1.0 ROM 0x675FA0-0x696350, pret's rom.yaml; loaded at segment 3 by
// 29BA0.c): fragment2.c func_878021B4 draws D_304F120 as 100x56 RGBA32 in 100x8
// strips. The asset is PERS-SZP: a header (its size at +8), then Yay0 data.
std::vector<uint8_t> logo_rgba() {
    constexpr uint32_t kStart = 0x675FA0, kEnd = 0x696350, kLogo = 0x4F120;
    std::ifstream file(rom_path(), std::ios::binary);
    if (!file) return {};
    const auto blob = read_rom(file, kStart, kEnd - kStart);
    if (blob.size() < 0x20 || std::string(blob.data(), 8) != "PERS-SZP") return {};
    const uint32_t header = be32(blob, 8);
    if (header >= blob.size()) return {};
    const auto data = yay0(std::vector<char>(blob.begin() + header, blob.end()));
    constexpr uint32_t kBytes = kLogoWidth * kLogoHeight * 4;
    if (data.size() < kLogo + kBytes) return {};
    return std::vector<uint8_t>(data.begin() + kLogo, data.begin() + kLogo + kBytes);
}

namespace {

// Move names come from the player's own ROM (a NUL-separated list in move-ID
// order), so no game text is copied into this project.
const std::vector<std::string>& move_names() {
    static std::vector<std::string> names;
    static std::once_flag loaded;
    std::call_once(loaded, [] {
        std::ifstream file(rom_path(), std::ios::binary);
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

// Keeps printable ASCII only: the text goes to Java through NewStringUTF, which
// aborts the app on bytes that aren't valid modified UTF-8 (game memory can hold
// anything, e.g. a stale record on the pick screen), and tabs/newlines split fields.
std::string clean(std::string text) {
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7F) out += c;
        else if (c == '\t' || c == '\n') out += ' ';
    }
    return out;
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
//
// Switching works the same way (func_8430E8E8 / func_8430E72C): B at the top
// menu sets mode 2, the next frame sets step 2 (a forced switch after a faint
// goes straight there), and then a button picks a team slot in pick order:
// with fewer than 4 in battle C-up/C-right/C-down; with more, C-up, C-left, B,
// C-right, C-down, A. The count is the side's team (+0x720 -> unk_08[0]) +0x002.
constexpr uint32_t kSideStates = 0x90010;     // fragment offset of D_84390010[2]
constexpr uint32_t kMenuBlock = 0x654;
constexpr uint32_t kControllers = 0x800A7320; // gControllers; Player 1 is the first
constexpr uint16_t kButtonA = 0x8000;
constexpr uint16_t kButtonB = 0x4000;
constexpr uint16_t kSwitch3[3] = { 0x0008, 0x0001, 0x0004 };
constexpr uint16_t kSwitch6[6] = { 0x0008, 0x0002, 0x4000, 0x0001, 0x0004, 0x8000 };
constexpr int64_t kHoldMs = 100;      // long enough for a couple of controller polls
constexpr int64_t kGiveUpMs = 1500;   // the move list never came up

enum class TapPhase { Idle, Start, Open, WaitMenu, Pick };
enum class Menu { Unknown, First, Moves, Switch };

static std::mutex s_tap_mutex;
static TapPhase s_phase = TapPhase::Idle;
static Menu s_target = Menu::Moves;   // the submenu the tap's button belongs to
static uint16_t s_pick_button = 0;    // a move button, or 0 for a switch (resolved later)
static int s_switch_slot = -1;
static int64_t s_phase_start = 0;
static int64_t s_tap_start = 0;

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Player 1's menu block in the battle engine, or 0.
static uint32_t player_menu(const uint8_t* rdram) {
    if (rdram == nullptr) return 0;
    const uint32_t frag = read32(rdram, kFragments + kBattleFrag * 8);
    if (frag == 0 || !in_rdram(frag + kSideStates, 8)) return 0;
    for (uint32_t side = 0; side < 2; ++side) {
        const uint32_t state = read32(rdram, frag + kSideStates + side * 4);
        if (!in_rdram(state, 0x724)) continue;
        if (read32(rdram, state + kMenuBlock + 0xC8) == kControllers) return state + kMenuBlock;
    }
    return 0;
}

static Menu battle_menu(const uint8_t* rdram) {
    const uint32_t menu = player_menu(rdram);
    if (menu == 0) return Menu::Unknown;
    const uint32_t mode = read32(rdram, menu + 0x04);
    const uint32_t step = read32(rdram, menu + 0x10);
    if (mode == 0 && step == 0) return Menu::First;
    if (mode == 1 && step == 1) return Menu::Moves;
    if (mode == 2 && step == 2) return Menu::Switch;
    return Menu::Unknown;
}

// The button that sends out team slot `slot`, or 0.
static uint16_t switch_button(const uint8_t* rdram, int slot) {
    const uint32_t menu = player_menu(rdram);
    if (menu == 0 || slot < 0) return 0;
    const uint32_t side = read32(rdram, menu - kMenuBlock + 0x720);
    if (!in_rdram(side, 0x10)) return 0;
    const uint32_t team = read32(rdram, side + 0x08);
    if (!in_rdram(team, 4)) return 0;
    const int count = read8(rdram, team + 0x02);
    if (slot >= count) return 0;
    return count < 4 ? (slot < 3 ? kSwitch3[slot] : 0) : (slot < 6 ? kSwitch6[slot] : 0);
}

static const char* menu_name(Menu menu) {
    switch (menu) {
        case Menu::First: return "first";
        case Menu::Moves: return "moves";
        case Menu::Switch: return "switch";
        default: return "unknown";
    }
}

void press(uint16_t buttons) {
    std::lock_guard lock(s_tap_mutex);
    s_target = Menu::Moves;
    s_pick_button = buttons;
    s_switch_slot = -1;
    s_phase = TapPhase::Start;
    s_tap_start = s_phase_start = now_ms();
}

void send_out(int slot) {
    std::lock_guard lock(s_tap_mutex);
    s_target = Menu::Switch;
    s_pick_button = 0;
    s_switch_slot = slot;
    s_phase = TapPhase::Start;
    s_tap_start = s_phase_start = now_ms();
}

// Called on every controller poll for Player 1, on the game thread. Walks a tap
// through: (at the top menu) press A or B, wait for the submenu, press the pick.
uint16_t tapped_buttons() {
    std::lock_guard lock(s_tap_mutex);
    if (s_phase == TapPhase::Idle) return 0;
    const int64_t now = now_ms();
    const uint8_t* rdram = recomp_runtime_get_rdram();
    const Menu menu = battle_menu(rdram);
    const uint16_t opener = s_target == Menu::Switch ? kButtonB : kButtonA;

    if (s_phase == TapPhase::Start) {
        std::fprintf(stderr, "[battle-panel] tap %s %#06x slot %d at menu %s\n", menu_name(s_target),
                     s_pick_button, s_switch_slot, menu_name(menu));
        if (menu == s_target) {
            s_phase = TapPhase::Pick;
        } else if (menu == Menu::First) {
            s_phase = TapPhase::Open;
        } else {
            s_phase = TapPhase::Idle; // not a menu this tap belongs to: press nothing
            return 0;
        }
        s_phase_start = now;
        if (s_phase == TapPhase::Pick && s_target == Menu::Switch) {
            s_pick_button = switch_button(rdram, s_switch_slot);
            if (s_pick_button == 0) s_phase = TapPhase::Idle;
        }
    }

    switch (s_phase) {
        case TapPhase::Open:
            if (now - s_phase_start < kHoldMs) return opener;
            s_phase = TapPhase::WaitMenu;
            s_phase_start = now;
            return 0;
        case TapPhase::WaitMenu:
            if (menu == s_target) {
                if (s_target == Menu::Switch) s_pick_button = switch_button(rdram, s_switch_slot);
                s_phase = s_pick_button != 0 ? TapPhase::Pick : TapPhase::Idle;
                s_phase_start = now;
                return s_pick_button;
            }
            if (now - s_tap_start > kGiveUpMs) {
                std::fprintf(stderr, "[battle-panel] %s menu didn't open (menu %s)\n", menu_name(s_target),
                             menu_name(menu));
                s_phase = TapPhase::Idle;
            }
            return 0;
        case TapPhase::Pick:
            if (now - s_phase_start < kHoldMs) return s_pick_button;
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

static std::string record_name(const uint8_t* rdram, uint32_t rec) {
    std::string name;
    for (uint32_t c = 0; c < 11; ++c) {
        const char ch = static_cast<char>(read8(rdram, rec + 0x2F + c));
        if (ch == '\0') break;
        name += ch;
    }
    return clean(name);
}

void hold(uint16_t buttons) {
    std::lock_guard lock(s_tap_mutex);
    s_target = Menu::Unknown;
    s_pick_button = buttons;
    s_switch_slot = -1;
    s_phase = TapPhase::Pick;
    s_tap_start = s_phase_start = now_ms();
}

std::string snapshot() {
    const uint8_t* rdram = recomp_runtime_get_rdram();
    dump_rdram_if_requested(rdram);
    if (rdram == nullptr) return "battle\t0\n";
    if (read32(rdram, kFragments + kBattleFrag * 8) == 0) {
        // The pre-battle pick screen (fragment64, fragment id 0x38): the team's
        // records are in slot order, which the buttons pick (fragment64.c,
        // func_848027F0): B, C-left, C-up, A, C-down, C-right; L undoes a pick.
        if (read32(rdram, kFragments + kPickFrag * 8) == 0) return "battle\t0\n";
        std::string out = "pick\t1\n";
        // The pick screen lists the full roster (fragment64.c func_84802F7C: the team
        // block's +0x214 points to it; count at +0x002, records at +0x028), not the
        // team list at kTeam, which after a cup or Gym Leader Castle battle holds
        // only that battle's picks, in pick order.
        uint32_t first = kTeam;
        uint32_t count = read8(rdram, kTeamCount);
        const uint32_t roster = read32(rdram, kTeamBlock + 0x214);
        if (in_rdram(roster, 0x28 + 6 * kRecordSize)) {
            const uint32_t roster_count = read8(rdram, roster + 0x002);
            if (roster_count >= 1 && roster_count <= 6) {
                first = roster + 0x028 + (kTeam - (kTeamBlock + 0x01C)); // same record layout
                count = roster_count;
            }
        }
        if (count < 1 || count > 6) count = 6;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t rec = first + i * kRecordSize;
            if (read8(rdram, rec + 8) == 0) continue; // empty slot
            out += "slot\t" + std::to_string(i) + "\t" + record_name(rdram, rec) + "\t" +
                   std::to_string(read8(rdram, rec + 0x23)) + "\n";
        }
        return out;
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

        const uint16_t hp = is_active ? read16(rdram, active + 0x0C) : read16(rdram, rec + 1);
        out += "mon\t" + record_name(rdram, rec) + "\t" + std::to_string(level) + "\t" + std::to_string(hp) + "\t" +
               std::to_string(read16(rdram, rec + 0x25)) + "\t" + status_text(read8(rdram, rec + 4)) + "\t" +
               (is_active ? "1" : "0") + "\t" + (read8(rdram, rec + 0x24) != 0 || hp == 0 ? "1" : "0");
        for (int m = 0; m < 4; ++m) {
            const uint8_t pp = (is_active ? read8(rdram, active + 0x32 + m) : read8(rdram, rec + 0x1F + m)) & 0x3F;
            const std::string move = moves[m] != 0 && moves[m] < names.size() ? names[moves[m]] : "";
            out += "\t" + clean(move) + "\t" + (move.empty() ? "" : std::to_string(pp));
        }
        // Team slot (pick order) and the button that sends it out.
        out += "\t" + std::to_string(i) + "\t" + std::to_string(switch_button(rdram, static_cast<int>(i)));
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

// Called by the launcher before anything else: the app folder, since SDL (which
// exe_dir() asks otherwise) isn't running yet.
extern "C" JNIEXPORT void JNICALL
Java_io_github_bearfeet0511_stadiumrecomp_BattlePanel_nativeSetDataDir(JNIEnv* env, jclass, jstring path) {
    const char* utf = env->GetStringUTFChars(path, nullptr);
    if (utf != nullptr) {
        pkmnstadium::android_data_dir() = utf;
        env->ReleaseStringUTFChars(path, utf);
    }
}

// Called once from BattlePanel.java: the logo as ARGB ints (100 x 56), or null.
extern "C" JNIEXPORT jintArray JNICALL
Java_io_github_bearfeet0511_stadiumrecomp_BattlePanel_nativeLogo(JNIEnv* env, jclass) {
    using namespace pkmnstadium::battle_panel;
    const auto rgba = logo_rgba();
    if (rgba.empty()) return nullptr;
    std::vector<jint> argb(kLogoWidth * kLogoHeight);
    for (size_t i = 0; i < argb.size(); ++i) {
        const uint8_t* p = &rgba[i * 4];
        argb[i] = static_cast<jint>((uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]);
    }
    jintArray out = env->NewIntArray(static_cast<jsize>(argb.size()));
    if (out != nullptr) env->SetIntArrayRegion(out, 0, static_cast<jsize>(argb.size()), argb.data());
    return out;
}

// Called from BattlePanel.java for a plain button (the pick screen's buttons).
extern "C" JNIEXPORT void JNICALL
Java_io_github_bearfeet0511_stadiumrecomp_BattlePanel_nativeHold(JNIEnv*, jclass, jint buttons) {
    pkmnstadium::battle_panel::hold(static_cast<uint16_t>(buttons));
}

// Called from BattlePanel.java when a team card is confirmed: send out that slot.
extern "C" JNIEXPORT void JNICALL
Java_io_github_bearfeet0511_stadiumrecomp_BattlePanel_nativeSwitch(JNIEnv*, jclass, jint slot) {
    pkmnstadium::battle_panel::send_out(static_cast<int>(slot));
}
#endif
