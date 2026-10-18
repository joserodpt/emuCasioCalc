#!/usr/bin/env bash
# Rebuilds emucasiofxcg if the sources changed, then runs it.
#   ./run.sh [emucasiofxcg options] [addin.g3a]
# Without an add-in the window opens empty (File > Open loads one).
# e.g. ./run.sh --scale 3 build-linux/gint-hello/GintHello.g3a
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
if [[ "$(uname)" == Darwin ]]; then
  BUILD="$ROOT/build"
else
  BUILD="$ROOT/build-linux"
fi
DEPS="$HOME/.local/casio-deps"  # SDL3 built from source (Linux machine without sudo)

if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
  cmake -S "$ROOT" -B "$BUILD" ${DEPS:+-DCMAKE_PREFIX_PATH="$DEPS"} >/dev/null
fi
# Incremental: only recompiles what changed. Quiet unless something fails.
if ! out=$(cmake --build "$BUILD" --target emucasiofxcg -j 2>&1); then
  echo "$out" >&2
  exit 1
fi

if [[ -d "$DEPS/lib" ]]; then
  export LD_LIBRARY_PATH="$DEPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$BUILD/emucasiofxcg" "$@"
