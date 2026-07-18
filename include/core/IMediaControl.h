// IMediaControl.h - OS media-control seam (osmedia-seam). Bidirectional:
// publishes now-playing (outbound, like INotify/IIpc) AND receives transport
// commands (inbound, new). One interface, two platform impls:
//   Windows: SMTC (ISystemMediaTransportControls) - src/platform/win/MediaControlSmtc.cpp
//   Linux:   MPRIS (org.mpris.MediaPlayer2) over sd-bus - src/platform/linux/MediaControlMpris.cpp
//
// Same content/transport split as the other seams: the now-playing -> metadata
// mapping lives consumer-side (UIManager reuses the resolved Discord feed); only
// delivery + command receipt live behind this seam. This header must NOT include
// any platform header - the portability seam test.
//
// Production default reached by name (mediaControl(), the notifier() shape); the
// consumer takes an IMediaControl* by constructor injection and a test passes a
// fake. No setX() global.
#pragma once
#include <cstdint>
#include <string>
#include <functional>
#include <vector>

namespace core {

// The art-floor sentinel (osmedia-art-floor): the consumer sets MediaMeta.art
// to this when resolved art is empty, so the OS card shows the bundled RE-MOCT
// logo instead of a blank slot - the SAME floor the Discord path uses. The
// logo BYTES cross the seam ONCE via setDefaultArt(); the impls map this
// sentinel to their native logo surface (SMTC in-memory thumbnail stream /
// MPRIS cached file:// artUrl). A leading \x01 can never collide with a real
// URL. The DECISION (empty -> floor) lives once in the consumer (floorArt).
inline constexpr char kMediaArtLogo[] = "\x01remoct_logo";

// Consumer-side floor: real art passes through; empty -> the logo sentinel.
inline std::string floorArt(const std::string& real) {
    return real.empty() ? std::string(kMediaArtLogo) : real;
}

// Resolved consumer-side (the reused Discord feed): artist/title/album already
// deinverted, plus art. Precedence the impl applies: art_bytes (embedded cover,
// e.g. a local file's picture) FIRST, then `art` as a URL (radio digital
// cover), then the kMediaArtLogo floor. `art` may be a URL, the sentinel, or
// empty. Nothing here parses or fetches; the impl maps to its native surface
// (SMTC in-memory thumbnail / MPRIS mpris:artUrl).
struct MediaMeta {
    std::string          artist, title, album, art;
    std::vector<uint8_t> art_bytes;   // embedded cover; takes precedence over art
};

enum class MediaStatus { Stopped, Playing, Paused };

// Inbound transport commands. Seek carries seconds: SeekToAbs = an absolute
// target (OS scrubber SetPosition); SeekByRel = a relative offset (MPRIS Seek).
enum class MediaCommand { Play, Pause, TogglePause, Stop, Next, Previous,
                          SeekToAbs, SeekByRel };
struct MediaEvent { MediaCommand cmd; double seconds = 0.0; };

class IMediaControl {
public:
    virtual ~IMediaControl() = default;

    // ── Outbound (UI thread) ────────────────────────────────────────────────
    // Full push on a metadata/status CHANGE (title/artist/album/art/status).
    virtual void updateNowPlaying(const MediaMeta&, MediaStatus,
                                  double position_s, double duration_s) = 0;
    // Cheap periodic refresh of the position/duration the OS reads on demand.
    // Updates cached values only; emits NO change signal on normal progression
    // (MPRIS Position is poll-only + Seeked-on-jump; SMTC interpolates the bar).
    virtual void updatePosition(double position_s, double duration_s,
                                MediaStatus) = 0;
    // No track / stopped.
    virtual void clear() = 0;

    // Hand the impl the bundled logo JPEG bytes ONCE (the consumer's
    // logoBytes()), so it can render the kMediaArtLogo floor: SMTC builds an
    // in-memory thumbnail stream over them; MPRIS writes a write-once cache file
    // and serves its file:// URL. Called at wire time. Best-effort - a failure
    // just means the floor shows nothing (as before), never an error.
    virtual void setDefaultArt(std::vector<uint8_t> jpeg) = 0;

    // ── Inbound ─────────────────────────────────────────────────────────────
    // The impl invokes the handler on an OS button/method. The handler MUST be
    // thread-safe (SMTC fires on a WinRT threadpool thread) and MUST only
    // enqueue - never touch playback directly.
    virtual void setCommandHandler(std::function<void(MediaEvent)>) = 0;

    // ── Servicing (UI thread, once per main-loop tick) ──────────────────────
    // Linux: sd_bus_process + a non-blocking poll (the getch()-timeout loop is
    // not a poll() the bus fd can fold into). Windows: no-op - SMTC is
    // COM/threadpool-driven.
    virtual void pump() = 0;
};

// Link-time production default (per platform), the notifier() pattern. No
// setMediaControl(): the single consumer takes an IMediaControl* by constructor
// injection; this only supplies the default. On a Windows build without the
// wingui window (no HWND) and on any unsupported platform it returns a no-op.
IMediaControl& mediaControl();

} // namespace core
