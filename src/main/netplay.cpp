#include "netplay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define close_socket closesocket
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
#define close_socket close
#endif

#include <SDL.h>

#include "app_paths.h"
#include "transfer_pak.h"
#include "update_check.h"

extern "C" uint8_t* recomp_runtime_get_rdram(void);

#ifndef PSR_GIT_COMMIT
#define PSR_GIT_COMMIT "unknown"
#endif

namespace pkmnstadium::netplay {
namespace {
    constexpr uint32_t magic = 0x4E525350; // "PSRN"
    constexpr uint32_t protocol_version = 4;
    constexpr const char* stadium_save_name = "pokestadium.us.1.0.bin";
    constexpr int max_players = 4;
    constexpr uint32_t default_delay = 2;
    constexpr uint32_t hash_interval = 120;
    constexpr int mismatches_to_stop = 3;
    // The game heap. Audio, profiler and OS state live below it and differ
    // between machines without affecting the game (CLAUDE.md, determinism).
    constexpr uint32_t heap_start = 0x00100000;
    constexpr uint32_t heap_end = 0x00800000;

    enum MessageType : uint8_t {
        MSG_HELLO = 1, MSG_INPUT = 2, MSG_HASH = 3, MSG_SAVE = 4, MSG_BYE = 5,
        MSG_PING = 6, MSG_PONG = 7, MSG_START = 8,
    };
    // Input delay from measured latency: enough reads of lead time to cover
    // the slowest input path at about 30 controller reads per second, plus
    // margin. Pings are spread over about a second and the delay follows the
    // slow ones (second-slowest of ping_count, ~90th percentile), so Wi-Fi
    // jitter is covered while a single freak spike doesn't inflate it.
    constexpr int ping_count = 10;
    constexpr auto ping_spacing = std::chrono::milliseconds(100);
    constexpr double ms_per_read = 1000.0 / 30.0;
    constexpr uint32_t delay_margin = 2;
    constexpr uint32_t max_delay = 20;
    constexpr uint8_t unknown_port = 0xFF;

#pragma pack(push, 1)
    struct Header {
        uint8_t type;
        uint8_t pad[3];
        uint32_t length;
    };
    struct Hello {
        uint32_t magic;
        uint32_t protocol;
        char commit[41];
        uint8_t carts;
    };
    // Host -> guest once everyone has joined: who you are and how to play.
    struct Start {
        uint8_t port;
        uint8_t players;
        uint32_t delay;
    };
    struct InputMessage {
        uint8_t port;
        uint32_t read;
        uint16_t buttons;
        float x;
        float y;
    };
    struct HashMessage {
        uint8_t port;
        uint32_t read;
        uint64_t hash;
    };
#pragma pack(pop)

    struct InputValue {
        uint16_t buttons = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    // One TCP connection: the host has one per guest, a guest one to the host.
    struct Link {
        socket_t sock = INVALID_SOCKET;
        int port = 0; // the N64 port of the player on the other end
        std::mutex send_mutex;
        std::thread receiver;
    };

    struct Session {
        bool active = false;
        bool host = false;
        int players = 2;
        int local_port = 0;
        uint32_t delay = default_delay;
        uint32_t fixed_delay = 0; // PSR_NETPLAY_DELAY; 0 = from measured latency
        std::vector<std::unique_ptr<Link>> links;

        std::mutex mutex;
        std::condition_variable arrived;
        bool broken = false;          // a connection dropped; the session is over
        int left_port = -1;           // who quit (said goodbye), if anyone
        int lost_port = -1;           // whose connection dropped without a goodbye
        uint32_t reads[max_players] = {};
        // Highest input read received per port: how far that player's game has gotten.
        uint32_t latest[max_players] = {};
        std::unordered_map<uint32_t, InputValue> local_inputs;
        std::unordered_map<uint32_t, InputValue> inputs[max_players];
        std::unordered_map<uint32_t, uint64_t> local_hashes;
        std::unordered_map<uint32_t, uint64_t> hashes[max_players];
        int mismatches[max_players] = {};

