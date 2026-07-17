# Session handoff - 2026-07-17 ~17:05 - abi-cluster slice B through OS media control

Everything on `experimental/win-pdcurses`. Where the branch stands at handoff:
tip `df683ce` (a CI dep fix on top of the osmedia feature `493ce29`). CI for
`df683ce` was watched at handoff - confirm green first thing (the fix adds
libsystemd-dev; expect green, locals were 28/28 Win + 29/29 Linux).

## Shipped today (chronological; each commit message is its own record)

| commit | slice | headline |
|---|---|---|
| `5dcfcdc` | abi-cluster slice B | copy/remux: FrameSync + ADTS->M4A mux + tee + recorder copy mode (CI green) |
| `84cc704` | rec-art-pending-race-fix | recorded cuts keep cover art through the split-trim hold (ring, not a slot) |
| `df6c033` | help-pane-scroll | Home/End/PgUp/PgDn in the help pane (mirror playlist nav semantics) |
| `493ce29` | osmedia-seam | OS media control: SMTC + MPRIS, bidirectional, seek coalescer, cover art |
| `df683ce` | ci fix | libsystemd-dev for the MPRIS build + gitignore local tools/ |

Plus a probe-only flight (no commit): `osmedia-phase0-probe` proved SMTC-under-GCC
and MPRIS-via-sd-bus reachable. Its harnesses live in `tools/osmedia/` - committed
to the repo, available for community use (see below).

## Where things stand

- **Backlog is EMPTY.** abi-cluster slice B was the last planned item; osmedia was
  a new feature arc (probe -> seam -> art floor -> album/radio art). v1.3.0 is
  still UNRELEASED - the CHANGELOG has grown a lot (decode formats, rip overhaul,
  stream recording + copy mode, batch RG, OS media control); release prep is the
  plausible next flight, Dos's call.
- **osmedia is fully in and Dos-hardware-gated**: media keys + OSD card + scrubber
  on Windows (SMTC), playerctl + widgets on Linux (MPRIS); cover art shows a local
  file's embedded picture or a radio track's resolved cover (station art or the
  iTunes/Deezer song lookup), RE-MOCT logo as the floor. Config toggle
  os_media_control (default on).
- **Uncommitted at handoff (ride a future docs commit):** docs/lessons.md (today's
  osmedia learnings appended), this handoff, docs/osmedia-seam-plan.md IS committed
  (in 493ce29). No code uncommitted.

## Tools (in the repo: `tools/osmedia/` - community-available)

`tools/osmedia/`: the Phase-0 feasibility harnesses, reusable when the
toolchain/deps/platform shift. Committed to the repo so anyone can use them.
- `smtc_probe.cpp` + `build-smtc.sh` - SMTC via the raw WinRT ABI under UCRT64
  GCC. Builds a real HWND, drives GetForWindow/DisplayUpdater/ButtonPressed.
- `mpris_probe.c` + `drive-mpris.sh` - MPRIS via sd-bus, driven by playerctl
  under dbus-run-session (the WSL2 box has no login session bus).
- `FINDINGS.md` - the full Phase-0 reachability report (link sets, ABI surface,
  dispatch model). Read this before touching the osmedia platform impls again.

## Findings that must not be re-learned (full text in lessons.md, osmedia section)

1. **WinRT from UCRT64 GCC works via the RAW ABI** (no cppwinrt/WRL): __uuidof
   resolves ABI:: interfaces; typed event delegate Invoke takes the ARGS
   INTERFACE not the runtimeclass; ref must be plain LONG; timeline +
   position-change on ...Controls2; IAsyncInfo/AsyncStatus are GLOBAL scope;
   SMTC thumbnail-from-bytes needs an in-memory stream (file:// is not a valid
   CreateFromUri scheme); link -lruntimeobject, plain main() not -municode;
   PDC_hWnd only valid after initscr() + only in REMOCT_PDCURSES.
2. **MPRIS/sd-bus needs no dedicated thread** with a getch()-timeout loop:
   per-tick sd_bus_process + poll(0); one thread owns the bus. -lsystemd.
   REQUIRES libsystemd-dev - add to CI (the df683ce lesson: a dep installed
   locally during a probe hid the CI gap).
3. **Bidirectional seam marshal**: OS callbacks only enqueue; the UI loop drains
   and routes to the EXISTING homes (AudioManager + manualNext/manualPrevious
   extracted from the case 'n'/'p' bodies). Transport dropped during an active
   CD rip only.
4. **Absolute seek does not fold to a delta at receive time** (a drag stream
   accumulates against a stale position) - a last-write-wins absolute lane
   resolved at flush against the live position.
5. **One radio-art home already existed** (radioArtKick -> radio_bytes_, shared
   by pane/recorder/Discord) - reuse it, drive it for the new consumer; late
   art re-publishes via an art-size term in the change trigger.
6. **Edit-placement trap**: an anchored Edit matched a non-unique pattern and
   put the media init in showTrackToast instead of the ctor -> startup segfault.
   Include a unique function-scoped landmark in the anchor; re-read after a
   structural insert; file-based trace milestones beat stderr for a wingui crash.

## Standing verification (unchanged)

ctest both toolchains (Win UCRT64 + WSL2 Debian ~/rb); fresh-link verify the
test binary before trusting a pass (false green has bitten twice); wingui exe
must be run (timeout 124 = alive, 139 = segfault) after any ctor/loop change;
Dos hardware-gates eyes-on + listening + media-key/OSD/playerctl.

## Next flight

v1.3.0 release prep is the plausible next item (rich CHANGELOG, still
UNRELEASED). Or Dos briefs something new. Later-items ledger carried forward:
album gain for batch-r128, GainScan recursion, np_pub_q_ LP64 look, .wvc hybrid,
local-file art async extraction (currently synchronous on track change - fine,
but a huge embedded image could hitch a switch), CD cover on the OS card.
