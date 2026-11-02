#include "update_check.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#endif

#include <SDL.h>
#include <miniz.h>

#include "app_paths.h"

#ifndef PSR_GIT_COMMIT
#define PSR_GIT_COMMIT "unknown"
#endif

namespace pkmnstadium::update_check {
const char* const releases_page = "https://github.com/bearfeet0511/PokemonStadiumRecomp/releases";

namespace {
    namespace fs = std::filesystem;

    constexpr const wchar_t* api_host = L"api.github.com";
    constexpr const wchar_t* api_path = L"/repos/bearfeet0511/PokemonStadiumRecomp/releases?per_page=20";

    enum class Phase { Idle, Available, Busy, Failed };

    struct Release {
        std::string tag;
        std::string name;
        std::string commit;
        std::string zip_url;  // the release's .zip asset
        std::string sha256;   // GitHub's digest of that asset (lowercase hex)
    };

    std::once_flag started;
    std::once_flag updating;
    std::mutex state_mutex;
    Phase phase = Phase::Idle;
    std::string banner_text;
    Release latest;

    void set_state(Phase p, const std::string& text) {
        std::scoped_lock lock(state_mutex);
        phase = p;
        banner_text = text;
        std::fprintf(stderr, "[update] %s\n", text.c_str());
    }

    std::string display_name(const Release& r) {
        return r.name.empty() ? r.tag : r.name;
    }

    // Working files: the download, the unpacked release, and the list of
    // replaced files to delete after the restart.
    fs::path update_dir() { return exe_dir() / "update"; }

    // The value of "key":"..." between `from` and `to` in GitHub's compact
    // JSON. Names, tags, URLs and digests need no escape handling beyond \".
    std::string json_string(const std::string& json, const std::string& key, size_t from, size_t to) {
        const std::string needle = "\"" + key + "\":\"";
        const size_t at = json.find(needle, from);
        if (at == std::string::npos || at >= to) {
            return "";
        }
        std::string value;
        for (size_t i = at + needle.size(); i < json.size() && json[i] != '"'; i++) {
            if (json[i] == '\\' && i + 1 < json.size()) {
                i++;
            }
            value += json[i];
        }
        return value;
    }

    // Releases newest first, as GitHub lists them. Each release object has
    // one top-level "tag_name"; its "target_commitish" and "name" follow it,
    // then its assets, before the next release's tag.
    std::vector<Release> parse_releases(const std::string& json) {
        std::vector<Release> releases;
        size_t at = json.find("\"tag_name\":\"");
        while (at != std::string::npos) {
            const size_t next = json.find("\"tag_name\":\"", at + 1);
            const size_t end = next == std::string::npos ? json.size() : next;
            Release r;
            r.tag = json_string(json, "tag_name", at, end);
            r.commit = json_string(json, "target_commitish", at, end);
            r.name = json_string(json, "name", at, end);
            r.zip_url = json_string(json, "browser_download_url", at, end);
            std::string digest = json_string(json, "digest", at, end);
            if (digest.rfind("sha256:", 0) == 0) {
                r.sha256 = digest.substr(7);
            }
            releases.push_back(r);
            at = next;
        }
        return releases;
    }

#ifdef _WIN32
    // GET an https URL. With `to_file`, streams the body there and reports
    // progress; otherwise returns it. Empty / false on any failure.
    bool http_get(const std::wstring& host, const std::wstring& path, std::string* body,
                  const fs::path* to_file = nullptr) {
        bool ok = false;
        HINTERNET session = WinHttpOpen(L"PokemonStadiumRecomp-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) {
            return false;
        }
        WinHttpSetTimeouts(session, 10000, 10000, 10000, 30000);
        HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                            WINHTTP_FLAG_SECURE)
                                       : nullptr;
        const wchar_t* accept = to_file != nullptr ? L"Accept: application/octet-stream\r\n"
                                                   : L"Accept: application/vnd.github+json\r\n";
        if (request != nullptr &&
            WinHttpSendRequest(request, accept, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            DWORD total = 0;
            size = sizeof(total);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &total, &size, WINHTTP_NO_HEADER_INDEX);
            std::ofstream file;
            if (to_file != nullptr) {
                file.open(*to_file, std::ios::binary | std::ios::trunc);
            }
            ok = status == 200 && (to_file == nullptr || file.good());
            uint64_t received = 0;
            int shown = -1;
            DWORD available = 0;
            while (ok && WinHttpQueryDataAvailable(request, &available) && available > 0) {
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
                    ok = false;
                    break;
                }
                received += read;
                if (to_file != nullptr) {
                    file.write(chunk.data(), read);
                    const int percent = total ? static_cast<int>(received * 100 / total) : 0;
                    if (percent != shown) {
                        shown = percent;
                        set_state(Phase::Busy, "Downloading " + display_name(latest) + "... " +
                                               std::to_string(percent) + "%");
                    }
                } else {
                    body->append(chunk.data(), read);
                }
            }
            ok = ok && (to_file == nullptr || (file.good() && (total == 0 || received == total)));
        }
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return ok;
    }

    std::string sha256_file(const fs::path& path) {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::string hex;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            return hex;
        }
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::ifstream file(path, std::ios::binary);
            std::vector<char> buffer(1 << 20);
            while (file) {
                file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                if (file.gcount() > 0) {
                    BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(file.gcount()), 0);
                }
            }
            unsigned char digest[32];
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                char buf[3];
                for (unsigned char b : digest) {
                    std::snprintf(buf, sizeof(buf), "%02x", b);
                    hex += buf;
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return hex;
    }

    // https://host/path -> host, path
    bool split_url(const std::string& url, std::wstring& host, std::wstring& path) {
        const std::string prefix = "https://";
        if (url.rfind(prefix, 0) != 0) {
            return false;
        }
        const size_t slash = url.find('/', prefix.size());
        if (slash == std::string::npos) {
            return false;
        }
        host = std::wstring(url.begin() + prefix.size(), url.begin() + slash);
        path = std::wstring(url.begin() + slash, url.end());
        return true;
    }
