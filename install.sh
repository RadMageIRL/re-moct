#!/usr/bin/env bash
# RE-MOCT installer - build (optional) and install to a prefix.
#
# Installation is delegated to CMake's own install rules rather than copying
# files by hand. That matters: the streaming plugin has to land in
# <bindir>/plugins/, because the host resolves it relative to the real path of
# the running executable (port::exeDir() + "/plugins"). Hand-copying it
# anywhere else produces a player that starts fine and then cannot open a
# single stream - so the layout is defined in exactly one place, CMakeLists.txt,
# and this script never second-guesses it.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD_DIR:-$SRC/build}"
PREFIX="${PREFIX:-/usr/local}"
DO_BUILD=1
MODE=install
WITH_TESTS=0

usage() {
  cat <<'EOF'
Usage: ./install.sh [options]
  --prefix DIR    install prefix (default /usr/local, or $PREFIX)
  --no-build      install an existing build, don't rebuild
  --link          symlink to the build tree instead of copying (dev mode)
  --with-tests    build the test tools too (default: player + plugin only)
  --tests-only    build just the test tools, and do not install
  --uninstall     remove installed files
  -h, --help      this message

Environment:
  BUILD_DIR       build directory (default ./build)
  PREFIX          install prefix (same as --prefix)
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)
      [ $# -ge 2 ] || { echo "--prefix needs a directory" >&2; exit 2; }
      [ -n "$2" ]  || { echo "--prefix needs a non-empty directory" >&2; exit 2; }
      PREFIX="$2"; shift 2 ;;
    --no-build)  DO_BUILD=0; shift ;;
    --link)      MODE=link; DO_BUILD=0; shift ;;
    --with-tests) WITH_TESTS=1; shift ;;
    --tests-only) MODE=tests; WITH_TESTS=1; shift ;;
    --uninstall) MODE=uninstall; DO_BUILD=0; shift ;;
    -h|--help)   usage; exit 0 ;;
    *)           echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

# Building is the whole point of --tests-only, and there is no install for
# --no-build to skip, so it has nothing to mean here. Normalised after the loop
# rather than in the branch so the two flags behave the same in either order -
# otherwise "--tests-only --no-build" would report success having built nothing.
[ "$MODE" = tests ] && DO_BUILD=1

