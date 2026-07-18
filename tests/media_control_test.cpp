// media_control_test - osmedia-seam: the routing + marshal + seek-coalescing
// contract, proven headless (no UIManager, no curses, no AudioManager) the same
// way the recorder engine is. Two standalone cores under test:
//   SeekCoalescer - the relative/absolute lanes + flush resolution. Includes the
//     REGRESSION GATE for absolute drag: a stream of SetPosition events in one
//     window lands at the FINAL absolute position, not an accumulated sum.
//   MediaRouter   - the split sink + the off-thread marshal: a command posted
//     from a non-UI thread applies only on the UI-thread drain, to the right sink.
#include "SeekCoalescer.h"
#include "MediaRouter.h"
#include "core/IMediaControl.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static bool approx(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    // ── SeekCoalescer: relative lane accumulates ─────────────────────────────
    {
        SeekCoalescer c;
        c.addRelative(+5.0, 0);
        c.addRelative(+5.0, 0);
        CHECK(c.pending());
        auto d = c.resolve(0, 100.0);            // live_pos irrelevant to rel
        CHECK(d.has_value() && approx(*d, 10.0));
        CHECK(!c.pending());                     // resolve clears
        CHECK(!c.resolve(0, 0.0).has_value());   // nothing pending -> nullopt
    }

    // ── SeekCoalescer: ABSOLUTE DRAG REGRESSION GATE ─────────────────────────
    // At 10 s, a drag 30 -> 60 in ONE window. Receive-time folding would give
    // (30-10)+(60-10)=+70 -> lands at 80 (the bug). Last-write-wins + flush-time
    // resolution gives (60 - live 10) = +50 -> lands at 60.
    {
        SeekCoalescer c;
        c.setAbsolute(30.0, 0);
        c.setAbsolute(60.0, 0);                  // last-write-wins
        auto d = c.resolve(0, 10.0);             // live position = 10
        CHECK(d.has_value() && approx(*d, 50.0));   // 60 - 10, lands at 60 NOT 80
    }

    // ── SeekCoalescer: absolute supersedes an accumulated relative delta ──────
    {
        SeekCoalescer c;
        c.addRelative(+5.0, 0);
        c.setAbsolute(60.0, 0);                  // clears the pending +5
        auto d = c.resolve(0, 10.0);
        CHECK(d.has_value() && approx(*d, 50.0));   // abs wins: 60 - 10
    }
    {
        SeekCoalescer c;
        c.setAbsolute(60.0, 0);
        c.addRelative(+5.0, 0);                  // abs still wins when both dirty
        auto d = c.resolve(0, 10.0);
        CHECK(d.has_value() && approx(*d, 50.0));
    }

    // ── SeekCoalescer: track-change drops a buffered seek ────────────────────
    {
        SeekCoalescer c;
        c.addRelative(+30.0, 1);                 // seek tied to track 1
        auto d = c.resolve(2, 0.0);              // current track is 2 now
        CHECK(!d.has_value());                   // dropped, not leaked onto track 2
        CHECK(!c.pending());
    }

    // ── MediaRouter: each command routes to its sink, carrying seconds ───────
    {
        MediaRouter r;
        std::string last; double secs = 0.0;
        MediaRouter::Sinks s;
        s.play        = [&]{ last = "play"; };
        s.pause       = [&]{ last = "pause"; };
        s.togglePause = [&]{ last = "toggle"; };
        s.stop        = [&]{ last = "stop"; };
        s.next        = [&]{ last = "next"; };
        s.prev        = [&]{ last = "prev"; };
        s.seekAbs     = [&](double v){ last = "abs"; secs = v; };
        s.seekRel     = [&](double v){ last = "rel"; secs = v; };
        r.setSinks(s);
        using C = core::MediaCommand;
        r.post({C::Play});       r.drain(); CHECK(last == "play");
        r.post({C::Pause});      r.drain(); CHECK(last == "pause");
        r.post({C::TogglePause});r.drain(); CHECK(last == "toggle");
        r.post({C::Stop});       r.drain(); CHECK(last == "stop");
        r.post({C::Next});       r.drain(); CHECK(last == "next");
        r.post({C::Previous});   r.drain(); CHECK(last == "prev");
        r.post({C::SeekToAbs, 60.0}); r.drain(); CHECK(last == "abs" && approx(secs, 60.0));
        r.post({C::SeekByRel, -5.0}); r.drain(); CHECK(last == "rel" && approx(secs, -5.0));
    }

    // ── MediaRouter: off-thread post applies ONLY on the UI-thread drain ─────
    {
        MediaRouter r;
        std::atomic<int> applied{0};
        MediaRouter::Sinks s;
        s.next = [&]{ applied.fetch_add(1); };
        r.setSinks(s);
        std::thread t([&]{ r.post({core::MediaCommand::Next}); });   // "OS thread"
        t.join();
        CHECK(applied.load() == 0);              // NOT applied inline off-thread
        CHECK(!r.empty());                       // it's queued
        r.drain();                               // the UI-thread drain
        CHECK(applied.load() == 1);              // applied exactly once, on drain
        CHECK(r.empty());
    }

    // ── osmedia-art-floor: the empty-art floor DECISION (consumer-side, once) ─
    // Real art passes through; empty resolves to the logo sentinel the impls
    // render (SMTC in-memory thumbnail / MPRIS cached file:// - hardware-gated).
    {
        CHECK(core::floorArt("") == std::string(core::kMediaArtLogo));   // empty -> logo
        CHECK(core::floorArt("http://x/a.jpg") == "http://x/a.jpg");     // real URL passes through
        CHECK(core::floorArt("file:///tmp/cover.jpg") == "file:///tmp/cover.jpg");
        CHECK(core::kMediaArtLogo[0] == '\x01');   // sentinel can't collide with a real URL/path
    }

    std::printf(g_fail ? "media_control_test: %d FAILED\n"
                       : "media_control_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
