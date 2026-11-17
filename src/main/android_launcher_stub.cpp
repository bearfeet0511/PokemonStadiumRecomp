// android_launcher_stub.cpp — Android stand-in for recompui_launcher.cpp.
//
// The recomp-ui launcher is a desktop SDL/OpenGL window, so Android boots straight
// into the game for now: Player 1 on the first connected gamepad, settings fixed
// to what suits a phone GPU. A touch-friendly launcher replaces this later.
// Settings that already have a launcher.cfg key (aspect_ratio) are read from it.

#include "recompui_launcher.h"

#include <cctype>
#include <cstdio>
#include <fstream>

#include <SDL.h>

#include "app_paths.h"

namespace pkmnstadium::recompui {
namespace {

std::string cfg_value(const std::string& key) {
    std::ifstream cfg(exe_dir() / "launcher.cfg");
    std::string line;
    while (std::getline(cfg, line)) {
        if (line.rfind(key + "=", 0) == 0) {
            std::string value = line.substr(key.size() + 1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
            return value;
        }
    }
    return "";
}

} // namespace

bool run(const char* rom_path_in, char* out_rom, std::size_t out_len) {
    std::snprintf(out_rom, out_len, "%s", rom_path_in != nullptr ? rom_path_in : "");
    return true;
}

PortAssignment port_assignment(int port) {
    PortAssignment assignment;
    if (port != 0) {
        return assignment;
    }
    assignment.enabled = true;
    assignment.kind = DeviceKind::Gamepad;
    SDL_GameControllerUpdate();
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            assignment.gamepad_instance = static_cast<int>(SDL_JoystickGetDeviceInstanceID(i));
            break;
        }
    }
    return assignment;
}

bool startup_fullscreen() { return true; }
std::string startup_graphics_api() { return "vulkan"; }
int startup_ds_option() { return 1; }
int startup_msaa() { return 0; }
// Off by default, as on PC: launcher.cfg aspect_ratio=widescreen turns it on.
bool startup_widescreen() { return cfg_value("aspect_ratio") == "widescreen"; }
std::string startup_audio_device() { return ""; }

} // namespace pkmnstadium::recompui
