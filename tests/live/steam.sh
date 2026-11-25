#!/bin/sh
# Bulwark Live on Steam, end to end, with the steam-mock test double in
# place of a Steam client: a hub (the shared "Steam"), one bwlive-steam per
# player, a dedicated host and a headless client. Both engines only run
# `live_connect 127.0.0.1:<their bridge>`; the duel is found through Steam
# lobbies and the client reaches the host's game over Steam networking.
#
# Usage: steam.sh path/to/build path/to/services/live/target/release
# Needs FTEQCC (to build the test progs); skipped if unset.
set -e
BUILD="$(cd "$1" && pwd)"
BIN="$(cd "$2" && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$FTEQCC" ]; then
	echo "FTEQCC not set, skipping steam test"
	exit 0
fi
OUT="$BUILD/steamtest"
rm -rf "$OUT"
mkdir -p "$OUT/game/maps" "$OUT/client/game" "$OUT/a" "$OUT/b"
(cd "$HERE/../bots" && "$FTEQCC" -Tfte progs.src > "$OUT/qcc.log" 2>&1) || { cat "$OUT/qcc.log"; exit 1; }
mv "$HERE/../bots/progs.dat" "$OUT/game/progs.dat"
rm -f "$HERE/../bots/progs.lno"
cp "$HERE/../data/maps/botmap.bsp" "$OUT/game/maps/"
cp "$HERE/../data/hltest.wad" "$OUT/game/"
cp -r "$OUT/game/maps" "$OUT/game/hltest.wad" "$OUT/client/game/"
PORT=${STEAM_PORT:-28950}
HUB=127.0.0.1:$((PORT + 49))

"$BIN/steam-mock-hub" $HUB > "$OUT/hub.log" 2>&1 &
HUBPID=$!
sleep 0.3
bridge() { # dir account name port
	(cd "$OUT/$1" && sleep 600 | STEAM_MOCK_HUB=$HUB STEAM_MOCK_ACCOUNT=$2 STEAM_MOCK_NAME=$3 \
		"$BIN/bwlive-steam" --steam-lib "$BIN/libsteam_mock.so" --bind 127.0.0.1:$4 > "$OUT/$1.log" 2>&1) &
}
bridge a 1001 host $PORT
bridge b 1002 guest $((PORT + 1))
sleep 0.5
"$BUILD/bulwark-ded" -basedir "$OUT" -game game +set name host +set live_game stiletto \
	+set live_hostcmd "echo hosting the match on botmap" \
	+set live_onconnect "live_queue duel 5" +live_connect 127.0.0.1:$PORT +maxplayers 4 +map botmap \
	> "$OUT/host.log" 2>&1 &
HOSTPID=$!
sleep 2
"$BUILD/bulwark" -headless -basedir "$OUT/client" -game game +set name guest +set live_game stiletto \
	+set live_onconnect "live_queue duel 6" +live_connect 127.0.0.1:$((PORT + 1)) \
	> "$OUT/client.log" 2>&1 &
CLIENTPID=$!
sleep ${STEAM_WAIT:-15}
kill $CLIENTPID $HOSTPID 2>/dev/null || true
sleep 0.5
kill -9 $CLIENTPID $HOSTPID 2>/dev/null || true
pkill -f "bwlive-steam --steam-lib $BIN" 2>/dev/null || true
pkill -f "sleep 600" 2>/dev/null || true
kill $HUBPID 2>/dev/null || true
wait 2>/dev/null || true

status=0
check() {
	if grep -q "$2" "$OUT/$1.log"; then
		echo "steam: $3: ok"
	else
		echo "steam: $3: FAIL (no \"$2\" in $OUT/$1.log)"
		status=1
	fi
}
check a "on Steam as host (account 1001)" "host's bridge is on Steam"
check host "live: connected as 1001" "host registered as its Steam account"
check client "live: connected as 1002" "client registered as its Steam account"
check b "joined a waiting ticket" "client found the host's ticket by lobby search"
check client "live: match on duel" "matched"
check host "(you)" "host chosen"
check client "connecting to live:1001" "client follows the host"
check host "guest entered the game" "client reached the game over Steam networking"
check b "game traffic over Steam" "game traffic went through Steam"
exit $status
