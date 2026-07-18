# aac-m4a-phase0-probe

Feasibility probe for AAC/M4A ripping. **Retained, reusable, OUT of the production
build.** Same discipline as `tools/osmedia/`: prove the unknowns standalone before
committing an encoder slice. Full findings and the Phase 1 recommendation live in
`docs/aac-m4a-phase0-report.md`.

## What it proves
1. The FDK-AAC **encoder** (`aacenc_lib.h`, `aacEncOpen`/`aacEncEncode`/`aacEncInfo`)
   is present and links against the already-shipped `libfdk-aac` on both toolchains,
   AAC-LC, with VBR (`AACENC_BITRATEMODE` 1-5) and CBR (`=0` + `AACENC_BITRATE`) both
   applying. No new lib, no new DLL.
2. A hand-rolled MP4 writer (`Mp4Mux.h`, no new dependency) assembles FDK's raw
   access units into a `.m4a` that RE-MOCT's **own** `src/AacDecoder.cpp` opens,
   reports the right length for, and **seeks correctly** in; then TagLib writes
   tags + a `covr` atom that read back, without breaking the sample table.

Verification drives the real production decode path (`AacDecoder` registered as a
miniaudio custom backend via `ma_decoder`) and is cross-checked with FFmpeg.

## Files
- `Mp4Mux.h` — the reusable minimal MP4 box writer (the Phase 1 artifact).
- `aac_m4a_probe.cpp` — encode -> mux -> verify (decode/length/seek) -> tag/art driver.
- `ma_impl.cpp` — the single miniaudio implementation TU (mirrors `AudioManager.cpp:1`).
- `build.sh` — builds on either toolchain.

## Run
```
# Windows / UCRT64
PATH="/c/msys64/ucrt64/bin:$PATH" ./build.sh && ./aac_m4a_probe.exe
# Linux / Debian (WSL2 or native)
./build.sh && ./aac_m4a_probe
```
Expect `24 passed, 0 failed`. It writes `out_vbr4.m4a` and `out_cbr128.m4a` for an
external-player eyes-on. Binaries and `out_*.m4a` are regenerable and not retained.
