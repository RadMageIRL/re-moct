// cd_pipeline_test.cpp — Slice 0 (Phase 2 prep): thin replay net, CD half.
//
// FakeCdIo injected into the REAL CDSource (ctor injection; the CdIoWin.cpp TU is
// linked only to satisfy core::cdio(), never invoked) — but unlike cd_toc_test this
// drives the full PLAYBACK pipeline: playTrack -> readerWorker (producer thread) ->
// int16 SPSC ring -> readFrames (consumer), the exact producer/consumer machinery
// the Phase 2 Source-interface refactor must keep byte-identical.
//
// The fake serves a deterministic per-sample pattern keyed on the absolute disc
// position (pat(k) below, never zero), so the consumer can verify byte-exact PCM
// fidelity through the ring across the thread boundary: no dropped, duplicated, or
// reordered samples. Covered contracts (each is behavior AudioManager relies on):
//   1. full-track fidelity + the track-end predicate (currentTrack>0, !isPlaying,
//      !paused — AudioManager.cpp's CD-branch track_ended condition)
//   2. seekTo: ring flush + reader restart at the target LBA (75 sectors/sec)
//   3. pause: silence out, ring NOT consumed, exact continuity on resume
//   4. consumer stall: producer backpressure (no ring overwrite) — continuity after
//   5. transient read error: exactly count*SECTOR_SAMPLES*2 silence samples written
//      in-stream, LBA advanced past the bad batch (the silence-fill parity)
//   6. media removal: ring flushed, hard stop, media_removed latched, silence out
//   7. playTrack while playing: old reader joined, ring flushed, new track's data
//   8. stop()/stopReader()/close() teardown states
//
// HONEST LIMIT: the consumer here is a test thread pacing itself, not the real
// 44.1kHz audio callback — real-time cadence and device interplay stay covered by
// the live gates on 7of9. What this proves headlessly is the producer/consumer
// SEMANTICS: ordering, flush, throttle, silence-fill, teardown.
#ifdef _WIN32
#include "CDSource.h"
#include "core/ICdIo.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Deterministic, never-zero sample for absolute int16-slot k on the disc
// (slot = lba * 1176 + index-within-sector; 1176 = 588 stereo frames * 2).
static int16_t pat(uint64_t k) { return (int16_t)(1 + (k % 32000)); }

static constexpr int SLOTS_PER_SECTOR = CDSource::SECTOR_SAMPLES * 2;   // 1176

// ─── Fake device / factory (pipeline flavor: readRaw serves the pattern) ───────

struct FakeState {                         // outlives any device CDSource discards
    core::CdToc toc{};
    std::atomic<bool>     media       { true };
    // Fail readRaw calls whose lba >= fail_at_lba, fail_budget times (reader thread
    // is the only readRaw caller, so plain load/sub ordering is fine).
    std::atomic<uint32_t> fail_at_lba { 0xFFFFFFFFu };
    std::atomic<int>      fail_budget { 0 };
    std::atomic<int>      raw_calls   { 0 };
    std::atomic<bool>     saw_want_c2 { false };   // playback path must never ask for C2
};

class FakeCdDevice final : public core::ICdDevice {
public:
    explicit FakeCdDevice(std::shared_ptr<FakeState> st) : st_(std::move(st)) {}
    bool readToc(core::CdToc& out) override { out = st_->toc; return true; }
    bool lastSessionFirstTrack(uint8_t& out) override { out = 1; return true; }
    bool readRaw(uint32_t lba, uint32_t sectors, bool want_c2,
                 void* out, std::size_t out_size, std::size_t& got) override {
        ++st_->raw_calls;
        if (want_c2) st_->saw_want_c2.store(true);
        if (lba >= st_->fail_at_lba.load() && st_->fail_budget.load() > 0) {
            st_->fail_budget.fetch_sub(1);
            return false;
        }
        auto* p = static_cast<int16_t*>(out);
        const uint64_t base = (uint64_t)lba * SLOTS_PER_SECTOR;
        const size_t   n    = (size_t)sectors * SLOTS_PER_SECTOR;
        for (size_t i = 0; i < n; ++i) p[i] = pat(base + i);
        got = out_size;
        return true;
    }
    void setSpeed(uint16_t) override {}
    bool mediaPresent() override { return st_->media.load(); }
    std::string model() override { return "FAKE DRIVE"; }
private:
    std::shared_ptr<FakeState> st_;
};

