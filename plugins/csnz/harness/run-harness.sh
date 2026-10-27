#!/usr/bin/env bash
# Run the harness against the UNMODIFIED mp.dll under Wine.
# cwd is set to Bin/ so mp.dll and its sibling runtime DLLs (msvcp140, vcruntime140,
# ucrtbase, api-ms-win-*) resolve. Nothing is written into the install; log goes to stdout.
#
#   CSNZ_ROOT=/path/to/CSNZ ./run-harness.sh [--entityapi]
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${CSNZ_ROOT:-$(cd "$HERE/../.." && pwd)}"
BIN="$ROOT/Bin"
HARNESS="$HERE/csnz_mp_harness.exe"
[ -f "$BIN/mp.dll" ] || { echo "mp.dll not found in $BIN (set CSNZ_ROOT)"; exit 1; }
[ -f "$HARNESS" ]    || { echo "harness not built; run: make"; exit 1; }

# Prefer the game's own VC++ runtime over Wine's builtins (Wine's msvcp140 lacks symbols the DLLs use).
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-msvcp140=n,b;msvcp140_1=n,b;msvcp140_2=n,b;vcruntime140=n,b;ucrtbase=n,b;concrt140=n,b;vccorlib140=n,b}"
export WINEDEBUG="${WINEDEBUG:-fixme-all}"
WINE="${CSNZ_WINE:-wine}"

cd "$BIN"
exec "$WINE" "$HARNESS" mp.dll "$@"
