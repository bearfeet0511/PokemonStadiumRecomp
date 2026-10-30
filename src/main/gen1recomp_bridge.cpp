#include "gen1recomp_bridge.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <SDL.h>

#include "app_paths.h"
#include "save_backup.h"

namespace pkmnstadium::gen1recomp {
namespace {
    namespace fs = std::filesystem;

    constexpr int port_count = 4;

    std::mutex bridge_mutex;
    bool prepared[port_count] = {};

    // LuaJIT, the pinned Gen1Recomp sources (src\, tools\save_convert\) and
    // merge.lua, copied next to the exe by the build.
    fs::path tool_dir() { return exe_dir() / "gen1recomp"; }
    fs::path luajit_path() {
#ifdef _WIN32
        return tool_dir() / "luajit.exe";
#else
        return tool_dir() / "luajit";
#endif
    }

    // Per-port session files: the exported cart before Stadium ran, the cart
    // Stadium plays on, the slot it came from, and the converter's output.
    fs::path work_dir() { return exe_dir() / "saves" / "gen1recomp"; }
    fs::path port_file(int port, const char* suffix) {
        return work_dir() / ("p" + std::to_string(port + 1) + suffix);
    }
    fs::path before_path(int port) { return port_file(port, ".before.sav"); }
    fs::path cart_path(int port) { return port_file(port, ".cart.sav"); }
    fs::path state_path(int port) { return port_file(port, ".state"); }
    fs::path log_path(int port) { return port_file(port, ".log"); }
    // Slot entries of Pokemon that left a cart, per playthrough, so they get
    // their identity back when they return (see merge.lua).
    fs::path departed_dir() { return work_dir() / "departed"; }

    // Slots live at <identity>\saves\<game>\slotN.lua.
    std::string game_of(const fs::path& slot) {
        return slot.parent_path().filename().string();
    }

    // Gen1Recomp's ROM cache for a game (built when the user imports a ROM).
    fs::path rom_cache(const std::string& game) {
#ifdef _WIN32
        const char* base = std::getenv("APPDATA");
        fs::path root = base ? fs::path(base) : fs::path();
#else
        const char* xdg = std::getenv("XDG_DATA_HOME");
        const char* home = std::getenv("HOME");
        fs::path root = xdg ? fs::path(xdg) : (home ? fs::path(home) / ".local" / "share" : fs::path());
#endif
        return root / "pokemon-love2d" / game;
    }

    std::string to_utf8(const fs::path& path) {
        const auto s = path.u8string();
        return std::string(s.begin(), s.end());
    }

    fs::path from_utf8(const std::string& s) {
        return fs::path(std::u8string(s.begin(), s.end()));
    }

    std::vector<char> read_bytes(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // FNV-1a over the file: detects a slot rewritten since Stadium exported it.
    std::string file_hash(const fs::path& path) {
        uint64_t hash = 1469598103934665603ull;
        for (char c : read_bytes(path)) {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ull;
        }
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
        return buf;
    }

    void notify(const std::string& message) {
        std::fprintf(stderr, "[gen1recomp] %s\n", message.c_str());
        std::fflush(stderr);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Gen1Recomp save", message.c_str(), nullptr);
    }

    struct State {
        fs::path slot;
        std::string hash;
    };

    bool read_state(int port, State& out) {
        std::ifstream file(state_path(port), std::ios::binary);
        std::string line;
        while (std::getline(file, line)) {
            if (line.rfind("slot=", 0) == 0) out.slot = from_utf8(line.substr(5));
            else if (line.rfind("hash=", 0) == 0) out.hash = line.substr(5);
        }
        return !out.slot.empty() && !out.hash.empty();
    }

    void write_state(int port, const State& state) {
        std::ofstream file(state_path(port), std::ios::binary | std::ios::trunc);
        file << "slot=" << to_utf8(state.slot) << "\nhash=" << state.hash << "\n";
    }

    void clear_session(int port) {
        std::error_code ec;
        fs::remove(state_path(port), ec);
        fs::remove(before_path(port), ec);
        fs::remove(cart_path(port), ec);
        prepared[port] = false;
    }

    // Two programs writing one slot is how saves get corrupted, so nothing is
    // merged while Gen1Recomp is open.
    bool gen1recomp_running() {
#ifdef _WIN32
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return false;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
            if (_wcsicmp(entry.szExeFile, L"gen1recomp.exe") == 0) {
                found = true;
                break;
            }
        }
        CloseHandle(snapshot);
        return found;
#else
        return false;
#endif
    }