        // Simulated network latency (PSR_NETPLAY_LATENCY_MS): incoming inputs
        // only become usable this long after they arrive (per hop, so a
        // relayed input is delayed twice, like on a real network).
        std::chrono::milliseconds latency{ 0 };
        struct Pending {
            std::chrono::steady_clock::time_point due;
            int port;
            uint32_t read;
            InputValue value;
        };
        std::vector<Pending> pending;

        // Pacing stats for netplay.log, per window of stats_interval reads.
        FILE* log_file = nullptr;
        std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
        double window_wait_ms = 0.0;
        double window_max_wait_ms = 0.0;
    } session;

    constexpr uint32_t stats_interval = 100;

    // To stderr and netplay.log next to the exe, which is kept even when the
    // game was started from a shortcut (so testers can send it in).
    void log(const std::string& message) {
        std::fprintf(stderr, "[netplay] %s\n", message.c_str());
        std::fflush(stderr);
        if (session.log_file != nullptr) {
            std::fprintf(session.log_file, "%s\n", message.c_str());
            std::fflush(session.log_file);
        }
    }

    std::string player_name(int port) {
        return "player " + std::to_string(port + 1);
    }

    void close_status();
    bool send_message(Link& link, MessageType type, const void* payload, uint32_t length);

    // Playing on after a failure would silently desync, so stop the game. The
    // host tells everyone else first, so they don't just see a lost connection.
    [[noreturn]] void fail(const std::string& message) {
        log(message);
        close_status();
        if (session.host) {
            const uint8_t me = static_cast<uint8_t>(session.local_port);
            for (auto& link : session.links) {
                send_message(*link, MSG_BYE, &me, sizeof(me));
            }
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Netplay", message.c_str(), nullptr);
        std::_Exit(1);
    }

    bool send_all(socket_t sock, const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            const int sent = ::send(sock, p, static_cast<int>(size), 0);
            if (sent <= 0) {
                return false;
            }
            p += sent;
            size -= static_cast<size_t>(sent);
        }
        return true;
    }

    bool recv_all(socket_t sock, void* data, size_t size) {
        char* p = static_cast<char*>(data);
        while (size > 0) {
            const int got = ::recv(sock, p, static_cast<int>(size), 0);
            if (got <= 0) {
                return false;
            }
            p += got;
            size -= static_cast<size_t>(got);
        }
        return true;
    }

    bool send_message(Link& link, MessageType type, const void* payload, uint32_t length) {
        std::scoped_lock lock(link.send_mutex);
        const Header header{ type, {}, length };
        return send_all(link.sock, &header, sizeof(header)) && send_all(link.sock, payload, length);
    }

    void send_to_all(MessageType type, const void* payload, uint32_t length, const Link* except = nullptr) {
        for (auto& link : session.links) {
            if (link.get() != except) {
                send_message(*link, type, payload, length);
            }
        }
    }

    bool recv_message(Link& link, Header& header, std::vector<uint8_t>& payload) {
        if (!recv_all(link.sock, &header, sizeof(header)) || header.length > 1 << 20) {
            return false;
        }
        payload.resize(header.length);
        return header.length == 0 || recv_all(link.sock, payload.data(), header.length);
    }

    uint64_t fnv_words(const uint8_t* data, size_t size) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i + 8 <= size; i += 8) {
            uint64_t w;
            std::memcpy(&w, data + i, 8);
            h = (h ^ w) * 1099511628211ull;
        }
        return h;
    }

