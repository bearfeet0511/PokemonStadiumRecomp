#include "netplay_probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" uint8_t* recomp_runtime_get_rdram(void);

namespace pkmnstadium::netplay_probe {
namespace {
    constexpr int port_count = 4;
    constexpr uint32_t rdram_size = 8u * 1024u * 1024u;
    constexpr uint32_t block_size = 64u * 1024u;
    constexpr uint32_t block_count = rdram_size / block_size;

#pragma pack(push, 1)
    struct Record {
        uint32_t poll;
        uint8_t controller;
        uint8_t got_response;
        uint16_t buttons;
        float x;
        float y;
    };
#pragma pack(pop)

    struct Probe {
        FILE* record = nullptr;
        FILE* hash = nullptr;
        std::string hash_path;
        bool replay = false;
        std::vector<Record> replay_by_port[port_count];
        uint32_t polls[port_count] = {};
        long long dump_at = -1;
    };

    std::mutex probe_mutex;

    const char* env(const char* name) {
        const char* value = std::getenv(name);
        return (value != nullptr && value[0] != '\0') ? value : nullptr;
    }

    Probe& probe() {
        static Probe p = [] {
            Probe p;
            if (const char* path = env("PSR_INPUT_RECORD")) {
                p.record = std::fopen(path, "wb");
            }
            if (const char* path = env("PSR_INPUT_REPLAY")) {
                if (FILE* f = std::fopen(path, "rb")) {
                    Record r;
                    while (std::fread(&r, sizeof(r), 1, f) == 1) {
                        if (r.controller < port_count) {
                            p.replay_by_port[r.controller].push_back(r);
                        }
                    }
                    std::fclose(f);
                    p.replay = true;
                }
            }
            if (const char* path = env("PSR_STATE_HASH")) {
                p.hash_path = path;
                p.hash = std::fopen(path, "w");
            }
            if (const char* at = env("PSR_STATE_DUMP_AT")) {
                p.dump_at = std::atoll(at);
            }
            std::fprintf(stderr, "[netplay-probe] record=%d replay=%d hash=%d dump_at=%lld\n",
                         p.record != nullptr, p.replay, p.hash != nullptr, p.dump_at);
            return p;
        }();
        return p;
    }

    // 64-bit words mixed FNV-style: fast enough to hash all of RDRAM per frame.
    uint64_t hash_words(const uint8_t* data, size_t size) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i + 8 <= size; i += 8) {
            uint64_t w;
            std::memcpy(&w, data + i, 8);
            h = (h ^ w) * 1099511628211ull;
        }
        return h;
    }

    void log_state(Probe& p, uint32_t poll) {
        const uint8_t* rdram = recomp_runtime_get_rdram();
        if (rdram == nullptr) {
            return;
        }
        uint32_t blocks[block_count];
        uint64_t whole = 1469598103934665603ull;
        for (uint32_t b = 0; b < block_count; b++) {
            const uint64_t h = hash_words(rdram + b * block_size, block_size);
            blocks[b] = static_cast<uint32_t>(h ^ (h >> 32));
            whole = (whole ^ h) * 1099511628211ull;
        }
        std::fprintf(p.hash, "%u %016llx", poll, static_cast<unsigned long long>(whole));
        for (uint32_t b = 0; b < block_count; b++) {
            std::fprintf(p.hash, " %08x", blocks[b]);
        }
        std::fputc('\n', p.hash);
        if (p.dump_at == poll) {
            const std::string path = p.hash_path + ".rdram" + std::to_string(poll) + ".bin";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(rdram, 1, rdram_size, f);
                std::fclose(f);
            }
        }
    }
}

bool filter_input(int controller, bool got_response, uint16_t* buttons, float* x, float* y) {
    if (controller < 0 || controller >= port_count) {
        return got_response;
    }
    std::scoped_lock lock(probe_mutex);
    Probe& p = probe();
    const uint32_t poll = p.polls[controller]++;

    if (p.replay) {
        const auto& recs = p.replay_by_port[controller];
        if (poll < recs.size()) {
            got_response = recs[poll].got_response != 0;
            *buttons = recs[poll].buttons;
            *x = recs[poll].x;
            *y = recs[poll].y;
        } else {
            *buttons = 0;
            *x = 0.0f;
            *y = 0.0f;
        }
    }
    if (p.record != nullptr) {
        const Record r{ poll, static_cast<uint8_t>(controller), static_cast<uint8_t>(got_response),
                        *buttons, *x, *y };
        std::fwrite(&r, sizeof(r), 1, p.record);
        if ((poll & 63) == 0) {
            std::fflush(p.record);
        }
    }
    if (p.hash != nullptr && controller == 0) {
        log_state(p, poll);
        if ((poll & 63) == 0) {
            std::fflush(p.hash);
        }
    }
    return got_response;
}
}