    // Runs LuaJIT from tool_dir() with the game's ROM cache on LUA_PATH, output
    // to the port's log. Returns the exit code (-1 if it couldn't start).
    int run_lua(int port, const std::string& game, const std::vector<fs::path>& args) {
        std::error_code ec;
        fs::create_directories(work_dir(), ec);
        const fs::path lua_path = rom_cache(game) / "?.lua;;";
#ifdef _WIN32
        std::wstring command = L"\"" + luajit_path().wstring() + L"\"";
        for (const fs::path& arg : args) {
            command += L" \"" + arg.wstring() + L"\"";
        }

        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        HANDLE log = CreateFileW(log_path(port).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = log;
        startup.hStdError = log;
        PROCESS_INFORMATION process{};

        // The child inherits this process's environment; set LUA_PATH for it only.
        wchar_t previous[4096];
        const DWORD had_previous = GetEnvironmentVariableW(L"LUA_PATH", previous, 4096);
        SetEnvironmentVariableW(L"LUA_PATH", lua_path.wstring().c_str());
        const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW, nullptr, tool_dir().c_str(),
                                            &startup, &process);
        SetEnvironmentVariableW(L"LUA_PATH", had_previous ? previous : nullptr);
        if (log != INVALID_HANDLE_VALUE) {
            CloseHandle(log);
        }
        if (!started) {
            return -1;
        }
        WaitForSingleObject(process.hProcess, 60000);
        DWORD exit_code = static_cast<DWORD>(-1);
        GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return static_cast<int>(exit_code);
#else
        std::string command = "cd '" + tool_dir().string() + "' && LUA_PATH='" + lua_path.string() +
                              "' '" + luajit_path().string() + "'";
        for (const fs::path& arg : args) {
            command += " '" + arg.string() + "'";
        }
        command += " > '" + log_path(port).string() + "' 2>&1";
        return std::system(command.c_str());
#endif
    }

    bool replace_file(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
        return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::error_code ec;
        fs::rename(from, to, ec);
        return !ec;
#endif
    }

