#!/bin/sh
# Bulwark Live end-to-end test: a bwlive-server, a dedicated server and a
# headless client on this machine. Both queue the duel playlist; the matcher
# pairs them, the server (queued first) hosts, the client connects to
# "live:<host id>" and must reach the game, first through the relay and
# then over a punched direct path.
# Usage: run.sh path/to/build path/to/bwlive-server
# LIVE_PUNCH=0 turns punching off, so everything goes through the relay.
# Needs FTEQCC (to build the test progs); skipped if unset.
set -e
BUILD="$(cd "$1" && pwd)"
LIVE="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$FTEQCC" ]; then
	echo "FTEQCC not set, skipping live test"
	exit 0
fi
OUT="$BUILD/livetest"
rm -rf "$OUT"
mkdir -p "$OUT/game/maps" "$OUT/client/game"
(cd "$HERE/../bots" && "$FTEQCC" -Tfte progs.src > "$OUT/qcc.log" 2>&1) || { cat "$OUT/qcc.log"; exit 1; }
mv "$HERE/../bots/progs.dat" "$OUT/game/progs.dat"
rm -f "$HERE/../bots/progs.lno"
cp "$HERE/../data/maps/botmap.bsp" "$OUT/game/maps/"
cp "$HERE/../data/hltest.wad" "$OUT/game/"
cp -r "$OUT/game/maps" "$OUT/game/hltest.wad" "$OUT/client/game/"
PORT=${LIVE_PORT:-27960}

"$LIVE" --bind 127.0.0.1:$PORT --http 127.0.0.1:$((PORT + 1)) > "$OUT/live.log" 2>&1 &
LIVEPID=$!
sleep 0.5
# the host queues first, so it waited longest and hosts; live_hostcmd keeps
# its map (the duel playlist's mode names duel_alley)
"$BUILD/bulwark-ded" -basedir "$OUT" -game game +set name host +set live_punch ${LIVE_PUNCH:-1} +set live_game stiletto \
	+set live_hostcmd "echo hosting" +set live_match_delay 0 \
	+set live_onconnect "live_queue duel 5" +live_connect 127.0.0.1:$PORT +maxplayers 4 +map botmap \
	> "$OUT/host.log" 2>&1 &
HOSTPID=$!
sleep 2
"$BUILD/bulwark" -headless -basedir "$OUT/client" -game game +set name guest +set live_punch ${LIVE_PUNCH:-1} +set live_game stiletto \
	+set live_match_delay 0 +set live_onconnect "live_queue duel 6" +live_connect 127.0.0.1:$PORT \
	> "$OUT/client.log" 2>&1 &
CLIENTPID=$!
sleep ${LIVE_WAIT:-15}
curl -s "http://127.0.0.1:$((PORT + 1))/" > "$OUT/status.json" || true
# headless clients don't stop on SIGTERM
kill -9 $CLIENTPID $HOSTPID $LIVEPID 2>/dev/null || true
wait 2>/dev/null || true

status=0
check() {
	if grep -q "$2" "$OUT/$1.log"; then
		echo "live: $3: ok"
	else
		echo "live: $3: FAIL (no \"$2\" in $OUT/$1.log)"
		status=1
	fi
}
check host "live: connected as" "host registered"
check client "live: connected as" "client registered"
check client "live: match on duel" "matched"
check host "(you)" "host chosen"
check client "connecting to live:" "client follows the host"
check host "guest entered the game" "client reached the game"
grep -q '"matches_made":1' "$OUT/status.json" && echo "live: status page counts the match: ok" || { echo "live: status page: FAIL"; status=1; }
RELAYED=$(sed -n 's/.*"relayed_bytes":\([0-9]*\).*/\1/p' "$OUT/status.json")
echo "live: relayed $RELAYED bytes (punching ${LIVE_PUNCH:-1})"
if [ "${LIVE_PUNCH:-1}" = 0 ] && [ "${RELAYED:-0}" -lt 2000 ]; then
	echo "live: game traffic went through the relay: FAIL"
	status=1
fi
exit $status
