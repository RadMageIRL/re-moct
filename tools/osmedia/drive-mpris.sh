#!/usr/bin/env bash
# osmedia-phase0-probe / Probe 2 build + drive (retained artifact). Run under
# WSL2 Debian. Builds the sd-bus MPRIS daemon, spawns an ephemeral session bus
# (the WSL2 box has no login session bus), and drives it with playerctl.
# Standalone - NOT part of the production build system.
set -e
cd "$(dirname "$0")"

echo "=== build ==="
cc -std=c11 -O0 -g -Wall -o mpris_probe mpris_probe.c $(pkg-config --cflags --libs libsystemd)
echo "built: mpris_probe  (link set: $(pkg-config --libs libsystemd))"

echo "=== drive (ephemeral session bus + playerctl) ==="
dbus-run-session -- bash -c '
  ./mpris_probe & PROBE=$!
  sleep 0.6
  echo "--- playerctl -l (players seen) ---";        playerctl -l || true
  echo "--- playerctl -p remoct status ---";         playerctl -p remoct status || true
  echo "--- playerctl -p remoct metadata ---";       playerctl -p remoct metadata || true
  echo "--- drive: play-pause / next / previous ---"
  playerctl -p remoct play-pause || true
  playerctl -p remoct next       || true
  playerctl -p remoct previous   || true
  playerctl -p remoct stop       || true
  sleep 0.4
  kill -TERM $PROBE 2>/dev/null || true
  wait $PROBE 2>/dev/null || true
'
