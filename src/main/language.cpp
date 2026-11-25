// language.cpp - see language.h.
//
// Stadium keeps all its text in one archive, "textdata" (pret's name; US ROM
// 0x783760). Layout: 8 zero bytes, u32 total size, u32 section count, then one
// 16-byte entry per section (u32 offset from the archive start, u32 size, two
// words the game uses at run time). The game loads the header once
// (2E110.c func_8002D510) and each section on demand from archive + offset.
//
// The European versions are different builds of the code, but their textdata
// has the same layout and the same sections in the same order, plus one
// European-only section at the end that the US code never asks for. So we
// leave the US code alone and:
//   - put the other language's whole archive in the runtime's ROM extension
//     (served to PI reads at ROM offset 0x02000000 and up), and
//   - rewrite the US header in place so each of the 46 US sections points
//     there (offset = 0x02000000 - 0x783760 + its offset in that archive).
// Nothing from any ROM is stored in this project; it's read at start-up.

#include "language.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "app_paths.h"
#include "librecomp/game.hpp"

namespace pkmnstadium::language {
namespace {

constexpr uint32_t kUsTextdata = 0x783760;
constexpr uint32_t kUsSections = 46;
constexpr uint32_t kHeaderSize = 0x10 + kUsSections * 0x10;

std::mutex g_mutex;
std::vector<uint8_t> g_archive; // the archive the game reads (empty = US, from the ROM)

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

std::string cfg_value(const std::string& key) {
    std::ifstream cfg(exe_dir() / "launcher.cfg");
    std::string line;
    while (std::getline(cfg, line)) {
        if (line.rfind(key + "=", 0) == 0) {
            std::string v = line.substr(key.size() + 1);
            while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
            return v;
        }
    }
    return "";
}

// Reads an N64 ROM in any of the three byte orders and returns it big-endian (.z64).
std::vector<uint8_t> read_z64(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (rom.size() < 0x1000) return {};
    if (rom[0] == 0x37 && rom[1] == 0x80) { // .v64: 16-bit swapped
        for (size_t i = 0; i + 1 < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
    } else if (rom[0] == 0x40 && rom[1] == 0x12) { // .n64: 32-bit little-endian
        for (size_t i = 0; i + 3 < rom.size(); i += 4) {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
    }
    if (rom[0] != 0x80 || rom[1] != 0x37) return {};
    return rom;
}

// Checks a textdata archive at `at`: sane header, every section inside it.
bool valid_archive(const std::vector<uint8_t>& rom, size_t at, uint32_t min_sections) {
    if (at + 0x10 > rom.size()) return false;
    const uint8_t* p = rom.data() + at;
    if (be32(p) != 0 || be32(p + 4) != 0) return false;
    const uint32_t size = be32(p + 8), count = be32(p + 12);
    if (count < min_sections || count > 64 || size < 0x10 + count * 0x10 || size > 0x40000) return false;
    if (at + size > rom.size()) return false;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t off = be32(p + 0x10 + k * 0x10), sz = be32(p + 0x14 + k * 0x10);
        if (off < 0x10 + count * 0x10 || off > size || sz > size - off) return false;
    }
    return true;
}

// Finds the text archive in another version of Stadium: a valid archive with
// 46-47 sections whose section 37 is the move-name table (165 strings, the
// first starting right after the table), as in the US one.
size_t find_archive(const std::vector<uint8_t>& rom) {
    constexpr uint32_t kMoveSection = 37, kMoves = 165;
    for (size_t at = 0x100000; at + 0x400 < rom.size(); at += 0x10) {
        if (rom[at + 8] != 0 || rom[at + 12] != 0 || rom[at + 13] != 0 || rom[at + 14] != 0) continue;
        const uint32_t count = rom[at + 15];
        if (count != kUsSections && count != kUsSections + 1) continue;
        if (!valid_archive(rom, at, kUsSections)) continue;
        const uint8_t* moves = rom.data() + at + be32(rom.data() + at + 0x10 + kMoveSection * 0x10);
        if (be32(moves) != kMoves || be32(moves + 4) != 4 + kMoves * 4) continue;
        return at;
    }
    return 0;
}

} // namespace

void apply() {
    const std::string lang = cfg_value("language");
    if (lang.empty() || lang == "en") return;
    const char region = lang == "es" ? 'S' : lang == "fr" ? 'F' : 0;
    if (region == 0) {
        std::fprintf(stderr, "[language] unknown language '%s'; keeping English\n", lang.c_str());
        return;
    }
    std::filesystem::path path = cfg_value("language_rom");
    if (!path.empty() && path.is_relative()) path = exe_dir() / path;
    if (path.empty() || !std::filesystem::exists(path)) {
        std::fprintf(stderr, "[language] %s chosen but its Stadium ROM isn't set or is missing; keeping English\n", lang.c_str());
        return;
    }
    const std::vector<uint8_t> other = read_z64(path);
    // Game code at 0x3B: "NPO" + region (E = US, S = Spain, F = France).
    if (other.size() < 0x40 || other[0x3B] != 'N' || other[0x3C] != 'P' || other[0x3D] != 'O' || other[0x3E] != uint8_t(region)) {
        std::fprintf(stderr, "[language] %s isn't the %s version of Pokemon Stadium; keeping English\n",
                     path.filename().string().c_str(), lang == "es" ? "Spanish" : "French");
        return;
    }
    const size_t at = find_archive(other);
    const std::span<const uint8_t> us = recomp::get_rom();
    if (at == 0 || us.size() < kUsTextdata + kHeaderSize || be32(us.data() + kUsTextdata + 12) != kUsSections) {
        std::fprintf(stderr, "[language] text not found (other ROM at 0x%zX); keeping English\n", at);
        return;
    }
    std::vector<uint8_t> archive(other.begin() + at, other.begin() + at + be32(other.data() + at + 8));

    // The US header, with every section pointing into the other archive.
    std::vector<uint8_t> header(us.begin() + kUsTextdata, us.begin() + kUsTextdata + kHeaderSize);
    put_be32(header.data() + 8, uint32_t(archive.size()));
    for (uint32_t k = 0; k < kUsSections; ++k) {
        uint8_t* e = header.data() + 0x10 + k * 0x10;
        put_be32(e, recomp::rom_extension_base - kUsTextdata + be32(archive.data() + 0x10 + k * 0x10));
        put_be32(e + 4, be32(archive.data() + 0x14 + k * 0x10));
        put_be32(e + 8, 0);
        put_be32(e + 12, 0);
    }
    if (!recomp::patch_rom(kUsTextdata, header.data(), header.size())) {
        std::fprintf(stderr, "[language] couldn't patch the text header; keeping English\n");
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        g_archive = archive;
    }
    recomp::set_rom_extension(std::move(archive));
    std::fprintf(stderr, "[language] %s text from %s (archive at 0x%zX, %u sections)\n",
                 lang.c_str(), path.filename().string().c_str(), at, be32(other.data() + at + 12));
}

std::vector<std::string> text_section(int section) {
    std::vector<uint8_t> archive;
    {
        std::lock_guard lock(g_mutex);
        archive = g_archive;
    }
    if (archive.empty()) {
        const std::span<const uint8_t> rom = recomp::get_rom();
        if (rom.size() < kUsTextdata + 0x10) return {};
        const uint32_t size = be32(rom.data() + kUsTextdata + 8);
        if (rom.size() < kUsTextdata + size) return {};
        archive.assign(rom.begin() + kUsTextdata, rom.begin() + kUsTextdata + size);
    }
    if (section < 0 || uint32_t(section) >= be32(archive.data() + 12)) return {};
    const uint32_t off = be32(archive.data() + 0x10 + section * 0x10);
    const uint32_t size = be32(archive.data() + 0x14 + section * 0x10);
    if (off + size > archive.size() || size < 4) return {};
    const uint8_t* d = archive.data() + off;
    // A string table: u32 count, then u32 offsets from the section start.
    const uint32_t count = be32(d);
    if (count == 0 || 4 + count * 4 > size) return {};
    std::vector<std::string> out;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t s = be32(d + 4 + i * 4);
        if (s >= size) break;
        const uint8_t* end = std::find(d + s, d + size, uint8_t(0));
        out.emplace_back(reinterpret_cast<const char*>(d + s), reinterpret_cast<const char*>(end));
    }
    return out;
}

} // namespace pkmnstadium::language
