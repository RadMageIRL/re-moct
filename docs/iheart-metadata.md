# Reading iHeartRadio's Live Metadata

*Field notes on getting correct now-playing data out of iHeart streams — the source ladder, the failure modes, and why the feed behaves the way it does.*

---

This writeup comes out of the work behind [RE-MOCT](https://github.com/RadMageIRL/re-moct), a terminal music player that handles iHeart stations natively. It is aimed at anyone building a player, a scrobbler, or a now-playing display that needs to work with iHeart. The audio always plays — the problem is purely the metadata, and it is more annoying than it should be because the fix is undocumented.

## There is no single source — there is a ladder

The common advice is “the metadata is not in the stream, read the API instead.” That is only half right, and believing it fully will cost you songs. In practice a station's now-playing can come from one of a few places, and which one has the song varies by station and even by *program* — with a floor beneath them for when none of the three answer:

1. **In-band, in the HLS segment tag.** Some stations DO embed the full song (artist + title) in the per-segment `#EXTINF` tag. When they do, this is the *best* source — it rides the audio and cannot go stale while music plays.
2. **The trackHistory API**, keyed on the station's numeric ID. Real-time-ish, but a *history* feed — it lags and can freeze for a long time (see "Reference implementation" below).
3. **currentTrackMeta** (the endpoint the web player itself polls). On some stations it returns the current track; on others it returns 204/empty for hours. Session/auth does not change this — it is server-side per station.
4. **The floor: no reachable source has it.** During some programming a station publishes no now-playing on *any* of the three sources above, and the official iHeart web player shows a plain “LIVE” too. This is not a fourth source you read — it is a floor you accept. When every reachable channel is dark, an honest LIVE is the correct answer, not a miss.

So the correct approach is not “read the API” — it is **read whichever source is freshest, prefer in-band when it carries the song, and fall back down the ladder.** The rest of this page is each rung and how it breaks.

> **Why it is like this — iHeart is not the origin.** The reason no single source is authoritative, and the reason the same song can appear under two different artists minutes apart, is that **iHeart does not author this metadata — it ingests it.** Track and artist data are supplied to iHeart by the music's *distributor*, pushed in as XML. iHeart states this itself: their own help documentation instructs artists that metadata corrections “need to be sent to iHeartRadio from your distributor… via XML,” not fixed by iHeart directly ([iHeart help: Updating Metadata](https://help.iheart.com/hc/en-us/articles/5107893006605)).
>
> That single fact explains the whole ladder below it. iHeart is a distributor-fed ingestion-and-display layer, reconciling asynchronous XML pushes from sources it does not control. When two sources disagree — a track credited to a group in one record and to its lead artist in another, both correct — iHeart surfaces the disagreement rather than resolving it, because the authoritative record lives one hop upstream of iHeart, and therefore two hops upstream of you. **No client reading iHeart can be more correct than the distributor feed iHeart received.** The staleness, the freezes, the relabels, and the empty responses are all symptoms of an ingest layer sitting on top of feeds that arrive at their own cadence — not bugs in your reader. This is the structural reason an honest floor, not a fabricated title, is the correct behavior when the sources go quiet or disagree.
>

## Finding the station ID

The station ID is the number in the stream URL:

```
https://stream.revma.ihrhls.com/zc4366/hls.m3u8   ->   station id 4366
```

## The now-playing endpoints

Two API endpoints are keyed on that ID. The **history** feed:

```
https://api.iheart.com/api/v3/live-meta/stream/<id>/trackHistory
```

returns a history array of recent tracks — artist, title, album, cover art, and timing (start time, and often an end time or duration). Unauthenticated and station-ID-keyed, so no login or token is required. And the **current-track** feed the web player polls:

```
https://us.api.iheart.com/api/v3/live-meta/stream/<id>/currentTrackMeta?defaultMetadata=true
```

This one returns the single current track. It is worth polling as a second opinion, but be warned: on some stations it returns 204 (no content) for the entire length of a program while a song is clearly airing. That is not an auth problem — sending a minted anonymous session, a real session, or nothing at all returns the same 204. It is a server-side, per-station gap. Do not build your display on it alone. If you also need the station's canonical name, there is a resolve endpoint:

```
https://api.iheart.com/api/v2/content/liveStations/<id>
```

## Why “just take entry 0” fails

The obvious implementation is “GET trackHistory, take the first entry, show the title.” It looks right and desyncs constantly in the wild. Three real-world behaviors break it:

1. **The first array entry is not reliably the newest.** Some stations resurface an older entry above newer ones. Scan for the entry with the newest start time rather than trusting array order.
2. **The feed can list not-yet-aired tracks.** Entries with a start time in the future (beyond now, plus a small grace window for clock skew) should be filtered out, or you will display the next song early.
3. **The feed can freeze or regress.** If the newest entry is older than one you have already shown, hold the current track rather than rewinding the display to a stale song. A monotonic guard on the accepted start time only ever suppresses a regression — it never invents a track.

There is also a useful staleness signal: comparing the current track's end time against now tells you whether it is still playing or ended N seconds ago. That lets you fall back to a plain “LIVE” indication during an ad break instead of leaving a finished song on screen.

**But do not underestimate how long it can freeze.** “The feed can regress” makes this sound occasional. It is not. On ad-dense or talk-heavy programming — a morning show, a countdown — `trackHistory` can post no new entry for *hours*. Measured directly: one station sat on the same entry for over four hours across a morning show while real songs came and went underneath. Your staleness signal will climb into the thousands of seconds and stay there. This is why `trackHistory` cannot be your only source: it is a *history* feed, and during exactly the programming where you most want a title, it goes silent. Its silence is not your bug — it is the feed. Read the in-band segment tag alongside it (see "In-band: the segment tag").

## Reference implementation

The core now-playing read from RE-MOCT, in C++ (using `nlohmann::json`), with the three guards:

```
// iHeart's live metadata is NOT in the HLS stream - it is a separate JSON API
// keyed on the station id from the stream URL:
//   .../zc4366/hls.m3u8  ->  id 4366  ->  .../live-meta/stream/4366/trackHistory
std::string IHeartRadio::pollNowPlaying(long* endedSecsAgo) {
    std::string body; long st = 0;
    if (!httpGet(meta_url_, body, st) || st != 200) return {};

    json j;
    try { j = json::parse(body); } catch (...) { return {}; }
    if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) return {};

    const json& data = j["data"];
    long now = (long)std::time(nullptr);
    const long FUTURE_GRACE = 60;   // tolerate small clock skew

    // Guard 1: array order is not reliable - scan for the newest AIRED entry.
    int best = -1; long bestStart = 0;
    for (int i = 0; i < (int)data.size(); ++i) {
        long s = data[i].value("startTime", 0L);
        if (s <= 0 || s > now + FUTURE_GRACE) continue;   // Guard 2: skip not-yet-aired
        if (best < 0 || s > bestStart) { best = i; bestStart = s; }
    }
    if (best < 0) return {};                              // nothing aired yet

    // Guard 3: monotonic - never rewind to an entry older than one already served.
    if (bestStart < accepted_max_start_) return {};
    accepted_max_start_ = bestStart;

    const json& t0 = data[best];
    long endTime = t0.value("endTime", 0L);
    long dur     = t0.value("trackDuration", 0L);
    if (endTime == 0 && bestStart > 0 && dur > 0) endTime = bestStart + dur;

    // Staleness: <=0 still playing, >0 ended N seconds ago, -1 unknown. Lets the
    // caller fall back to "LIVE" during an ad break rather than show an ended song.
    if (endedSecsAgo) *endedSecsAgo = (endTime > 0) ? (now - endTime) : -1;

    std::string artist = t0.value("artist", std::string());
    std::string title  = t0.value("title",  std::string());
    if (artist.empty() && title.empty()) return {};
    return artist.empty() ? title
         : (title.empty() ? artist : artist + " - " + title);
}
```

The language does not matter; the shape does. Poll `trackHistory` on a timer (the web player polls every ~10 seconds, a reasonable cadence), apply the three guards, and dedup so you only fire a display update on an actual change.

## In-band: the segment tag (often the best source)

Each HLS segment carries an in-band tag in its `#EXTINF` line. On many stations that tag contains the **actual song**, not just a music/ad flag:

```
#EXTINF:10,title="Listen To Your Heart",artist="D.H.T.",url="song_spot=\"M\" ..."
```

When a station populates `title=`/`artist=` like this, read it — and **prefer it over trackHistory**. It rides the audio, so unlike the history feed it cannot go stale while music plays. A station that carries the song in-band will stay accurate through the exact ad-dense windows where `trackHistory` freezes. This is why the ladder puts in-band at the top.

The catch: **not every station does this.** Some leave the tag on a boundary marker (`title="Spot Block End"`, `length="00:00:00"`, or blank) while a real song plays — those give you nothing and you fall back down the ladder. So treat in-band as “best when present, absent on some stations,” not universal.

The same tag also carries the music-vs-ad signal — the green/amber strip at the top of this page:

- **Music:** `song_spot="M"` (or `"F"`) with a real artist.
- **Paid ad:** `song_spot="T"` with a real `spotInstanceId` (not `"-1"`).
- **Station id / jingle / promo:** `song_spot="T"` with `spotInstanceId="-1"` — a mixed bucket; not a paid ad.

Two gotchas that matter if you try to *act* on the ad signal (e.g. to skip or re-pin around breaks):

1. `song_spot="T"` is a mixed bucket (jingles + paid ads), so classify on `spotInstanceId`, not the letter, or you will mislabel station liners as ads.
2. **The paid-ad markers are frequently absent for most of a real break.** On many stations the primary manifest goes to a blank/“Spot Block” slate mid-break rather than tagging each ad with its `spotInstanceId`. Measured across real ad pods, the explicit paid-ad marker was present in only a few percent of them. So you cannot detect a whole break by waiting for `spotInstanceId`; the honest read is “marked ad” for the segments that carry it and “unknown — probably a break” for the blank stretches. Duration and the absence of a song, not the ad flag, are what actually delimit a break.

**This is undocumented and can change.** These are the endpoints the web player itself uses, discoverable in any browser's network inspector while a station plays. iHeart can change them at any time — treat the exact paths as “confirm against a live capture,” not a stable contract.

**Behavior varies by station and was observed in mid-2026.** Which rung of the ladder has the song, whether the manifest carries the title in-band, and whether `currentTrackMeta` or `trackHistory` answer all differ station to station; some stations publish nothing at all during some programming. Everything here was measured against specific stations at a specific time; verify against the station you actually target.

**Timeline drift is not a metadata bug.** A native player can fall behind the live edge and lag the broadcast (see "Stay at the live edge"). Before blaming the feed, check how far behind the edge you are.

**Sometimes nobody has the title — and that is correct.** During some programming no source publishes a song, and the official iHeart web player shows a plain “LIVE” too. Degrading to an honest LIVE in those moments is matching the official behavior, not failing.

**Ads are part of the broadcast.** Reading the metadata correctly tells you what is playing; it does not change what is playing. Every listener, including the official web player, receives the same ad-supported stream.

## If you are not a browser: stay at the live edge

One failure looks like a metadata bug but is not. A browser HLS player stays glued to the **live edge** by default — it keeps a small buffer and always fetches the newest segment. A native player with its own buffering does not do this for free: on a long stretch it can drift *behind* the live edge and play buffered audio that lags the broadcast by many seconds. When that happens, you are hearing an older segment (say, still in a talk break) while the live edge has already moved on to the next song — so your screen shows LIVE while “the current song” is playing on the broadcast. It reads as “we lost a song,” but the real problem is timeline drift, not metadata.

The manifest gives you the tool to detect it: the newest segment's media sequence number minus the sequence you are about to play is how many segments you are behind the edge. If that gap grows past a small threshold, re-anchor to the live edge (re-fetch the playlist and jump forward). Browsers do this invisibly; a native player has to do it on purpose. If your titles are correct but occasionally lag the broadcast, this — not the API — is usually why.

## Takeaway

Do not try to read iHeart now-playing from timed metadata inside the HLS segments the ID3 way — but do not assume a single API replaces it either. There is a ladder: read the **in-band #EXTINF song tag** when the station carries it (it can't go stale while music plays), fall back to **trackHistory** with the three guards (scan newest, filter future, stay monotonic) and the staleness signal, optionally cross-check **currentTrackMeta**, and accept the floor beneath all three: on some stations, during some programming, none of them answer. Prefer whichever source is freshest. And be honest about the floor: `trackHistory` freezes for the length of some programs, the paid-ad markers are mostly absent mid-break, and during certain programming — a countdown, a talk block — *no* source has a title, and the official iHeart web player shows a plain “LIVE” too. Matching it means falling back to an honest LIVE in exactly those moments, not inventing a title nobody published.

And the reason none of this is your bug to fix: iHeart does not originate this metadata — it ingests distributor-supplied XML and displays a best-effort reconciliation of it. The authoritative record lives upstream of iHeart, so no reader can be more correct than the feed iHeart received. Build to that ceiling honestly rather than fabricating a certainty the source never had.

This came out of building [RE-MOCT](https://github.com/RadMageIRL/re-moct), a keyboard-driven terminal music player and CD ripper for Windows and Linux that handles iHeart stations natively. If it is useful for your own player or scrobbler, that is the point — the metadata problem did not need to stay undocumented.