# An empty prefix would silently mean "/" - i.e. install into /bin - and a
# relative one would resolve against whatever directory the user happened to be
# in when they ran this, which is not what anyone means by --prefix. Refuse the
# first and make the second absolute.
[ -n "$PREFIX" ] || { echo "PREFIX must not be empty" >&2; exit 2; }
case "$PREFIX" in
  /*) ;;
  *)  PREFIX="$(cd "$(dirname "$PREFIX")" 2>/dev/null && pwd)/$(basename "$PREFIX")" \
        || { echo "cannot resolve prefix: $PREFIX" >&2; exit 2; } ;;
esac

BIN="$BUILD/bin/remoct"
DEST_BIN="$PREFIX/bin/remoct"
MANIFEST="$BUILD/install_manifest.txt"

# Elevate only when the destination really isn't writable. Testing the leaf
# directory alone is wrong when it does not exist yet: a fresh
# "--prefix ~/.local" would test a missing ~/.local/bin, read it as
# unwritable, and then sudo would create a root-owned directory inside the
# user's home. Walk up to the nearest ancestor that exists and test that.
# Skipped entirely for --tests-only, which never touches the prefix: this probe
# EXITS when the prefix is unwritable and sudo is missing, which would otherwise
# refuse to build test tools on a box that simply has no sudo - over a directory
# that run is never going to write to.
SUDO=""
if [ "$MODE" != tests ] && [ "$(id -u)" -ne 0 ]; then
  probe="$PREFIX/bin"
  while [ ! -e "$probe" ]; do probe="$(dirname "$probe")"; done
  if [ ! -w "$probe" ]; then
    command -v sudo >/dev/null 2>&1 || {
      echo "$PREFIX is not writable and sudo is not available." >&2
      echo "Re-run as root, or choose a writable prefix:" >&2
      echo "  ./install.sh --prefix \"\$HOME/.local\"" >&2
      exit 1
    }
    SUDO=sudo
  fi
fi

if [ "$MODE" = uninstall ]; then
  if [ -f "$MANIFEST" ]; then
    # Remove exactly what was installed, rather than guessing at a layout that
    # may have moved since.
    #
    # `|| [ -n "$f" ]` is load-bearing: CMake writes install_manifest.txt with NO
    # trailing newline, so a plain `while read` reports failure on the final entry
    # and drops it. That entry is the last thing installed - the stream plugin -
    # which would survive every uninstall and be left behind on the user's system.
    while IFS= read -r f || [ -n "$f" ]; do
      if [ -n "$f" ]; then $SUDO rm -f "$f"; fi
    done < "$MANIFEST"
  else
    echo "no $MANIFEST - removing the known layout instead"
    $SUDO rm -f "$DEST_BIN" "$PREFIX/bin/remoct_logo.jpg"
    $SUDO rm -f "$PREFIX"/bin/plugins/remoct_stream.* 2>/dev/null || true
  fi
  # Only if we emptied it; never recursive, in case something else lives there.
  $SUDO rmdir "$PREFIX/bin/plugins" 2>/dev/null || true
  hash -r 2>/dev/null || true
  echo "removed RE-MOCT from $PREFIX"
  exit 0
fi

command -v cmake >/dev/null 2>&1 || {
  echo "cmake is required but not installed." >&2
  exit 1
}

if [ "$DO_BUILD" = 1 ]; then
  # Pick the generator only on a FIRST configure: passing -G at an existing
  # build directory whose generator differs is a hard cmake error. Ninja when
  # it's there, otherwise whatever cmake defaults to - a missing ninja should
  # not be the difference between installing and not.
  # Which targets exist is CMake's decision, not this script's - the same rule
  # the plugin layout follows. All this does is forward the choice. Passed on
  # EVERY configure rather than only the first: CMake caches the option, so a
  # build directory configured once without the tests would silently keep
  # skipping them, and `ctest` reports success when it finds no tests at all.
  if [ "$WITH_TESTS" = 1 ]; then tests_cfg=ON; else tests_cfg=OFF; fi

  if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    gen=()
    command -v ninja >/dev/null 2>&1 && gen=(-G Ninja)
    cmake -S "$SRC" -B "$BUILD" ${gen[@]+"${gen[@]}"} -DCMAKE_BUILD_TYPE=Release \
          -DREMOCT_BUILD_TESTS="$tests_cfg"
  else
    cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
          -DREMOCT_BUILD_TESTS="$tests_cfg"
  fi

  # --tests-only builds the grouping target, which pulls every test tool and the
  # one plugin they exercise, and leaves the player unbuilt.
  target=()
  [ "$MODE" = tests ] && target=(--target remoct_tests)

  jobs="$(nproc 2>/dev/null || echo 2)"
  log="$BUILD/install-build.log"
  # Inside `if !`, set -e stands down and pipefail still reports cmake's status,
  # so a failed build reaches this message instead of aborting the script at the
  # pipeline with the explanation unprinted.
  if ! cmake --build "$BUILD" -j "$jobs" ${target[@]+"${target[@]}"} 2>&1 | tee "$log"; then
    echo "build failed - not installing (full log: $log)" >&2
    exit 1
  fi
fi

# Nothing below this point applies to a tests-only run: there is no player
# binary to check for, no prefix to install into, and no installed command to
# report a version. Leaving before the executable check matters - that check
# would otherwise fail a perfectly good build with "build first".
if [ "$MODE" = tests ]; then
  echo "built the test tools in $BUILD (nothing installed)"
  echo "run them with: ctest --test-dir \"$BUILD\" --no-tests=error --output-on-failure"
  exit 0
fi

[ -x "$BIN" ] || {
  echo "no binary at $BIN - build first (drop --no-build/--link)" >&2
  exit 1
}

if [ "$MODE" = link ]; then
  $SUDO mkdir -p "$PREFIX/bin"
  $SUDO ln -sfn "$BIN" "$DEST_BIN"
  # No plugin symlink needed: the host resolves plugins from the RESOLVED path
  # of the executable (/proc/self/exe follows the link), so a symlinked remoct
  # still finds $BUILD/bin/plugins. That is the point of dev mode - rebuild and
  # the installed command picks it up with no reinstall.
  echo "linked $DEST_BIN -> $BIN"
else
  $SUDO cmake --install "$BUILD" --prefix "$PREFIX"
  # The manifest is what --uninstall reads. Written as root during a sudo
  # install, it would otherwise block the next ordinary build from rewriting it.
  if [ -n "$SUDO" ] && [ -f "$MANIFEST" ]; then
    $SUDO chown "$(id -u):$(id -g)" "$MANIFEST" 2>/dev/null || true
  fi
  echo "installed to $PREFIX"
fi

hash -r 2>/dev/null || true

case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) echo "note: $PREFIX/bin is not on your PATH - add it to run 'remoct' by name" ;;
esac

"$DEST_BIN" --version
