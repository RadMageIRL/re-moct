# Session handoff - 2026-08-15 11:27

**RE-MOCT 1.6.2 IS RELEASED.** Tag `1.6.2` → `35c9e95`, `experimental/win-pdcurses` / `dev` / `main`
all in sync with origin, working tree clean. **The only thing outstanding is Dos cutting the Windows
binary and publishing the release page** - both his, both deliberate.

Continues `docs/session-handoff-2026-08-12-2007.md`. That handoff ended with stream recovery shipped
and the 1.6.2 ceremony outstanding. This one closes the release and adds three things to it.

---

## NEXT SESSION STARTS HERE

1. **Nothing is running, nothing is half-done.** Both toolchains green, everything pushed.
2. **1.6.2 shipped today.** `docs/CLAUDE.md` is already flipped, in the same sitting, with the tag
   and merge SHA - the step that went stale for a day on 1.6.1 did not this time.
3. **READ THE CONFIGURE BANNER before trusting any build.** Still the cheapest mistake available.
4. **Three things are parked and visible on `docs/roadmap.md`** - the m4b bug and two unverified
   items. §5. None blocks anything.
5. `docs/LOCKED-CODE.md` before anything near the CD path. The audio thread is also locked and the
   m4b bug may eventually reach it - §5.1 says what would decide that.

---

## 1. What shipped in 1.6.2 since the last handoff

**Bit-perfect playback and the output-rate indicator** (`59fc75f`, and four follow-ups). The feature
is `bit_perfect=1` in `remoct.conf`, config-only, lossless local files. **The finding was worth more
than the feature and that is not a figure of speech:** all 11 WASAPI endpoints on the Windows box
report native 48000 and none offers 44100, so every CD rip had been resampled by Windows for years,
invisibly. The indicator now says so - `44.1 kHz -> 48` - **ungated**, because it is true whether or
not anyone opted in.

**The two-machine result is the thing to remember.** Windows: rips convert, Opus does not. Linux:
rips are untouched, Opus converts. Same code, same library, **opposite answers** - because it reports
the output device, not the collection. On the Windows box the *lossy* file was the one reaching the
DAC untouched.

**All four states are hardware-verified**, including `=` firing against a real DAC on the Linux box
(2026-08-13). That was the last item on the built-but-unverified list and it came off.

**The About screen says where the project lives** (`06be569`) - `github.com/RadMageIRL/re-moct` and
`re-moct.app`, no `https://` prefix, placed above the keybindings hint so a short window drops the
hint before the address. Checking widths found two pre-existing problems: centring used `strlen`
rather than display columns, and the draw was unbounded so any line wider than the pane **wrapped**
onto the row below - reachable today, since the pane is half the screen and the version line needs
~134 columns. Both fixed; a wrap is now a clean cut.

**The About version line named "ncurses" on both platforms** and had been false for every shipped
Windows build since 1.6.1. It now follows the same switch CMake gates the link on - which is also the
whole of the PDCursesMod credit, at the cost of no extra row.

---

## 2. The m4b bug: recon done, deliberately NOT fixed

`docs/RECON-m4b-chirp.md` (**untracked by ruling** - parked, not shipped). Roadmap entry has the
durable version.

**Six things it is not, each by measurement:** decode, device bring-up on the isolated production
path, the pitch reading on the healthy path, varispeed, loudness, and the rate indicator.

**It does not reproduce.** Five runs, three files, warm-up on **and** off, from zero and from Dos's
own resume positions, driving the real `LocalFileSource` through a real device bring-up. Device came
up 44100 → internal 44100 every time; zero under-fills; the device never self-stopped. The pitch
hypothesis predicts **+8.8%** consumption and measurement showed **+0.7%**.

**Varispeed was the strongest live candidate and is ruled out, not confirmed** - `speed_` genuinely
survives a track change, but `[+16%]` was on screen with playback correct *at that speed*.

**`24.0 kHz -> 48` on an m4b is explicitly NOT a symptom** and the roadmap says so, because a future
session finding that in a bug report would otherwise read it as evidence.

**One question decides the shape:** *first file after launch, or only after something else played
first?* That separates a first-bring-up or per-track teardown failure - neither of which needs to
reach the audio thread - from the callback body and crossfade arming, which might. **Until it
reproduces, whether it needs a locked-code ruling is unanswerable**, which is why it is parked and
not scoped.

---

## 3. The ceremony, and the mistake in it

Standard chain: `experimental/win-pdcurses` → `dev` → `main` → annotated tag `1.6.2`, bare, no `v`.

