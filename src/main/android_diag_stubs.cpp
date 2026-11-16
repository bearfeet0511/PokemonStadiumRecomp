// android_diag_stubs.cpp — Android stand-ins for post_mortem.cpp and debug_server.cpp.
//
// Both are built on Win32 (DbgHelp stack walks, Winsock). On Android the shared
// state below still exists because the runner reads it, but nothing writes reports
// or listens on a port; logcat and adb cover that job for now.

#include "debug_server.h"
#include "post_mortem.hpp"

extern "C" void psr_post_mortem_dump(const char*, struct _EXCEPTION_POINTERS*) {}

namespace pkmnstadium {
namespace dbg {

void start(int) {}
void shutdown() {}

std::atomic<bool>     g_fast_forward{false};
std::atomic<bool>     g_enable_instant_present_request{false};
std::atomic<uint64_t> g_vi_ticks{0};
std::atomic<uint64_t> g_frame_count{0};

std::atomic<bool>     g_input_override_active{false};
std::atomic<uint16_t> g_buttons_override{0};
std::atomic<int>      g_stick_x_override{0};
std::atomic<int>      g_stick_y_override{0};

std::atomic<float>    g_audio_volume{1.0f};

std::atomic<uint64_t> g_send_dl_count{0};
std::atomic<uint64_t> g_update_screen_count{0};
std::atomic<uint64_t> g_send_dl_audio_count{0};
std::atomic<uint64_t> g_send_dl_gfx_count{0};
std::atomic<uint64_t> g_send_dl_other_count{0};

} // namespace dbg
} // namespace pkmnstadium
