#pragma once

#include <cstdint>

// Online play by input lockstep for 2-4 players: host = player 1, guests =
// players 2-4 in join order. Each guest has one TCP connection to the host
// (the only one who forwards a port), and the host relays every player's
// input to everyone. Design and slice plan: plans/netplay.md.
//
// Configured in the launcher's ONLINE PLAY card (launcher.cfg netplay=,
// netplay_address=, netplay_port=, netplay_players=).
// PSR_NETPLAY=host:<port>[:<players>] or join:<address>:<port> overrides it; PSR_NETPLAY_DELAY=<reads> fixes the
// input delay (otherwise measured at connect); PSR_NETPLAY_LATENCY_MS
// simulates network latency for local testing.
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
    // session (one per player, the rest empty), identical on every machine.
    bool controller_present(int controller);

    // Closes the connection. Call once on quit.
    void shutdown();
}
