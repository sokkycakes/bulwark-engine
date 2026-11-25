#!/bin/sh
# Steam Input tests: the engine under a stand-in libsteam_api (Steam can't
# sign in on a build machine) and under the environment Steam gives a
# non-Steam game. Needs a built engine (build/bulwark) and Xvfb.
#
#   tests/steaminput/run.sh [build dir]
#
# Every case runs the real engine for a few dozen frames in an Xvfb window
# and checks what it printed. docs/spec/steam-input.md says what each
# case stands for.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$REPO/build}
ENGINE=$BUILD/bulwark
WORK=$(mktemp -d)
trap 'kill $XVFB 2>/dev/null; rm -rf "$WORK"' EXIT

cc -shared -fPIC -O1 -o "$WORK/libsteam_api.so" "$REPO/tests/steaminput/mock_steam_api.c" || exit 1
python3 "$REPO/tests/steaminput/check_manifest.py" "$REPO/data/steam_input" || exit 1

DISP=:$((90 + $$ % 9))
Xvfb "$DISP" -screen 0 1024x600x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1

# a shortcut's CGameID: (crc | 0x80000000) << 32 | type 2 << 24
SHORTCUT_ID=15386537552735371264
pass=0
fail=0
mkdir -p "$WORK/base"
# commands whose arguments start with + can't go on the command line
printf 'in_glyph +jump\nin_glyph +attack\n' >"$WORK/base/glyphs.cfg"
printf 'in_glyph +jump 1\nin_glyph +jump 2\n' >"$WORK/base/glyphs2.cfg"

# run <name> <frames> [VAR=value...] -- <engine args...>; output in $WORK/<name>.out
run() {
	name=$1 frames=$2
	shift 2
	envs=""
	while [ "$1" != "--" ]; do envs="$envs $1"; shift; done
	shift
	: >"$WORK/$name.log"
	(cd "$WORK" && env -i PATH="$PATH" HOME="$WORK" DISPLAY="$DISP" SDL_AUDIODRIVER=dummy \
		MOCK_STEAM_LOG="$WORK/$name.log" $envs \
		timeout 120 "$ENGINE" -basedir "$WORK" -enginedata "$REPO/data" -frames "$frames" "$@" \
		>"$WORK/$name.out" 2>&1)
}

expect() { # expect <name> <file> <text>
	if grep -qF -- "$3" "$WORK/$2"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $1: no \"$3\" in $2"
		sed 's/^/    /' "$WORK/$2" | grep -v ALSA | tail -25
	fi
}

refuse() { # refuse <name> <file> <text>
	if grep -qF -- "$3" "$WORK/$2"; then
		fail=$((fail + 1))
		echo "FAIL $1: \"$3\" in $2"
	else
		pass=$((pass + 1))
	fi
}

# 1. As a non-Steam game, Steam running: ISteamInput starts under app 480
#    with the action manifest, a Steam Deck connects, in-game actions run
#    their commands, the menu set drives the menu keys, prompts show the
#    Deck's glyphs, and Steam's virtual gamepad (SDL's copy) is ignored.
cat >"$WORK/deck.txt" <<'EOF'
connect 7 14
origins jump 333
origins key_gp_a 333
origins attack 358
wait 3
press 7 key_gp_dpad_down
wait 2
release 7 key_gp_dpad_down
wait 6
press 7 jump
wait 2
release 7 jump
press 7 attack
wait 2
release 7 attack
wait 30
EOF
run deck 70 SteamGameId=$SHORTCUT_ID BW_STEAM_API_LIB="$WORK/libsteam_api.so" MOCK_STEAM_SCRIPT="$WORK/deck.txt" -- \
	-developer +bind SI_JUMP "echo SI_JUMP_DOWN" \
	+joy_virtual_dev 0x28de 0x11ff "Steam Virtual Gamepad pad 0" \
	+wait +wait +in_glyph GP_A +wait +wait +wait +wait +wait +wait +steaminput_set InGame \
	+wait +wait +wait +wait +wait +wait +wait +wait +wait +wait +wait +wait +wait +wait +wait \
	+joy_virtual_set 0 button 0 1 +wait +wait +joy_virtual_set 0 button 0 0 \
	+exec glyphs.cfg +in_steaminput_status +in_deviceids +bind SI_SLOT1 +bind SI_PAUSE +in_padglyphs
expect "non-Steam game: Steam API started as app 480" deck.log "init appid=480 gameid=480"
expect "manifest is absolute and loads both sets" deck.log "sets=2 actions=33"
expect "status names the launch" deck.out "launched: non-Steam game"
expect "the Deck is a Steam Input controller" deck.out "controller 0: Steam Input (steamdeck glyphs)"
expect "menu set first" deck.log "activate 7 Menu"
# (with no game loaded the console has the keys, so the checks read the
# engine's trace of each action's key rather than the bound commands)
expect "menu action pressed GP_DPAD_DOWN" deck.out "controller 0 key_gp_dpad_down down (GP_DPAD_DOWN)"
expect "menu action released" deck.out "controller 0 key_gp_dpad_down up (GP_DPAD_DOWN)"
expect "in-game set when asked" deck.log "activate 7 InGame"
expect "jump action pressed SI_JUMP" deck.out "controller 0 jump down (SI_JUMP)"
expect "attack action pressed SI_ATTACK" deck.out "controller 0 attack down (SI_ATTACK)"
refuse "a Menu action doesn't fire in game" deck.out "controller 0 key_gp_a down"
expect "default binding from bindings.txt" deck.out '"SI_SLOT1" = "slot1"'
expect "default binding for pause" deck.out '"SI_PAUSE" = "togglemenu"'
expect "prompt for +jump is the Deck's A" deck.out "+jump: {images}/glyphs/steamdeck/south.png \"A\""
expect "prompt for +attack is the Deck's R2" deck.out "+attack: {images}/glyphs/steamdeck/rt.png \"R2\""
expect "virtual gamepad seen as SDL pad" deck.out "Steam Virtual Gamepad pad 0"
expect "Steam's virtual gamepad doesn't double the input (prompts stay the Deck's)" deck.out "in_padglyphs\" is \"steamdeck"
expect "SDL pad marked unused" deck.out "unused while Steam Input has controllers"

