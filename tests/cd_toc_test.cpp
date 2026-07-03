// cd_toc_test.cpp — Phase 1 slice 8: core::ICdIo seam, consumer-side TOC logic.
//
// FakeCdIo injected into the REAL CDSource via constructor injection (the
// CdIoWin.cpp TU is linked only to satisfy core::cdio(); never invoked).
// Proves the consumer's parse/guard logic against the interface: Relish-shaped
// non-standard pregap, Enhanced-CD data track + session-2 leadout, malformed-TOC
// clamps, drive-model → AccurateRip offset lookup, media checks.
//
// HONEST LIMIT: a fake cannot prove the Windows impl builds RAW_READ_INFO right
// or that a real drive returns the right bytes — that proof is the Joan Osborne
// Relish gate (12/12 AR v2 conf 200, byte-identical CRCs).
#ifdef _WIN32
#include "CDSource.h"
#include "core/ICdIo.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ─── Fake device / factory ────────────────────────────────────────────────────

struct FakeState {                       // outlives any device CDSource discards
    core::CdToc  toc{};
    core::CdToc  toc2{};                 // returned on the SECOND readToc call
    bool         has_toc2        = false;
    bool         toc_ok          = true;
    uint8_t      last_sess_first = 1;    // 1 = single session
    bool         last_sess_ok    = true;
    bool         media           = true;
    std::string  model           = "FAKE DRIVE";
    int          toc_reads       = 0;
    int          raw_reads       = 0;
    uint32_t     last_lba        = 0;
    uint32_t     last_sectors    = 0;
    bool         last_want_c2    = false;
    std::size_t  last_out_size   = 0;
};

class FakeCdDevice final : public core::ICdDevice {
public:
    explicit FakeCdDevice(std::shared_ptr<FakeState> st) : st_(std::move(st)) {}
    bool readToc(core::CdToc& out) override {
        if (!st_->toc_ok) return false;
        ++st_->toc_reads;
        out = (st_->has_toc2 && st_->toc_reads >= 2) ? st_->toc2 : st_->toc;
        return true;
    }
    bool lastSessionFirstTrack(uint8_t& out) override {
        if (!st_->last_sess_ok) return false;
        out = st_->last_sess_first;
        return true;
    }
    bool readRaw(uint32_t lba, uint32_t sectors, bool want_c2,
                 void* out, std::size_t out_size, std::size_t& got) override {
        ++st_->raw_reads;
        st_->last_lba      = lba;
        st_->last_sectors  = sectors;
        st_->last_want_c2  = want_c2;
        st_->last_out_size = out_size;
        std::memset(out, 0, out_size);   // deterministic silence
        got = out_size;
        return true;
    }
    void setSpeed(uint16_t) override {}
    bool mediaPresent() override { return st_->media; }
    std::string model() override { return st_->model; }
private:
    std::shared_ptr<FakeState> st_;      // state owned by the FACTORY, not the device
};

class FakeCdIo final : public core::ICdIo {
public:
    std::shared_ptr<FakeState> st = std::make_shared<FakeState>();
    bool        fail_open  = false;
    std::string opened_spec;
    std::unique_ptr<core::ICdDevice> open(const std::string& drive) override {
        opened_spec = drive;
        if (fail_open) return nullptr;
        return std::make_unique<FakeCdDevice>(st);
    }
};

// ─── TOC builders ─────────────────────────────────────────────────────────────

static void setMsf(core::CdTocEntry& e, uint32_t lba) {
    e.msf[0] = (uint8_t)(lba / 4500);          // minutes (60*75)
    e.msf[1] = (uint8_t)((lba % 4500) / 75);   // seconds
    e.msf[2] = (uint8_t)(lba % 75);            // frames
}

// entries[i-1] for track i, entries[n] = lead-out (track 0xAA) — the drive shape.
static void buildToc(core::CdToc& toc, const std::vector<uint32_t>& starts,
                     uint32_t leadout, uint32_t data_mask = 0) {
    toc.first = 1;
    toc.last  = (uint8_t)starts.size();
    for (size_t i = 0; i < starts.size(); ++i) {
        toc.entries[i].track   = (uint8_t)(i + 1);
        toc.entries[i].control = (data_mask >> i) & 1 ? 0x04 : 0x00;
        setMsf(toc.entries[i], starts[i]);
    }
    toc.entries[starts.size()].track = 0xAA;
    setMsf(toc.entries[starts.size()], leadout);
}

