#pragma once

// ─── The AccurateRip verdict, and how it is spelled ──────────────────────────
//
// CD-S4. Seven readers consume this enum. The ones that were wrong were wrong
// for one shared reason: each hand-rolled a ternary or a switch that listed the
// values it happened to care about and let a FALL-THROUGH absorb the rest. So a
// value nobody listed came out as whichever label sat on the else branch, and
// "we never asked" was printed as "no match".
//
// The labels live here, in switches with NO `default:`, so a new enumerator is
// surfaced rather than absorbed. Two mechanisms, and it is worth being exact
// about what each one actually does, because measured:
//
//   * -Wswitch WARNS (it is in -Wall) when a case is missing - but only in
//     translation units built with -Wall, which is the app (src/CDRipper.cpp
//     includes this via CDRipper.h). The test targets build with plain
//     -std=c++20, so no warning appears there. And there is no -Werror anywhere
//     in this project, so it is a warning, never an error.
//   * ar_status_label_test FAILS. That is the hard gate, and the reason the
//     fall-through below returns a deliberately absurd string instead of a
//     plausible one: a missing case has to be loud somewhere that blocks.
//
// NotQueried is not a failure. It means the question was never asked - which is
// exactly what happens in [C] CUETools, [Y] Local and [B] Local 2-pass, because
// fetchARData runs in [A] AccurateRip mode only.

#include <string>

enum class ARStatus { NotQueried, Matched_v2, Matched_v1, NotFound, NetworkError, ReadError };

// Short form, used inside brackets in the rip log's per-track summary.
inline const char* arStatusLabel(ARStatus s) {
    switch (s) {
        case ARStatus::Matched_v2:   return "AR v2 OK";
        case ARStatus::Matched_v1:   return "AR v1 OK";
        case ARStatus::NotFound:     return "AR not found";
        case ARStatus::NetworkError: return "AR net err";
        case ARStatus::ReadError:    return "AR inconclusive: read err";
        case ARStatus::NotQueried:   return "AR not queried";
    }
    // Deliberately NOT a plausible label. If a status ever reaches here the log
    // should look obviously broken rather than quietly wrong - quietly wrong is
    // the entire defect this file exists to end. It is also what lets a test
    // prove the switch is exhaustive: delete a case and this string appears.
    return "AR ???";
}

// Long form, used on the per-track Pass 1 / Pass 2 lines. A separate wording
// because that line has room to explain itself and the summary line does not;
// every string here is the one the log already printed, EXCEPT NetworkError,
// which used to fall through to "no match" - the defect this slice exists for.
inline const char* arStatusPassLabel(ARStatus s) {
    switch (s) {
        case ARStatus::Matched_v2:   return "AR v2 OK";
        case ARStatus::Matched_v1:   return "AR v1 OK";
        case ARStatus::NotFound:     return "no match";
        case ARStatus::NetworkError: return "network error";
        case ARStatus::ReadError:    return "preamble read error (inconclusive)";
        case ARStatus::NotQueried:   return "not queried";
    }
    return "???";              // same reasoning as above - visibly broken, not plausible
}