    std::vector<char> read_stadium_save() {
        std::ifstream file(std::filesystem::current_path() / "saves" / stadium_save_name, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // Guests play the session on a copy of the host's Stadium save (as the 3DS
    // port does), in its own folder so their own save is never touched.
    void use_session_save(const std::vector<uint8_t>& bytes) {
        const std::filesystem::path dir = std::filesystem::current_path() / "saves" / "netplay-session";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        if (!bytes.empty()) {
            std::ofstream file(dir / stadium_save_name, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file) {
                fail("Couldn't write the session save to " + dir.string() + ".");
            }
        }
#ifdef _WIN32
        _putenv_s("RECOMP_SAVE_DIR", dir.string().c_str());
#else
        setenv("RECOMP_SAVE_DIR", dir.string().c_str(), 1);
#endif
        log("playing on the host's Stadium save (" + std::to_string(bytes.size()) + " bytes)");
    }

    // Status shown while connecting, so the window-less wait doesn't look like
    // nothing happened. Shown on its own thread so the connection proceeds
    // meanwhile; close_status() dismisses it once the game is starting.
    constexpr const char* status_title = "Netplay - connecting";
    std::atomic<bool> status_closed_by_game{ false };

    // The status has a single Cancel button: dismissing it means the player
    // gave up waiting, so quit instead of waiting on without a window.
    void show_status(const std::string& message) {
        status_closed_by_game = false;
        std::thread([message] {
            const SDL_MessageBoxButtonData cancel{
                SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" };
            const SDL_MessageBoxData box{ SDL_MESSAGEBOX_INFORMATION, nullptr, status_title,
                                          message.c_str(), 1, &cancel, nullptr };
            int pressed = 0;
            SDL_ShowMessageBox(&box, &pressed);
            if (!status_closed_by_game) {
                log("cancelled while connecting");
                std::_Exit(0);
            }
        }).detach();
    }

    void close_status() {
        status_closed_by_game = true;
#ifdef _WIN32
        // The box may still be opening; give it a moment.
        for (int attempt = 0; attempt < 20; attempt++) {
            if (HWND box = FindWindowA(nullptr, status_title)) {
                PostMessageA(box, WM_CLOSE, 0, 0);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#endif
    }

    [[noreturn]] void session_over() {
        int left, lost;
        {
            std::scoped_lock lock(session.mutex);
            left = session.left_port;
            lost = session.lost_port;
        }
        if (left >= 0) {
            fail(left == 0 && !session.host ? "The host ended the session."
                                            : "Player " + std::to_string(left + 1) + " left the game.");
        }
        if (lost >= 0 && session.host) {
            fail("Lost the connection to " + player_name(lost) + ".");
        }
        fail("Lost the connection to the host.");
    }

    bool any_cart() {
        for (int port = 0; port < max_players; port++) {
            if (transfer_pak::has_transfer_pak(port)) {
                return true;
            }
        }
        return false;
    }

    // Called with session.mutex held whenever a hash is recorded. Every guest
    // is checked against the host: the host compares each guest's hash with
    // its own; a guest compares the host's with its own.
    void compare_hashes(uint32_t read) {
        const auto local = session.local_hashes.find(read);
        if (local == session.local_hashes.end()) {
            return;
        }
        for (int port = 0; port < session.players; port++) {
            if (port == session.local_port) {
                continue;
            }
            const auto theirs = session.hashes[port].find(read);
            if (theirs == session.hashes[port].end()) {
                continue;
            }
            if (theirs->second == local->second) {
                session.mismatches[port] = 0;
            } else if (++session.mismatches[port] >= mismatches_to_stop) {
                fail("The games went out of sync (" + player_name(port) + ", controller read " +
                     std::to_string(read) + "), so the session was stopped.");
            } else {
                log("state hash mismatch with " + player_name(port) + " at read " + std::to_string(read) +
                    " (" + std::to_string(session.mismatches[port]) + " in a row)");
            }
            session.hashes[port].erase(theirs);
        }
        // Keep our own hashes around for slower players, but not forever.
        if (read >= 10 * hash_interval) {
            session.local_hashes.erase(read - 10 * hash_interval);
        }
    }

    void store_input(const InputMessage& m) {
        const InputValue value{ m.buttons, m.x, m.y };
        session.latest[m.port] = std::max(session.latest[m.port], m.read);
        if (session.latency.count() > 0) {
            session.pending.push_back({ std::chrono::steady_clock::now() + session.latency, m.port, m.read, value });
        } else {
            session.inputs[m.port][m.read] = value;
        }
        session.arrived.notify_all();
    }

    void receive_loop(Link* link) {
        Header header;
        std::vector<uint8_t> payload;
        while (recv_message(*link, header, payload)) {
            if (header.type == MSG_INPUT && payload.size() == sizeof(InputMessage)) {
                InputMessage m;
                std::memcpy(&m, payload.data(), sizeof(m));
                if (m.port >= session.players || m.port == session.local_port) {
                    continue;
                }
                {
                    std::scoped_lock lock(session.mutex);
                    store_input(m);
                }
                // The host relays each guest's input to the other guests.
                if (session.host) {
                    send_to_all(MSG_INPUT, &m, sizeof(m), link);
                }
            } else if (header.type == MSG_HASH && payload.size() == sizeof(HashMessage)) {
                HashMessage m;
                std::memcpy(&m, payload.data(), sizeof(m));
                if (m.port < session.players) {
                    std::scoped_lock lock(session.mutex);
                    session.hashes[m.port][m.read] = m.hash;
                    compare_hashes(m.read);
                }
            } else if (header.type == MSG_BYE) {
                const int port = payload.size() == 1 ? payload[0] : link->port;
                {
                    std::scoped_lock lock(session.mutex);
                    session.left_port = port;
                }
                if (session.host) {
                    const uint8_t who = static_cast<uint8_t>(port);
                    send_to_all(MSG_BYE, &who, sizeof(who), link);
                }
            }
        }
        // Connection gone: the session can't continue for anyone.
        std::scoped_lock lock(session.mutex);
        if (session.left_port < 0) {
            session.lost_port = link->port;
            if (session.host) {
                // Tell the others who dropped, so they don't wait forever.
                const uint8_t who = static_cast<uint8_t>(link->port);
                send_to_all(MSG_BYE, &who, sizeof(who), link);
            }
        }
        session.broken = true;
        session.arrived.notify_all();
    }

    void hash_state(uint32_t read) {
        const uint8_t* rdram = recomp_runtime_get_rdram();
        if (rdram == nullptr) {
            return;
        }
        const HashMessage m{ static_cast<uint8_t>(session.local_port), read,
                             fnv_words(rdram + heap_start, heap_end - heap_start) };
        // Guests check against the host, so only the host's hash goes to everyone.
        send_to_all(MSG_HASH, &m, sizeof(m));
        std::scoped_lock lock(session.mutex);
        session.local_hashes[read] = m.hash;
        compare_hashes(read);
    }

    void configure_socket(socket_t sock) {
        int no_delay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
    }

    // Both sides send HELLO and check the other's.
    void exchange_hello(Link& link) {
        Hello mine{};
        mine.magic = magic;
        mine.protocol = protocol_version;
        std::snprintf(mine.commit, sizeof(mine.commit), "%s", PSR_GIT_COMMIT);
        mine.carts = any_cart() ? 1 : 0;
        if (!send_message(link, MSG_HELLO, &mine, sizeof(mine))) {
            fail("Lost the connection during the handshake.");
        }
        Header header;
        std::vector<uint8_t> payload;
        if (!recv_message(link, header, payload) || header.type != MSG_HELLO || payload.size() != sizeof(Hello)) {
            fail("The other side didn't answer the netplay handshake.");
        }
        Hello theirs;
        std::memcpy(&theirs, payload.data(), sizeof(theirs));
        theirs.commit[sizeof(theirs.commit) - 1] = '\0';
        if (theirs.magic != magic || theirs.protocol != protocol_version) {
            fail("The other side isn't running a compatible version of Pokemon Stadium Recomp.");
        }
        if (std::strcmp(theirs.commit, mine.commit) != 0) {
            fail(std::string("Everyone needs the same version. Download the latest test build from\n") +
                 update_check::releases_page + "\n\n(Yours: " + std::string(mine.commit).substr(0, 7) +
                 ", theirs: " + std::string(theirs.commit).substr(0, 7) + ")");
        }
        // Carts stay unplugged in online play for now: controller_present()
        // reports no pak on any port, so every game boots identically.
        if (mine.carts || theirs.carts) {
            log("Transfer Pak carts are configured but stay unplugged during online play");
        }
    }

    // Host: ping one guest; returns its second-slowest round trip in ms.
    double measure_rtt(Link& link) {
        std::vector<double> rtts_ms;
        for (int i = 0; i < ping_count; i++) {
            if (i > 0) {
                std::this_thread::sleep_for(ping_spacing);
            }
            const uint32_t seq = static_cast<uint32_t>(i);
            const auto sent = std::chrono::steady_clock::now();
            Header header;
            std::vector<uint8_t> payload;
            if (!send_message(link, MSG_PING, &seq, sizeof(seq)) || !recv_message(link, header, payload) ||
                header.type != MSG_PONG) {
                fail("Lost the connection to " + player_name(link.port) + " while measuring latency.");
            }
            std::this_thread::sleep_for(session.latency); // simulated guest-to-host trip
            rtts_ms.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - sent).count());
        }
        std::sort(rtts_ms.begin(), rtts_ms.end());
        char line[200];
        std::snprintf(line, sizeof(line), "%s round trip best %.0f / median %.0f / slow %.0f ms",
                      player_name(link.port).c_str(), rtts_ms.front(), rtts_ms[rtts_ms.size() / 2],
                      rtts_ms[rtts_ms.size() - 2]);
        log(line);
        return rtts_ms[rtts_ms.size() - 2];
    }

    void host_session(uint16_t port, int players) {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener, max_players) != 0) {
            fail("Couldn't listen on port " + std::to_string(port) + ". Is it already in use?");
        }
        const int guests = players - 1;
        log("hosting on port " + std::to_string(port) + " for " + std::to_string(players) +
            " players, waiting for " + std::to_string(guests) + " to join");
        show_status("Hosting a " + std::to_string(players) + "-player game on port " + std::to_string(port) +
                    ", waiting for " + std::to_string(guests) + (guests == 1 ? " other player" : " other players") +
                    " to join.\n\nThe game starts as soon as everyone has connected. Cancel stops hosting.");

        // Guests become players 2, 3, 4 in the order they join.
        while (static_cast<int>(session.links.size()) < guests) {
            const socket_t client = ::accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                fail("Accepting a player's connection failed.");
            }
            configure_socket(client);
            auto link = std::make_unique<Link>();
            link->sock = client;
            link->port = static_cast<int>(session.links.size()) + 1;
            exchange_hello(*link);
            log(player_name(link->port) + " joined");
            session.links.push_back(std::move(link));
        }
        close_socket(listener);

        // Everyone gets the host's save; then measure each connection.
        const std::vector<char> save = read_stadium_save();
        double slowest_rtt_ms = 0.0;
        for (auto& link : session.links) {
            if (!send_message(*link, MSG_SAVE, save.data(), static_cast<uint32_t>(save.size()))) {
                fail("Lost the connection to " + player_name(link->port) + " while sending the Stadium save.");
            }
            slowest_rtt_ms = std::max(slowest_rtt_ms, measure_rtt(*link));
        }
        // An input's slowest path: guest to host (2 players), or guest to host
        // to another guest (3+), which is at most one full slow round trip.
        const double path_ms = players == 2 ? slowest_rtt_ms / 2.0 : slowest_rtt_ms;
        if (session.fixed_delay == 0) {
            const auto lead = static_cast<uint32_t>(std::ceil(path_ms / ms_per_read));
            session.delay = std::min(max_delay, std::max(default_delay, lead + delay_margin));
        } else {
            session.delay = session.fixed_delay;
        }
        log("slowest input path " + std::to_string(static_cast<int>(path_ms)) + " ms -> input delay " +
            std::to_string(session.delay) + " reads" + (session.fixed_delay ? " (fixed by PSR_NETPLAY_DELAY)" : ""));
        for (auto& link : session.links) {
            const Start start{ static_cast<uint8_t>(link->port), static_cast<uint8_t>(players), session.delay };
            if (!send_message(*link, MSG_START, &start, sizeof(start))) {
                fail("Lost the connection to " + player_name(link->port) + " while starting.");
            }
        }
        session.players = players;
        session.local_port = 0;
    }

