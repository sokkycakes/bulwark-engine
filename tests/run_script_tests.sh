#!/bin/sh
# Runs the scripting layer tests: Lua, JavaScript and QuakeC entity scripts on
# Nuclide's base game. Usage: run_script_tests.sh <bulwark-ded> <nuclide base dir>
# FTEQCC (a build tool only) compiles the QC script; skipped if unset.
set -e
DED="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
BASE="$(cd "$2" && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$FTEQCC" ]; then
	echo "FTEQCC not set, skipping script tests"
	exit 0
fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
(cd "$HERE/script/qc" && "$FTEQCC" progs.src > "$WORK/qcc.log" 2>&1) || { cat "$WORK/qcc.log"; exit 1; }
rm -f "$HERE/script/game/scripts/vscripts/test_qc.lno"
mkdir -p "$WORK/scripttest/maps"
ln -s "$BASE" "$WORK/base"
cp -r "$HERE/script/game/." "$WORK/scripttest/"
mv "$HERE/script/game/scripts/vscripts/test_qc.dat" "$WORK/scripttest/scripts/vscripts/"
# the test map is Nuclide's ai.bsp with its entities replaced by maps/scripttest.ent
ln -s "$BASE/maps/ai.bsp" "$WORK/scripttest/maps/scripttest.bsp"

"$DED" -basedir "$WORK" -game base -game scripttest -frames 60 +map scripttest \
	'+script printl("ok console lua")' \
	'+script_js printl("ok console js")' \
	'+script_event test_event value 42 who console' \
	'+script_timeout 0.2' \
	'+script while true do end' \
	'+script_js while (true) {}' \
	'+script_timeout 5' \
	'+script printl("ok after timeout")' > "$WORK/out.log" 2>&1 || { cat "$WORK/out.log"; exit 1; }

fail=0
while IFS= read -r line; do
	if ! grep -qxF "$line" "$WORK/out.log"; then
		echo "missing: $line"
		fail=1
	fi
done < "$HERE/script/expected.txt"
if grep -q "^FAIL" "$WORK/out.log"; then
	grep "^FAIL" "$WORK/out.log"
	fail=1
fi
# exactly the two runaway scripts may raise errors
errors=$(grep -c "script error" "$WORK/out.log" || true)
if [ "$errors" != 2 ]; then
	echo "expected 2 script errors (the timeouts), got $errors:"
	grep -A3 "script error" "$WORK/out.log"
	fail=1
fi
if [ $fail = 1 ]; then
	echo "--- log ---"
	cat "$WORK/out.log"
	exit 1
fi
echo "script tests passed ($(wc -l < "$HERE/script/expected.txt") checks)"
