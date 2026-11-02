#include "netplay.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

#include "transfer_pak.h"

extern "C" uint8_t* recomp_runtime_get_rdram(void);

#ifndef PSR_GIT_COMMIT
#define PSR_GIT_COMMIT "unknown"
#endif

namespace pkmnstadium::netplay {
namespace {
    constexpr uint32_t magic = 0x4E525350; // "PSRN"
    constexpr uint32_t protocol_version = 2;
    constexpr const char* stadium_save_name = "pokestadium.us.1.0.bin";
    constexpr uint32_t default_delay = 2;
    constexpr uint32_t hash_interval = 120;
    constexpr int mismatches_to_stop = 3;
    // The game heap. Audio, profiler and OS state live below it and differ
    // between machines without affecting the game (CLAUDE.md, determinism).
    constexpr uint32_t heap_start = 0x00100000;
    constexpr uint32_t heap_end = 0x00800000;

    enum MessageType : uint8_t { MSG_HELLO = 1, MSG_INPUT = 2, MSG_HASH = 3, MSG_SAVE = 4, MSG_BYE = 5 };

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
        uint32_t delay;
        uint8_t carts;
    };
    struct InputMessage {
        uint32_t read;
        uint16_t buttons;
        float x;
        float y;
    };
    struct HashMessage {
        uint32_t read;
        uint64_t hash;
    };
#pragma pack(pop)

    struct InputValue {
        uint16_t buttons = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Session {
        bool active = false;
        int local_port = 0;
        int remote_port = 1;
        uint32_t delay = default_delay;
        socket_t sock = INVALID_SOCKET;
        std::thread receiver;
        std::mutex mutex;
        std::mutex send_mutex;
        std::condition_variable arrived;
        bool connected = false;
        bool other_left = false;
        uint32_t reads[2] = {};
        std::unordered_map<uint32_t, InputValue> local_inputs;
        std::unordered_map<uint32_t, InputValue> remote_inputs;
        std::unordered_map<uint32_t, uint64_t> local_hashes;
        std::unordered_map<uint32_t, uint64_t> remote_hashes;
        int mismatches = 0;
    } session;

    void log(const std::string& message) {
        std::fprintf(stderr, "[netplay] %s\n", message.c_str());
        std::fflush(stderr);
    }

    // Playing on after a failure would silently desync, so stop the game.
    [[noreturn]] void fail(const std::string& message) {
        log(message);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Netplay", message.c_str(), nullptr);
        std::_Exit(1);
    }

    bool send_all(const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            const int sent = ::send(session.sock, p, static_cast<int>(size), 0);
            if (sent <= 0) {
                return false;
            }
            p += sent;
            size -= static_cast<size_t>(sent);
        }
        return true;
    }

    bool recv_all(void* data, size_t size) {
        char* p = static_cast<char*>(data);
        while (size > 0) {
            const int got = ::recv(session.sock, p, static_cast<int>(size), 0);
            if (got <= 0) {
                return false;
            }
            p += got;
            size -= static_cast<size_t>(got);
        }
        return true;
    }

    bool send_message(MessageType type, const void* payload, uint32_t length) {
        std::scoped_lock lock(session.send_mutex);
        const Header header{ type, {}, length };
        return send_all(&header, sizeof(header)) && send_all(payload, length);
    }

