#!/bin/sh
# uri_get/uri_post acceptance test: POST bodies, methods, error statuses,
# the stdb_token header only for stdb_url, and callbacks reaching the
# addprogs module that asked. Usage: run.sh path/to/bulwark-ded
# Set FTEQCC to the compiler; skipped if unset. Needs python3 for the mock.
set -e
DED="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$FTEQCC" ]; then
	echo "FTEQCC not set, skipping uri tests"
	exit 0
fi
OUT="$(dirname "$DED")/uritest"
mkdir -p "$OUT/game/maps"
(cd "$HERE" && "$FTEQCC" -Tfte progs.src > "$OUT/qcc.log" 2>&1 && "$FTEQCC" -Tfte add.src >> "$OUT/qcc.log" 2>&1) ||
	{ cat "$OUT/qcc.log"; exit 1; }
mv "$HERE/progs.dat" "$HERE/uriadd.dat" "$OUT/game/"
rm -f "$HERE"/*.lno
cp "$HERE/../data/maps/botmap.bsp" "$OUT/game/maps/"
cp "$HERE/../data/hltest.wad" "$OUT/game/"

PORT=$((20000 + $$ % 20000))
python3 "$HERE/mock.py" $PORT 2> "$OUT/mock.log" &
MOCK=$!
trap 'kill $MOCK 2>/dev/null' EXIT
sleep 1
# 127.0.0.1 is the API, localhost (same server, other spelling) is "another host"
"$DED" -basedir "$OUT" -game game -frames 3000 +set stdb_url "http://127.0.0.1:$PORT" +set stdb_token secret123 \
	+set uritest_base "http://127.0.0.1:$PORT" +set uritest_other "http://localhost:$PORT" +map botmap > "$OUT/run.log" 2>&1 || true
grep '^URI\|^DONE' "$OUT/run.log" || true

status=0
check() {
	if grep -q "$2" "$OUT/run.log"; then echo "uri $1: PASS"; else echo "uri $1: FAIL"; status=1; fi
}
check "post with token" '^URI main id 1 code 200 .*"method": "POST".*"auth": "Bearer secret123".*"ctype": "application/json", "body": "\[\\"sokky\\"\]"'
check "get with token" '^URI main id 2 code 200 .*"method": "GET".*"auth": "Bearer secret123"'
check "no token elsewhere" '^URI main id 3 code 200 .*"auth": null'
check "addprogs callback" '^URI add id 4 code 200 .*"path": "/v1/database/csnz/sql".*"ctype": "text/plain", "body": "SELECT 1"'
check "error status" '^URI main id 5 code 400 .*"path": "/err"'
check "method" '^URI main id 6 code 200 .*"method": "PUT".*"body": "{}"'
if grep -q '^URI main id 4' "$OUT/run.log"; then echo "uri addprogs answer went to main: FAIL"; status=1; fi
exit $status
