# DESIGN NOTE - Library slice 12: remove the redundant per-handler scroll math

**Scope ID:** LIB-S12. **Status:** proposed, awaiting sign-off. Nothing implemented.
**Probed against tip `f127f78`** on 2026-07-26 - a fresh read, not the LIB-S9 enumeration.

Short note, because the slice is small. **One finding, and it is a scope question.**

---

## 1. The four sites are exactly where LIB-S9 said, unchanged by LIB-S11

| site | line |
|---|---|
| `drawDirBrowser` - **the invariant, STAYS** | `src/UIManager.cpp:2906` |
| `jumpToBrowserIndex` | `:3787` |
| `[Drives]` F12 refresh | `:7422` |
| `navigatePage` | `:8934` |
| `navigateHomeEnd` | `:8951` |

The anchors held. LIB-S11 moved line numbers but touched none of these.

## 2. They are safely redundant - checked, not assumed

The removal is only safe if nothing reads `dir_scroll_` between a handler running and the next draw.
Every read in the file:

- `:3012` - the draw loop, `int idx = dir_scroll_ + i`, which runs **after** the invariant at `:2906`
- `:3807` - inside `ensureDirCursorVisible` itself
- `:1727`/`:1731` - the dir-poll refresh, which saves and restores its own value and is not one of
  these sites
- `:8908`/`:8917` - see §3

**Nothing else reads it.** So a handler that moves the cursor and returns is always followed by the
invariant before any consumer sees the scroll offset. All four are redundant, and none has a reason
to stay.

## 3. THE FINDING: there are SIX, not four

`navigateDown` (`:8903`) and `navigateUp` (`:8915`) each carry their own **inline** scroll nudge:

```cpp
void UIManager::navigateDown() {
    if (focus_ == Pane::DirBrowser) {
        int v = paneVisibleRows(win_dir_);
        if (dir_cursor_+1 < (int)dir_entries_.size()) {
            ++dir_cursor_;
            if (dir_cursor_ >= dir_scroll_+v) ++dir_scroll_;    // <-- this
        }
    } else {
        if (pl_cursor_+1 < (int)playlist_.size()) ++pl_cursor_;   // scroll follows via the invariant
    }
}
```

**The playlist branch three lines below it is the shape this slice is arguing for**, and says so in
its own comment. The browser branch is the hand-written version of what the invariant now does:
`++dir_scroll_` when the cursor passes the bottom, `dir_scroll_ = dir_cursor_` when it passes the
top - minimal scroll by one, in both directions, which is exactly `panescroll::ensureVisible`.

**Why LIB-S9 did not list them for removal:** its §2 table classified them as row 17,
"inline nudge, correct" - which was true *at the time*, because before the invariant they were the
mechanism rather than a duplicate of it. Post-invariant they are as redundant as the other four.

**They are also the ones that matter most.** This slice's stated rationale is that leaving the
pattern in place invites the next person to add a fifth. `j`/`k` are the simplest and most-read
handlers in the file, and they sit directly beside a playlist branch demonstrating the correct
shape - so they are the likeliest thing to be copied, and the likeliest to teach the wrong lesson.

**Recommendation: remove all six.** But it is two sites beyond what the brief authorised, and the
brief says nothing else rides along, so **it is Dos's call and I am not assuming it.**

**If ruled out:** the four go, `navigateDown`/`navigateUp` keep their inline math, and the header
comment says so explicitly rather than leaving the inconsistency unexplained. The slice works either
way.

## 4. Comments

`UIManager.h:243-247` currently says the browser's calls "are kept but redundant - the helper is
idempotent. Do not add new ones." Once they are gone that is stale in its central claim, so it
becomes a plain statement that both panes reconcile at draw time and that per-handler scroll math is
the defect LIB-S9 removed. `:319-325` (the two wrappers) already describes the shared invariant
correctly and needs nothing.

One consequential detail: removing the nudge from `navigateDown` leaves `int v = paneVisibleRows(...)`
unused, so that line goes too. That is the only line removed which is not itself scroll math.

## 5. Tests and gate

**No test changes.** `pane_scroll_test` covers the helper and is untouched; nothing asserts the
per-handler calls, which is itself the point - they were never the contract.

**Gate as the brief states it.** The specific risk being checked is that the invariant really does
cover every path S9 claimed, so the interesting cases are the ones where a handler used to do the
work: `j`/`k` at the bottom and top edges of a long listing (if §3 is approved), PgUp/PgDn, Home/End,
`\` jump, and `[Drives]` F12.

**If any of them changes rendered behaviour, that is a finding about LIB-S9's invariant, not a reason
to restore the call** - stated in the brief and repeated here because it is the whole point of the
slice.

## 6. Files

`src/UIManager.cpp` (four or six deletions plus one now-unused local), `include/UIManager.h` (one
comment), `docs/library-slice12-plan.md`. **No CHANGELOG entry** - there is no user-visible change,
and claiming one would be inventing a behaviour difference this slice must not have.

**Not touched:** `PaneScroll.h`, the playlist pane and its call site, and everything else.
