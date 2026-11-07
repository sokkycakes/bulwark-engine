#!/bin/sh
# Bulwark Live co-op playlist test: a playlist with one team and a game
# mode. A dedicated server (no map loaded) and a headless client both queue
# it; the server hosts, applies the mode's settings, loads the mode's map by
# itself (live_hostcmd is empty) and the client joins it.
# Usage: coop.sh path/to/build path/to/bwlive-server
# Needs FTEQCC (to build the test progs); skipped if unset.
set -e
BUILD="$(cd "$1" && pwd)"
LIVE="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$FTEQCC" ]; then
	echo "FTEQCC not set, skipping live co-op test"
	exit 0
fi
OUT="$BUILD/livecoop"
rm -rf "$OUT"
mkdir -p "$OUT/game/maps" "$OUT/client/game"
(cd "$HERE/../bots" && "$FTEQCC" -Tfte progs.src > "$OUT/qcc.log" 2>&1) || { cat "$OUT/qcc.log"; exit 1; }
mv "$HERE/../bots/progs.dat" "$OUT/game/progs.dat"
rm -f "$HERE/../bots/progs.lno"
cp "$HERE/../data/maps/botmap.bsp" "$OUT/game/maps/"
cp "$HERE/../data/hltest.wad" "$OUT/game/"
cp -r "$OUT/game/maps" "$OUT/game/hltest.wad" "$OUT/client/game/"
cat > "$OUT/playlists.json" <<'JSON'
[{ "id": "testfight", "name": "Test Firefight", "game": "cooptest", "team_size": 2, "teams": 1,
   "stages": [{ "after": 0 }],
   "modes": [{ "id": "waves", "name": "Waves", "maps": ["botmap"],
               "settings": { "coop_testwaves": "1", "coop_testdelay": "30" } }] }]
JSON
PORT=${LIVE_PORT:-27964}

"$LIVE" --bind 127.0.0.1:$PORT --http 127.0.0.1:$((PORT + 1)) --playlists "$OUT/playlists.json" > "$OUT/live.log" 2>&1 &
LIVEPID=$!
sleep 0.5
"$BUILD/bulwark-ded" -basedir "$OUT" -game game +set name host +set live_game cooptest +maxplayers 4 \
	+set live_onconnect "live_queue testfight" +live_connect 127.0.0.1:$PORT \
	> "$OUT/host.log" 2>&1 &
HOSTPID=$!
sleep 2
"$BUILD/bulwark" -headless -basedir "$OUT/client" -game game +set name guest +set live_game cooptest \
	+set live_onconnect "live_queue testfight" +live_connect 127.0.0.1:$PORT \
	> "$OUT/client.log" 2>&1 &
CLIENTPID=$!
sleep ${LIVE_WAIT:-15}
curl -s "http://127.0.0.1:$((PORT + 1))/" > "$OUT/status.json" || true
kill $CLIENTPID $HOSTPID $LIVEPID 2>/dev/null || true
wait 2>/dev/null || true

status=0
check() {
	if grep -q "$2" "$OUT/$1.log"; then
		echo "live coop: $3: ok"
	else
		echo "live coop: $3: FAIL (no \"$2\" in $OUT/$1.log)"
		status=1
	fi
}
check client "live: match on testfight, waves on botmap" "matched with the mode and map"
check host "(you)" "host chosen"
check host 'live: coop_testwaves "1"' "host applied the mode's settings"
check host 'live: coop_testdelay "30"' "host applied every setting"
check client "connecting to live:" "client follows the host"
check host "guest entered the game" "client reached the host's map"
grep -q '"modes":\[{"id":"waves"' "$OUT/status.json" && echo "live coop: status page lists the modes: ok" || { echo "live coop: status page: FAIL"; status=1; }
exit $status
