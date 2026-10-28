#!/bin/sh
# Font reload regression test (docs/spec/gdi-text.md, "Search-path changes").
# A font loaded before a game directory is mounted must be resolved again
# once the directory is mounted: the face index and the font slot both
# depend on the search path. Skipped if no test TrueType file is installed.
set -e
BULWARK="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
FACE=""
for f in /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf:"DejaVu Sans" \
	/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf:"Liberation Sans"; do
	if [ -f "${f%%:*}" ]; then FACE="${f%%:*}"; FAMILY="${f#*:}"; break; fi
done
if [ -z "$FACE" ]; then
	echo "no test font installed, skipping font tests"
	exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/base" "$TMP/mod/fonts"
cp "$FACE" "$TMP/mod/fonts/testface.ttf"

run() {
	"$BULWARK" -headless -basedir "$TMP" -game base -frames 40 \
		+set r_gdifont 1 +set r_gdifont_systemfonts 0 "$@" > "$TMP/log" 2>&1 || true
}
fail=0

# 1. Same font name before and after the mount: the slot must be reloaded.
run +loadfont "$FAMILY,fonts/nope.otf" 16 +gamedir mod +wait 5 \
	+loadfont "$FAMILY,fonts/nope.otf" 16 +listfonts
if grep -q "$FAMILY,fonts/nope.otf (gdifont)" "$TMP/log"; then
	echo "PASS reload after gamedir"
else
	echo "FAIL reload after gamedir"; cat "$TMP/log"; fail=1
fi

# 2. A different face looked up after the mount: the index must be rescanned.
run +loadfont "Nonexistent Face" 16 +gamedir mod +wait 5 \
	+loadfont "$FAMILY" 16 +listfonts
if grep -q " $FAMILY (gdifont)" "$TMP/log"; then
	echo "PASS index rescan after gamedir"
else
	echo "FAIL index rescan after gamedir"; cat "$TMP/log"; fail=1
fi
# 3. A font loaded with r_gdifont 0 is loaded again once it is 1, and
#    listfonts says which entry of the list was used.
run +set r_gdifont 0 +gamedir mod +wait 5 +loadfont "$FAMILY,fonts/testface.ttf" 16 \
	+set r_gdifont 1 +loadfont "$FAMILY,fonts/testface.ttf" 16 +listfonts
if grep -q "$FAMILY,fonts/testface.ttf (gdifont) using $FAMILY\$" "$TMP/log"; then
	echo "PASS reload after r_gdifont"
else
	echo "FAIL reload after r_gdifont"; cat "$TMP/log"; fail=1
fi
exit $fail
