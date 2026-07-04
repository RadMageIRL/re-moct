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
| `iheart_http_dump.cpp` | Reproduces RE-MOCT's digital (web-player) iHeart handshake, then polls the resolved variant playlist and dumps every `.m3u8` manifest revision verbatim - to inspect the steering tags (`EXT-X-PROGRAM-DATE-TIME`, `DISCONTINUITY`, `MEDIA-SEQUENCE`). Audio segments are not downloaded. |
| `SniffIHeartRadio.cpp` | Active prober (not a packet sniffer) that walks iHeart's undocumented now-playing API for a station and dumps every stage's raw + pretty JSON - how a `zc####` HLS URL resolves to now-playing metadata. |
| `StreamHandshakeProbe.cpp` | Answers whether iHeart's ad-reduced "digital" rendition will mint a token for an **arbitrary** listenerId/profileId or requires a real session. Prints the full redirect chain for RAW vs. web-player requests. |
| `LatencyProbe.cpp` | Measures how far RE-MOCT's raw iHeart rendition sits behind the web player's, decomposing the metadata/audio lead into prime-buffer, live-edge lag, and metadata-ahead-of-playout components (shared segment-number timeline). |
| `radio_probe.cpp` | Mirrors `StreamSource::connect()` exactly (same UA, timeouts, flags, `Icy-MetaData` header) but with no decoder/ring/UI - isolates whether a station periodically drops the connection independently of RE-MOCT's playback code. |
| `lb_probe.cpp` | Headless verification of the ListenBrainz client: prompts for a token (hidden), validates it, sends one `playing_now` + one completed listen, prints the result URL. Depends on the project's `ListenBrainz`/`Log` sources (see its header). |

## Building a tool

Each file's header comment carries its exact build line. The self-contained WinINet
probes are typically just:

```bash
# MSYS2 UCRT64 shell
g++ -std=c++20 tools/src/iheart_http_dump.cpp -o iheart_http_dump.exe -lwininet
```

> **Note:** the build/run lines inside the historical headers may reference the paths
> they were authored at (e.g. an old scratch directory). They are frozen field-note
> snapshots - adjust the paths to your checkout. `lb_probe.cpp` is not standalone; it
> links a few of the project's own `src/` files.

Related iHeart case-study material - the reverse-engineering write-up and the reference
module snapshot - lives under [`docs/IHeartRadio/`](../docs/IHeartRadio/).