#endif

    // Files an update must never overwrite: the player's saves, settings,
    // ROM copies and logs. Release zips don't contain them anyway.
    bool is_protected(const fs::path& rel) {
        const std::string first = rel.begin() == rel.end() ? "" : rel.begin()->string();
        if (first == "saves" || first == "update" || first == "mods" || first == "mod_config") {
            return true;
        }
        const std::string name = rel.filename().string();
        std::string ext = rel.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        static const char* protected_names[] = {
            "launcher.cfg", "rom.cfg", "input.cfg", "keybinds.ini", "netplay.log",
            "last_error.log", "last_run_report.json", "last_run_input_history.json",
        };
        for (const char* p : protected_names) {
            if (name == p) {
                return true;
            }
        }
        // (Not .lua: gen1recomp\ ships the converter as .lua files. Players'
        // Gen1Recomp slots live in %APPDATA%, and Stadium's saves under saves\.)
        return ext == ".z64" || ext == ".n64" || ext == ".v64" || ext == ".sav" ||
               ext == ".gb" || ext == ".gbc";
    }

    // Unpack the release zip into `into`, normalizing Windows-style paths.
    // Refuses unsafe paths (.., absolute) and protected files.
    bool unpack(const fs::path& zip_path, const fs::path& into, std::vector<fs::path>& files) {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0)) {
            return false;
        }
        bool ok = true;
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count && ok; i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st)) {
                ok = false;
                break;
            }
            std::string name = st.m_filename;
            std::replace(name.begin(), name.end(), '\\', '/');
            if (st.m_is_directory || name.empty() || name.back() == '/') {
                continue;
            }
            const fs::path rel = fs::path(name).lexically_normal();
            if (rel.is_absolute() || rel.has_root_name() || rel.string().rfind("..", 0) == 0) {
                ok = false;
                break;
            }
            if (is_protected(rel)) {
                std::fprintf(stderr, "[update] skipping protected file in the zip: %s\n", name.c_str());
                continue;
            }
            const fs::path dest = into / rel;
            std::error_code ec;
            fs::create_directories(dest.parent_path(), ec);
            ok = mz_zip_reader_extract_to_file(&zip, i, dest.string().c_str(), 0);
            files.push_back(rel);
        }
        mz_zip_reader_end(&zip);
        return ok;
    }

    // Swap the new files in. A running exe/DLL can't be overwritten but can
    // be renamed, so each existing file moves to <name>.old first; on any
    // failure everything is put back. The .old files are deleted on the next
    // start (they're listed in update/replaced.txt).
    bool install(const fs::path& staged, const std::vector<fs::path>& files) {
        const fs::path root = exe_dir();
        std::vector<fs::path> renamed, copied;
        bool ok = true;
        std::error_code ec;
        for (const fs::path& rel : files) {
            const fs::path dest = root / rel;
            fs::path old = dest;
            old += ".old";
            if (fs::exists(dest, ec)) {
                fs::remove(old, ec);
                fs::rename(dest, old, ec);
                if (ec) {
                    ok = false;
                    break;
                }
                renamed.push_back(rel);
            }
            fs::create_directories(dest.parent_path(), ec);
            fs::copy_file(staged / rel, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                ok = false;
                break;
            }
            copied.push_back(rel);
        }
        if (!ok) {
            for (const fs::path& rel : copied) {
                fs::remove(root / rel, ec);
            }
            for (const fs::path& rel : renamed) {
                fs::path old = root / rel;
                old += ".old";
                fs::rename(old, root / rel, ec);
            }
            return false;
        }
        std::ofstream list(update_dir() / "replaced.txt", std::ios::trunc);
        for (const fs::path& rel : renamed) {
            list << rel.generic_string() << ".old\n";
        }
        return true;
    }

    // After an update: delete the replaced files and the working folder.
    void clean_up_previous_update() {
        std::error_code ec;
        std::ifstream list(update_dir() / "replaced.txt");
        std::string line;
        while (std::getline(list, line)) {
            if (!line.empty()) {
                fs::remove(exe_dir() / fs::path(line), ec);
            }
        }
        list.close();
        fs::remove_all(update_dir(), ec);
    }

    void restart() {
#ifdef _WIN32
        // A testing override would make the new version think it's old again.
        SetEnvironmentVariableW(L"PSR_UPDATE_TEST_COMMIT", nullptr);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(exe) + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           exe_dir().wstring().c_str(), &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
#endif
        std::_Exit(0);
    }

    void run_update() {
#ifdef _WIN32
        Release target;
        {
            std::scoped_lock lock(state_mutex);
            target = latest;
        }
        const std::string manual = " Use Release page to update by hand.";
        if (target.zip_url.empty() || target.sha256.empty()) {
            set_state(Phase::Failed, "This release has no installable download." + manual);
            return;
        }
        std::error_code ec;
        fs::remove_all(update_dir(), ec);
        fs::create_directories(update_dir(), ec);
        const fs::path zip = update_dir() / "release.zip";
        std::wstring host, path;
        if (!split_url(target.zip_url, host, path) || !http_get(host, path, nullptr, &zip)) {
            set_state(Phase::Failed, "Downloading the update failed." + manual);
            return;
        }
        set_state(Phase::Busy, "Checking the download...");
        if (sha256_file(zip) != target.sha256) {
            fs::remove_all(update_dir(), ec);
            set_state(Phase::Failed, "The download didn't match its checksum, so it wasn't installed." + manual);
            return;
        }
        set_state(Phase::Busy, "Installing " + display_name(target) + "...");
        const fs::path staged = update_dir() / "staged";
        std::vector<fs::path> files;
        if (!unpack(zip, staged, files) || files.empty()) {
            fs::remove_all(update_dir(), ec);
            set_state(Phase::Failed, "Unpacking the update failed." + manual);
            return;
        }
        if (!install(staged, files)) {
            set_state(Phase::Failed, "Couldn't replace the program files (is the folder read-only?)." + manual);
            return;
        }
        set_state(Phase::Busy, "Restarting into " + display_name(target) + "...");
        restart();
#else
        set_state(Phase::Failed, "Automatic updates aren't supported on this system yet.");
#endif
    }

    void check() {
#ifdef _WIN32
        std::string json;
        http_get(api_host, api_path, &json);
        const std::vector<Release> releases = parse_releases(json);
#else
        const std::vector<Release> releases;
#endif
        // PSR_UPDATE_TEST_COMMIT pretends to be another build (testing only).
        const char* test_commit = std::getenv("PSR_UPDATE_TEST_COMMIT");
        const std::string own = (test_commit != nullptr && test_commit[0] != '\0') ? test_commit : PSR_GIT_COMMIT;
        // Which release is this exe? A build matching none is a developer
        // build: don't nag it.
        size_t mine = releases.size();
        for (size_t i = 0; i < releases.size(); i++) {
            if (releases[i].commit == own) {
                mine = i;
                break;
            }
        }
        if (mine == releases.size() || mine == 0) {
            std::fprintf(stderr, "[update] %zu releases; %s\n", releases.size(),
                         mine == 0 ? "this build is the latest" : "this build isn't a release");
            return;
        }
        {
            std::scoped_lock lock(state_mutex);
            latest = releases.front();
        }
        set_state(Phase::Available, "Update available: " + display_name(releases.front()) +
                                        " (you have " + display_name(releases[mine]) + ")");
    }
}

void start() {
    std::call_once(started, [] {
        clean_up_previous_update();
        std::thread(check).detach();
    });
}

int notice(char* text, size_t text_cap, char* url, size_t url_cap) {
    std::scoped_lock lock(state_mutex);
    if (phase == Phase::Idle) {
        return 0;
    }
    std::snprintf(text, text_cap, "%s", banner_text.c_str());
    std::snprintf(url, url_cap, "%s/tag/%s", releases_page, latest.tag.c_str());
    return phase == Phase::Available ? 1 : phase == Phase::Busy ? 2 : 3;
}

void start_update() {
    std::call_once(updating, [] {
        set_state(Phase::Busy, "Starting the update...");
        std::thread(run_update).detach();
    });
}

void open_url(const char* url) {
    SDL_OpenURL(url);
}
}
