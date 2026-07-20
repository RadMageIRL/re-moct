# RE-MOCT tools

Standalone, educational probes written during development to validate protocols and
timing **before** integrating them into RE-MOCT. They are **not part of the build** -
each is a single translation unit you compile on demand. The sources are tracked;
prebuilt binaries are not (build them yourself with the one-liner in each file's header).

These captured the "probe-first" discipline: prove a parsing/protocol assumption with a
throwaway tool, *then* write the real module. Several read as annotated field notes on
iHeart's undocumented API and ICY/HLS streaming behavior.

## The tools (`tools/src/`)

| Tool | What it does |
|------|--------------|
| `iheart_http_dump.cpp` | Reproduces RE-MOCT's digital (web-player) iHeart handshake, then polls the resolved variant playlist and dumps every `.m3u8` manifest revision verbatim - to inspect the steering tags (`EXT-X-PROGRAM-DATE-TIME`, `DISCONTINUITY`, `MEDIA-SEQUENCE`). Audio segments are not downloaded. Also carries an ad-tier classifier (`--analyze`/`--diff`) and a **dual-station `trackHistory` capture mode** (`--capture`) that logs the full `data[]` feed for two+ stations as JSON-lines plus the player's would-select state (the four `pollNowPlaying` guards ported verbatim), for the Z100-vs-Breeze ad-boundary metadata differential. The capture mode also reads an **in-band channel** per tick (the media manifest's newest segment: class + raw artist/title), so trackHistory feed health is never mistaken for music airtime; the summary reports `feedHealth%` vs in-band airtime vs the `trackHistory-BLIND-while-music%` accuracy gap. Needs `-Ilib` (vendored `json.hpp`); see the file header for usage and the Phase 1/2 plan. |
| `nowplaying_wide_probe.cpp` | Wide now-playing visibility probe: fires station x session x channel (ctm / trackHistory / graphql GetOnAirNow / raw in-band ground truth) every tick and logs one JSONL record per tick, so channel behavior is measured as **rates with n over a window** - never concluded from a single sample. Sessions: `none` / `anon` / `minted` (links `IHeartIdentity` for the persisted anonymous profileId) / `legit` (env `NP_LEGIT_BEARER`/`NP_LEGIT_COOKIE` or per-station cookies in `--stations=id:slug:cookie`); `--none-only` collapses the axis once proven null. All HTTP through `core::IHttp` (links `HttpWinInet.cpp`; Linux = swap in `HttpCurl.cpp`). Findings to date: session axis is null (auth is not the lever), graphql `GetOnAirNow` is the show schedule (not the song), ctm is intermittent and adds 0% song-level marginal over trackHistory, and ~13-30% of aired song-minutes (per station) have **every** server channel dark - in-band only. |
| `np_ledger.py` | The acceptance-output analyzer for `nowplaying_wide_probe` captures: a **song-minutes ledger** - for each song that actually aired (in-band = truth), did each channel show it correctly, misses cause-tagged (`ctm-204` / `feed-frozen-stale` / `schedule-not-song`) - plus per-station channel coverage, the without-ctm test (ctm's marginal value over trackHistory), and the **freeze-window marginal** (does one channel rescue song-minutes while the other is frozen; how many song-minutes are both-frozen/in-band-only). Rates alone hide the answer; the ledger is the deliverable. |
| `false_live_cross.py` | The **false-LIVE measurement**: crosses a `nowplaying_wide_probe` capture against a *concurrent* deep-analysis log (Ctrl+A) and counts song-minutes where a real song was determinably airing while the committed display read LIVE. Classifier-independent - the probe names songs via the raw in-band tag / ctm / trackHistory, so it sees false-LIVE minutes the deep log's own `mfCls` structurally cannot (classifier-quiet freezes). Strict anti-stale rule: a tick counts only if the raw tag names the song or ctm+trackHistory independently agree (a stale trackHistory entry naming the previous song through a genuine ad break must not count). Reports total, the `mfCls`-blind share, per-channel naming, and episodes >=30s with timestamps for eyeball verification. Verify the deep log's `stationId` matches the crossed station first. |
| `gate_replay.cpp` | Offline, read-only **shadow gate replay**: replays deep-analysis logs through a mirror of the now-playing SM's target ladder plus a proposed alternative gate, wall-clock-weighted and cause-tagged, and reports recovered-vs-regressed song-minutes plus the **target-vs-committed delta** (did the SM target a song the commit path never showed - with a sustained-run measure separating real stucks from per-boundary onset-lag slivers). Carries its own trust metric: the mirror must reproduce the recorded `tgtKind` (~100%) before any comparison is believed. Proves display-logic changes on recorded air before touching the app. |
| `SniffIHeartRadio.cpp` | Active prober (not a packet sniffer) that walks iHeart's undocumented now-playing API for a station and dumps every stage's raw + pretty JSON - how a `zc####` HLS URL resolves to now-playing metadata. |
| `StreamHandshakeProbe.cpp` | Answers whether iHeart's ad-reduced "digital" rendition will mint a token for an **arbitrary** listenerId/profileId or requires a real session. Prints the full redirect chain for RAW vs. web-player requests. |
| `LatencyProbe.cpp` | Measures how far RE-MOCT's raw iHeart rendition sits behind the web player's, decomposing the metadata/audio lead into prime-buffer, live-edge lag, and metadata-ahead-of-playout components (shared segment-number timeline). |
| `radio_probe.cpp` | Mirrors `StreamSource::connect()` exactly (same UA, timeouts, flags, `Icy-MetaData` header) but with no decoder/ring/UI - isolates whether a station periodically drops the connection independently of RE-MOCT's playback code. |
| `lb_probe.cpp` | Headless verification of the ListenBrainz client: prompts for a token (hidden), validates it, sends one `playing_now` + one completed listen, prints the result URL. Depends on the project's `ListenBrainz`/`Log` sources (see its header). |

## Building a tool

Each file's header comment carries its exact build line. The self-contained WinINet
probes are typically just:

```bash
# MSYS2 UCRT64 shell (-Ilib pulls in the vendored json.hpp the capture mode uses)
g++ -std=c++20 -Ilib tools/src/iheart_http_dump.cpp -o iheart_http_dump.exe -lwininet
```

> **Note:** the build/run lines inside the historical headers may reference the paths
> they were authored at (e.g. an old scratch directory). They are frozen field-note
> snapshots - adjust the paths to your checkout. `lb_probe.cpp` is not standalone; it
> links a few of the project's own `src/` files.

Related iHeart case-study material - the reverse-engineering write-up and the reference
module snapshot - lives under [`docs/IHeartRadio/`](../docs/IHeartRadio/).
