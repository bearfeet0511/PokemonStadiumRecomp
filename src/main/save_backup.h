#pragma once

#include <filesystem>
#include <string>

namespace pkmnstadium {
    // Directory for timestamped save backups: saves\backups next to the exe.
    std::filesystem::path backup_dir();

    // Copies `src` to backup_dir()/<tag>-YYYYMMDD-HHMMSS<ext> and prunes that
    // tag's backups to the newest few. Returns the copy's path, or an empty
    // path if `src` doesn't exist or the copy failed.
    std::filesystem::path backup_save(const std::filesystem::path& src, const std::string& tag);
}
