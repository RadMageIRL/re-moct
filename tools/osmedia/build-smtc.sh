#!/usr/bin/env bash
# osmedia-phase0-probe / Probe 1 build (retained artifact). Run under MSYS2
# UCRT64: PATH must lead with /c/msys64/ucrt64/bin. Standalone - NOT part of
# the production build system.
set -e
export PATH="/c/msys64/ucrt64/bin:$PATH"
cd "$(dirname "$0")"
g++ -std=c++17 -o smtc_probe.exe smtc_probe.cpp \
    -lruntimeobject -lole32 -luser32 -lgdi32
echo "built: smtc_probe.exe"
