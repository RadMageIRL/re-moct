# osmedia-seam - PLAN

**Stage:** PLAN deliverable. Anchors resolved against the live checkout; design +
diff shape below. No source changed. plan-first cadence: review -> implement ->
debrief -> Dos hardware gate -> commit. Do NOT commit.
**Baseline:** `experimental/win-pdcurses` @ head (`df6c033`, CI-green).
**Predecessor:** `osmedia-phase0-probe` (probes/osmedia/FINDINGS.md) - both
platforms proven reachable.
**Seek:** fully in v1 (Dos locked) - bidirectional scrubber, position+duration
reported, inbound absolute/relative seek accepted, routed through the coalescer.

## 0. Anchor resolution (live tree - no material divergence)

| anchor | live location | note |
|---|---|---|
| resolved metadata feed + change-trigger | `updateScrobbler` UIManager.cpp:4860-4990; deinvert @4912; discord trigger @4919-4920 | tap here (outbound) |
| transport fns | AudioManager.h: `pause`/`togglePause`/`stop`/`seekTo`/`seekBy` | inbound sink (non-seek) |
| next/prev logic | INLINE in `case 'n'` UIManager.cpp:6397-6431 / `case 'p'` :6433-6447 | NOT a method - see 5 |
| jumpToPlaylistIndex | UIManager.cpp:3068 | cursor jump only, NOT play - not the next/prev home |
| seek coalescer | `requestSeek` :934, `flushPendingSeek` :920 (applies `audio_.seekBy`, DELTA-based), drained in loop :1119-1121 | seek sink |
| main loop | `while (running_)` ~:1114; input `getch()` under `timeout(80)` (:295, :1450) | NOT poll() - see 3 |
| seam pattern | `core::INotify` + `core::notifier()` (INotify.h); UIManager ctor `notify_(notify ? notify : &core::notifier())` :223-224 | mirror exactly |
| platform wiring | CMake `if(WIN32)` :91-97 / `if(UNIX)` :106-112; impls src/platform/{win,linux}/ | add one file each |
| config toggle | `config_.discord_presence` (Config.h:35, load/save Config.cpp) | mirror for `os_media_control` |
| HWND | `extern "C" HWND PDC_hWnd` (UIManager.cpp:103, `#ifdef PDCURSES`) | SMTC target, wingui-only |

**Divergences:** none material. The brief's "next/prev goes through
jumpToPlaylistIndex" is imprecise against live: jumpToPlaylistIndex only moves
the cursor; the real next/prev PLAY logic is inline in the key cases (queue
priority, `playlist_.next()/previous()`, CD reopen, follow-mode cursor,
`maybePreloadNext`). Routing plan adjusts accordingly (5). Flagged, not blocking.

## 1. The seam - `core::IMediaControl` (new: include/core/IMediaControl.h)

Mirrors INotify (interface in core, no platform header, production default by
name, constructor injection, fake in tests). Departure: bidirectional +
needs servicing, so it adds a command handler and a `pump()`.

```cpp
namespace core {
struct MediaMeta {                 // resolved consumer-side (reused Discord feed)
    std::string artist, title, album, art_key;   // art_key = image key already computed
};
enum class MediaStatus { Stopped, Playing, Paused };
enum class MediaCommand { Play, Pause, TogglePause, Stop, Next, Previous,
                          SeekToAbs, SeekByRel };
struct MediaEvent { MediaCommand cmd; double seconds = 0.0; };  // seconds for the two seeks

class IMediaControl {
public:
    virtual ~IMediaControl() = default;
    // Outbound (UI thread). Full push on metadata/status change:
    virtual void updateNowPlaying(const MediaMeta&, MediaStatus,
                                  double position_s, double duration_s) = 0;
    // Cheap periodic push for the scrubber (UI thread, ~1x/tick):
    virtual void updatePosition(double position_s, double duration_s, MediaStatus) = 0;
    virtual void clear() = 0;                       // stopped / no track
    // Inbound. Handler invoked by the impl on an OS button/method; it MUST be
    // thread-safe (SMTC fires on a threadpool thread) and MUST only enqueue.
    virtual void setCommandHandler(std::function<void(MediaEvent)>) = 0;
    // Service the transport (Linux: sd_bus_process + non-blocking poll; Windows:
    // no-op - SMTC is COM/threadpool-driven). Called once per main-loop tick.
    virtual void pump() = 0;
};
IMediaControl& mediaControl();     // link-time production default (per platform)
}
```

Header includes only `<string>`, `<functional>` - the portability seam test holds.

## 2. Outbound - tap the resolved Discord feed (no second assembler)

In `updateScrobbler`, right after `deinvertArtist` (:4912), the resolved
`artist/track/album/pos/dur` and `radio_art` already exist. Add, independent of
the discord toggle (its own trigger vars so the two features are orthogonal):

```cpp
if (config_.os_media_control) {
    std::string art = audio_.streamMode() ? audio_.streamArtUrl() : /* file art key */;
    if (artist != media_artist_ || track != media_track_ || art != media_art_) {
        media_artist_ = artist; media_track_ = track; media_art_ = art;
        media_->updateNowPlaying({artist, track, album, art},
                                 audioStatus(), (double)pos, (double)dur);
    }
}
```

