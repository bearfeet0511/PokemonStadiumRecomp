#pragma once

#include <cstdint>

// Online play by input lockstep: host = player 1, guest = player 2, over one
// TCP connection the host makes reachable by forwarding a port. Design and
// slice plan: plans/netplay.md.
//
// Enabled by PSR_NETPLAY=host:<port> or PSR_NETPLAY=join:<address>:<port>;
// PSR_NETPLAY_DELAY=<reads> sets the input delay (default 2).
namespace pkmnstadium::netplay {
    // Reads the local controller (the device this machine's launcher assigns
    // to player 1) into the outputs; returns whether one is connected.
    using LocalInputFn = bool (*)(uint16_t* buttons, float* x, float* y);

    // True once a session is configured. Valid after start().
    bool active();

    // Connects and handshakes when PSR_NETPLAY is set; otherwise does
    // nothing. Call after the launcher and Transfer Pak setup, before the
    // game starts. Exits the process with a message if the session can't be
    // established, since playing on would silently desync.
    void start();

    // Lockstep controller read for N64 port `controller`. Replaces the
    // outputs with the session's input and returns whether the port responds.
    bool get_input(int controller, LocalInputFn local,
                   uint16_t* buttons, float* x, float* y);

    // Whether N64 port `controller` has a controller plugged in during the
    // session (ports 1-2 yes, 3-4 no), identical on both machines.
    bool controller_present(int controller);

    // Closes the connection. Call once on quit.
    void shutdown();
}
