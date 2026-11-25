#!/bin/sh
# Stiletto Proto on stock FTEQW, matched and played over Steam with
# bwlive-steam's UDP tunnel (steam-mock in place of a Steam client). FTEQW
# has no Live client, so the bridges are driven from their stdin and start
# the games themselves when the match is made: the host's FTEQW loads the
# playlist's map with its settings as a listen server, the guest's connects
# to 127.0.0.1:<tunnel port>, and every game packet between them crosses
# Steam networking.
#
# Usage: steam_fteqw.sh path/to/services/live/target/release path/to/fteqw-gl BASEDIR [OUT]
#   BASEDIR holds base/ and stbuilt/ as for steam_stiletto.sh. Needs Xvfb.
set -e
BIN="$(cd "$1" && pwd)"
FTE="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
BASE="$(cd "$3" && pwd)"
OUT="${4:-$(mktemp -d)}"
rm -rf "$OUT"
mkdir -p "$OUT/a" "$OUT/b"
for p in a b; do
	ln -s "$BASE/base" "$OUT/$p/base"
	ln -s "$BASE/stbuilt" "$OUT/$p/stbuilt"
	# FTEQW writes its config and downloads here, never into the shared base
	mkdir -p "$OUT/$p/local" && : > "$OUT/$p/local/fte.cfg"
	# stock FTEQW finds its game through a manifest: Nuclide's base, then Stiletto
	printf 'FTEMANIFEST 1\nGAME stiletto\nNAME "Stiletto Proto"\nGAMEDIR base\nGAMEDIR stbuilt\nGAMEDIR local\ndisablehomedir 1\n' > "$OUT/$p/default.fmf"
done
PORT=${STEAM_PORT:-29350}
HUB=127.0.0.1:$((PORT + 49))
GAMEPORT=${STEAM_GAME_PORT:-27500}
SECS=${STEAM_SECS:-60}

Xvfb :91 -screen 0 1024x768x24 > /dev/null 2>&1 &
X1=$!
Xvfb :92 -screen 0 1024x768x24 > /dev/null 2>&1 &
X2=$!
"$BIN/steam-mock-hub" $HUB > "$OUT/hub.log" 2>&1 &
HUBPID=$!
sleep 1
FTEARGS="-basedir . -nosound +set vid_fullscreen 0 +set vid_width 640 +set vid_height 480 +set sv_public 0 +set cl_splitscreen 0 +set cfg_save_auto 0"
# the host's game listens on GAMEPORT; the guest's connects to its bridge's tunnel
(cd "$OUT/a" && { sleep 2; echo "queue duel"; sleep $SECS; echo quit; } | DISPLAY=:91 STEAM_MOCK_HUB=$HUB STEAM_MOCK_ACCOUNT=3001 STEAM_MOCK_NAME=ann \
	"$BIN/bwlive-steam" --steam-lib "$BIN/libsteam_mock.so" --bind 127.0.0.1:$PORT --game stiletto \
	--tunnel --game-port $GAMEPORT --tunnel-port $((PORT + 20)) \
	--on-match-host "exec '$FTE' $FTEARGS +set name ann +set sv_port $GAMEPORT +set net_port $GAMEPORT \$BW_SETTINGS +map \$BW_MAP > fte.log 2>&1" \
	> "$OUT/a.log" 2>&1) &
(cd "$OUT/b" && { sleep 3; echo "queue duel"; sleep $SECS; echo quit; } | DISPLAY=:92 STEAM_MOCK_HUB=$HUB STEAM_MOCK_ACCOUNT=3002 STEAM_MOCK_NAME=bo \
	"$BIN/bwlive-steam" --steam-lib "$BIN/libsteam_mock.so" --bind 127.0.0.1:$((PORT + 1)) --game stiletto \
	--tunnel --tunnel-port $((PORT + 21)) \
	--on-match-join "sleep 8; exec '$FTE' $FTEARGS +set name bo +connect \$BW_CONNECT > fte.log 2>&1" \
	> "$OUT/b.log" 2>&1) &
sleep $((SECS + 5))
pkill -f "^$FTE" 2>/dev/null || true
sleep 1
pkill -9 -f "^$FTE" 2>/dev/null || true
pkill -f "^$BIN/bwlive-steam" 2>/dev/null || true
kill $HUBPID $X1 $X2 2>/dev/null || true
wait 2>/dev/null || true

status=0
check() {
	if grep -q "$2" "$OUT/$1"; then
		echo "steam+fteqw: $3: ok"
	else
		echo "steam+fteqw: $3: FAIL (no \"$2\" in $OUT/$1)"
		status=1
	fi
}
check a.log "match .* on duel on duel_alley, 2 seats, host 3001 (you)" "matched on duel, ann hosts"
check b.log "connect 127.0.0.1:$((PORT + 21)) to play" "bo's bridge offers the tunnel"
check a.log "tunnel: 3002 connects to the server" "bo's packets reach ann's server through Steam"
check b.log "game traffic over Steam" "game traffic crosses Steam"
check a/fte.log "client ann connected" "ann's FTEQW runs the match on duel_alley"
check a/fte.log "client bo connected" "bo joined ann's server through the tunnel"
check b.log "[0-9]\{6,\} in" "bo's game received the server's stream (and downloads) over Steam"
if [ -e "$BASE/base/fte.cfg" ] && [ "$BASE/base/fte.cfg" -nt "$OUT/a/default.fmf" ]; then
	echo "steam+fteqw: FTEQW wrote $BASE/base/fte.cfg; delete it, or Bulwark runs it"
	status=1
fi
exit $status
