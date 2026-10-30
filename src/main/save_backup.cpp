#include "save_backup.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <system_error>
#include <vector>

#include "app_paths.h"

namespace pkmnstadium {
namespace {
    constexpr size_t backups_kept = 10;

    std::string timestamp() {
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &local);
        return buf;
    }

    // Deletes all but the newest `backups_kept` files named "<tag>-<timestamp><ext>".
    // The timestamp sorts lexically, so name order is age order.
    void prune(const std::string& tag) {
        std::error_code ec;
        std::vector<std::filesystem::path> matches;
        for (const auto& entry : std::filesystem::directory_iterator(backup_dir(), ec)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_regular_file() && name.size() > tag.size() + 1 &&
                name.compare(0, tag.size() + 1, tag + "-") == 0) {
                matches.push_back(entry.path());
            }
        }
        if (matches.size() <= backups_kept) {
            return;
        }
        std::sort(matches.begin(), matches.end());
        for (size_t i = 0; i < matches.size() - backups_kept; i++) {
            std::filesystem::remove(matches[i], ec);
        }
    }
}

std::filesystem::path backup_dir() {
    return exe_dir() / "saves" / "backups";
}

std::filesystem::path backup_save(const std::filesystem::path& src, const std::string& tag) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(src, ec)) {
        return {};
    }
    std::filesystem::create_directories(backup_dir(), ec);
    const std::filesystem::path dst =
        backup_dir() / (tag + "-" + timestamp() + src.extension().string());
    if (!std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec)) {
        std::fprintf(stderr, "[backup] failed to back up %s: %s\n",
                     src.string().c_str(), ec.message().c_str());
        return {};
    }
    std::fprintf(stderr, "[backup] %s -> %s\n", src.string().c_str(), dst.string().c_str());
    prune(tag);
    return dst;
}
}
