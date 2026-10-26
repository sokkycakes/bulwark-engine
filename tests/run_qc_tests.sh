#!/bin/sh
# Compiles tests/qc with FTEQCC (a build tool only, not part of the engine)
# and runs them with qcrun. Set FTEQCC to the compiler; skipped if unset.
set -e
QCRUN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$FTEQCC" ]; then
	echo "FTEQCC not set, skipping QC tests"
	exit 0
fi
OUT="$(cd "$(dirname "$QCRUN")" && pwd)/tests"
mkdir -p "$OUT"
(cd "$HERE/qc" && "$FTEQCC" -Tfte progs.src > "$OUT/vmtest.log" 2>&1) || { cat "$OUT/vmtest.log"; exit 1; }
mv "$HERE/qc/vmtest.dat" "$OUT/vmtest.dat"
rm -f "$HERE/qc/vmtest.lno"
"$QCRUN" "$OUT/vmtest.dat" main
