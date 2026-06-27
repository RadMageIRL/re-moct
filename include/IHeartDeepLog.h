#pragma once
#ifdef _WIN32

#include <string>
#include <cstddef>

// ─────────────────────────────────────────────────────────────────────────────
// IHeartDeepLog — opt-in, structured deep-capture log for iHeart now-playing
// reconciliation. Default OFF; toggled at runtime (Ctrl+A). When ON, every call
// to StreamSource::updateIHeartNowPlaying emits one NDJSON record describing the
// tick's inputs (manifest classification, trackHistory, staleness), the computed
// target, the debounce state machine, AND the live audio playback position — the
// last of which the standalone SniffIHeartRadio probe cannot capture, and which
// is what makes the metadata-vs-audio timeline skew measurable after the fact.
//
// Output: %APPDATA%\RE-MOCT\logs\remoct-deep-analysis-YYYYMMDD-HHMMSS.log
//   - one JSON object per line (NDJSON); the first line of each file is a
//     "_meta" record (schema version, sample rate, heartbeat, size cap)
//   - size roll: a new dated file is started when the current one reaches ~5 MB
//   - retention: capture files older than 5 days are deleted on enable and on
//     each roll (active file is never touched)
//
// Write policy is event-driven + heartbeat: a record is written when any
// SEMANTIC field changes, OR when the heartbeat interval (30 s) elapses with no
// change. A metadata freeze therefore appears as identical heartbeat records
// with a climbing thEnded / advancing mfSeq, never as an absence of data — which
// is the exact signature we need to characterise the Z100 dual-blind.
//
// Threading: all filesystem I/O happens on the producer thread inside emit().
// The UI thread only flips an atomic via toggle(). Self-contained: depends on
// json.hpp + Win32 + the standard library. Windows-only, mirroring IHeartRadio.
// ─────────────────────────────────────────────────────────────────────────────
namespace IHeartDeepLog {

// One reconciliation tick, populated by the caller from local + member state.
// Enum-like fields are passed as short strings ("Song"/"Ad"/"Live"/"None") so
// this header carries no dependency on StreamSource internals.
struct Record {
    // identity / timing
    long        stationId = 0;
    std::string station;            // iHeart 'name'
    double      audioSec  = 0.0;    // frames_drained_ / SAMPLE_RATE — the skew anchor
    int         posSec    = 0;      // StreamSource::positionSec() (coarse)

    // manifest (-P): live-edge classification + raw strings
    std::string mfCls;              // "Song" | "Ad" | "None"
    std::string mfArtist, mfTitle, mfSong;
    long        mfSeq     = -1;     // EXT-X-MEDIA-SEQUENCE (manifest-freeze detector)
    std::size_t mfBodyLen = 0;
    bool        pdt       = false;  // manifest carries EXT-X-PROGRAM-DATE-TIME (false today)
    bool        spotPaid  = false;  // newest seg is a PAID ad (song_spot=T, real spotInstanceId)
    std::string spotInstanceId;     // newest seg's spotInstanceId ("-1" = station id/promo; real id = paid spot)
    std::string cartcutId;          // newest seg's cartcutId — the ad's identity. Same id repeating across a
                                    // block = a STUCK loop; a sequence of distinct ids = a genuine long pod.

    // trackHistory (the only live now-playing endpoint; currentTrackMeta is 410)
    std::string th;                 // cached "Artist - Title"
    long        thEnded   = -1;     // now - endTime; climbs during a freeze
    bool        thCurrent = false;

    // computed target for this tick (pre-debounce)
    std::string tgtKind;            // "Song" | "Ad" | "Live"
    std::string tgtDisp;

    // debounce state machine, as of tick ENTRY (i.e. result of the previous tick)
    std::string stState, stDisp;    // committed
    std::string pendKind, pendDisp; // pending candidate
    int         streak    = 0;

    // currentTrackMeta probe (web-player source of truth). Populated only while the
    // deep log is enabled. Distinct from trackHistory: the CURRENT track per iHeart,
    // with epochs normalised to seconds. ctmStatus is the raw HTTP code (200/410/...).
    bool        ctmOk           = false;
    long        ctmStatus       = 0;
    std::string ctmArtist, ctmTitle, ctmAlbum;
    long long   ctmTrackId      = 0;
    long long   ctmStartSec     = 0;
    long long   ctmEndSec       = 0;
    long        ctmDurationSec  = 0;
    long long   ctmEndedSecsAgo = -1;
    std::string ctmImage;
    std::string ctmDataSource;

    // stream mode (raw broadcast vs digital web-player rendition) + connect bookkeeping
    std::string streamMode;          // "raw" | "digital"
    bool        digitalRequested = false;
    bool        digitalActive    = false;
    int         connectSeq       = 0;
};

// Flip on/off. Returns the new state. Called from the UI thread (Ctrl+A). On a
// 0->1 transition the next emit() starts a fresh capture file.
bool toggle();
bool enabled();

// Emit one tick. No-op when disabled. Applies the change/heartbeat write policy,
// size roll and retention internally. Call once per updateIHeartNowPlaying.
void emit(const Record& r);

// Parse EXT-X-MEDIA-SEQUENCE from a media playlist body (-1 if absent).
long extractMediaSeq(const std::string& body);

// Absolute path of the active capture file, or "" if not currently logging.
std::string path();

} // namespace IHeartDeepLog

#endif // _WIN32
