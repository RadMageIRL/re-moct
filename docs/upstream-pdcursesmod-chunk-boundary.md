# Upstream report: PDCursesMod — fullwidth glyph split across a MAX_PACKET_LEN chunk boundary aborts

Prepared for <https://github.com/Bill-Gray/PDCursesMod>. Not yet filed — awaiting Dos's go, since
it posts publicly under his GitHub identity.

---

## Title

`PDC_transform_line_sliced`: leading-dummy repair runs once instead of per chunk, aborting when a fullwidth glyph straddles a `MAX_PACKET_LEN` boundary

## Body

**Build:** wingui (GDI) port, `-DPDC_WIDE -DPDC_FORCE_UTF8`, 64-bit `chtype` (so
`USING_COMBINING_CHARACTER_SCHEME` is on). MSYS2 UCRT64, GCC 15.2, Windows 11. Asserts enabled
(no `-DNDEBUG`). Pinned at `d9a59832658294c797447747f1664832fc21136e`.

**Symptom:** hard abort (`0xc0000409`, `ucrtbase!abort`) while resizing a window wider than 89
columns with double-width (CJK) glyphs on screen.

```
_assert(message="i > 1 || ch != MAX_UNICODE",
        file="pdcurses/refresh.c", line=206)
PDC_transform_line_sliced ()
doupdate ()
...
```

### Cause

`PDC_transform_line_sliced` repairs a span that begins on `DUMMY_CHAR_NEXT_TO_FULLWIDTH` — the
placeholder in a fullwidth glyph's second cell — by backing up one cell onto the glyph's first
half:

```c
#ifdef PDC_WIDE
    if( x && (*srcp & A_CHARTEXT) == DUMMY_CHAR_NEXT_TO_FULLWIDTH)
    {                   /* starting on a dummy next to a fullwidth */
        x--;
        srcp--;
        len++;
    }
#endif
    while( len)
    {
        ...
        x += i;
        len -= i;
        srcp += i;
    }
```

**The repair sits above the loop, so it runs once — for the first chunk only.** The loop then cuts
the span into `MAX_PACKET_LEN - 1` = 89 cell chunks, and each `srcp += i` can land the next chunk
on a dummy whose fullwidth first half is in the chunk just drawn. The packet loop then computes
`i == 1` with `ch == MAX_UNICODE`, and the assert fires:

```c
while( i < MAX_PACKET_LEN - 1
             && (ch = (srcp[i - 1] & A_CHARTEXT)) < MAX_UNICODE
             && i < len)
   i++;
if( i == 1 && ch == MAX_UNICODE)
    fprintf( stderr, "line %d, x=%d, len=%d\n", lineno, x, len);
assert( i > 1 || ch != MAX_UNICODE);
```

The `fprintf` immediately above the assert suggests the case is already known to be reachable.

Note the failing `x` is **non-zero** — 89, 178, … — so the existing repair would have handled it
correctly had it been in scope. Nothing is wrong with the repair itself; it is only in the wrong
place.

### Second face, same boundary: the trailing-dummy trim

Fixing the above left a second abort at the same boundary — this time
`assert( (srcp[len - 1] & A_CHARTEXT) != MAX_UNICODE)` in `wingui/pdcdisp.c`, i.e. a chunk *ending*
on a dummy rather than starting on one. Same cause:

```c
PDC_transform_line( lineno, x, i - ((ch == MAX_UNICODE) ? 1 : 0), srcp);
```

The trim reads `ch`, but `ch` is **stale** whenever the inner loop exits on its *first* condition
(`i` reaching `MAX_PACKET_LEN - 1`). That exit never evaluates `ch` for the current `i`, so `ch`
still holds `srcp[i - 2]` and **`srcp[i - 1]` is never examined at all**. With a fullwidth glyph
straddling the boundary, the packet is handed to `PDC_transform_line` ending on the dummy and its
own assert aborts.

The fix is to test the packet's actual last cell. It is identical to the current behaviour
everywhere else: when the loop exits *because* `srcp[i - 1]` is a dummy, `ch` is that same cell and
both forms trim to `i - 1`.

### Cursor split

`PDC_transform_line_sliced` also splits a span around the cursor and recurses. That needs no
separate handling — the recursion re-enters the same loop, so both repairs above cover it.

### Reachability

Needs a line wider than 89 cells **and** a fullwidth glyph straddling a chunk boundary. Our
application only started hitting it after it stopped substituting `?` for CJK: before that
`PDC_wcwidth` never returned 2, no dummy cell ever existed, and the case was unreachable by
construction. Several hundred automated ASCII-only resizes never reproduced it; a single
border-drag with Japanese text on screen does so within seconds.

## Suggested patch

Move the repair inside the loop, and add the `x == 0` arm the original lacked:

```diff
-#ifdef PDC_WIDE
-    if( x && (*srcp & A_CHARTEXT) == DUMMY_CHAR_NEXT_TO_FULLWIDTH)
-    {                   /* starting on a dummy next to a fullwidth */
-        x--;
-        srcp--;
-        len++;
-    }
-#endif
     while( len)
     {
 #ifdef PDC_WIDE
         int i = 1;
         chtype ch;
 
+        if( (*srcp & A_CHARTEXT) == DUMMY_CHAR_NEXT_TO_FULLWIDTH)
+        {
+            if( x)          /* back up onto the glyph's first half */
+            {
+                x--;
+                srcp--;
+                len++;
+            }
+            else            /* at column 0 the first half is off-line to the
+                               left:  nothing to back onto,  nothing to draw. */
+            {
+                x++;
+                srcp++;
+                if( !--len)
+                    break;
+            }
+        }
+
         while( i < MAX_PACKET_LEN - 1
                      && (ch = (srcp[i - 1] & A_CHARTEXT)) < MAX_UNICODE
                      && i < len)
```

The `x == 0` arm covers a span that begins on a dummy at column 0, where the glyph's first half is
off-line to the left: there is nothing to back onto and nothing to draw, so the cell is skipped.
`len` is asserted `> 0` on entry; if the decrement empties the span the loop simply ends.

And for the trailing case, test the packet's real last cell rather than the stale `ch`:

```diff
-        PDC_transform_line( lineno, x,
-                          i - ((ch == MAX_UNICODE) ? 1 : 0), srcp);
+        draw = i;
+        if( (srcp[i - 1] & A_CHARTEXT) == MAX_UNICODE)
+            draw = i - 1;       /* never END a packet on a dummy */
+        if( draw)
+            PDC_transform_line( lineno, x, draw, srcp);
```

(`draw` declared alongside `i`.) `x`, `len` and `srcp` still advance by `i`, so the dummy is
consumed exactly as before. The `if( draw)` guard covers `draw == 0`, which the first hunk makes
unreachable but which would otherwise trip `assert( len)` inside `PDC_transform_line`.

Verified against the reproducer — a border-drag with CJK on screen on a window wider than 89
columns, previously a reliable abort within seconds.