# 2. The same, launched by Steam as a Steam app (SteamAppId set): Steam's
#    app id is kept.
printf 'connect 3 1\nwait 40\n' >"$WORK/app.txt"
run app 30 SteamAppId=480 SteamGameId=480 BW_STEAM_API_LIB="$WORK/libsteam_api.so" MOCK_STEAM_SCRIPT="$WORK/app.txt" -- \
	+wait +wait +wait +in_steaminput_status
expect "Steam app launch" app.out "launched: Steam app"
expect "Steam's app id used" app.log "init appid=480"
expect "original Steam Controller glyphs" app.out "controller 0: Steam Input (steamcontroller glyphs)"

# 3. Steam isn't running: plain SDL. A Steam Deck read directly (SDL's
#    HIDAPI driver reports 28de:1205) gets the Deck's prompts and its back
#    buttons are keys.
run nosteam 20 MOCK_STEAM_RUNNING=0 BW_STEAM_API_LIB="$WORK/libsteam_api.so" -- \
	+joy_virtual_dev 0x28de 0x1205 "Steam Deck" +wait +wait +exec glyphs.cfg +bind GP_L4 "echo L4" +in_steaminput_status
expect "says Steam isn't running" nosteam.out "Steam isn't running"
expect "Deck seen through SDL" nosteam.out "(steamdeck glyphs)"
expect "SDL prompt for +jump on a Deck" nosteam.out "+jump: {images}/glyphs/steamdeck/south.png"
refuse "no Steam API started" nosteam.log "init appid"

# 4. A non-Steam game without libsteam_api (or with Steam Input off): Steam
#    still hands the game its virtual gamepad, and SteamVirtualGamepadInfo
#    says what is really behind each one.
cat >"$WORK/vgi.txt" <<'EOF'
[slot 0]
name=Steam Deck
VID=0x28de
PID=0x1205
type=steamdeck
handle=1234
[slot 1]
name=DualSense Wireless Controller
VID=0x054c
PID=0x0ce6
type=ps5
EOF
run vgp 20 SteamGameId=$SHORTCUT_ID SteamVirtualGamepadInfo="$WORK/vgi.txt" BW_STEAM_API_LIB="$WORK/none.so" -- \
	+joy_virtual_dev 0x28de 0x11ff "Steam Virtual Gamepad pad 0" +joy_virtual_dev 0x28de 0x11ff "Steam Virtual Gamepad pad 1" \
	+wait +wait +exec glyphs2.cfg +in_glyph GP_B 2 +in_steaminput_status
expect "no library: plain controllers" vgp.out "no libsteam_api"
expect "virtual pad 0 is the Deck" vgp.out "steamdeck glyphs, through Steam's virtual gamepad 0"
expect "virtual pad 1 is a DualSense" vgp.out "ps5 glyphs, through Steam's virtual gamepad 1"
expect "Deck prompt" vgp.out "+jump: {images}/glyphs/steamdeck/south.png \"A\""
expect "DualSense prompt" vgp.out "+jump: {images}/glyphs/ps5/south.png \"Cross\""
expect "DualSense east" vgp.out "GP_B: {images}/glyphs/ps5/east.png \"Circle\""
expect "launch seen as non-Steam game" vgp.out "launched: non-Steam game"

# 5. Plain SDL, no Steam at all: an Xbox pad and a Switch Pro pad.
run plain 20 BW_STEAM_API_LIB="$WORK/none.so" -- \
	+joy_virtual_dev 0x045e 0x0b12 "Xbox Series" +joy_virtual_dev 0x057e 0x2009 "Switch Pro" \
	+wait +wait +exec glyphs2.cfg +webcore_query glyph:GP_A +in_steaminput_status
expect "Xbox prompt" plain.out "+jump: {images}/glyphs/xbox/south.png \"A\""
expect "Switch's south button says B" plain.out "+jump: {images}/glyphs/switch/south.png \"B\""
expect "launch is plain" plain.out "launched: not from Steam"
expect "Panorama's fte_query(\"glyph:GP_A\")" plain.out "{images}/glyphs/xbox/south.png|A"

# 6. in_steaminput 0 turns it off.
run off 20 SteamGameId=$SHORTCUT_ID BW_STEAM_API_LIB="$WORK/libsteam_api.so" -- \
	+in_steaminput 0 +in_steaminput_restart +in_steaminput_status
expect "off by cvar" off.out "Steam Input: off"

# every prompt file the cases named exists
for f in $(grep -ho '{images}/glyphs/[a-z0-9]*/[a-z0-9]*\.png' "$WORK"/*.out | sort -u); do
	p="$REPO/data/panorama/images/glyphs/${f#\{images\}/glyphs/}"
	if [ -f "$p" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL missing $p"; fi
done

echo "steaminput: $pass passed, $fail failed"
[ "$fail" = 0 ]
