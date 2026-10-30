#pragma once

#include <cstdint>

// Determinism probe for netplay, all opt-in via environment variables:
//   PSR_INPUT_RECORD=<file>   record every controller read, by poll number
//   PSR_INPUT_REPLAY=<file>   feed recorded input instead of live input
//   PSR_STATE_HASH=<file>     on each controller-1 read, log a hash of RDRAM
//                             (whole + per 64 KiB block) to compare runs
//   PSR_STATE_DUMP_AT=<poll>  also write the full RDRAM at that poll to
//                             <PSR_STATE_HASH>.rdram<poll>.bin
// Controller reads happen on the game thread at a fixed point in its logic,
// so a poll number names the same moment in every run, unlike wall time.
// Pair with RECOMP_DETERMINISTIC_CLOCK=1 (runtime) for a fixed game clock.
namespace pkmnstadium::netplay_probe {
    // Called after live input for `controller` was read into the outputs.
    // Records it, or replaces it with the recording, and hashes state.
    // Returns whether the controller responds.
    bool filter_input(int controller, bool got_response,
                      uint16_t* buttons, float* x, float* y);
}
