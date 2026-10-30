#!/bin/sh
# Bot framework acceptance tests (docs/spec/bots.md). Compiles tests/bots
# with FTEQCC, then runs each scenario headless on tests/data/maps/botmap.bsp
# and checks for PASS. Usage: run.sh path/to/bulwark-ded [scenario...]
# Set FTEQCC to the compiler; skipped if unset. BOTTEST_ARGS adds console
# commands (for example "+set bottest_debug path"), BOTTEST_SEEDS runs each
# scenario once per seed (default "0").
set -e
DED="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
shift
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$FTEQCC" ]; then
	echo "FTEQCC not set, skipping bot tests"
	exit 0
fi
SCENARIOS="${*:-basic l4d pnav tf}"
OUT="$(dirname "$DED")/bottest"
mkdir -p "$OUT/game/maps" "$OUT/game/data"
(cd "$HERE" && "$FTEQCC" -Tfte progs.src > "$OUT/qcc.log" 2>&1) || { cat "$OUT/qcc.log"; exit 1; }
mv "$HERE/progs.dat" "$OUT/game/progs.dat"
rm -f "$HERE/progs.lno"
cp "$HERE/../data/maps/botmap.bsp" "$OUT/game/maps/"
cp "$HERE/../data/hltest.wad" "$OUT/game/"
cp "$HERE/../data/botmap.way" "$OUT/game/data/"

status=0
for s in $SCENARIOS; do
	for seed in ${BOTTEST_SEEDS:-0}; do
		log="$OUT/$s-$seed.log"
		# 72 frames a second; the scenarios end themselves, this is a backstop
		"$DED" -basedir "$OUT" -game game -frames 30000 +maxplayers 16 +set bottest "$s" +set bottest_seed "$seed" \
			$BOTTEST_ARGS +map botmap > "$log" 2>&1 || true
		grep '^TEST' "$log" || true
		if grep -q '^PASS' "$log"; then
			echo "bots $s (seed $seed): PASS"
		else
			echo "bots $s (seed $seed): FAIL (log in $log)"
			status=1
		fi
	done
done
exit $status
