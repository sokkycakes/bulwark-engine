#!/bin/sh
# Bulwark Live lobby test: an online lobby found by room code over the
# internet path. One headless client creates an online lobby (its room code
# is its Live party's code), a second joins by that code, both ready up, and
# the host's listen server takes the guest in over "live:<id>".
# Usage: lobby.sh path/to/build path/to/bwlive-server   (needs FTEQCC)
set -e
BUILD="$(cd "$1" && pwd)"
LIVE="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$FTEQCC" ]; then
	echo "FTEQCC not set, skipping live lobby test"
	exit 0
fi
OUT="$BUILD/livelobby"
rm -rf "$OUT"
mkdir -p "$OUT/a/game/maps" "$OUT/b/game/maps"
(cd "$HERE/../bots" && "$FTEQCC" -Tfte progs.src > "$OUT/qcc.log" 2>&1) || { cat "$OUT/qcc.log"; exit 1; }
for d in a b; do
	cp "$HERE/../bots/progs.dat" "$OUT/$d/game/"
	cp "$HERE/../data/maps/botmap.bsp" "$OUT/$d/game/maps/"
	cp "$HERE/../data/hltest.wad" "$OUT/$d/game/"
done
rm -f "$HERE/../bots/progs.dat" "$HERE/../bots/progs.lno"
PORT=${LIVE_PORT:-27970}
COMMON="${LIVE_EXTRA} +fps_max 100 +set live_game lobbytest +set lobby_defaultmap botmap +set lobby_readytime 1 +set lobby_port 0"

"$LIVE" --bind 127.0.0.1:$PORT > "$OUT/live.log" 2>&1 &
LIVEPID=$!
sleep 0.5
# frames at 100 a second: the host readies after 6 s, once the guest is in
stdbuf -oL "$BUILD/bulwark" -headless -basedir "$OUT/a" -game game +set name hostess $COMMON \
	+set live_onconnect "lobby_create online; wait 800; lobby_ready" +live_connect 127.0.0.1:$PORT \
	> "$OUT/host.log" 2>&1 &
HOSTPID=$!
CODE=""
for i in $(seq 1 40); do
	sleep 0.5
	CODE=$(sed -n 's/^lobby: room code \([A-Z0-9]*\) (Bulwark Live)$/\1/p' "$OUT/host.log" | head -1)
	[ -n "$CODE" ] && break
done
echo "live lobby: room code ${CODE:-none}"
stdbuf -oL "$BUILD/bulwark" -headless -basedir "$OUT/b" -game game +set name guest $COMMON \
	+set live_onconnect "lobby_join $CODE; wait 300; lobby_ready" +live_connect 127.0.0.1:$PORT \
	> "$OUT/guest.log" 2>&1 &
GUESTPID=$!
sleep ${LIVE_WAIT:-14}
kill $GUESTPID $HOSTPID $LIVEPID 2>/dev/null || true
wait 2>/dev/null || true

status=0
check() {
	if grep -q "$2" "$OUT/$1.log"; then
		echo "live lobby: $3: ok"
	else
		echo "live lobby: $3: FAIL (no \"$2\" in $OUT/$1.log)"
		status=1
	fi
}
[ -n "$CODE" ] || { echo "live lobby: host got a room code: FAIL"; status=1; }
check guest "in party $CODE" "guest found the room over Live"
check guest "lobby: joined, seat" "guest took a lobby seat"
check guest "the game is on live:" "guest told where the game is"
check host "guest entered the game" "guest reached the host's game"
exit $status