    void join_session(const std::string& address, const std::string& port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (::getaddrinfo(address.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
            fail("Couldn't resolve the host address " + address + ".");
        }
        log("joining " + address + ":" + port);
        show_status("Connecting to " + address + ":" + port +
                    "...\n\nThe game starts as soon as the host and everyone else are ready. "
                    "Cancel stops trying.");
        // The host may still be starting up, so keep trying for a while.
        socket_t sock = INVALID_SOCKET;
        for (int attempt = 0; attempt < 60 && sock == INVALID_SOCKET; attempt++) {
            for (addrinfo* a = found; a != nullptr; a = a->ai_next) {
                socket_t s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (s != INVALID_SOCKET && ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) {
                    sock = s;
                    break;
                }
                if (s != INVALID_SOCKET) {
                    close_socket(s);
                }
            }
            if (sock == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        ::freeaddrinfo(found);
        if (sock == INVALID_SOCKET) {
            fail("Couldn't connect to " + address + ":" + port + ". Is the host running, and is the port forwarded?");
        }
        configure_socket(sock);
        auto link = std::make_unique<Link>();
        link->sock = sock;
        link->port = 0; // the host
        exchange_hello(*link);

        Header header;
        std::vector<uint8_t> payload;
        if (!recv_message(*link, header, payload) || header.type != MSG_SAVE) {
            fail("Didn't receive the host's Stadium save.");
        }
        use_session_save(payload);
        // Answer the host's latency pings until it starts the game (it may be
        // waiting for other players to join first).
        while (true) {
            if (!recv_message(*link, header, payload)) {
                fail("Lost the connection to the host while waiting to start.");
            }
            if (header.type == MSG_PING) {
                std::this_thread::sleep_for(session.latency); // simulated host-to-guest trip
                send_message(*link, MSG_PONG, payload.data(), static_cast<uint32_t>(payload.size()));
            } else if (header.type == MSG_START && payload.size() == sizeof(Start)) {
                Start start;
                std::memcpy(&start, payload.data(), sizeof(start));
                session.local_port = start.port;
                session.players = start.players;
                session.delay = start.delay;
                break;
            }
        }
        session.links.push_back(std::move(link));
    }
}

namespace {
    std::string trim(const std::string& s) {
        const size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            return "";
        }
        return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
    }

    bool all_digits(const std::string& s) {
        return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
    }

    // What people paste into the address box: "203.0.113.5", "host.example",
    // "203.0.113.5:7311", "[2001:db8::1]:7311" or a bare IPv6 address. A port
    // written with the address wins over the port box.
    void split_address(std::string& address, std::string& port) {
        address = trim(address);
        if (!address.empty() && address.front() == '[') {
            const size_t close = address.find(']');
            if (close != std::string::npos) {
                const std::string rest = address.substr(close + 1);
                if (rest.size() > 1 && rest[0] == ':' && all_digits(rest.substr(1))) {
                    port = rest.substr(1);
                }
                address = address.substr(1, close - 1);
            }
            return;
        }
        const size_t colon = address.find(':');
        if (colon != std::string::npos && address.find(':', colon + 1) == std::string::npos &&
            all_digits(address.substr(colon + 1))) {
            port = address.substr(colon + 1);
            address = address.substr(0, colon);
        }
    }

    // The launcher's ONLINE PLAY card, saved in launcher.cfg as
    // netplay=off|host|join, netplay_address, netplay_port, netplay_players.
    // Returns the same spec format as PSR_NETPLAY, or "" when online play is off.
    std::string spec_from_launcher() {
        std::ifstream file(exe_dir() / "launcher.cfg");
        std::string line, mode = "off", address, port = "7311", players = "2";
        while (std::getline(file, line)) {
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos) {
                continue;
            }
            const std::string key = trim(line.substr(0, eq));
            const std::string value = trim(line.substr(eq + 1));
            if (key == "netplay") mode = value;
            else if (key == "netplay_address") address = value;
            else if (key == "netplay_port" && all_digits(value)) port = value;
            else if (key == "netplay_players" && all_digits(value)) players = value;
        }
        if (mode == "host") {
            return "host:" + port + ":" + players;
        }
        if (mode == "join") {
            split_address(address, port);
            if (address.empty()) {
                fail("Online play is set to Join, but no host address was entered. "
                     "Type the host's address in the launcher's ONLINE PLAY card.");
            }
            return "join:" + address + ":" + port;
        }
        return "";
    }
}

bool active() {
    return session.active;
}

void start() {
    // PSR_NETPLAY (test scripts) overrides the launcher's setting.
    const char* setting = std::getenv("PSR_NETPLAY");
    const std::string spec = (setting != nullptr && setting[0] != '\0') ? std::string(setting)
                                                                       : spec_from_launcher();
    if (spec.empty()) {
        return;
    }
    const bool host = spec.rfind("host:", 0) == 0;
    const bool join = spec.rfind("join:", 0) == 0;
    if (!host && !join) {
        fail("PSR_NETPLAY must be host:<port>[:<players>] or join:<address>:<port>.");
    }
    session.host = host;
    if (const char* delay = std::getenv("PSR_NETPLAY_DELAY"); delay != nullptr && delay[0] != '\0') {
        session.fixed_delay = static_cast<uint32_t>(std::atoi(delay));
    }
    if (const char* latency = std::getenv("PSR_NETPLAY_LATENCY_MS"); latency != nullptr && latency[0] != '\0') {
        session.latency = std::chrono::milliseconds(std::atoi(latency));
    }
    session.log_file = std::fopen((std::filesystem::current_path() / "netplay.log").string().c_str(), "w");
    log(std::string("build ") + PSR_GIT_COMMIT + ", " + spec +
        (session.latency.count() > 0 ? ", simulated latency " + std::to_string(session.latency.count()) + " ms" : ""));

    // Every machine must read the same game clock.
#ifdef _WIN32
    _putenv_s("RECOMP_DETERMINISTIC_CLOCK", "1");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    setenv("RECOMP_DETERMINISTIC_CLOCK", "1", 1);
#endif

    if (host) {
        // host:<port>[:<players>]
        const std::string rest = spec.substr(5);
        const size_t colon = rest.find(':');
        const int port = std::atoi(rest.substr(0, colon).c_str());
        const int players = colon == std::string::npos ? 2 : std::atoi(rest.substr(colon + 1).c_str());
        host_session(static_cast<uint16_t>(port), std::clamp(players, 2, max_players));
    } else {
        const std::string rest = spec.substr(5);
        const size_t colon = rest.rfind(':');
        if (colon == std::string::npos) {
            fail("PSR_NETPLAY=join needs an address and a port, like join:203.0.113.5:7311.");
        }
        join_session(rest.substr(0, colon), rest.substr(colon + 1));
    }
    close_status();
    session.active = true;
    for (auto& link : session.links) {
        Link* raw = link.get();
        link->receiver = std::thread([raw] { receive_loop(raw); });
    }
    log("connected as " + player_name(session.local_port) + (session.host ? " (host)" : "") + " of " +
        std::to_string(session.players) + ", input delay " + std::to_string(session.delay));
}

bool get_input(int controller, LocalInputFn local, uint16_t* buttons, float* x, float* y) {
    if (controller < 0 || controller >= session.players) {
        return false;
    }
    const uint32_t read = session.reads[controller]++;
    if (controller == 0 && read % hash_interval == 0) {
        hash_state(read);
    }
    if (controller == 0 && read > 0 && read % stats_interval == 0) {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - session.window_start).count();
        std::string others;
        {
            std::scoped_lock lock(session.mutex);
            for (int port = 0; port < session.players; port++) {
                if (port != session.local_port) {
                    others += " P" + std::to_string(port + 1) + "=" + std::to_string(session.latest[port]);
                }
            }
        }
        char line[240];
        std::snprintf(line, sizeof(line),
                      "read %u: %.1f reads/s, waited %.0f ms total (max %.0f ms) in the last %u reads; "
                      "others have sent up to read%s",
                      read, stats_interval / seconds, session.window_wait_ms, session.window_max_wait_ms,
                      stats_interval, others.c_str());
        log(line);
        session.window_start = now;
        session.window_wait_ms = 0.0;
        session.window_max_wait_ms = 0.0;
    }

