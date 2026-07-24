# Podcast chapters from feed-referenced JSON - design of record

Status: PLAN, awaiting Dos's approval. No code written.

Measured basis (2026-07-23): Dos's Jupiter Broadcasting feeds carry
`<podcast:chapters url type="application/json">` on 879 of 1,540 items (LUP
404/676, Self-Hosted 149/151, Coder 275/602, Office Hours 37/39, Extras
14/72); every downloaded LUP episode has one. Zero ID3 CHAP/CTOC frames in any
downloaded file across all shows - ID3 stays dead. The sampled documents are
Podcasting 2.0 chapters JSON v1.1.0: a "chapters" array of {startTime seconds,
title, ...} - nearly 1:1 with the existing chapter model (start_sec + title).

---

## 1. When the fetch happens - the argument

**Policy: fetch on first need, never in bulk, never on the UI thread.** Two
trigger points, one shared cache, at most one network hit per episode ever:

1. **Download-time (primary).** The download worker already has the network
   open for a 100 MB audio file; the ~2 KB chapters document rides along on
   completion, exactly as the worker already caches feed art on complete
   ("online guaranteed - downloaded eps show poster offline"). A downloaded
   episode therefore has chapters OFFLINE, which matters: the episode you
   pulled at home and open on the couch without wifi must not have lost its
   chapter list to lazy fetching.
2. **Browse-time, lazy and async (the `;` case).** `;` on a NOT-downloaded
   episode whose item carries a chapters URL kicks a background fetch
   (cache-first), mirroring the feed/art async idiom - atomic active/done +
   mutex-guarded result + want-key staleness guard + run-loop pickup + join in
   the destructor. The status line shows "Loading chapters ..." in the podcast
   voice; the pane opens when the fetch lands. One fetch, cached forever, and
   the keypress never blocks - network inside a keypress is only acceptable
   when it is not inside the keypress.

Rejected: **feed-refresh bulk** (879 fetches to populate lists mostly never
opened - absurd, per the brief); **download-time only** (fails the
browse-before-download case the chapter fix just made the point of `;`);
**lazy-only** (fails offline playback of a downloaded episode, and every
legacy download from before this slice would need the network at `;` time
forever - with priming, only until first view).

Legacy downloads (pre-slice files with no sidecar) are covered by trigger 2.

## 2. Where the pieces live

- **`include/PodcastFeed.h`** - `PodcastEpisode` gains `chapters_url`
  (`<podcast:chapters url="...">`, attr-read helper already exists for
  enclosure/itunes:image). Parser stays header-inline and pure.
- **`include/PodcastChapters.h`** (NEW, header-inline, pure) -
  `parsePodcastChaptersJson(const std::string&) -> std::vector<Mp4Chapter>`.
  Reuses the existing `Mp4Chapter` chapter model (start_sec + title), so the
  pane, seek, and cursor logic see no new type. Parsing via the vendored
  `lib/json.hpp` in no-exceptions mode (`parse(s, nullptr, false)`) - nlohmann
  is already in product TUs (CDRipper, CoverArt, LastFm): no new dependency.
- **`PodcastClient`** - `fetchChapters(url) -> {std::string body; bool
  fetched}`, thin wrapper over the existing seam with tight caps (1 MB /
  15 s / same UA). The 64 MB feed cap is wrong for a KB-scale document.
- **`UIManager`** - sidecar resolution, the async browse fetch worker +
  per-tick pickup, the dispatch (below), download-worker priming
  (`PodcastQueueItem` gains `chapters_url`, resolved at enqueue like the art
  fields), and the message split (below).

## 3. Cache: beside the episode

`<episode cache file>.chapters.json` - plain string append to the existing
cache path (NEVER `fs::path` composition on titles: episode cache paths are
already `pathSafeAscii`, and string-append keeps the CP1252 trap out). The
sidecar stores the RAW fetched JSON; the parser is the single trust boundary
on every read. The sidecar may exist without the audio (browse-fetched) -
harmless. Episode delete (`d`) removes the sidecar with the audio: it is
derived, refetchable data, unlike resume state which stays by design.

## 4. The dispatch in refreshChaptersIfNeeded