Plus a per-tick position push in the main loop (reusing the position the
transport bar already reads): `media_->updatePosition(posNow, durNow, status)`.
`clear()` on stop. `audioStatus()` maps AudioManager state -> MediaStatus.

**REQUIRED CHANGE 2 - position is a cached-value update, no per-tick signal.**
`updatePosition` refreshes the cached value the transport returns on READ but
emits NO change signal per tick:
- MPRIS: `Position` is excluded from `PropertiesChanged` by spec (clients poll
  it; `Seeked` marks discontinuities). `updatePosition` updates the cached
  Position only, emits nothing; `Seeked` fires ONLY on an actual jump (a seek or
  track change), never on normal progression. Per-tick emit would be a spec
  violation + ~12 Hz bus spam.
- SMTC: `UpdateTimelineProperties` pushes on state-change / seek / track-change
  only (the OS interpolates the bar itself), NOT every tick.
- Net: per-tick = refresh cached values; signals only on discontinuity. The impl
  tracks the last-published metadata/status and a jump flag to decide when to emit.

## 3. Threading / marshal - one queue, drained in the loop

A thread-safe command queue on UIManager (mutex + small `std::deque<MediaEvent>`;
commands are human-rare). The handler registered via `setCommandHandler` just
enqueues. The main loop drains once per tick, beside `flushPendingSeek`:

```cpp
media_->pump();                        // Linux: service the bus (callbacks enqueue)
for (MediaEvent e : drainMediaQueue()) applyMediaCommand(e);   // UI thread
```

- **Windows/SMTC:** ButtonPressed fires on a WinRT threadpool thread ->
  enqueue (thread-safe) -> loop drains. `pump()` is a no-op.
- **Linux/MPRIS:** the live loop is `getch()` under `timeout(80)`, NOT a
  `poll()`, so the sd-bus fd cannot fold into the wait. Instead `pump()` calls
  `sd_bus_process(bus, NULL)` (non-blocking) each ~80 ms tick; the vtable
  callbacks run on the UI thread there and enqueue, and the same drain applies
  them. Worst-case command latency ~80 ms (imperceptible for media keys) and the
  bus is touched by ONE thread only (outbound updates are UI-thread too), so no
  sd-bus concurrency to manage. Rejected alternative: a dedicated dispatch
  thread (adds bus-ownership/marshal-for-outbound complexity for no perceptible
  latency win); revisit only if sub-80 ms ever matters.

Audio thread is never touched by any OS callback.

## 4. Inbound split sink - `applyMediaCommand` (UI thread)

- `Play` -> if paused `audio_.togglePause()` else ensure playing; `Pause` ->
  `audio_.pause()`; `TogglePause` -> `audio_.togglePause()`; `Stop` -> `audio_.stop()`.
- `Next` -> `manualNext()`; `Previous` -> `manualPrevious()` (see 5).
- `SeekByRel` and the TUI `[`/`]` keys -> `requestSeek(e.seconds)` - the
  existing relative lane, UNCHANGED.
- `SeekToAbs` -> the NEW absolute lane (REQUIRED CHANGE 1). It does NOT fold to a
  delta at receive time - an OS scrubber DRAG emits a stream of SetPosition
  events inside one coalescing window, and receive-time folding accumulates them
  against a stale (unflushed) position (30-then-60 in one window would land at 80,
  not 60). Instead:
  - The coalescer gains an absolute lane: `std::optional<double> pending_seek_abs_`
    beside the relative `pending_seek_`.
  - `SeekToAbs` sets `pending_seek_abs_ = e.seconds` (LAST-WRITE-WINS) and clears
    the pending relative delta - an absolute "go here" supersedes accumulated
    deltas. No positionSec() read at receive time.
  - `flushPendingSeek` resolves the abs target at FLUSH time against the LIVE
    position: `audio_.seekBy(*pending_seek_abs_ - audio_.positionSec())`, then
    clears it. Still the one `seekBy` home (never raw `seekTo`), so the MP3
    bit-reservoir fix applies.
  - Precedence when both lanes are dirty in one window: the abs target wins (the
    definitive user intent); documented at the field. The `seek_stamp_`
    track-guard applies to both lanes (drop a buffered seek if the track changed).

## 5. Next/Previous one-home (the required refactor)

The next/prev PLAY logic is inline in `case 'n'/'p'`. To route the OS command
through the IDENTICAL logic (repeat/queue/CD/follow-mode), extract the two case
bodies verbatim into private methods:

- `void manualNext();`  <- body of case 'n' (:6397-6431)
- `void manualPrevious();`  <- body of case 'p' (:6433-6447)

