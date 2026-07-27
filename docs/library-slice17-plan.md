# LIB-S17 - the directory poll evicts you from every virtual section

**Design note. Not greenlit. Untracked until it is.**

Branch `experimental/win-pdcurses`, tip `ce2286b`. Reported from hardware as Linux-only.

---

## 0. Verdict

**The hypothesis is correct, and the bug is bigger than the report in two directions.**

1. It is **not `[Library]`-specific.** `[Radio]`, `[Podcasts]`, `[FAVs]`, `[Recent]` and `[Books]`
   are all exposed by the same line. Only `[Drives]` is guarded.
2. It is **not Linux-specific.** Measured on both platforms: the poll's condition fires on
   Windows too. Linux is where it fires *by itself*; Windows needs something to actually change
   the folder, which happens all the time. **Windows has the identical defect today.**

It is also **not search-specific** - the poll runs regardless. The search bar is why it looks like
one second rather than two (§2).

---

## 1. The mechanism, read from the tree

[UIManager.cpp:1730-1746](src/UIManager.cpp#L1730-L1746), in the main loop:

```cpp
// Periodically check if the current directory changed on disk
if (!in_drive_list_ && ++dir_poll_ticks_ >= DIR_POLL_INTERVAL) {
    dir_poll_ticks_ = 0;
    try {
        auto mtime = fs::last_write_time(current_dir_);
        if (mtime != dir_mtime_) {
            dir_mtime_ = mtime;
            ...
            refreshDir();
```

**The guard is `!in_drive_list_` and nothing else.** Every other virtual section leaves the poll
running, and `refreshDir()` is the reset-trap site itself
([UIManager.cpp:9638-9650](src/UIManager.cpp#L9638)):

```cpp
void UIManager::refreshDir() {
    in_drive_list_ = false;
    in_recent_     = false;
    in_favs_       = false;
    in_radio_      = false;
    in_books_      = false;
    in_podcasts_     = false;
    in_podcast_feed_ = false;
    in_library_      = false;   // the reset trap: BOTH sites, or a refresh leaks
    libnav::reset(lib_nav_);
    ...
```

So the poll calls the one function whose job is to tear down every section, while a section is on
screen. `current_dir_` still holds wherever the folder browser was, so `refreshDir()` relists that -
which is exactly the reported symptom: **the pane reverts to `/mnt/hgfs` with the pin list showing.**

**Why `[Drives]` alone is guarded** is the tell. Someone hit this in the drive list, where
`current_dir_` can be stale, and guarded that one case instead of the class. The comment says
"Periodically check if the current directory changed" - and the precondition it actually needs is
*"the browser is currently showing `current_dir_`"*, which is a different statement and is false in
seven sections.

---

## 2. Why "about a second" rather than two

`DIR_POLL_INTERVAL` is **25** ([UIManager.cpp:1289](src/UIManager.cpp#L1289)) and the loop is
`timeout(80)`, which reads as 2 seconds. It is not, because **`dir_poll_ticks_` increments once per
loop ITERATION, not once per 80 ms**. `getch()` returns immediately when a key is waiting
([UIManager.cpp:1704](src/UIManager.cpp#L1704)), so typing spins the loop as fast as the keystrokes
arrive. Twenty-five characters into the collection-search bar is well under a second.

**And the counter is not reset on section entry.** `refreshDir()` zeroes it
([:9652](src/UIManager.cpp#L9652)); `enterLibrarySection`, `enterPodcastSection` and
`showRadioStationList` do not. So ticks accrued while browsing carry straight into the section, and
the eviction can land almost immediately after entry. That accounts for the variability as well as
the timing.

This is why it presents as search-related: the search bar is the one place in `[Library]` where a
user types fast enough to spin the poll. **Entering `[Library]` and doing nothing hits it too, just
at the full two seconds.**

---

## 3. Measured - the trigger, on both platforms

The product's exact condition (`last_write_time(dir) != dir_mtime_`), against a real directory:

| filesystem | baseline | after a file is created there | after one is removed |
|---|---|---|---|
| Windows NTFS `C:\Users\david\Music` | no | **YES** | no |
| Linux ext4 `/home/dostrom` | no | **YES** | **YES** |
| Linux 9p/drvfs `/mnt/c/...` | no | **YES** | **YES** |

**So the defect is live on Windows.** Enter `[Library]`, let a download finish or a rip write a
track into whatever folder the browser was last showing, and within two seconds the section throws
you out. Nobody hit it because it needs a coincidence; `/mnt/hgfs` removes the coincidence.

**What I could NOT reproduce, and am not claiming.** I have no HGFS mount. I sampled
`last_write_time` 40 times over ~3 s on every filesystem available here - ext4, 9p/drvfs, `/tmp`,
`/mnt` - and **all were stable**, so the spontaneous firing is specific to `/mnt/hgfs` and is
**inferred, not measured**: vmhgfs-fuse's mount root is a synthetic listing of the shares, and a
moving or per-stat mtime there would explain it exactly. That inference is not load-bearing - the
fix is required either way, because the poll must not run in a section regardless of what the
filesystem reports.

---

## 4. A second defect found alongside it

[UIManager.cpp:8980-8984](src/UIManager.cpp#L8980) - the live-search hook:

```cpp
if (goto_active_ && input_mode_ == InputMode::LibrarySearch
    && goto_input_ != lib_nav_.query) {
    lib_nav_.query = goto_input_;
    showLibrarySearch();
}
```

**No `in_library_` term.** After the poll has cleared the flags, the search bar is still open and
still bound to `InputMode::LibrarySearch`, so **the next keystroke repopulates the pane with library
search results while `in_library_` is false.** The pane then alternates between folder browser and
library results depending on which ran last, with every section-scoped key taking the
folder-browser branch. This is consistent with the capture showing the bar open over a reverted
pane, and it needs fixing whether or not it is currently reachable by another route.

---

## 5. Proposed fix

### 5.1 The poll's precondition, stated correctly

The poll may only run when the browser is actually listing `current_dir_`:

```cpp
if (!inVirtualSection() && ++dir_poll_ticks_ >= DIR_POLL_INTERVAL) {
```

with one predicate declared beside the flags it reads:

```cpp
// Is the browser showing something OTHER than current_dir_'s real contents?
// Every virtual section populates dir_entries_ from its own source, so anything
// keyed on current_dir_ - the mtime poll above all - is meaningless there.
bool inVirtualSection() const {
    return in_drive_list_ || in_recent_ || in_favs_ || in_radio_
        || in_books_ || in_podcasts_ || in_library_;
}
```

`in_podcast_feed_`, `in_radio_search_` and `in_podcastindex_search_` are sub-modes of sections
already listed, so naming them would be redundant, not safer.

### 5.2 The thing to decide - THIS WOULD BE THE THIRD COPY OF THAT LIST

`refreshDir()` and `enterDriveList()` already carry the same set of flags, and their own comments
say so: *"the reset trap: BOTH sites, or a refresh leaks"* and *"the same complete list, and it must
stay the same at both sites"*. A predicate makes it **three** places that have to agree, and a
list living in two places is precisely the `BrowserPins` failure LIB-S3b existed to fix.

**(a) Add the predicate only.** Smallest change, fixes the bug, accepts a third copy.

**(b) Consolidate: one `clearSectionFlags()` and one `inVirtualSection()`, both next to the flag
declarations, with `refreshDir()` and `enterDriveList()` calling the setter.** Three lists become
one place. It touches two load-bearing functions, but only to replace a literal run of assignments
with a call that performs the identical assignments - and both are covered by the existing
section-entry/exit gate.

**I recommend (b)**, on the same reasoning that produced `BrowserPins.h` and
`panescroll::ensureVisible`, and because this bug is *itself* an instance of the list being
maintained by hand - `!in_drive_list_` is a fourth, partial copy that fell a section behind and then
five more. But it is a wider blast radius than the bug strictly needs, so it is Dos's call.

### 5.3 The search hook

Add `in_library_ &&` to the condition at :8980. One term.

### 5.4 Not proposed

No change to `DIR_POLL_INTERVAL`, no debouncer, no watch daemon, no change to what `refreshDir()`
does. Leaving a section already routes through `refreshDir()`
([:10731](src/UIManager.cpp#L10731)), which re-baselines `dir_mtime_` - so skipping the poll inside
a section cannot leave a stale baseline that fires on the way out. Verified, not assumed.

---

## 6. Tests

The poll lives in `run()`, which needs curses, so the predicate is what gets tested rather than the
loop. A new `browser_section_test` (device-free, both jobs) over a small struct mirroring the flags
is not worth it; instead:

1. **`inVirtualSection()` covers every flag** - a table-driven check that setting each of the seven
   flags alone returns true, and none returns false. This is the test that catches an eighth section
   being added and the predicate not updated, which is the whole risk of §5.2.
2. Under (b): `clearSectionFlags()` clears every flag the predicate reads, asserted by setting all
   seven, calling it, and requiring `!inVirtualSection()`. The two helpers then pin each other.
3. `library_level_test` already covers `libnav::reset`; no change.

Both require the helpers to be reachable from a test, which means they go somewhere a test can link
- a small header beside `BrowserPins.h`, since `UIManager.h` drags curses. **If Dos prefers them as
private members on `UIManager`, they are untestable and blocks 1-2 are dropped; say which.**

The hardware gate is the real proof either way.

---

## 6a. BUILT - option (b), helpers in a header

**Gates: Windows 50/50, Linux 51/51** (+`browser_sections_test`).

### The shape, and a divergence from what the ruling assumed

The ruling said the substitution is "two hand-written flag lists become one call to one setter".
That is what shipped - but the obvious way to write it was not available. Turning the ten flags into
a struct would have renamed **262 reference sites** across `UIManager`, measured, which is not a
narrow mechanical change and is not what "no logic change" meant.

So the members stay where they are, and the single enumeration is an **array of pointers to them**,
built in the constructor beside the declarations:

```cpp
section_flags_[0] = &in_drive_list_;   ... [9] = &in_podcastindex_search_;
```

`BrowserSections.h` owns the two operations over that array - `anySet` and `clearAll` - pure and
testable, taking `(bool* const*, size_t)` so a test can point them at its own bools. Zero renames,
one enumeration, helpers in a header. `UIManager::inVirtualSection()` and `clearSectionFlags()` are
three lines between them.

### The substitution proved mechanical, not merely equivalent

Required by the ruling, and done by script against `git HEAD` rather than by eye:

| check | result |
|---|---|
| both old sites touched the same set | **OK** |
| old `refreshDir` cleared set == `section_flags_` | **OK** |
| old `enterDriveList` touched set == `section_flags_` | **OK** |
| the one flag it set *true* was `in_drive_list_` | **OK** |
| **order preserved** (old `refreshDir` order == array order) | **OK** |
| no flag dropped / no flag added | **OK** |
| `libnav::reset` still called, now once inside the helper | **OK** |
| `refreshDir` has no residual flag assignment | **OK** |

And the diff itself is the plainest evidence: **19 hand-written flag assignments removed, 0 added.**
The single `in_drive_list_ = true` did not move far enough for git to call it changed.

One check in the script was wrong on the first run and said MISMATCH: it compared
`enterDriveList`'s *cleared* set (nine) against the array (ten), having forgotten that the tenth is
set *true* there. The script was fixed, not the code.

### Mutations - two, each verified to have landed

| mutation | models | result |
|---|---|---|
| **M1** - `anySet` looks at `flags[0]` only | **the shipped bug, generalised** - a guard that tests one flag of ten | **10 failures**: nine flags alone, plus the section-with-sub-mode case |
| **M2** - `clearAll` stops one short | the reset trap leaking a sub-mode - the `in_radio_search_` bug the old comment describes | **3 failures** |

M1 is the defect this slice fixes, expressed in the helper: `!in_drive_list_` *is* `anySet` over a
set of one. The first mutation attempt did not apply - the anchor was written with CRLF against a
file the Write tool had created with LF - and the assertion caught it at zero matches. **Fifth time
that step has paid.**

### What the test cannot prove, stated rather than implied

`browser_sections_test` proves the two operations cannot miss a member of the set they are given.
It **cannot** prove that `section_flags_` lists all ten members, because reaching that array means
linking `UIManager`, which drags curses. The structural guarantee is that the enumeration now exists
in **one** place instead of four, sitting beside the members it names - not that forgetting an
eleventh is impossible. `BrowserPins.h` has the same limit for the same reason, and it is recorded
at the array itself.

## 7. Gate - eyes-on, both platforms

**Linux, the reported case:** enter `[Library]` from `/mnt/hgfs` and wait five seconds - it stays.
Type into `|` search - it stays. Same for `[Radio]`, `[Podcasts]`, `[FAVs]`, `[Recent]`, `[Books]`.

**Windows, the case nobody has hit yet:** enter `[Library]`, then have something write a file into
the folder the browser was last showing (finish a podcast download into it, or copy a file in from
Explorer). Before this fix the section exits within two seconds; after it, it stays.

**Both:** leaving a section still relists the folder correctly and picks up any change that happened
while you were away - the poll being skipped inside a section must not cost the refresh on exit.
`[Drives]` still refreshes on F12. A directory changing while the folder browser IS showing it still
auto-refreshes, cursor and scroll preserved - that behaviour is the point of the poll and must
survive.

**Verification split as in LIB-S4 through LIB-S16.** Brace-balance and scoped-diff audit.