The seam anticipated by the ID3 investigation, now used:

    refreshChaptersIfNeeded(path, sidecar = "")     // overload, default empty
      1. MP4-family extension  -> parseMp4Chapters(path)      [UNCHANGED]
      2. still empty && sidecar exists -> parse the sidecar JSON

Only the `;` handler's EPISODE branch passes a sidecar (it alone knows the
episode); books and plain files call the one-arg form and cannot reach step 2
- zero behavioral or fs-probe change outside podcasts, honoring "nothing
moves". An `.m4a` episode keeps its embedded chapters as today; the feed JSON
fills in only when the file yields none. (Podcasting 2.0 suggests the feed
document *supersede* embedded chapters; deliberately not adopted in v1 to
keep the MP4 path untouched - flagged for a future ruling if a show ever
ships both and they disagree.)

Memoization note: `chapters_for_path_` keys the cache as today; the episode
branch re-arms `chapters_ep_*` exactly as the shipped fix does.

## 5. Hostile input - files from the open internet

- **Fetch caps**: 1 MB body, 15 s timeout, cancel-able like every fetch.
- **Parse**: nlohmann no-exceptions mode; anything malformed -> `discarded` ->
  empty chapter list -> honest "no chapters". Invalid UTF-8 fails the parse.
- **Shape validation**: top level must be an object with a `chapters` array;
  `version` is read if present but absent/unknown does not reject (the array
  is the contract). Per entry: `startTime` must be a finite number >= 0
  (negative, NaN, string, absent -> entry skipped); `title` a string (absent
  -> "Chapter N"), trimmed to 256 chars and passed through
  `sanitizeForDisplay` at parse time so control characters never reach
  curses. Per-chapter `img`/`url`/`toc` extras ignored - the model is
  start + title.
- **Order and degeneracy**: entries sorted by startTime (out-of-order exists
  in the wild); exact-duplicate startTimes deduped keep-first. Zero-length
  and overlap are meaningless in a start-point model. startTime past the end
  of file is kept - seekTo already clamps to duration.
- **Volume caps**: chapter count capped at 512 (a 100k-entry array must not
  build a UI list); one pass + one sort, nothing quadratic - the stsc-class
  lesson applied to JSON is "linear or reject", and both parse and validate
  are linear.

## 6. The viewing/playing split (replaces the shipped message)

"Download the episode to view chapters" was correct when chapters lived only
in files; for JSON it is wrong on the VIEW half and stays right on the PLAY
half. New behavior for a NOT-downloaded episode:

- `;` with a chapters URL -> **viewing works**: cache-first async fetch,
  "Loading chapters ..." status, pane opens on arrival; fetch failure ->
  honest status ("Couldn't fetch chapters"). No file needed.
- `;` with no chapters URL -> "No chapters in this track" (honest: nothing
  embedded reachable, nothing published).
- **Chapter-select Enter** on a not-downloaded episode -> toast **"Download
  the episode to play from a chapter"** - the old message MOVES from
  view-time to play-time, where it is still true: episodes play as local
  files by architecture and the live stream path cannot seek (settled).
  A possible future nicety - enqueue the download and seek to the chosen
  chapter when it lands - is real machinery (a pending-seek through the
  download queue) and is NOT in this slice; separate ruling if wanted.
- Downloaded episode: identical to today - chapter-select through
  `playEpisodeFile`, identity kept, resume suppression exactly as shipped.

## 7. What must not move (and does not)

- `parseMp4Chapters` and every MP4 path: untouched.
- The `;` handler's shape (toggle, focused-pane refresh, open-cursor logic):
  unchanged; the episode branch's else-arm changes message/behavior only as
  specified in section 6 - which the brief explicitly requests.
- Chapter-select seek and BOTH resume suppressions (book pre-latch, episode
  pending-clear): byte-identical.
- Feed subscription flow, feed refresh cadence: untouched (parsing one new
  per-item attribute is not a subscription change).
- No ID3 work. No new dependencies. No crossfade/resolver/queue contact.

## 8. Tests (machine, both platforms)

- `podcast_parse_test`: inline case - `<podcast:chapters url>` extraction into
  `chapters_url` (present, absent, malformed attr).