    // Merges the port's cart back into its slot. Leaves the session pending
    // (to retry next launch) when it can't merge safely yet.
    void merge_port(int port) {
        State state;
        if (!read_state(port, state)) {
            return;
        }
        const std::string game = game_of(state.slot);
        const std::string tag = "gen1recomp-" + game + "-" + state.slot.stem().string();
        std::error_code ec;
        if (!fs::exists(cart_path(port), ec) || !fs::exists(before_path(port), ec)) {
            clear_session(port);
            return;
        }
        if (read_bytes(cart_path(port)) == read_bytes(before_path(port))) {
            std::fprintf(stderr, "[gen1recomp] port %d: no changes to merge\n", port + 1);
            clear_session(port);
            return;
        }
        if (gen1recomp_running()) {
            notify("Gen1Recomp is open, so Stadium's changes to " + state.slot.filename().string() +
                   " weren't merged yet. They're kept and will be merged the next time you start "
                   "Stadium with Gen1Recomp closed.");
            return;
        }
        if (!fs::exists(state.slot, ec) || file_hash(state.slot) != state.hash) {
            const fs::path kept = backup_save(cart_path(port), "gen1recomp-conflict-" + game);
            notify(state.slot.filename().string() + " changed after Stadium started (was it played "
                   "in Gen1Recomp meanwhile?). Stadium's changes were not merged, so newer progress "
                   "isn't overwritten. They're kept in:\n" + kept.string());
            clear_session(port);
            return;
        }

        fs::path merged = state.slot;
        merged += ".stadium-tmp";
        fs::remove(merged, ec);
        fs::create_directories(departed_dir(), ec);
        const int rc = run_lua(port, game, {
            tool_dir() / "merge.lua", state.slot, before_path(port), cart_path(port), merged, game,
            departed_dir() });
        if (rc != 0 || !fs::exists(merged, ec)) {
            notify("Merging Stadium's changes into " + state.slot.filename().string() +
                   " failed. They're kept and will be retried next launch. Details: " +
                   log_path(port).string());
            return;
        }
        backup_save(state.slot, tag);
        if (!replace_file(merged, state.slot)) {
            notify("Couldn't replace " + state.slot.string() + ". Stadium's changes are kept and "
                   "will be retried next launch.");
            return;
        }
        std::fprintf(stderr, "[gen1recomp] port %d: merged into %s\n", port + 1, state.slot.string().c_str());
        clear_session(port);
    }
}

bool is_slot(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".lua";
}

std::filesystem::path prepare_cart(int port, const std::filesystem::path& slot) {
    std::scoped_lock lock(bridge_mutex);
    if (port < 0 || port >= port_count) {
        return {};
    }
    std::error_code ec;
    // A crash or a busy Gen1Recomp can leave the last session unmerged.
    merge_port(port);
    if (fs::exists(state_path(port), ec)) {
        notify("Stadium's changes from last time still aren't merged into the save, so the "
               "Transfer Pak in port " + std::to_string(port + 1) + " was left empty.");
        return {};
    }

    if (!fs::is_regular_file(slot, ec)) {
        notify("Gen1Recomp save not found: " + slot.string());
        return {};
    }
    if (!fs::exists(luajit_path(), ec) || !fs::exists(tool_dir() / "merge.lua", ec) ||
        !fs::exists(tool_dir() / "tools" / "save_convert" / "convert.lua", ec)) {
        notify("Gen1Recomp support files are missing from " + tool_dir().string() +
               ". Rebuild after running tools\\setup_libs.ps1.");
        return {};
    }
    const std::string game = game_of(slot);
    if (!fs::exists(rom_cache(game) / "data" / "generated" / "pokemon.lua", ec)) {
        notify("Gen1Recomp's " + game + " ROM cache wasn't found in " + rom_cache(game).string() +
               ". Start Gen1Recomp once with your " + game + " ROM so it builds the cache.");
        return {};
    }

    fs::create_directories(work_dir(), ec);
    backup_save(slot, "gen1recomp-" + game + "-" + slot.stem().string());
    fs::remove(before_path(port), ec);
    fs::remove(cart_path(port), ec);
    const int rc = run_lua(port, game, {
        fs::path("tools") / "save_convert" / "convert.lua", "export", slot, before_path(port), game });
    if (rc != 0 || !fs::exists(before_path(port), ec)) {
        notify("Couldn't convert " + slot.filename().string() + " for Stadium. Details: " +
               log_path(port).string());
        return {};
    }
    fs::copy_file(before_path(port), cart_path(port), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        notify("Couldn't create the Transfer Pak cart in " + work_dir().string());
        return {};
    }
    write_state(port, State{ slot, file_hash(slot) });
    prepared[port] = true;
    std::fprintf(stderr, "[gen1recomp] port %d: %s -> %s\n", port + 1, slot.string().c_str(),
                 cart_path(port).string().c_str());
    return cart_path(port);
}

void finish_all() {
    std::scoped_lock lock(bridge_mutex);
    for (int port = 0; port < port_count; port++) {
        if (prepared[port]) {
            merge_port(port);
        }
    }
}
}