`case 'n'/'p'` become one-line calls to them (behavior-identical); the OS drain
calls the SAME methods. This is the one-home the brief mandates and is NOT a
key-binding change (the binding key->action is unchanged; only the body moves to
a callable home). Routing an OS Next through `handleInput('n')` is REJECTED: it
would misfire inside modal overlays (a popup's own 'n'); OS Next must always mean
next-track. **Reported path: `manualNext`/`manualPrevious`.**

## 6. Platform impls

**Windows - src/platform/win/MediaControlSmtc.cpp** (raw WinRT ABI per FINDINGS):
- Construct on the UI thread: `RoInitialize(RO_INIT_MULTITHREADED)`, interop
  factory, `GetForWindow(PDC_hWnd, ...)`, enable flags, `add_ButtonPressed`
  (+ `PlaybackPositionChangeRequested` -> `SeekToAbs`) -> handler enqueues.
- `updateNowPlaying` -> DisplayUpdater (Music title/artist) + PlaybackStatus;
  `updatePosition` -> `UpdateTimelineProperties`; `clear` -> disable + status.
- Body gated on `#ifdef PDCURSES` (wingui owns PDC_hWnd); non-wingui Windows ->
  the no-op impl. Link: `-lruntimeobject -lole32 -luser32 -lgdi32`, plain
  `main()` (no `-municode`).

**Linux - src/platform/linux/MediaControlMpris.cpp** (sd-bus per FINDINGS):
- `sd_bus_open_user`, export `org.mpris.MediaPlayer2` + `.Player` vtables,
  request `org.mpris.MediaPlayer2.remoct`. Methods (Play/Pause/PlayPause/Stop/
  Next/Previous, Seek, SetPosition) -> handler enqueues. Properties
  PlaybackStatus / Metadata (incl `mpris:length`, `mpris:artUrl` from art_key) /
  Position / CanGoNext/Previous/Play/Pause/Control.
- `updateNowPlaying`/`updatePosition` set cached values + emit
  `PropertiesChanged` (and `Seeked` on a position jump); `pump()` ->
  `sd_bus_process` loop. Link: `-lsystemd`.

**mediaControl() default:** returns the platform singleton (SMTC on wingui,
MPRIS on UNIX, a `NoopMediaControl` otherwise) - the `notifier()` shape. A new
`src/platform/{win,linux}/` TU per CMake's existing `if(WIN32)`/`if(UNIX)` lists;
a shared `NoopMediaControl` for the fallback.

## 7. Config + wiring

- `DigiConfig::os_media_control` (bool, default true) in Config.h; load/save in
  Config.cpp mirroring `discord_presence`; a keybinding is OUT (non-goal - no new
  binding), toggled via config only for v1.
- `UIManager` ctor gains `core::IMediaControl* media = nullptr`,
  `media_(media ? media : &core::mediaControl())` - main.cpp unchanged (uses the
  default, exactly as it does for notify). Ctor registers the command handler
  (enqueue). Tests inject a fake.

## 8. Test plan (tests/media_control_test, both toolchains)

- **FakeMediaControl** (records updateNowPlaying/updatePosition/clear calls;
  exposes `fire(MediaEvent)` to simulate an OS command, optionally from a
  spawned thread).
- Outbound: a simulated track change drives `updateNowPlaying` once with the
  resolved metadata; position ticks call `updatePosition`.
- Inbound routing (via a thin testable `applyMediaCommand` seam on a UIManager
  test harness, or a routing free-function the handler calls): Pause -> the
  AudioManager pause path; Next/Previous -> `manualNext/Previous`; SeekByRel ->
  `pending_seek_`/`seek_dirty_` set (NOT a raw `seekTo`); SeekToAbs -> delta
  computed, coalescer set.
- Marshal: `fire()` from a non-UI thread leaves state untouched until the loop
  drain runs, then applies - proves no inline application.
- ctest both toolchains, full suite green; fresh-link verification (confirm the
  `Linking ... media_control_test` line before trusting the pass).

Note: driving the full UIManager loop headlessly is heavy; the test targets the
routing + marshal seam (a small `applyMediaCommand` + the queue) directly, the
same way the recorder tests exercise the engine without the UI. If review wants
the outbound tap tested through the real `updateScrobbler`, that needs a
UIManager test harness - flag as a scope bump or keep it at the seam.

## 9. Diff shape (surgical; on greenlight)

New: `include/core/IMediaControl.h`, `src/platform/win/MediaControlSmtc.cpp`,
`src/platform/linux/MediaControlMpris.cpp`, a shared `NoopMediaControl`
(header-inline or a small TU), `tests/media_control_test.cpp`.
Edited: `UIManager.h` (ctor param, `media_`, queue, `manualNext/Previous`,
`applyMediaCommand`, `audioStatus`), `UIManager.cpp` (extract n/p bodies; the
outbound tap in updateScrobbler; the loop pump+drain; ctor handler registration),
`Config.h`/`Config.cpp` (`os_media_control`), `CMakeLists.txt` (two platform
files + `-lruntimeobject`/`-lsystemd` link), `tests/CMakeLists.txt`,
`CHANGELOG.md` (Added, hyphens only).

## 10. Open items for review
1. Test depth (8): seam-level routing/marshal (recommended) vs a full UIManager
   harness for the outbound tap.
2. `manualNext/Previous` extraction (5) - confirm it's in-scope (it is the
   one-home the brief asks; flagged only because it edits the n/p case bodies).