class FakeCdIo final : public core::ICdIo {
public:
    std::shared_ptr<FakeState> st = std::make_shared<FakeState>();
    std::unique_ptr<core::ICdDevice> open(const std::string&) override {
        return std::make_unique<FakeCdDevice>(st);
    }
};

// ─── TOC builder (drive shape: entries[i-1] = track i, entries[n] = lead-out) ──

static void setMsf(core::CdTocEntry& e, uint32_t lba) {
    e.msf[0] = (uint8_t)(lba / 4500);
    e.msf[1] = (uint8_t)((lba % 4500) / 75);
    e.msf[2] = (uint8_t)(lba % 75);
}

static void buildToc(core::CdToc& toc, const std::vector<uint32_t>& starts, uint32_t leadout) {
    toc.first = 1;
    toc.last  = (uint8_t)starts.size();
    for (size_t i = 0; i < starts.size(); ++i) {
        toc.entries[i].track   = (uint8_t)(i + 1);
        toc.entries[i].control = 0x00;
        setMsf(toc.entries[i], starts[i]);
    }
    toc.entries[starts.size()].track = 0xAA;
    setMsf(toc.entries[starts.size()], leadout);
}

// ─── Consumer helpers ──────────────────────────────────────────────────────────

// One paced consumer read: 512 frames -> 1024 int16 samples appended to out.
// float = int16/32768 is exact in a float, so the round-trip recovers the exact
// int16 the producer wrote. Reads through core::ISource — the slice-A contract
// — so every fidelity assertion below also proves the interface dispatch.
static void readChunk(core::ISource& src, std::vector<int16_t>& out) {
    float f[512 * 2];
    src.readFrames(f, 512);
    for (int i = 0; i < 512 * 2; ++i)
        out.push_back((int16_t)lrintf(f[i] * 32768.0f));
}

// Drain until the track has stopped AND the ring is provably empty, collecting
// every sample. The reader (fake-fast) finishes long before the paced consumer,
// and legitimate in-stream silence exists (the error silence-fill), so "a few
// quiet reads" is NOT an end signal — instead we require a consecutive zero-run
// longer than the whole ring: the ring can't hold that much, so it must be dry.
static void drainAll(CDSource& cd, std::vector<int16_t>& out, int max_ms = 20000) {
    const uint64_t RING_SAMPLES =
        (uint64_t)CDSource::SECTOR_SAMPLES * CDSource::RING_SECTORS * 2;
    DWORD t0 = GetTickCount();
    uint64_t zero_run = 0;
    while ((long)(GetTickCount() - t0) < max_ms) {
        size_t before = out.size();
        readChunk(cd, out);
        bool all_zero = true;
        for (size_t i = before; i < out.size(); ++i)
            if (out[i] != 0) { all_zero = false; break; }
        zero_run = all_zero ? zero_run + (out.size() - before) : 0;
        if (all_zero && !cd.isPlaying() && zero_run > RING_SAMPLES + 4096) return;
        // Pace only while the reader is live (never outrun the producer); once it
        // has stopped the ring is a static buffer and can be drained flat out.
        if (cd.isPlaying()) Sleep(1);
    }
    CHECK(!"drainAll timed out");
}

// Script matcher: after stripping leading zeros, the collected stream must be the
// given segments back-to-back (slot-keyed pattern data, or an exact run of zeros),
// followed by nothing but zeros.
struct Seg { bool zeros; uint64_t slot; uint64_t count; };

static void matchStream(const std::vector<int16_t>& v, const std::vector<Seg>& script,
                        const char* label) {
    size_t i = 0;
    while (i < v.size() && v[i] == 0) ++i;               // ring warm-up silence
    for (const auto& s : script) {
        for (uint64_t k = 0; k < s.count; ++k, ++i) {
            if (i >= v.size()) {
                ++g_fail;
                std::printf("FAIL [%s] stream ended early (segment slot=%llu k=%llu)\n",
                            label, (unsigned long long)s.slot, (unsigned long long)k);
                return;
            }
            int16_t want = s.zeros ? 0 : pat(s.slot + k);
            if (v[i] != want) {
                ++g_fail;
                std::printf("FAIL [%s] mismatch at stream idx %zu: got %d want %d "
                            "(segment slot=%llu k=%llu)\n",
                            label, i, (int)v[i], (int)want,
                            (unsigned long long)s.slot, (unsigned long long)k);
                return;
            }
        }
    }
    for (; i < v.size(); ++i) {
        if (v[i] != 0) {
            ++g_fail;
            std::printf("FAIL [%s] trailing non-silence at idx %zu (%d)\n", label, i, (int)v[i]);
            return;
        }
    }
}

