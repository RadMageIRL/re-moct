#!/usr/bin/env bash
# build.sh — build the aac-m4a phase-0 probe on either toolchain.
# Windows/UCRT64:  PATH="/c/msys64/ucrt64/bin:$PATH" ./build.sh
# Linux/Debian:    ./build.sh   (run from WSL2 or native)
#
# Retained probe, OUT of the production build (aac-m4a-phase0-probe). Rebuild it
# against the current tree before trusting a result — it links src/AacDecoder.cpp
# so it verifies through the real production decode path.
set -e
cd "$(dirname "$0")"

COMMON="-std=c++20 -O2 -I. -I../../include -I../../lib"

if [[ "$OS" == "Windows_NT" || -n "$MSYSTEM" ]]; then
    g++ $COMMON -I/c/msys64/ucrt64/include/taglib \
        aac_m4a_probe.cpp ../../src/AacDecoder.cpp ma_impl.cpp \
        -lfdk-aac -ltag -lole32 -lwinmm -luuid \
        -o aac_m4a_probe.exe
    echo "built aac_m4a_probe.exe"
else
    g++ $COMMON -I/usr/include/taglib \
        aac_m4a_probe.cpp ../../src/AacDecoder.cpp ma_impl.cpp \
        -lfdk-aac -ltag -lpthread -lm -ldl \
        -o aac_m4a_probe
    echo "built aac_m4a_probe"
fi
