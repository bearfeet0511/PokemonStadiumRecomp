#pragma once

#include <cstddef>

// Asks GitHub in the background whether this fork has published a newer
// release than the one this exe was built from, for the launcher's "update
// available" banner. A build that isn't any release (a developer build)
// never reports an update.
namespace pkmnstadium::update_check {
    // Starts the background check. Does nothing if already started.
    void start();

    // recomp-ui's GameInfo.update_notice: 1 with text + download page URL once
    // a newer release is known, 0 otherwise (still checking, up to date, dev
    // build, offline).
    int notice(char* text, size_t text_cap, char* url, size_t url_cap);

    // recomp-ui's GameInfo.open_url: opens a page in the default browser.
    void open_url(const char* url);

    // The releases page, for messages that tell players to update.
    extern const char* const releases_page;
}
