#!/bin/sh
# Bulwark Live drop-in test (Horde style): a dedicated server queues a
# drop-in playlist alone and starts its match by itself; a client queues
# later and drops into the running match; when the host kicks it, the
# client tells the server it left, its seat is freed and the host is told.
# Usage: dropin.sh path/to/build path/to/bwlive-server
# Needs FTEQCC (to build the test progs); skipped if unset.
set -e
BUILD="$(cd "$1" && pwd)"
LIVE="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$FTEQCC" ]; then
	echo "FTEQCC not set, skipping live co-op test"
	exit 0
fi
OUT="$BUILD/livedropin"
rm -rf "$OUT"
mkdir -p "$OUT/game/maps" "$OUT/client/game"
(cd "$HERE/../bots" && "$FTEQCC" -Tfte progs.src > "$OUT/qcc.log" 2>&1) || { cat "$OUT/qcc.log"; exit 1; }
mv "$HERE/../bots/progs.dat" "$OUT/game/progs.dat"
rm -f "$HERE/../bots/progs.lno"
cp "$HERE/../data/maps/botmap.bsp" "$OUT/game/maps/"
cp "$HERE/../data/hltest.wad" "$OUT/game/"
cp -r "$OUT/game/maps" "$OUT/game/hltest.wad" "$OUT/client/game/"
cat > "$OUT/playlists.json" <<'JSON'
[{ "id": "testhorde", "name": "Test Horde", "game": "cooptest", "team_size": 8, "teams": 1,
   "stages": [{ "after": 0 }, { "after": 2 }], "min_players": 1, "drop_in": true,
   "modes": [{ "id": "horde", "maps": ["botmap"] }] }]
JSON
PORT=${LIVE_PORT:-27968}

"$LIVE" --bind 127.0.0.1:$PORT --http 127.0.0.1:$((PORT + 1)) --playlists "$OUT/playlists.json" > "$OUT/live.log" 2>&1 &
LIVEPID=$!
sleep 0.5
# the host kicks the guest after a while (its hostcmd loads the map itself,
# then waits); the guest's game then reports that it left the match
"$BUILD/bulwark-ded" -basedir "$OUT" -game game +set name host +set live_game cooptest +maxplayers 4 \
	+set live_hostcmd "map botmap; defer 12 kick guest" \
	+set live_onconnect "live_queue testhorde" +live_connect 127.0.0.1:$PORT \
	> "$OUT/host.log" 2>&1 &
HOSTPID=$!
sleep 6
"$BUILD/bulwark" -headless -basedir "$OUT/client" -game game +set name guest +set live_game cooptest +fps_max 100 \
	+set live_onconnect "live_queue testhorde" +live_connect 127.0.0.1:$PORT \
	> "$OUT/client.log" 2>&1 &
CLIENTPID=$!
sleep 10
curl -s "http://127.0.0.1:$((PORT + 1))/" > "$OUT/status.json" || true
sleep ${LIVE_TAIL:-25}
# headless clients don't stop on SIGTERM
kill -9 $CLIENTPID $HOSTPID $LIVEPID 2>/dev/null || true
wait 2>/dev/null || true

status=0
check() {
	if grep -q "$2" "$OUT/$1.log"; then
		echo "live dropin: $3: ok"
	else
		echo "live dropin: $3: FAIL (no \"$2\" in $OUT/$1.log)"
		status=1
	fi
}
check host "live: match on testhorde, horde on botmap, 1 players, host .* (you)" "host started alone"
check client "live: match on testhorde, horde on botmap, 2 players" "client dropped into the running match"
check host "live: the match now has 2 players" "host told of the drop-in"
check client "connecting to live:" "client follows the host"
check host "guest entered the game" "client reached the host's game"
check client "live: left the match" "client reported leaving"
check host "live: the match now has 1 player$" "seat freed after the client left"
grep -q '"drop_ins":1' "$OUT/status.json" && echo "live dropin: status page counts the drop-in: ok" || { echo "live dropin: status page: FAIL"; status=1; }
grep -q '"matches_made":1' "$OUT/status.json" && echo "live dropin: one match, not two: ok" || { echo "live dropin: match count: FAIL"; status=1; }
exit $status
