# rec-art-pending-race-fix - PLAN

**Stage:** PLAN deliverable. Anchors resolved against the live checkout, diff
shape proposed. No source changed yet. Plan-first cadence: review -> implement
-> debrief -> Dos hardware gate -> commit. Do NOT commit or push.
**Baseline:** `experimental/win-pdcurses` @ head (`5dcfcdc`, slice B, CI-green).

## 0. Anchor resolution (live tree - no divergence from brief)

Every symbol resolved; line numbers shifted from the diagnosis run by slice B,
symbols identical. Nothing sits on the audio thread.

| symbol | live location | role |
|---|---|---|
| `onArt(np)` slot write | StreamRecorder.cpp:176-185 | replace with ring insert |
| pending members | StreamRecorder.h:206-210 | replace with the ring |
| `start()` pending reset | StreamRecorder.cpp:120-124 | reset the ring |
| PCM match | StreamRecorder.cpp:562-566 (`rollCut`) | replace with lookup + evict |
| copy match | StreamRecorder.cpp:826-831 (`rollCopyCut`) | replace with lookup + evict |
| hold arm | StreamRecorder.cpp:601-607 (`workerLoop`) | unchanged (context) |
| `cut_raw_` assigns | 637/647 (workerLoop), 898/904 (copyLoop) | unchanged |
| UIManager push | UIManager.cpp:1147 | unchanged (already passes raw np) |

