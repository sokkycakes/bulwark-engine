#!/bin/sh
# Stiletto Proto on Bulwark, through Bulwark Live on Steam (steam-mock in
# place of a Steam client): two headless players queue the duel playlist,
# Steam lobbies pair them, the host loads the playlist's map with its
# settings, the guest joins over Steam networking, they play a set (the
# guest loses every round with `kill`), the host's rules report the set,
# and both players' bridges record it and swap fighter cards.
#
# Usage: steam_stiletto.sh path/to/build path/to/services/live/target/release BASEDIR
#   BASEDIR holds base/ (a nuclide checkout's base, with maps/duel_alley.bsp)
#   and stbuilt/ (Stiletto's progs compiled from that branch); see
#   docs/handoff-stiletto.md, "Building and testing".
set -e
BUILD="$(cd "$1" && pwd)"
BIN="$(cd "$2" && pwd)"
BASE="$(cd "$3" && pwd)"
OUT="$BUILD/steamstiletto"
rm -rf "$OUT"
mkdir -p "$OUT/a" "$OUT/b" "$OUT/host/out" "$OUT/guest/out"
for p in host guest; do
	ln -s "$BASE/base" "$OUT/$p/base"
	ln -s "$BASE/stbuilt" "$OUT/$p/stbuilt"
done
PORT=${STEAM_PORT:-28950}
HUB=127.0.0.1:$((PORT + 49))
SECS=${STEAM_SECS:-240}

# both pick a hero and lock it whenever asked; the guest dies every few
# seconds once a round is on, so the host wins each game
python3 - "$OUT" "$SECS" <<'EOF'
import sys
out, secs = sys.argv[1], int(sys.argv[2])
for who in ("host", "guest"):
    lines = ["set lobby_readytime 3"]
    for t in range(8, secs - 10, 2):
        lines.append(f"in {t} cmd arena_enter")
        lines.append(f"in {t} cmd ready")
        lines.append(f"in {t} cmd selecthero hero_juliet")
        lines.append(f"in {t} cmd lockhero")
        if who == "guest" and t % 4 == 0:
            lines.append(f"in {t} kill")
    lines.append(f"in {secs - 8} live_status")
    lines.append(f"in {secs - 8} live_state")
    open(f"{out}/{who}/out/flow.cfg", "w").write("\n".join(lines) + "\n")
EOF

"$BIN/steam-mock-hub" $HUB > "$OUT/hub.log" 2>&1 &
HUBPID=$!
sleep 0.3
for x in "a 1001 Ann $PORT" "b 1002 Bo $((PORT + 1))"; do
	set -- $x
	(cd "$OUT/$1" && sleep 3600 | STEAM_MOCK_HUB=$HUB STEAM_MOCK_ACCOUNT=$2 STEAM_MOCK_NAME=$3 \
		"$BIN/bwlive-steam" --steam-lib "$BIN/libsteam_mock.so" --bind 127.0.0.1:$4 > "$OUT/$1.log" 2>&1) &
done
sleep 0.5
run() { # who bridge-port extra...
	who=$1; port=$2; shift 2
	"$BUILD/bulwark" -headless -basedir "$OUT/$who" -game base -game stbuilt -game out \
		+set webcore_menumap "" +set name $who +set live_game stiletto +set live_port 0 \
		+exec flow.cfg "$@" +live_connect 127.0.0.1:$port > "$OUT/$who.log" 2>&1 &
}
run host $PORT +set live_onconnect "live_queue duel"
HOSTPID=$!
sleep 1
run guest $((PORT + 1)) +set live_onconnect "live_queue duel"
GUESTPID=$!
sleep $SECS
kill -9 $GUESTPID $HOSTPID 2>/dev/null || true
pkill -f "^$BIN/bwlive-steam" 2>/dev/null || true
pkill -f "sleep 3600" 2>/dev/null || true
kill $HUBPID 2>/dev/null || true
wait 2>/dev/null || true

status=0
check() {
	if grep -q "$2" "$OUT/$1.log"; then
		echo "steam+stiletto: $3: ok"
	else
		echo "steam+stiletto: $3: FAIL (no \"$2\" in $OUT/$1.log)"
		status=1
	fi
}
check host "live: connected as 1001" "host on Live through its Steam bridge"
check guest "live: connected as 1002" "guest on Live through its Steam bridge"
check guest "live: match on duel" "matched on duel"
check host "(you)" "host chosen"
check host 'live: g_gametype "duel"' "host applied the playlist's settings"
check guest "connecting to live:1001" "guest follows the host"
check a "set 1 of match" "host reported a set"
check a "set recorded: W" "host's record has the win"
check b "set recorded: L" "guest's record has the loss"
check guest '"cards":{.*"1001":{"id":1001' "guest has the host's fighter card"
check host '"cards":{.*"1002":{"id":1002' "host has the guest's fighter card"
exit $status