**I stamped the release 2026-08-13. The clock, the tag and both merges said 2026-08-15.** I took the
date from the conversation instead of running `date` - in the step that exists to catch exactly that.
Corrected on the branches, but the tag had already been pushed, so **the tagged tree carried the
wrong release date**. Dos ruled: move it, because nothing had consumed it and that window is the only
time it is free. Tag re-pointed `a1d2698` → `35c9e95` and force-pushed; verified from inside the tag
that the CHANGELOG now reads `2026-08-15`.

**Deliberately left alone:** the `2026-08-12` stamps on bit-perfect and stream recovery, and
`2026-08-13` on `=` firing. `git log` confirms those are accurate - moving five correct records to
tidy one wrong one is the wrong trade.

**Known loose end, flagged not chased:** `CLAUDE.md` inside the tag says `merge a1d2698` while the tag
sits on `35c9e95`. Both are true about different things (the release merge, and the follow-up
carrying the corrected date). Making them identical costs another commit → merge → merge → tag move,
and each lap is a fresh chance at the error just corrected.

### Other ceremony findings

- **`docs/index.html` needed no version reconcile** - already fully 1.6.2, no `1.6.1` anywhere. Its
  *release blurb* was stale though, still describing 1.6.2 as the scanner and the shimmer.
- **The CHANGELOG summary contradicted four of its own entries.** It promised *"nothing changes about
  how anything is played, ripped or recorded"* - true when written, false once entries below
  described an inaudible drop, a dead stream stopping, a recording finalized and an exclusive device.
- **`RE-MOCT-doc.html` does not exist** anywhere in the tree. Reported rather than invented.
- README and index.html both named ncurses for a two-platform product; fixed in three places. README
  also gained `bit_perfect` in the sample config, since it is config-only and Configuration is the
  only place a user could find it.

---

## 4. Docs changed

| file | tracked | what |
|---|---|---|
| `CHANGELOG.md` | yes | `[1.6.2] - 2026-08-15`; summary rewritten to stop contradicting itself; `-> dsp` also names balance |
| `docs/CLAUDE.md` | yes | release paragraph flipped with tag + merge SHA. **Back to 201 lines** by folding the 1.6.1 detail now it is two releases back |
| `docs/roadmap.md` | yes | bit-perfect fully verified and off the unverified list; the m4b entry rewritten around what is ruled out |
| `docs/lessons.md` | yes | the clock-not-the-conversation lesson; and what a non-reproduction is worth (§6) |
| `README.md` | yes | curses libraries per platform; `bit_perfect` documented |
| `docs/index.html` | yes | release blurb; badge and footer name both curses libraries |
| `docs/RECON-m4b-chirp.md` | **no** | parked, not shipped - untracked by ruling |

---

## 5. Parked and visible, none blocking

**5.1 - The m4b bug.** §2. The one open question is the launch-ordering one.

**5.2 - The art picker's iTunes/Deezer fallback rows.** Built, gated, **never run live** - reaching
them needs a Discogs-sourced disc or an MB release with no CAA front cover, and neither exists in the
collection.

**5.3 - Does the GHD3N answer C2 over SG_IO?** `docs/phase3-slice6-design.md` §2 accepted an honest
limit on the premise that the drive is non-C2, **which is false**. If the CHECK CONDITION had another
cause then `use_c2` is true on real Linux and the de-interleave runs having never executed on
hardware. **More reachable now than when it was recorded** - there is a real Linux box in play.

**5.4 - `docs/warn-sweep-plan.md`**, still not greenlit, now down to the original GCC 15.2 inventory
since both GCC 16 rows were deleted. Its live section is the Linux-only `-Wtype-limits` trio - three
cooldowns that never block on Linux, two of them in the stream machinery.

---

## 6. Process notes from this stretch

1. **Check the clock, not the conversation.** §3. The date step catches the date error only if the
   date comes from `date`.
2. **A pushed tag is free to move only while nothing has consumed it.** Check consumption first and
   let that decide, rather than reaching for `--force` because it is convenient.
3. **When a probe does not reproduce, write down what it could not observe.** That turns a dead end
   into a ranked search space - it is the only reason the m4b bug could be parked honestly rather
   than guessed at.
4. **A success code says the call returned, not that you got what you asked for.** `ma_device_init`
   with exclusive share mode returns success while silently converting. Same shape as AccurateRip: a
   match proves the stream you checksummed is correct, not the file you kept.
5. **Measure the endpoint, not the abstraction.** Every conclusion in the bit-perfect work came from
   probing the real devices; reasoning about the path would have produced the opposite answer, since
   miniaudio's `internalSampleRate` reports the rate we asked for and cannot see Windows resampling
   below it.
6. **`git add -A docs` will eat the untracked fence.** It swept four not-greenlit notes and eighteen
   handoffs into the index; caught at the staged diff. Stage by name near that directory.