**Audio-thread check (brief's stop condition):** PASS. `pending_*` + `meta_mtx_`
are documented "Never touched by the audio thread" (StreamRecorder.h:201-202);
the audio thread's only entry is `capture()` -> `push()`, which touches `ring_`
and `wpos_`/`dropped_` exclusively. The ring lives beside `pending_*` under
`meta_mtx_`, touched only in `onArt` (UI thread) and the two roll paths
(worker thread) - exactly where the slot is touched today. No divergence to
report; proceeding to the diff shape.

## 1. The ring (StreamRecorder.h)

Replace the three scalar pending-art members with one bounded, raw-np-keyed
ring. `std::deque` for clean evict-oldest/evict-on-use; capacity 3.

```cpp
// Pending cover art (rec-cover-art): a small raw-np-keyed ring, NOT a single
// slot - the split-trim hold keeps a cut open across later songs, whose art
// would otherwise clobber the one slot before the held cut rolls
// (rec-art-pending-race-fix). Newest wins on a key collision; insert past
// capacity evicts oldest; a roll takes its cut's entry and evicts it. A cut
// discarded to ads/ never consumes its entry - the capacity bound (not a map)
// is what reclaims those. Guarded by meta_mtx_; never touched by the audio thread.
struct PendingArt { std::string key; std::vector<uint8_t> bytes; const char* mime = nullptr; };
static constexpr size_t kPendingArtMax = 3;
std::deque<PendingArt> pending_arts_;
```

- Remove `pending_art_raw_`, `pending_art_`, `pending_art_mime_`.
- Add `#include <deque>` (header currently pulls `<vector>/<string>/<mutex>` but
  not `<deque>`).
- New private helper declaration beside the other worker-side helpers:
  `bool takePendingArt(const std::string& key, std::vector<uint8_t>& out, const char*& mime);`

## 2. onArt - ring insert (StreamRecorder.cpp:176-185)

```cpp
void StreamRecorder::onArt(const std::string& raw_now_playing,
                           std::vector<uint8_t> bytes) {
    if (!running_.load(std::memory_order_relaxed)) return;
    const char* mime = artMimeFromMagic(bytes);
    if (!mime) return;                 // reject early: no broken picture blocks
    std::lock_guard<std::mutex> lk(meta_mtx_);
    // Newest wins on a key collision (a song's art re-fetching replaces it).
    for (auto it = pending_arts_.begin(); it != pending_arts_.end(); ++it)
        if (it->key == raw_now_playing) { pending_arts_.erase(it); break; }
    pending_arts_.push_back({ raw_now_playing, std::move(bytes), mime });
    if (pending_arts_.size() > kPendingArtMax) pending_arts_.pop_front();  // evict oldest
}
```

## 3. The lookup helper (new; StreamRecorder.cpp)

Both roll sites carry the identical locked-read block today; factor it so the
diff stays surgical and the two paths cannot drift.

```cpp
// Worker thread: take the art queued for `key` (a cut's raw now-playing), if
// present, and evict it. Returns true and fills out/mime on a hit; leaves them
// untouched on a miss (the caller's no-art path). One locked read, like the
// slot it replaces.
bool StreamRecorder::takePendingArt(const std::string& key,
                                    std::vector<uint8_t>& out, const char*& mime) {
    std::lock_guard<std::mutex> lk(meta_mtx_);
    for (auto it = pending_arts_.begin(); it != pending_arts_.end(); ++it)
        if (it->key == key) {
            out  = std::move(it->bytes);
            mime = it->mime;
            pending_arts_.erase(it);
            return true;
        }
    return false;
}
```

## 4. Both match sites (StreamRecorder.cpp:562-566 and 826-831)

Each locked block collapses to:

```cpp
std::vector<uint8_t> art;
const char* art_mime = nullptr;
takePendingArt(cut_raw_, art, art_mime);
```

The `!pending_art_.empty()` guard drops out: on a miss `art` stays empty and the
`tagCut`/`tagCutM4a` art branches already gate on `!art.empty() && art_mime`, so
behavior on the no-art path is byte-identical to today.

## 5. start() reset (StreamRecorder.cpp:120-124)

Replace the three pending-art clears with `pending_arts_.clear();` (keep
`pending_raw_.clear();`).

## 6. Test (stream_record_test - closes the blind spot)

New case, default hold on (the interaction test 4b never exercised - it uses
`split_offset_ms = 0`). Models fetch latency so the ordering is the real one:
B's art lands while A's cut is still held.

- `split_offset_ms = 1200` (the shipped default).
- Song A = `"Shins, The - New Slang"` (inverted - proves the fix is independent
  of the deinvert path), art A (300-byte JPEG).
- Song B = `"Sia - Unstoppable"` (forward), art B (500-byte JPEG - a DIFFERENT
  size so cross-contamination is detectable, not just presence).
- Sequence: onTitle(A), onArt(A), push 1 s; onTitle(B) [hold arms, A still open];
  onArt(B) [would clobber the old single slot]; push >1.2 s so A's hold expires
  and A rolls; onTitle(C=stop) / stop so B rolls.
- Assert: `cuts[0]` (A) `has_art` and `art_size == 300`; `cuts[1]` (B) `has_art`
  and `art_size == 500`. Each cut carries ITS OWN art, proven by size.
- Opus-only for speed (readOpus already returns has_art + art_size).

This is the promoted form of the diagnostic that reproduced the bug
(hold=0 -> art kept; hold=1200 -> art lost); with the ring both hold values
keep each cut's art.

## 7. Gates

- ctest both toolchains (Windows UCRT64 + WSL2 Debian) - full suite.
- **Freshly-linked binary verification (brief's explicit ask - false green
  twice):** build the `stream_record_test` target and CONFIRM the
  `Linking CXX executable ... stream_record_test` line appears in the build
  output before trusting the run; if the linker is skipped (up-to-date), touch
  the TU or clean the target so the assertion runs against new code.
- CHANGELOG: a Fixed entry (recorded cuts now keep their cover art when the
  split hold is on), hyphens only, no em-dash.

## 8. Scope / non-goals honored

- No key-derivation change (keys already agree - the diagnosis proved it).
- No fetch/provider or embed change.
- No CD-path / `ar_crc.*` change; the Joan Osborne gate does not apply.
- Surgical diff only: one header member swap + include, one helper, three call
  sites (onArt + two rolls), one reset line, one new test, one CHANGELOG line.
  No full-file replacements.

## 9. Divergences / notes

1. None from the brief's anchors - all symbols resolved as described.
2. The helper (§3) is a small addition beyond a literal slot->ring swap; it
   exists only to keep the two roll sites from duplicating the lookup and
   drifting (the same one-home discipline the rest of the recorder follows).
   If review prefers the lookup inlined at both sites instead, that is a trivial
   variant - flag it and I will inline.
3. `deque` over a fixed `array`: capacity 3 is tiny either way; deque gives
   clean `pop_front`/`erase` for evict-oldest and evict-on-use. No heap concern
   at this size and cadence (one alloc per song change, off the audio thread).
