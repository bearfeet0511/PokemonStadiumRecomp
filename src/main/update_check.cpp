#include "update_check.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

#include <SDL.h>

#ifndef PSR_GIT_COMMIT
#define PSR_GIT_COMMIT "unknown"
#endif

namespace pkmnstadium::update_check {
const char* const releases_page = "https://github.com/bearfeet0511/PokemonStadiumRecomp/releases";

namespace {
    constexpr const wchar_t* api_host = L"api.github.com";
    constexpr const wchar_t* api_path = L"/repos/bearfeet0511/PokemonStadiumRecomp/releases?per_page=20";

    std::once_flag started;
    std::mutex result_mutex;
    bool newer_available = false;
    std::string newer_text;
    std::string newer_url;

    struct Release {
        std::string tag;
        std::string name;
        std::string commit;
    };

    // The value of "key":"..." at or after `from` in GitHub's JSON. Release
    // names and tags are plain text, so no escape handling beyond \" is needed.
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
    // exactly one top-level "tag_name"; its "target_commitish" and "name"
    // follow it before the next release's tag.
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
            releases.push_back(r);
            at = next;
        }
        return releases;
    }

#ifdef _WIN32
    std::string fetch_releases() {
        std::string body;
        HINTERNET session = WinHttpOpen(L"PokemonStadiumRecomp-update-check", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) {
            return body;
        }
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
        HINTERNET connection = WinHttpConnect(session, api_host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", api_path, nullptr, WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                       : nullptr;
        if (request != nullptr &&
            WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            DWORD available = 0;
            while (status == 200 && WinHttpQueryDataAvailable(request, &available) && available > 0) {
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
                    break;
                }
                body.append(chunk.data(), read);
            }
        }
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return body;
    }
#else
    std::string fetch_releases() {
        return ""; // Not implemented off Windows yet.
    }
#endif

    void check() {
        const std::vector<Release> releases = parse_releases(fetch_releases());
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
        const Release& latest = releases.front();
        std::scoped_lock lock(result_mutex);
        newer_available = true;
        newer_text = "Update available: " + (latest.name.empty() ? latest.tag : latest.name) +
                     " (you have " + (releases[mine].name.empty() ? releases[mine].tag : releases[mine].name) + ")";
        newer_url = std::string(releases_page) + "/tag/" + latest.tag;
        std::fprintf(stderr, "[update] %s\n", newer_text.c_str());
    }
}

void start() {
    std::call_once(started, [] { std::thread(check).detach(); });
}

int notice(char* text, size_t text_cap, char* url, size_t url_cap) {
    std::scoped_lock lock(result_mutex);
    if (!newer_available) {
        return 0;
    }
    std::snprintf(text, text_cap, "%s", newer_text.c_str());
    std::snprintf(url, url_cap, "%s", newer_url.c_str());
    return 1;
}

void open_url(const char* url) {
    SDL_OpenURL(url);
}
}
