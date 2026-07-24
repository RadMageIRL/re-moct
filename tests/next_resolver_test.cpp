// next_resolver_test.cpp - XF C3: resolveNextPath precedence table.
//
// The resolver (NextArm.h) is the ONE authority on "what plays after the current
// track": repeat-one -> nothing; queue head first; else peekNext(); CD/stream
// rejected. This table pins that precedence against the REAL PlaylistManager -
// queue, shuffle, and repeat semantics included - device-free, so it runs on
// both toolchains and in CI. (The reconcile half needs real arming and lives in
// xfade_handoff_test's P4 block, which needs an audio device.)
//
// Only resolveNextPath is instantiated here - reconcileNextArm (which would drag
// AudioManager linkage) is never called, so this target links PlaylistManager
// alone. Entry paths need not exist on disk: addTrack's TagLib probe fails soft
// and still appends the row.
#include "NextArm.h"
#include "PlaylistManager.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main() {
    const std::string A = "C:\\xr\\a.mp3", B = "C:\\xr\\b.mp3", C = "C:\\xr\\c.mp3";
    const std::string STREAM = "https://example.com/live/hls.m3u8";
    const std::string CDT    = "G:CD Track 03";   // the synthetic CD-row format

    // ── empty playlist -> nothing ─────────────────────────────────────────────
    {
        PlaylistManager pl;
        CHECK(resolveNextPath(pl).empty());
    }

    // ── plain advance: peekNext successor ────────────────────────────────────
    {
        PlaylistManager pl;
        pl.addTrack(A); pl.addTrack(B); pl.addTrack(C);
        pl.selectAt(0);
        CHECK(resolveNextPath(pl) == B);
        pl.selectAt(1);
        CHECK(resolveNextPath(pl) == C);
        // repeat-off at the last row: nothing follows
        pl.selectAt(2);
        CHECK(resolveNextPath(pl).empty());
        // repeat-all wraps to the first row
        pl.setRepeat(RepeatMode::All);
        CHECK(resolveNextPath(pl) == A);
    }

    // ── repeat-one beats everything: nothing arms, even with a queue ─────────
    {
        PlaylistManager pl;
        pl.addTrack(A); pl.addTrack(B);
        pl.selectAt(0);
        pl.setRepeat(RepeatMode::One);
        CHECK(resolveNextPath(pl).empty());
        PlaylistEntry qe; qe.path = C;
        pl.queueAdd(qe);
        CHECK(resolveNextPath(pl).empty());
        // leaving repeat-one, the queue head takes over
        pl.setRepeat(RepeatMode::Off);
        CHECK(resolveNextPath(pl) == C);
    }

    // ── queue precedence: head beats peekNext; FIFO means head only ──────────
    {
        PlaylistManager pl;
        pl.addTrack(A); pl.addTrack(B);
        pl.selectAt(0);
        PlaylistEntry q1; q1.path = C;
        PlaylistEntry q2; q2.path = B;
        pl.queueAdd(q1);
        CHECK(resolveNextPath(pl) == C);
        pl.queueAdd(q2);                    // stacking does not change the head
        CHECK(resolveNextPath(pl) == C);
        pl.queuePop();                      // pop reveals the next head
        CHECK(resolveNextPath(pl) == B);
        pl.queueClear();                    // empty queue falls back to peekNext
        CHECK(resolveNextPath(pl) == B);
        // queueRemoveAt(0) on a rebuilt queue also re-resolves
        pl.queueAdd(q1); pl.queueAdd(q2);
        pl.queueRemoveAt(0);
        CHECK(resolveNextPath(pl) == B);
    }

    // ── CD and stream heads arm nothing (those transitions stay hard cuts) ───
    {
        PlaylistManager pl;
        pl.addTrack(A); pl.addTrack(B);
        pl.selectAt(0);
        PlaylistEntry qs; qs.path = STREAM;
        pl.queueAdd(qs);
        CHECK(resolveNextPath(pl).empty()); // stream head: no arm, NOT peekNext
        pl.queueClear();
        PlaylistEntry qc; qc.path = CDT;
        pl.queueAdd(qc);
        CHECK(resolveNextPath(pl).empty()); // CD head: same
        pl.queueClear();
        // stream/CD as the playlist SUCCESSOR is rejected too
        PlaylistManager pl2;
        pl2.addTrack(A); pl2.addTrack(STREAM);
        pl2.selectAt(0);
        CHECK(resolveNextPath(pl2).empty());
    }

    // ── shuffle: the resolver mirrors peekNext's shuffle successor ───────────
    {
        PlaylistManager pl;
        pl.addTrack(A); pl.addTrack(B); pl.addTrack(C);
        pl.setShuffle(true);
        pl.selectAt(0);
        auto peek = pl.peekNext();
        if (peek.has_value())
            CHECK(resolveNextPath(pl) == peek.value());
        else
            CHECK(resolveNextPath(pl).empty());
        // and the queue still beats the shuffle successor
        PlaylistEntry qe; qe.path = C;
        pl.queueAdd(qe);
        CHECK(resolveNextPath(pl) == C);
    }

    if (g_fail == 0) { std::printf("next_resolver_test: ALL PASS\n"); return 0; }
    std::printf("next_resolver_test: %d FAILURE(S)\n", g_fail);
    return 1;
}