- **`podcast_chapters_test`** (NEW, header-only, device-free): valid v1.1.0
  doc; garbage bytes; truncated JSON; non-object top level; missing chapters
  key; entry-level: negative/NaN/string/absent startTime, absent title
  synthesis, 256-char trim, control-char sanitization; out-of-order sort;
  duplicate dedupe; 100k-entry cap; 1 MB-boundary body.
- `podcast_client_test`: `fetchChapters` request shape + caps via FakeHttp.
- UI wiring (async fetch, status line, message split): hardware-gated by Dos,
  per convention.

## 9. Approval Q&A (Dos, pre-code) - three answers + two minors

**A1 - chapters failure never costs the episode.** The chapters fetch runs
strictly AFTER `fetchToFile` success, outside the retry state machine
entirely: the episode is already complete (file present = truth, glyphs
update, play_when_done fires) before the chapters attempt starts. A 404 /
timeout / garbage body is LOGGED (the existing Log facility) AND DROPPED -
`attempts` is never touched, nothing is requeued, no toast. The sidecar is
simply absent, and the browse-time lazy fetch remains a natural self-heal:
the next `;` on that episode retries with the then-current URL. A dead
chapters URL can cost at most the chapters, never the audio.

**A2 - staleness: refetch on URL change, checked at point-of-use.** The
sidecar pair is `<file>.chapters.json` (raw doc, the trust boundary) plus
`<file>.chapters.src` (one line: the URL that produced it). At each of the
two fetch/use points - download-time priming and `;` evaluation - if a
sidecar exists but `.src` differs from the episode's CURRENT `chapters_url`
(feeds re-fetch on every enter, so it is fresh), both files are deleted and
the fetch runs again. O(1) per use, and deliberately NOT checked at
feed-enter: per-episode fs probing on feed entry is exactly the load-path
regression the slice-5 postmortem reverted. Accepted gap, stated as a
decision: a publisher who edits the document BEHIND THE SAME URL stays
stale in our cache. A time-based revalidation (e.g. at most once a week)
would close it and is left un-adopted for now - separate ruling if wanted.

**A3 - `;` hammering against an in-flight fetch.** Single-flight worker with
a want-key (the episode id), latest-wins - the art/feed idiom:

- Press 1 (no sidecar, URL known): fetch starts, status "Loading chapters
  ...", the pane does NOT open (nothing to show), pane state unchanged.
- Press 2 (same episode, fetch in flight): no second fetch, no cancel - the
  want-key matches, the press re-affirms the status line. Idempotent under
  hammering; a fetch in flight is never restarted or aborted by `;`.
- `;` on a DIFFERENT episode mid-flight: want-key moves; the old result
  still lands in ITS cache when done (never wasted) but does not open; the
  new episode's fetch starts (single worker, queued behind the join).
- Landing: pane auto-opens only if the want-key still matches the currently
  highlighted episode; otherwise cache-silently and clear the status. The
  want-key IS "what the user last asked for".
- Press after landing: normal toggle-close. Press after close: sidecar hit,
  instant open, no network.
- Failure: status "Couldn't fetch chapters", want-key cleared; a LATER `;`
  retries (one attempt per explicit press - human-rate, no storm, no latch
  needed).

**Minor 1 - directory existence:** browse-fetched sidecar writes go through
the dir-ensuring path (the `episodeCachePath` mkdir wrapper idiom), so a
feed never downloaded from gets its directory created with the sidecar.

**Minor 2 - `type` other than application/json:** the parser reads the
`type` attr; absent or `application/json` -> eligible to fetch (an absent
type that turns out to be XML parses to discarded -> honest no-chapters at
one bounded fetch's cost); any OTHER explicit type -> no fetch at all ->
falls through to the honest no-chapters message.

## 10. Hardware gate (proposed for Dos)

1. `;` on a downloaded LUP episode -> chapters appear (from the sidecar,
   offline OK after first fetch/priming).
2. Select a chapter -> playback starts there; resume does not yank it.
3. `;` on a NOT-downloaded chaptered episode -> "Loading chapters ...", then
   the list; select -> "Download the episode to play from a chapter".
4. `;` on an episode with no published chapters -> honest no-chapters.
5. Airplane test: downloaded-with-sidecar episode shows chapters offline.
6. Delete an episode -> sidecar gone; re-download -> chapters return.
7. Books, playlist chapters, `.m4a` embedded chapters: unchanged.
8. Both platforms.