// Incremental cursor verifier for the pause/stall blocks: verifies a chunk
// continues the expected slot sequence exactly (leading zeros allowed only until
// the first data sample ever seen).
struct Cursor { uint64_t slot; bool started = false; };

static void feed(Cursor& c, const std::vector<int16_t>& chunk, const char* label) {
    for (size_t i = 0; i < chunk.size(); ++i) {
        if (!c.started) {
            if (chunk[i] == 0) continue;                 // warm-up
            c.started = true;
        }
        int16_t want = pat(c.slot);
        if (chunk[i] != want) {
            ++g_fail;
            std::printf("FAIL [%s] cursor mismatch at slot %llu: got %d want %d\n",
                        label, (unsigned long long)c.slot, (int)chunk[i], (int)want);
            return;
        }
        ++c.slot;
    }
}

static bool waitFor(int timeout_ms, bool (*pred)(CDSource&), CDSource& cd) {
    DWORD t0 = GetTickCount();
    while ((long)(GetTickCount() - t0) < timeout_ms) {
        if (pred(cd)) return true;
        Sleep(5);
    }
    return false;
}

int main() {
    // Disc: T1 @ LBA 182 (Relish homage — non-standard pregap), 150 sectors (2s);
    // T2 @ 332, 150 sectors; lead-out 482.
    constexpr uint32_t T1 = 182, T2 = 332, LEN = 150;

    FakeCdIo io;
    buildToc(io.st->toc, {T1, T2}, T2 + LEN);
    CDSource cd(&io);
    CHECK(cd.open("F"));
    CHECK(cd.tracks().size() == 2);
    CHECK(cd.tracks()[0].start_lba == T1 && cd.tracks()[0].length_lba == LEN);

    // ── 0. The core::ISource contract surface (slice A) ──────────────────────
    core::ISource& s = cd;
    {
        auto sc = s.caps();
        CHECK(sc.seekable && sc.finite && !sc.live);      // declared, matching reality
        CHECK(!s.seekTo(1.0));                            // nothing playing -> false
        CHECK(s.positionSec() == 0.0);
        CHECK(s.durationSec() == 0.0);                    // no current track yet
    }

    // ── 1. Full-track PCM fidelity + track-end predicate ─────────────────────
    {
        CHECK(cd.playTrack(1));
        CHECK(cd.isPlaying());
        CHECK(s.durationSec() == 2.0);                    // 150 sectors / 75 via the interface
        Sleep(100);                                       // let the reader prefill the ring
        std::vector<int16_t> v;
        drainAll(cd, v);
        matchStream(v, {{false, (uint64_t)T1 * SLOTS_PER_SECTOR, (uint64_t)LEN * SLOTS_PER_SECTOR}},
                    "fidelity");
        // AudioManager's CD track-end predicate, exactly:
        CHECK(cd.currentTrack() == 1);
        CHECK(!cd.isPlaying());
        CHECK(!cd.paused());
        CHECK(!io.st->saw_want_c2.load());                // playback never requests C2
    }

    // ── 2. Seek: flush + restart at target LBA (75 sectors/sec) ──────────────
    {
        CHECK(cd.playTrack(1));
        Sleep(100);
        std::vector<int16_t> pre;
        readChunk(cd, pre);                               // consume a little, discard
        CHECK(s.seekTo(1.0));                             // via the interface -> LBA 182 + 75 = 257
        std::vector<int16_t> v;
        drainAll(cd, v);
        const uint32_t tgt = T1 + 75;
        matchStream(v, {{false, (uint64_t)tgt * SLOTS_PER_SECTOR,
                         (uint64_t)(T1 + LEN - tgt) * SLOTS_PER_SECTOR}}, "seek");
    }

    // ── 3+4. Pause continuity, then consumer-stall backpressure ──────────────
    {
        CHECK(cd.playTrack(2));
        Sleep(100);
        Cursor cur{ (uint64_t)T2 * SLOTS_PER_SECTOR };
        std::vector<int16_t> chunk;
        for (int i = 0; i < 20; ++i) {                    // steady consumption
            chunk.clear(); readChunk(cd, chunk); feed(cur, chunk, "pause-pre"); Sleep(1);
        }
        CHECK(cur.started);

        cd.pause(true);                                   // silence out, ring untouched
        for (int i = 0; i < 3; ++i) {
            chunk.clear(); readChunk(cd, chunk);
            for (int16_t s : chunk) if (s != 0) { CHECK(!"paused readFrames not silent"); break; }
        }
        cd.pause(false);
        chunk.clear(); readChunk(cd, chunk);
        feed(cur, chunk, "pause-resume");                 // exact continuation

        Sleep(300);                                       // consumer stall: producer must throttle,
        for (int i = 0; i < 20; ++i) {                    // NOT overwrite unread ring data
            chunk.clear(); readChunk(cd, chunk); feed(cur, chunk, "stall-resume"); Sleep(1);
        }
        cd.stop();
        CHECK(!cd.isPlaying());
        CHECK(cd.currentTrack() == 0);
    }

    // ── 5. Transient read error: exact silence-fill, LBA advances past batch ──
    {
        // Batches start at 182 and advance 20: the batch at LBA 282 fails once ->
        // 20 sectors of silence in-stream, data resumes at 302.
        io.st->fail_at_lba.store(282);
        io.st->fail_budget.store(1);
        CHECK(cd.playTrack(1));
        Sleep(100);
        std::vector<int16_t> v;
        drainAll(cd, v);
        matchStream(v, {
            {false, (uint64_t)T1  * SLOTS_PER_SECTOR, (uint64_t)(282 - T1) * SLOTS_PER_SECTOR},
            {true,  0,                                (uint64_t)20 * SLOTS_PER_SECTOR},
            {false, (uint64_t)302 * SLOTS_PER_SECTOR, (uint64_t)(T1 + LEN - 302) * SLOTS_PER_SECTOR},
        }, "silence-fill");
        io.st->fail_at_lba.store(0xFFFFFFFFu);
        io.st->fail_budget.store(0);
    }

    // ── 6. Media removal: hard stop, flush, latch ─────────────────────────────
    {
        io.st->fail_at_lba.store(282);
        io.st->fail_budget.store(1 << 20);
        io.st->media.store(false);
        CHECK(cd.playTrack(1));
        CHECK(waitFor(3000, [](CDSource& c){ return c.mediaRemoved(); }, cd));
        CHECK(!cd.isPlaying());
        CHECK(cd.currentTrack() == 0);
        {   // silence out after the hard stop
            std::vector<int16_t> chunk;
            readChunk(cd, chunk);
            bool silent = true;
            for (int16_t s : chunk) if (s != 0) { silent = false; break; }
            CHECK(silent);
        }
        io.st->media.store(true);
        io.st->fail_at_lba.store(0xFFFFFFFFu);
        io.st->fail_budget.store(0);
        cd.clearMediaRemoved();
        CHECK(!cd.mediaRemoved());
    }

    // ── 7. playTrack while playing: join, flush, new track's data ─────────────
    {
        CHECK(cd.playTrack(1));
        Sleep(100);
        std::vector<int16_t> pre;
        readChunk(cd, pre);                               // consume a little of T1
        CHECK(cd.playTrack(2));                           // switch mid-play
        std::vector<int16_t> v;
        drainAll(cd, v);
        matchStream(v, {{false, (uint64_t)T2 * SLOTS_PER_SECTOR,
                         (uint64_t)LEN * SLOTS_PER_SECTOR}}, "switch");
    }

    // ── 8. stopReader leaves the device open (the CDRipper handoff contract) ──
    {
        CHECK(cd.playTrack(1));
        Sleep(50);
        cd.stopReader();
        CHECK(cd.isOpen());                               // device still open for ripping
        CHECK(!cd.isPlaying());
        CHECK(cd.currentTrack() == 0);
        cd.close();
        CHECK(!cd.isOpen());
    }

    if (g_fail == 0) { std::printf("cd_pipeline_test: ALL PASS\n"); return 0; }
    std::printf("cd_pipeline_test: %d FAILURE(S)\n", g_fail);
    return 1;
}
#else
int main() { return 0; }
#endif
