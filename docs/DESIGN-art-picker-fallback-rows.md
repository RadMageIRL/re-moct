# DESIGN — art picker: iTunes and Deezer rows

**Design note. Not greenlit. Untracked until it is.**

**This proposal touches / does not touch AR_PREGAP, `ar_crc.*`, or the read addressing.**
**Does not touch.** Cover-art HTTP and picker UI only. No CD read, no rip path, no CAA behaviour.

Report first, per the brief. Two collisions, then the four questions. Everything marked **measured**
was run against the live APIs on 2026-08-12 with the project's own gate album (Joan Osborne,
*Relish*).

---

## COLLISION 1 — "these candidates already arrive" is false at picker time

**This is the blocker, and it decides whether the feature is buildable as scoped.**

`bytesByText` is called from **`src/CDRipper.cpp:1708`**, inside `CDRipper::worker` — that is, after
the confirm screen has been dismissed and the rip has started. The picker runs from the confirm
modal, **before** that. At picker time the iTunes and Deezer responses **do not exist anywhere in the
process.**

What actually happens today on the two disc kinds the brief targets — and they are *different*, which
the brief treats as one case:

| disc | confirm-modal preview | `^`-picker today |
|---|---|---|
| **Discogs release, no MBID** | **nothing** — `refreshRipArt` only fetches when `!mbid.empty()` (`UIManager.cpp:13232`) | **refuses to open.** `openArtPicker` returns early with the toast *"No cover-art listing for this release"* (`:13239-13244`) |
| **MB release, CAA has no front** | `frontThumbByMbid` returns `{}`, box stays empty | opens, then says *"The archive lists no images for this release"* (`:13270`) |

So the brief's *"Dos currently gets one image with no way to change it"* is right about the
**outcome** — the rip embeds one image — but the image is chosen inside the ripper, long after the
only screen that could have offered a choice. **The picker is not "empty" on these discs; on the
commonest one it never opens.**

**Consequence:** rows cannot be fed from candidates that "already arrive", because none have. The
picker must issue the iTunes and Deezer searches itself.

**Whether that is "a new request" depends on which reading you meant, so I am not deciding it:**

- **No new endpoint, no new API surface, no new kind of call.** They are the same two GETs
  `bytesByText` already makes, with the same terms, moved earlier in the same user action.
- **But they are two GETs that do not happen today at picker time**, and they happen on opening the
  picker whether or not anything is chosen.

**Cost if built:** opening the picker on a no-CAA disc = 2 search GETs (~3 KB of JSON each), plus one
250 px thumbnail GET per row the cursor rests on — the identical debounced, on-demand pattern the CAA
rows already use (`serviceArtPicker`, `art_thumb_settle_ < 3`, `UIManager.cpp:13292-13305`). It is
**not** a thumbnail per row up front.

**Double-search risk, and it is avoidable.** If the picker searches and Dos then chooses, the choice
becomes `art_override_` (`UIManager.cpp:7481` → `CDRipper.cpp:1699`) and `bytesByText` never runs —
one round total. If he opens the picker and chooses **nothing**, the ripper searches again — two
rounds. Holding the picker's chosen *bytes* is what already prevents the first case; nothing prevents
the second, and it is one extra search per abandoned picker.

**If the answer is "that is a new call, don't", the feature stops here** and this note is the reason
why. Everything below assumes it is ruled acceptable.

---

## COLLISION 2 — the two sources are not symmetrical

Measured, `results[0]` / `data[0]` field inventories:

| | iTunes | Deezer |
|---|---|---|
| artist | `artistName` | `artist.name` |
| title | `collectionName` | `title` |
| **year** | `releaseDate` (`1995-03-21T…`) | **none** |
| **country** | `country` (`USA`) | **none** |
| track count | `trackCount` | `nb_tracks` |
| type | `collectionType` (`Album`) | `record_type` (`album`) |
| 250 px thumb | by substitution — see below | `cover_medium` |

**Deezer album search returns no release date and no country.** A row template with year and country
columns is therefore two blank columns wide on every Deezer row. That is the "either looking broken"
risk in the brief's own words, and it is real.

---

## MEASURED — 250 px thumbnails exist on both. Checked, not assumed.

The brief asked for this explicitly.

**iTunes.** `artworkUrl100` ends literally `…/100x100bb.jpg`. The existing code already string-swaps
`100x100` → `600x600` (`CoverArt.cpp:226-227`). The same swap to `250x250` works:

