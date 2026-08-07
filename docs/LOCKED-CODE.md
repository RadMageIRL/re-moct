# LOCKED-CODE

**Locked means the conversation does not happen.** If a design note proposes touching any of this, the answer is no — not "let's gate it carefully," not "let's scope a probe first." A plausible argument with a test attached is still no. The discussion ends at the proposal.

The rule forbids the discussion, not just the change. A well-argued note is exactly the failure mode this list exists to stop.

## The declaration — required, on the first line

**Every proposal that touches the CD path opens with this line, verbatim, before anything else:**

> This proposal touches / does not touch AR_PREGAP, ar_crc.*, or the read addressing.

**If the answer is "touches": stop there and say so. Do not propose it.** No brief, no probe attached, no scoped exception, no "touches it only to rename." The declaration is not a disclaimer you write and then continue past — a "touches" answer ends the proposal at that line.

The declaration comes first because by the time a proposal is written it is already persuasive. Stating the answer before the argument means the argument never gets made.

Set by Dos, 2026-07-27, after the 1.5.0 ceremony.

## The list

| Locked | Why |
|---|---|
| `ar_crc.*` | AccurateRip CRC path. No edits, no refactors, no "equivalent" rewrites. |
| `AR_PREGAP = 150` | Disc-absolute AccurateRip disc-ID origin. Never T1-relative. HydrogenAudio 97603. |
| CD read addressing rule | `LBA = frame − 150`. `start_frame` is never a read address; `lba()` is. See CD-ADDRESSING-LOCKED.md. |
| Disc-ID math | `computeCDDB`, `fetchARData`, `tocOffsets`, CTDB — all keep `start_frame`. |
| Audio thread | No blocking, no state mutation from outside the defined paths. |
| `CursesSeam.h` | Not renamed to `Curses.h`. Windows case-insensitivity lets `<curses.h>` shadow it. |
| `libfdk-aac-2.dll` dynamic | Exe and `remoct_stream` both use it. Two static copies = dual-state aliasing. |
| `abi_version = 1` | Compat is struct-append + `min(struct_size)` + per-fn null check. |
| `Ctrl+T` | Classic/Awesome toggle only. Never overloaded. |

## If you think something here is wrong

Say so to Dos, in one line, and stop. Do not write the brief. Do not attach the probe. Do not pre-build the case.
