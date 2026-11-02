#pragma once

#include <cstddef>

// Asks GitHub in the background whether this fork has published a newer
// release than the one this exe was built from, for the launcher's "update
// available" banner, and can install it: download the release zip, check it
// against GitHub's SHA-256, swap in the new program files (never saves or
// configs) and restart. A build that isn't any release (a developer build)
// never reports an update.
namespace pkmnstadium::update_check {
    // Starts the background check (and cleans up after a previous update).
    // Does nothing if already started.
    void start();

    // recomp-ui's GameInfo.update_notice. Returns 0 = nothing to show,
    // 1 = an update is available, 2 = updating (text shows progress),
    // 3 = the update failed (text says why); fills the banner text and the
    // release page URL.
    int notice(char* text, size_t text_cap, char* url, size_t url_cap);

    // recomp-ui's GameInfo.start_update: install the available update in the
    // background; on success the launcher restarts into the new version.
    void start_update();

    // recomp-ui's GameInfo.open_url: opens a page in the default browser.
    void open_url(const char* url);

    // The releases page, for messages that tell players to update.
    extern const char* const releases_page;
}