```
100x100 -> HTTP 200    6,475 bytes
250x250 -> HTTP 200   26,430 bytes
600x600 -> HTTP 200  130,189 bytes
```

**Deezer.** The search response already carries `cover_small` (56), **`cover_medium` (literally a
`…/250x250…` URL)**, `cover_big` (500), `cover_xl` (1000). The current code reads **only** `cover_xl`
and `cover_big` (`CoverArt.cpp:252-253`); `cover_medium` is present in the JSON already parsed and
simply unread.

**So both sources give a 250 px thumbnail from the response already in hand** — the same size CAA
publishes (`CaaImage::thumb_url`, "the 250px variant, for the preview"), the same `cover::render`
path, no extra call to discover it. **This is the one part of the brief that needs nothing negotiated.**

---

## MEASURED — the real discriminator is track count, and today's automatic pick gets *Relish* wrong

Both services, same query, return exactly two albums:

| | iTunes | Deezer |
|---|---|---|
| row 0 | **Relish (Expanded Edition)** — 20 tracks | **Relish** — 12 tracks |
| row 1 | **Relish** — 12 tracks | Relish (Expanded Edition) — 20 tracks |

Same artist. Same year. Same country. **Track count is the only field that separates them** — and the
disc's own TOC count is available at picker time (`audio_.cdSource().tracks().size()`, already used in
the confirm modal at `UIManager.cpp:2404`).

**Now the part that matters for ordering.** `album_overlap` counts how many tokens of the *requested*
album appear in the candidate. For `album = "Relish"` that is one token, `"relish"`, which is present
in **both** titles:

```
album_overlap("Relish", "Relish")                     = 1
album_overlap("Relish", "Relish (Expanded Edition)")  = 1
```

The scores **tie**, and `best_score` uses a strict `>` (`CoverArt.cpp:223`), so the first-listed
candidate wins. **On iTunes that is the Expanded Edition — so today RE-MOCT embeds 20-track art on a
12-track *Relish* disc, and the decision is made by iTunes' result order, not by any scoring of
ours.** That is the brief's *"where the automatic pick is most likely to be wrong"*, measured, on the
project's own gate album.

*(No change to that scoring is proposed — the brief lists it as a non-goal. It is reported because it
determines the answer to the ordering question below.)*

---

## Q1 — The row

**CAA rows lead with the comment because it is the only discriminating field there. Here the
discriminating field is the track count, and the row that shows it already exists.**

`CandidateRow` + `formatCandidateRow` (`include/MBLookup.h:241-264`) is a pure, unit-tested
(`tests/disc_pick_test.cpp:237-264`) layout already shared by `^F` and `^R`, carrying exactly:
`artist`, `title`, `disambig`, `year`, `country`, `right` (a track count, rendered `"19t"`), and
`from_discogs` → `[D]`.

**That is this row.** Not a new grammar — the row Dos already reads fluently, filled from a third
source:

```
This disc has 12 tracks.

 1. Joan Osborne   Relish (Expanded Edition)      1995  USA  20t  [iT]
 2. Joan Osborne   Relish                         1995  USA  12t  [iT]   (automatic)
 3. Joan Osborne   Relish                                     12t  [dz]
 4. Joan Osborne   Relish (Expanded Edition)                  20t  [dz]
```

- **The disc's own track count goes in the header line, not the rows.** It is one fact about the
  disc, not a property of any candidate, and stating it once lets every `20t` speak for itself
  without RE-MOCT ranking on it.
- **No "matches" marker on rows.** Equality is a fact, but a 12-track *different* album matches a
  12-track disc just as exactly, so a tick would read as verification the data cannot support.
  Showing both numbers is the honest form and Dos does the comparison in one glance.
- **The disambiguation is already inside the title** for these services ("Relish (Expanded
  Edition)"), so `disambig` stays empty and the existing "disambiguation gets its space first"
  logic (`MBLookup.h:274-278`) simply does not fire.
- **Blank year/country on Deezer rows is the honest rendering** — `formatCandidateRow` already omits
  empty fields rather than padding them (`:261-263`), so Deezer rows come out shorter, not
  hole-punched. That is Collision 2 answered by a function that already handles it.
- Strings are folded **before** filling the struct, per the header's standing contract (`:226-228`).

