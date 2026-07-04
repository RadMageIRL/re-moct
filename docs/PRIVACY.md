# Privacy Policy for RE-MOCT

**Last Updated:** June 25, 2026

Your privacy is a priority. RE-MOCT is a local-first, terminal-based application: it runs on your machine, plays your files, and stores its data on disk. The developer operates **no servers** and collects **no data** from you.

However, RE-MOCT is not an offline-only application. Several of its features - metadata lookup, internet radio, scrobbling, and Discord presence - work by contacting third-party services. This policy explains exactly what is sent, to whom, and when.

## 1. What the Developer Collects

**Nothing.** RE-MOCT contains no telemetry. It does not track your usage patterns, keystrokes, listening history, or preferences, and it transmits nothing to the developer or to any analytics service. There is no "phone home."

## 2. Local Data

All of your audio files, playlists, ripped tracks, and configuration stay on your local machine. RE-MOCT only reads the directories you explicitly point it at for playback and ripping; it does not scan the rest of your file system.

Configuration - including recent/favorite tracks and the credentials described in Section 4 - is stored in a plaintext file:

- **Windows:** `%APPDATA%\RE-MOCT\remoct.conf`
- **Linux:** `~/.config/RE-MOCT/remoct.conf`

## 3. Third-Party Network Services

RE-MOCT contacts external services to provide specific features. These requests are made **as a result of actions you take** (identifying a disc, ripping, tuning a station, enabling scrobbling) - they are not background activity. The relevant services are:

| Feature | Service(s) | What is sent |
| --- | --- | --- |
| Disc / album identification | MusicBrainz, Discogs | Disc IDs (TOC-derived) and artist/title text queries |
| Rip verification | AccurateRip, CUETools Database | Disc IDs and per-track checksums |
| Cover art | Cover Art Archive, iTunes, Deezer | Release IDs and artist/track text |
| Station discovery | radio-browser.info | Search terms and station requests |
| Internet radio | iHeart (`api.iheart.com`, `stream.revma.ihrhls.com`) | Station/stream requests and now-playing polling |
| Scrobbling | Last.fm, ListenBrainz | The artist, track, album, and timestamp of what you play |
| Rich Presence | Discord (local IPC) | Currently-playing artist/track, sent to the Discord client running on your machine |

These services are operated by their respective providers and are governed by **their own privacy policies**, not this one.

## 4. Credentials and Authentication

Scrobbling and other authenticated features require you to provide credentials, which RE-MOCT stores **unencrypted** in the configuration file described in Section 2:

- **Last.fm:** API key and session key
- **ListenBrainz:** user token

Anyone with read access to that file can read those secrets. Protect it accordingly, and revoke the credentials from the respective service if you believe it has been exposed.

## 5. Your Control

The network features above are optional. If you do not configure scrobbling, do not use internet radio, and do not run metadata lookups or rips, RE-MOCT can be used purely as a local player with no outbound network traffic for those features. Scrobbling and Discord presence in particular only activate once you provide credentials or have Discord running.

## 6. Contact

If you have questions about this policy or the software's behavior, please reach out via the [GitHub repository issues page](https://github.com/RadMageIRL/re-moct/issues).
