#!/bin/sh
# bmfont tests: the BDF fixture, then the same font as PCF in every layout
# mkpcf.py can write. Needs python3 for the PCF files; without it only the
# BDF is checked.
set -e
TEST="$1"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
set -- "$DIR/test.bdf"
if command -v python3 >/dev/null 2>&1; then
	n=0
	for opts in "" "--msbyte --msbit" "--msbit" "--msbyte" "--pad 1" "--pad 2 --unit 2 --msbyte" \
		"--pad 4 --unit 4" "--pad 4 --unit 4 --msbyte" "--pad 8 --unit 4 --msbit" "--compressed" "--compressed --msbyte --msbit --gzip" "--gzip"; do
		n=$((n + 1))
		python3 "$DIR/mkpcf.py" "$DIR/test.bdf" "$TMP/t$n.pcf" $opts
		set -- "$@" "$TMP/t$n.pcf"
	done
fi
exec "$TEST" "$@"
