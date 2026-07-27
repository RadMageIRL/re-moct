# LIB-S16 - view navigation and genre variant grouping

**Design note. Not greenlit. Untracked until it is.**

Branch `experimental/win-pdcurses`, tip `84c074a`. Every anchor read on the live tree today.
The last library slice.

---

## 0. Disconfirmation report

Three entries. One half of Part 1 is refuted, the other half is confirmed but **much narrower than
described**, and the probe turned up **a bug nobody reported**.

### 0.1 REFUTED - the pane already says which view is showing, at every level

The brief says *"No signal of which view you are in. Even once home is reachable, the pane does not
say where you currently are."*

The browser header has been fully level-aware since LIB-S10
([UIManager.cpp:2948-3000](src/UIManager.cpp#L2948)). All six levels are named:

| level | header today |
|---|---|
| Genres | `[Library] genres (Enter:artists  %:stats  [Back]/Left:leave)` |
| Stats | `[Library] most played <count> (Enter:play  a:add  %:next  [Back]/Left:back)` |
| Artists, no filter | `[Library] (Enter:albums  g:genres  %:stats  F12:rescan  [Back]/Left:leave)` |
| Artists, in a genre | `[Library] <genre> (Enter:albums  [Back]/Left:genres)` |
| Albums | `[Library] <artist> (Enter:tracks  a:add album  [Back]/Left:artists)` |
| Tracks | `[Library] <artist> - <album> (Enter:play  a:add  q:queue  [Back]/Left:albums)` |
| Results | `[Library] search "<query>" <count> (Enter:play  a:add  [Back]/Left:back)` |

It names the view, the active genre, the artist and album, the result and stat counts, **and where
Left goes from here** - the last of which is the thing LIB-S4 fixed in four sections at once. There
is no missing signal to add. Proposing new signalling UI here would be building a second answer to a
question already answered, which is what this campaign has spent fourteen slices not doing.

**One honest gap, and it is one word.** At Stats the header says `%:next`, which does not say that
the third press leaves the level. See §2.3.

### 0.2 CONFIRMED, but it is ONE KEY, not the set

*"No clean way back to the default artist view."* True, and the cause is specific. `[Library]` has
**three key-reached views** - Genres (`g`), Stats (`%`), Results (`|`). Two of them already unwind
correctly. **`g` is the only one that does not**, and the reason is that it is the only one that
does not use the idiom the other two share.

```cpp
// UIManager.cpp:8016 - the 'g' handler, in full
lib_nav_.level = libnav::Level::Genres;
showLibraryGenres();
```

It assigns the level directly. It never captures `return_to`. And `ascend()` at Genres returns
`LeaveSection` ([LibraryNav.h:231-232](include/LibraryNav.h#L231-L232)).

Measured, driving the real `libnav` device-free (probe §A, §B):

```
press g                 level=Genres   return_to=Artists
Left at Genres       -> LeaveSection
>> LEAVES THE SECTION - no route back to the unfiltered artist list

press % (1)             level=Stats    return_to=Artists
press % (2)             level=Stats    return_to=Artists
press % (3)             level=Artists  return_to=Artists
>> third press returned to Artists
```

So `%` is already a toggle and the header already advertises it. `|`/Results already unwinds to
`return_to`. **The set is not incoherent - one member of it was wired a different way.** That is a
smaller and more fixable finding than "the key set is wrong", and I am not going to inflate it: the
brief invited "the set feels wrong" as a legitimate conclusion, and having measured the set, it is
not the conclusion the tree supports.

**The header at Genres is not lying** - it says `[Back]/Left:leave`, and leaving is what it does.
The honesty is intact; the behaviour is the odd one out.

### 0.3 NEW - a reachable bug nobody reported: Left goes dead at Stats

`return_to` is **one slot** ([LibraryNav.h:121](include/LibraryNav.h#L121)) and both `beginSearch`
and `beginStats` write it with `s.level` whenever the level is not their own. Nothing stops a
**view** level being written into it. Probe §C, on today's code:

```
press %  -> Stats      level=Stats    return_to=Artists
press |  -> Results    level=Results  return_to=Stats     <-- a VIEW captured as the way back
Left at Results        level=Stats    return_to=Stats
Left at Stats          level=Stats    return_to=Stats
>> *** LEFT IS A NO-OP - the level did not change ***
```

Probe §D confirms it does not recover: Left is dead at Stats from then on, three presses running.
The user is not trapped in the section - `%` still cycles out and the section can be left another
way - but **`[Back]` and Left stop working**, which is precisely the pair LIB-S4 established must
mean the same thing everywhere.

This is the **single-slot-two-roles** shape the XF campaign paid for in C1, where
`next_track_info_` served a pending swap and a teardown at once. Same fix shape: make the slot
unable to hold the wrong kind of value.

**It is reachable today with two keypresses from a fresh section entry**, and it is not in the
brief. Reporting it rather than folding it in silently.

### 0.4 The genre numbers moved

The brief says 35 raw genre values. Measured on the live index today: **38 distinct raw tag values,
30 distinct values after `/` and `;` splitting** - and 30 is the number that matters, because the
split list is what `[Library]` genres draws.

Not a contradiction: LIB-S10 measured 35 on a **2,157**-record index, and the index is now
**2,775** records. The collection grew. Recorded so the next reader knows which number is which.

---

## 1. PART 2 FIRST - the genre key, because it is measured and settled

Taking Part 2 first because it is the smaller, fully-decided half.

### 1.1 The key

```cpp
// libidx::detail::genreKey - the GROUPING key for genre display.
// Fold ASCII case, drop ASCII punctuation and whitespace, keep every byte above
// 0x7F verbatim.
inline std::string genreKey(const std::string& g) {
    std::string o; o.reserve(g.size());
    for (char c : g) {
        const unsigned char u = (unsigned char)c;
        if (u > 0x7F)                       o.push_back(c);          // never touched
        else if (u >= 'A' && u <= 'Z')      o.push_back((char)(u - 'A' + 'a'));
        else if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) o.push_back(c);
        // everything else ASCII - space, hyphen, ampersand, comma, dot - dropped
    }
    return o;
}
```

**Non-ASCII bytes pass through untouched and are never case-folded.** That is the same rule
`foldPathKey` follows and for the same reason: a correct case fold above ASCII needs a Unicode table
this program does not have and should not grow, and a wrong one merges genres that are not the same.
Two genre values differing only in a non-ASCII character therefore stay two rows, which is the safe
direction to fail.

### 1.2 Measured over EVERY raw value - the check the brief demanded

Run over all 30 post-split values on the live index. **29 rows out, exactly ONE group merges:**

```
key 'postpunk'  -> 7 records, displayed as 'Post Punk'
      'Post Punk'   5
      'Post-Punk'   2
```

Exactly the predicted outcome, and nothing else moved. The full table, with the cases that matter:

| raw value | key | records | merges with |
|---|---|---|---|
| `Rock` | `rock` | 1014 | nothing |
| `Pop` | `pop` | 312 | nothing |
| **`Hip-Hop`** | **`hiphop`** | **49** | **nothing - untouched, because nothing is split** |
| **`Folk, World, & Country`** | **`folkworldcountry`** | **41** | **nothing - does NOT merge with `Folk` (3)** |
| `R&B` | `rb` | 38 | nothing |
| `Pop-Rock` | `poprock` | 12 | nothing - not `Pop`, not `Rock` |
| `Indie Pop` | `indiepop` | 12 | nothing - not `Indie` (54), not `Pop` |
| `Stage & Screen` | `stagescreen` | 12 | nothing |
| `Post Punk` / `Post-Punk` | `postpunk` | 5 + 2 = **7** | **each other** |

The two live risks were `Hip-Hop` (which hyphen-splitting breaks, and which is why LIB-S10 stopped)
and `Folk, World, & Country` against `Folk` (which a rule that dropped commas *and split* would
merge). **Neither moves**, because this operation only ever joins values whose alphanumerics are
identical in order - and `folkworldcountry` is not `folk`.

### 1.3 The consequence the brief did not name - the FILTER must use the key too

`libidx::genres()` is not the only reader. Descending into a genre sets `lib_nav_.genre` to the row
identity, and the levels beneath filter through
[`trackHasGenre`](include/LibraryIndex.h#L439), which compares with `icmp` - an exact
case-insensitive match. It has two callers: `artists(idx, genre)`
([:890](include/LibraryIndex.h#L890)) and the genre-filtered album query
([:928](include/LibraryIndex.h#L928)).

**Grouping the list without changing the filter ships a row that lies:** the genre list would show
one `Post Punk` row counting 7 records, and opening it would list the 5. So `trackHasGenre` compares
on `genreKey` as well. One key, three readers, no second rule - the `foldPathKey` shape exactly.

This stays inside the constraint: `genres()` and `trackHasGenre` are **queries**. No index format
change, no stored normalised genre, no tag write. The tag on disk and the bytes in `library.idx` are
untouched, and a rescan produces the identical file.

### 1.4 Which label wins, and the tie rule

**Most records wins** - `Post Punk` (5) over `Post-Punk` (2). On a **tie**, the variant that sorts
first under `detail::icmp` wins. A tie needs a deterministic answer rather than "whichever the hash
map yielded first", or the genre list reorders between runs on the same collection; `icmp` is the
comparison `sortUniqueCI` already uses, so it is not a new rule either.

### 1.5 Composition with `/` and `;` splitting - unchanged, and it composes cleanly

Splitting runs **first**, keying second: `Rock / Folk, World, & Country` splits to `Rock` and
`Folk, World, & Country`, and each is keyed independently. The brief's ruling stands untouched -
commas and ampersands are not separators, hyphens are not separators, and this slice does not make
them any.

---

## 2. PART 1 - the fix, which is to make `g` obey the rule the other two already follow

### 2.1 One predicate, and the trap closes by construction

```cpp
// The three levels reached by a KEY rather than by descending. They unwind to
// return_to; the hierarchy levels unwind to their parent.
inline bool isViewLevel(Level l) {
    return l == Level::Genres || l == Level::Stats || l == Level::Results;
}
```

**`return_to` may only ever hold a level that is not a view.** Every capture becomes:

```cpp
if (!isViewLevel(s.level)) s.return_to = s.level;
```

That one guard fixes §0.3 by making the bad state unrepresentable rather than by handling it - the
XF C3 lesson. `%` from Stats into Results can no longer overwrite the way home, because Stats is
never written into the slot in the first place. Left keeps working at every level.

### 2.2 `g` becomes a toggle, exactly like `%`

New `libnav::toggleGenres(State&)`, a sibling of `cycleStats` and written to look like one:

```cpp
inline Action toggleGenres(State& s) {
    if (s.level != Level::Genres) {
        if (!isViewLevel(s.level)) s.return_to = s.level;
        s.level = Level::Genres;
        return Action::Repopulate;
    }
    s.level = s.return_to;          // second press leaves, like %'s third
    return Action::Repopulate;
}
```

And `ascend()` at Genres returns to `return_to` rather than `LeaveSection`, so `[Back]`, Left and a
second `g` all agree about where "back" is - which is the whole of LIB-S4's rule applied to the one
level that was missing it.

**Leaving the section from Genres now takes two presses** (Genres to Artists, Artists out) where it
took one. That is the cost, it is stated rather than hidden, and it buys the retrace being uniform:
every Left undoes exactly one thing.

The existing genre *hierarchy* path is untouched: Enter on a genre still descends to filtered
Artists, and Left from filtered Artists still returns to Genres and drops the filter
([LibraryNav.h:218-229](include/LibraryNav.h#L218)). That branch is explicit and does not go through
`return_to`.

### 2.3 Header - three small truths, no new surface

Given §0.1, the header needs correcting rather than extending:

| level | now | proposed |
|---|---|---|
| Genres | `... %:stats  [Back]/Left:leave` | `... g:back  %:stats  [Back]/Left:back` |
| Stats | `... %:next  [Back]/Left:back` | `... %:next/out  [Back]/Left:back` |

`leave` becomes `back` because that is what it will do. `%:next/out` closes the one honest gap in
§0.1 at a cost of four columns. Nothing else changes, and no name budget shrinks.

### 2.4 No new binding - so nothing needs a sweep, but here is the sweep

`g` and `%` are reused; nothing new is bound. The sweep was re-run anyway against the live tree,
because the free list has been handed over wrong twice.

**Method: every `case '<c>':` label AND every `ch == '<c>'` comparison** - the second is how `?`
escaped two surveys. Result:

**Free printable ASCII: `"` `#` `$` `&` `'` `(` `)` `:` `^` and the digits `6` `7` `8` `9`.**

`?` is **TAKEN**, via a pre-switch `if` at [UIManager.cpp:6757](src/UIManager.cpp#L6757) - the Help
pane, confirmed again. `g`/`G` at :8004, `%` at :8090, `|` at :8033, `@` at :8059.

### 2.5 `?` help pane

**No change proposed.** The two keys it would document are both stated in the header of the level
they work on, which is where someone actually looks. Adding a second place that has to agree is the
`BrowserPins` failure in miniature - LIB-S3b existed because a list of section names lived in two
places. Raising rather than deciding: **if Dos wants the help pane to carry the library keys, say
so** and it is a few lines.

---

## 3. Not touched

Audio thread, `ar_crc`, CD path, rip path, `ConvertJob`, scrobblers, `Config`, `Version.h`, the
index FORMAT, `LibraryScanner`, `PlaylistManager`, every other section. `Ctrl+T` stays
Classic/Awesome only. No new `in_*` section flag - the new state is one predicate over the existing
`Level` enum, per the constraint.

LIB-S9's draw-time scroll invariant covers the regrouped genre list for free; no per-handler nudge is
added. Identities stay strings - a genre row's identity remains the display label, and the key is
computed at compare time, never stored.

**Files expected to change:** `include/LibraryNav.h`, `include/LibraryIndex.h`, `src/UIManager.cpp`,
`tests/library_level_test.cpp`, `tests/library_index_test.cpp`, `CHANGELOG.md`, and this document.
Anything beyond that is a divergence and gets reported.

---

## 4. Tests

**`library_level_test`** (existing, device-free, both jobs) gains:

1. `g` from Artists then `g` again returns to Artists. From Albums, returns to Albums.
2. `ascend` at Genres returns `Repopulate` to `return_to`, never `LeaveSection`.
3. **The §0.3 regression, written as the probe found it:** `%` then `|` then Left then Left leaves
   Left still working, and the level actually changes on every press. Asserted as *level changed*,
   not as a specific destination, because a no-op is the failure.
4. `return_to` never holds a view level, after every combination of the three view keys.
5. The genre hierarchy path is unchanged: Genres, Enter, Left returns to Genres with the filter
   dropped.

**`library_index_test`** (existing) gains:

6. `genreKey` on the live values: `Post Punk` and `Post-Punk` key equal; **`Hip-Hop` keys to
   `hiphop` and equals nothing else**; `Folk, World, & Country` does not equal `Folk`; `Pop-Rock`
   equals neither `Pop` nor `Rock`.
7. `genres()` over a fixture holding both Post Punk variants yields **one row**, labelled by the
   more common one, and the tie case is deterministic.
8. **`trackHasGenre` matches across variants** - a `Post-Punk` track is found when filtering on
   `Post Punk`. The §1.3 lie, pinned.
9. Non-ASCII: a genre with a high byte groups with itself, is not case-folded, and does not merge
   with a different one. Byte-exact round trip.
10. Splitting still composes: `Rock / Folk, World, & Country` yields two genres, neither merged.

**Mutation-tested, and the mutation verified to have landed**, per LIB-S15: drop the `isViewLevel`
guard and confirm block 3 fails; make `genreKey` strip hyphens *and split* and confirm the `Hip-Hop`
block fails. Mutating toward the **forbidden** change, not only the old one - the S15 lesson.

---

## 5. Gate - eyes-on, both platforms

**Part 1.** From artists: `g`, then `%`, then `|`, and back to artists without guessing. `g` twice
returns where you started. `[Back]` and Left do the same thing at every level and **never do
nothing**. Specifically: `%` then `|` then Left then Left - Left must keep working (the §0.3 bug).
The header names the view at every level and says where Left goes. Cursor stays visible per LIB-S9.

**Part 2.** `Post Punk` and `Post-Punk` are **one row of 7**, and opening it lists tracks from
both - not 5. **`Hip-Hop` is one row of 49.** `Folk, World, & Country` survives whole and is
separate from `Folk`. `Pop-Rock` is separate from `Pop` and `Rock`. A planted non-ASCII genre value
groups and displays correctly on both platforms.

Everything LIB-S3 through LIB-S15 still works; every other section enters, draws, exits, plays.

**Verification split as in LIB-S4 through LIB-S15.** Brace-balance and scoped-diff audit.

---

## 5a. BUILT - measured results

**Gates: Windows 49/49, Linux 50/50.** No new test target - the two existing library tests grew.

### The shipped code over Dos's real index

Not the fixtures - `genres()` and `trackHasGenre()` as shipped, over the live 2,775-record
`library.idx`:

**30 raw post-split values in, 29 rows out. One merge.**

| row | records reached THROUGH THE FILTER | verdict |
|---|---|---|
| `Post Punk` | **7** | the merge, and the row opens onto all 7 - §1.3 holds |
| `Hip-Hop` | **49** | untouched |
| `Folk, World, & Country` | **41** | whole |
| `Folk` | **3** | still separate |

`Post-Punk` no longer appears as a row of its own. Every other row is unchanged.

### Three mutations, each verified to have landed first

| mutation | models | result |
|---|---|---|
| **M1** - drop the `isViewLevel` guard from `captureReturn` | the dead-Left bug | **12+ failures**, including all six orderings of `g`/`%`/`\|` and the exact `%`-then-`\|` sequence |
| **M2** - `splitGenres` splits on `-` as well | **the LIB-S10 rejected fix** | **11 failures** - `Hip-Hop` becomes `Hip` + `Hop`, `Pop-Rock` splits, and the pre-existing S10 blocks fire too |
| **M3** - `trackHasGenre` back to `icmp` | **the row that lies** | **3 failures** - the merged row lists one artist where it should list two |

M2 is the forbidden change, not the old one - the S15 lesson applied. M3 exists because M1 and M2
between them would not have caught a grouped list with an ungrouped filter.

### Two pre-existing test assertions were changed, deliberately

`library_level_test` asserted `ascend from Genres leaves the section`. That **was** correct and is
what made `g` one-way. The old line is kept in a comment beside the new one, so the change reads as
a decision rather than as drift.

### The help pane needed no structural change

The fence said to stop and raise it if the library content did not fit. It fits: the pane already
scrolls (`help_scroll_`, `j`/`k`, PgUp/PgDn, Home/End, clamped at draw time), and it is now 89 rows
across 7 sections. Ten rows were added and no mechanism changed.

## 5b. The `?` audit - what was found

Asked for explicitly. **Nothing in `?` names the wrong key** - no repeat of the `[FAVs]`
`f`-versus-`*` class of error. `*` correctly says star/favourite; `e` correctly says the EQ. What was
there was **stale by omission**, in four places:

| entry | was | why it was wrong |
|---|---|---|
| `g` | "Goto directory (Tab = complete)" | since LIB-S10, `g` inside `[Library]` is genres |
| `F12` | "Refresh the [Drives] list" | since LIB-S6 it also rescans the library |
| `i` | "Track info popup" | since LIB-S10 it follows the browser cursor, either pane |
| `a` | "Add selection to playlist (recursive)" | LIB-AA made it whole-album on a library album row |

**And the tag editor was undiscoverable.** LIB-S14 shipped tag editing on browser rows, and `?` never
mentioned it. **The precise route is `i` then `e`**, not a bare `e` - a bare `e` is the EQ pane. The
brief's list said "`e` tag editing now works on browser rows", which would have put the wrong key in
the help pane had it gone in unread. Verified at
[UIManager.cpp:7125](src/UIManager.cpp#L7125) - the editor lives inside
`right_pane_ == RightPane::TrackInfo`.

Missing outright and now added: the section's existence, the config toggle and root, `@`, `|`, `%`,
the library meaning of `F12`, and `Esc` to cancel a scan.

## 6. One thing raised rather than decided

**Should `g` still be reachable from Albums and Tracks?** It is today, and under this design
pressing `g` at Tracks and `g` again returns to Tracks. That is coherent. But `g` at depth also
leaves `artist` and `album` set while the genre list is showing - harmless, since nothing reads them
at that level and the toggle restores them - and it is the one place where the model holds state
that is not visible. It works and it is tested; naming it because it is the kind of thing that gets
rediscovered as a defect later.

No other item is deferred. This is the last library slice.