int main() {
    // ── 1. Relish-shaped disc: 12 tracks, T1 at LBA 182 (non-standard pregap) ──
    // The pregap must SURVIVE into start_lba (msf_to_lba adds no ±150 — the
    // "do NOT subtract 150" contract that the AR disc-ID math depends on).
    {
        FakeCdIo io;
        std::vector<uint32_t> starts;
        for (uint32_t i = 0; i < 12; ++i) starts.push_back(182 + i * 15000);
        buildToc(io.st->toc, starts, 182 + 12 * 15000);
        CDSource cd(&io);
        CHECK(cd.open("G"));
        CHECK(io.opened_spec == "G");
        CHECK(cd.isOpen());
        CHECK(cd.tracks().size() == 12);
        CHECK(cd.tracks()[0].start_lba == 182);          // pregap intact
        CHECK(cd.tracks()[0].length_lba == 15000);
        CHECK(cd.tracks()[11].start_lba == 182 + 11 * 15000);
        CHECK(cd.fullLeadoutLba() == 182 + 12 * 15000);
        CHECK(cd.dataTrackLbas().empty());
        auto offs = cd.tocOffsets();                     // DiscID shape
        CHECK(offs.size() == 13);
        CHECK(offs[0] == 182);
        CHECK(offs[12] == 182 + 12 * 15000);
        CHECK(cd.driveOffset() == 0);                    // "FAKE DRIVE" unknown
        cd.close();
        CHECK(!cd.isOpen());
    }

    // ── 2. Enhanced CD: audio + trailing data track, second session ──────────
    {
        FakeCdIo io;
        // 3 audio tracks + track 4 data (control 0x04), session 2 first track = 4
        buildToc(io.st->toc, {150, 20000, 40000, 60000}, 80000, /*data_mask=*/0b1000);
        io.st->last_sess_first = 4;                      // multi-session
        io.st->has_toc2 = true;
        buildToc(io.st->toc2, {150, 20000, 40000, 60000}, 95000, 0b1000);  // bigger leadout
        CDSource cd(&io);
        CHECK(cd.open("D"));
        CHECK(cd.tracks().size() == 3);                  // data track excluded
        CHECK(cd.dataTrackLbas().size() == 1);
        CHECK(cd.dataTrackLbas()[0] == 60000);
        CHECK(cd.fullLeadoutLba() == 95000);             // session-2 leadout wins
        CHECK(io.st->toc_reads == 2);                    // second READ_TOC issued
        // Audio track 3 length still runs to the data track start (TOC end);
        // the Enhanced-CD cap happens lazily in CDRipper at rip time.
        CHECK(cd.tracks()[2].length_lba == 60000 - 40000);
    }

    // ── 3. Single-session disc must NOT re-read the TOC ──────────────────────
    {
        FakeCdIo io;
        buildToc(io.st->toc, {150, 20000}, 40000);
        io.st->last_sess_first = 1;
        CDSource cd(&io);
        CHECK(cd.open("D"));
        CHECK(io.st->toc_reads == 1);
        CHECK(cd.fullLeadoutLba() == 40000);
    }

    // ── 4. Malformed TOC guards (baseline clamps) ─────────────────────────────
    {
        FakeCdIo io;                                     // last < first -> reject
        buildToc(io.st->toc, {150, 20000}, 40000);
        io.st->toc.first = 5; io.st->toc.last = 2;
        CDSource cd(&io);
        CHECK(!cd.open("D"));
        CHECK(!cd.isOpen());
    }
    {
        FakeCdIo io;                                     // all-data disc -> no audio
        buildToc(io.st->toc, {150, 20000}, 40000, 0b11);
        CDSource cd(&io);
        CHECK(!cd.open("D"));
    }
    {
        FakeCdIo io;                                     // TOC read fails
        io.st->toc_ok = false;
        CDSource cd(&io);
        CHECK(!cd.open("D"));
    }
    {
        FakeCdIo io;                                     // device open fails
        io.fail_open = true;
        CDSource cd(&io);
        CHECK(!cd.open("D"));
    }

    // ── 5. Drive model → AccurateRip offset (hand-tuned table hit) ────────────
    {
        FakeCdIo io;
        buildToc(io.st->toc, {150}, 20000);
        io.st->model = "ASUS BW-16D1HT";
        CDSource cd(&io);
        CHECK(cd.open("D"));
        CHECK(cd.driveModel() == "ASUS BW-16D1HT");
        CHECK(cd.driveOffset() == +6);
    }

    // ── 6. Media checks route through the seam ────────────────────────────────
    {
        FakeCdIo io;
        buildToc(io.st->toc, {150}, 20000);
        CDSource cd(&io);
        CHECK(cd.open("D"));
        CHECK(cd.checkMedia());
        CHECK(!cd.mediaRemoved());
        io.st->media = false;
        CHECK(!cd.checkMedia());
        CHECK(cd.mediaRemoved());                        // flag latched
    }

    if (g_fail == 0) { std::printf("cd_toc_test: ALL PASS\n"); return 0; }
    std::printf("cd_toc_test: %d FAILURE(S)\n", g_fail);
    return 1;
}
#else
int main() { return 0; }
#endif