    bool recv_message(Header& header, std::vector<uint8_t>& payload) {
        if (!recv_all(&header, sizeof(header)) || header.length > 1 << 20) {
            return false;
        }
        payload.resize(header.length);
        return header.length == 0 || recv_all(payload.data(), header.length);
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

    // The guest plays the session on a copy of the host's Stadium save (as the
    // 3DS port does), in its own folder so the guest's save is never touched.
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

    [[noreturn]] void connection_lost() {
        bool left;
        {
            std::scoped_lock lock(session.mutex);
            left = session.other_left;
        }
        fail(left ? "The other player left the game." : "Lost the connection to the other player.");
    }

    bool any_cart() {
        for (int port = 0; port < 4; port++) {
            if (transfer_pak::has_transfer_pak(port)) {
                return true;
            }
        }
        return false;
    }

    // Called with session.mutex held, whenever either side's hash for `read`
    // is recorded.
    void compare_hashes(uint32_t read) {
        const auto local = session.local_hashes.find(read);
        const auto remote = session.remote_hashes.find(read);
        if (local == session.local_hashes.end() || remote == session.remote_hashes.end()) {
            return;
        }
        if (local->second == remote->second) {
            session.mismatches = 0;
        } else if (++session.mismatches >= mismatches_to_stop) {
            fail("The two games went out of sync (desync at controller read " +
                 std::to_string(read) + "), so the session was stopped.");
        } else {
            log("state hash mismatch at read " + std::to_string(read) + " (" +
                std::to_string(session.mismatches) + " in a row)");
        }
        session.local_hashes.erase(local);
        session.remote_hashes.erase(remote);
    }

    void receive_loop() {
        Header header;
        std::vector<uint8_t> payload;
        while (recv_message(header, payload)) {
            std::scoped_lock lock(session.mutex);
            if (header.type == MSG_INPUT && payload.size() == sizeof(InputMessage)) {
                InputMessage m;
                std::memcpy(&m, payload.data(), sizeof(m));
                session.remote_inputs[m.read] = InputValue{ m.buttons, m.x, m.y };
                session.arrived.notify_all();
            } else if (header.type == MSG_HASH && payload.size() == sizeof(HashMessage)) {
                HashMessage m;
                std::memcpy(&m, payload.data(), sizeof(m));
                session.remote_hashes[m.read] = m.hash;
                compare_hashes(m.read);
            } else if (header.type == MSG_BYE) {
                session.other_left = true;
            }
        }
        std::scoped_lock lock(session.mutex);
        session.connected = false;
        session.arrived.notify_all();
    }

    void hash_state(uint32_t read) {
        const uint8_t* rdram = recomp_runtime_get_rdram();
        if (rdram == nullptr) {
            return;
        }
        const HashMessage m{ read, fnv_words(rdram + heap_start, heap_end - heap_start) };
        send_message(MSG_HASH, &m, sizeof(m));
        std::scoped_lock lock(session.mutex);
        session.local_hashes[read] = m.hash;
        compare_hashes(read);
    }

    socket_t host_session(uint16_t port) {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener, 1) != 0) {
            fail("Couldn't listen on port " + std::to_string(port) + ". Is it already in use?");
        }
        log("hosting on port " + std::to_string(port) + ", waiting for the guest");
        // Listening already, so a guest that connects while this is open is
        // picked up as soon as it's dismissed.
        const std::string waiting = "Hosting on port " + std::to_string(port) +
            ".\n\nPress OK, then have the other player join using your address. "
            "The game starts when they connect.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Netplay", waiting.c_str(), nullptr);
        const socket_t client = ::accept(listener, nullptr, nullptr);
        close_socket(listener);
        if (client == INVALID_SOCKET) {
            fail("Accepting the guest's connection failed.");
        }
        return client;
    }

    socket_t join_session(const std::string& address, const std::string& port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (::getaddrinfo(address.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
            fail("Couldn't resolve the host address " + address + ".");
        }
        log("joining " + address + ":" + port);
        // The host may still be starting up, so keep trying for a while.
        for (int attempt = 0; attempt < 60; attempt++) {
            for (addrinfo* a = found; a != nullptr; a = a->ai_next) {
                socket_t sock = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (sock != INVALID_SOCKET &&
                    ::connect(sock, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) {
                    ::freeaddrinfo(found);
                    return sock;
                }
                if (sock != INVALID_SOCKET) {
                    close_socket(sock);
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        ::freeaddrinfo(found);
        fail("Couldn't connect to " + address + ":" + port + ". Is the host running, and is the port forwarded?");
    }

    void handshake(bool host) {
        Hello mine{};
        mine.magic = magic;
        mine.protocol = protocol_version;
        std::snprintf(mine.commit, sizeof(mine.commit), "%s", PSR_GIT_COMMIT);
        mine.delay = session.delay;
        mine.carts = any_cart() ? 1 : 0;
        if (!send_message(MSG_HELLO, &mine, sizeof(mine))) {
            fail("Lost the connection during the handshake.");
        }
        Header header;
        std::vector<uint8_t> payload;
        if (!recv_message(header, payload) || header.type != MSG_HELLO || payload.size() != sizeof(Hello)) {
            fail("The other side didn't answer the netplay handshake.");
        }
        Hello theirs;
        std::memcpy(&theirs, payload.data(), sizeof(theirs));
        theirs.commit[sizeof(theirs.commit) - 1] = '\0';
        if (theirs.magic != magic || theirs.protocol != protocol_version) {
            fail("The other side isn't running a compatible version of Pokemon Stadium Recomp.");
        }
        if (std::strcmp(theirs.commit, mine.commit) != 0) {
            fail(std::string("Both players need the same build. Yours: ") + mine.commit +
                 ", theirs: " + theirs.commit + ".");
        }
        if (mine.carts || theirs.carts) {
            fail("Transfer Pak carts aren't supported in netplay yet. Both players need to remove their carts.");
        }
        if (host) {
            const std::vector<char> save = read_stadium_save();
            if (!send_message(MSG_SAVE, save.data(), static_cast<uint32_t>(save.size()))) {
                fail("Lost the connection while sending the Stadium save.");
            }
        } else {
            session.delay = theirs.delay; // the host's setting wins
            if (!recv_message(header, payload) || header.type != MSG_SAVE) {
                fail("Didn't receive the host's Stadium save.");
            }
            use_session_save(payload);
        }
    }
}

bool active() {
    return session.active;
}

void start() {
    const char* setting = std::getenv("PSR_NETPLAY");
    if (setting == nullptr || setting[0] == '\0') {
        return;
    }
    const std::string spec = setting;
    const bool host = spec.rfind("host:", 0) == 0;
    const bool join = spec.rfind("join:", 0) == 0;
    if (!host && !join) {
        fail("PSR_NETPLAY must be host:<port> or join:<address>:<port>.");
    }
    if (const char* delay = std::getenv("PSR_NETPLAY_DELAY"); delay != nullptr && delay[0] != '\0') {
        session.delay = static_cast<uint32_t>(std::atoi(delay));
    }

    // Both machines must read the same game clock.
#ifdef _WIN32
    _putenv_s("RECOMP_DETERMINISTIC_CLOCK", "1");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    setenv("RECOMP_DETERMINISTIC_CLOCK", "1", 1);
#endif

    if (host) {
        session.sock = host_session(static_cast<uint16_t>(std::atoi(spec.c_str() + 5)));
    } else {
        const std::string rest = spec.substr(5);
        const size_t colon = rest.rfind(':');
        if (colon == std::string::npos) {
            fail("PSR_NETPLAY=join needs an address and a port, like join:203.0.113.5:7311.");
        }
        session.sock = join_session(rest.substr(0, colon), rest.substr(colon + 1));
    }
    int no_delay = 1;
    setsockopt(session.sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

    handshake(host);
    session.local_port = host ? 0 : 1;
    session.remote_port = host ? 1 : 0;
    session.connected = true;
    session.active = true;
    session.receiver = std::thread(receive_loop);
    log(std::string("connected as ") + (host ? "host (player 1)" : "guest (player 2)") +
        ", input delay " + std::to_string(session.delay));
}

bool get_input(int controller, LocalInputFn local, uint16_t* buttons, float* x, float* y) {
    if (controller < 0 || controller > 1) {
        return false;
    }
    const uint32_t read = session.reads[controller]++;
    if (controller == 0 && read % hash_interval == 0) {
        hash_state(read);
    }

    InputValue value;
    if (controller == session.local_port) {
        InputValue live;
        local(&live.buttons, &live.x, &live.y);
        const InputMessage m{ read + session.delay, live.buttons, live.x, live.y };
        if (!send_message(MSG_INPUT, &m, sizeof(m))) {
            connection_lost();
        }
        std::scoped_lock lock(session.mutex);
        session.local_inputs[read + session.delay] = live;
        if (auto it = session.local_inputs.find(read); it != session.local_inputs.end()) {
            value = it->second;
            session.local_inputs.erase(it);
        }
    } else if (read >= session.delay) {
        std::unique_lock lock(session.mutex);
        session.arrived.wait(lock, [&] {
            return session.remote_inputs.count(read) != 0 || !session.connected;
        });
        const auto it = session.remote_inputs.find(read);
        if (it == session.remote_inputs.end()) {
            lock.unlock();
            connection_lost();
        }
        value = it->second;
        session.remote_inputs.erase(it);
    }
    *buttons = value.buttons;
    *x = value.x;
    *y = value.y;
    return true;
}

bool controller_present(int controller) {
    return controller == 0 || controller == 1;
}

void shutdown() {
    if (!session.active) {
        return;
    }
    session.active = false;
    // Lets the other side say "the other player left" rather than "lost the connection".
    send_message(MSG_BYE, nullptr, 0);
    close_socket(session.sock);
    if (session.receiver.joinable()) {
        session.receiver.detach();
    }
}
}