    InputValue value;
    if (controller == session.local_port) {
        InputValue live;
        local(&live.buttons, &live.x, &live.y);
        const InputMessage m{ static_cast<uint8_t>(controller), read + session.delay, live.buttons, live.x, live.y };
        // A guest sends to the host (which relays it); the host sends to every guest.
        for (auto& link : session.links) {
            if (!send_message(*link, MSG_INPUT, &m, sizeof(m))) {
                session_over();
            }
        }
        std::scoped_lock lock(session.mutex);
        session.local_inputs[read + session.delay] = live;
        if (auto it = session.local_inputs.find(read); it != session.local_inputs.end()) {
            value = it->second;
            session.local_inputs.erase(it);
        }
    } else if (read >= session.delay) {
        auto& inputs = session.inputs[controller];
        const auto wait_start = std::chrono::steady_clock::now();
        std::unique_lock lock(session.mutex);
        while (inputs.count(read) == 0 && !session.broken) {
            // Release simulated-latency inputs whose time has come.
            auto next_due = std::chrono::steady_clock::time_point::max();
            const auto now = std::chrono::steady_clock::now();
            for (auto p = session.pending.begin(); p != session.pending.end();) {
                if (p->due <= now) {
                    session.inputs[p->port][p->read] = p->value;
                    p = session.pending.erase(p);
                } else {
                    next_due = std::min(next_due, p->due);
                    ++p;
                }
            }
            if (inputs.count(read) != 0) {
                break;
            }
            // Wake at least once a second so a stall gets reported.
            const auto wake = std::min(next_due, now + std::chrono::seconds(1));
            session.arrived.wait_until(lock, wake);
            const double stalled_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - wait_start).count();
            if (stalled_s >= 2.0 && std::fmod(stalled_s, 5.0) < 1.0) {
                char line[160];
                std::snprintf(line, sizeof(line),
                              "still waiting for %s's read %u (%.0f s); %zu delayed inputs pending",
                              player_name(controller).c_str(), read, stalled_s, session.pending.size());
                log(line);
            }
        }
        const double waited_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - wait_start).count();
        session.window_wait_ms += waited_ms;
        session.window_max_wait_ms = std::max(session.window_max_wait_ms, waited_ms);
        const auto it = inputs.find(read);
        if (it == inputs.end()) {
            lock.unlock();
            session_over();
        }
        value = it->second;
        inputs.erase(it);
    }
    *buttons = value.buttons;
    *x = value.x;
    *y = value.y;
    return true;
}

bool controller_present(int controller) {
    return controller >= 0 && controller < session.players;
}

void shutdown() {
    if (!session.active) {
        return;
    }
    session.active = false;
    // Lets the others say "player N left" rather than "lost the connection".
    const uint8_t me = static_cast<uint8_t>(session.local_port);
    for (auto& link : session.links) {
        send_message(*link, MSG_BYE, &me, sizeof(me));
        close_socket(link->sock);
        if (link->receiver.joinable()) {
            link->receiver.detach();
        }
    }
}
}