**One pre-existing trap, flagged not fixed:** `formatCandidateRow` truncates with `substr` and
documents itself "ASCII by contract". Folded CJK passes through verbatim since 1.6.1, so a
sufficiently long CJK title can be split mid-character *in `^F` today* — and reusing the function
here extends that exposure to a third list. Not introduced by this work, not proposed for fix here,
but it is a real consequence of the reuse and you should know it before approving the reuse.

## Q2 — Is the source visible? **Yes, and `[D]` is the precedent but not the letter.**

With both services in one list, the same album appears twice with near-identical text (rows 2 and 3
above). **Without a marker that reads as a bug**, which is precisely the failure the brief names.

**Do not reuse `[D]`.** It already means *Discogs* in the `^F` and `^R` lists Dos reads fluently, and
`[D]` for Deezer would give one bracket two meanings in two lists. Proposed: **`[iT]`** and
**`[dz]`** — two characters, visually distinct from each other and from `[D]`, same tail position
`from_discogs` uses. This needs one extra field on `CandidateRow` (a short source tag) rather than
overloading the existing bool.

## Q3 — Ordering: **keep each service's own order. Do not sort by our score.**

Measured above: on *Relish* the two candidates **tie at 1** on `album_overlap`. Sorting by it would
produce an order decided by nothing, presented as a ranking — the same objection that keeps CAA in
archive order, arriving by a different road. CAA's reason is "free text supports no ordering"; here
the reason is stronger, because we *have* a score and it is **measurably non-discriminating on the
first album tested.**

Service order also makes the list honest about the automatic path: iTunes is tried first and its
first row is what the automatic pick took, so **iTunes' row 1 is the "(automatic)" row** and it sits
at the top where it would be looked for.

**Grouped by service, iTunes then Deezer** — mirroring `bytesByText`'s own precedence exactly. Not
interleaved: interleaving would imply a cross-service ranking that nothing computes.

## Q4 — Both sources at once? **One list, grouped, both visible.**

The user's question is "which cover", not "which service", so a split pane doubles the UI for a
distinction that is metadata. Grouping plus the source tag keeps provenance legible without a second
widget.

**Do not dedupe across services.** Two rows titled "Relish" are two *different images* from two
catalogues — sometimes different masters, different crops, different quality. Same title is not same
image, and collapsing them would hide the choice this feature exists to offer.

**CAA rows and fallback rows can never coexist**, by construction: the fallback runs only when CAA
returned nothing (`CDRipper.cpp:1703-1708`). So the picker shows CAA rows **or** fallback rows, never
mixed — which is why the two row grammars (comment-led vs candidate-row) can differ without either
looking broken. **The two sources that must share a list are iTunes and Deezer, and they share
`CandidateRow`.**

---

> **BUILT AND SHIPPED 2026-08-12, FALLBACK HALF UNTESTED.** Approved and
> implemented as proposed, with one change: the automatic marker is a star in the
> source tag (`[iT*]`) rather than an appended "(automatic)", because
> `formatCandidateRow` pads to exactly `width` and reserving room for the word
> truncated the title - the one field that distinguishes these rows - at the
> picker's real 50-column width. **Dos has no Discogs-sourced disc and no disc
> where CAA lacks a front cover**, so the fallback rows have never run live.
> Recorded as an OPEN VERIFICATION entry in `docs/roadmap.md`, with what would
> settle it. The CAA half is unchanged and was regression-tested.

## Gates and the live test

Both toolchains, `EXIT=0`, ctest both, banner read and quoted — standard.

**The live test needs a disc where CAA comes up empty, and I cannot confirm one is to hand.** Per the
brief: saying so is a finding, not a pass. Two ways to reach the state, in order of likelihood:

1. **Any Discogs-sourced release** — no MBID, so CAA is never queried at all. This is the common case
   and the one where the picker currently refuses to open.
2. An MB release whose CAA entry has no front cover.

If neither is available, the honest outcome is *"built, gated, not live-tested"* — and the CAA
regression half (*a disc with CAA art behaves exactly as it does today*) **is** testable on any
normal disc and should be run regardless.

---

## Summary of what needs a ruling before code

1. **Collision 1** — the picker must issue the two searches itself. Same endpoints, new timing.
   **If that counts as "a new call", the feature stops.**
2. `[iT]` / `[dz]` as source tags, and one new field on `CandidateRow`.
3. Reuse of `formatCandidateRow`, accepting its documented ASCII-truncation limitation on a third
   list.
