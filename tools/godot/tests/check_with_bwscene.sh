#!/bin/sh
# Runs the Godot tests, then checks that Bulwark spawns the same entities
# from the scene the addon wrote as from the original.
#   tools/godot/tests/check_with_bwscene.sh <godot> <bwscene>
set -e
GODOT=${1:-godot}
BWSCENE=${2:-build/bwscene}
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
"$GODOT" --headless --path "$REPO/tools/godot" --script res://tests/run_tests.gd -- "$REPO" "$OUT"
"$BWSCENE" -basedir "$REPO/tests/scene" -game data ents "$REPO/tests/scene/data/maps/test.scene" > "$OUT/orig.ents" 2>/dev/null
"$BWSCENE" -basedir "$REPO/tests/scene" -game data ents "$OUT/roundtrip.scene" > "$OUT/godot.ents" 2>/dev/null
"$BWSCENE" check "$OUT/built.scene" > /dev/null
python3 - "$OUT/orig.ents" "$OUT/godot.ents" <<'PY'
import re, sys
def ents(path):
    out, cur = [], None
    for line in open(path):
        line = line.strip()
        if line == "{": cur = {}
        elif line == "}": out.append(cur)
        else:
            m = re.match(r'"([^"]*)" "([^"]*)"', line)
            if m: cur[m.group(1)] = m.group(2)
    return out
def same(a, b):
    if a == b: return True
    try:
        fa, fb = [float(x) for x in a.split()], [float(x) for x in b.split()]
        return len(fa) == len(fb) and all(abs(x - y) < 0.01 for x, y in zip(fa, fb))
    except ValueError:
        return False
a, b = ents(sys.argv[1]), ents(sys.argv[2])
bad = len(a) != len(b)
for i, (x, y) in enumerate(zip(a, b)):
    for k in sorted(set(x) | set(y)):
        if not same(x.get(k, ""), y.get(k, "")):
            print("entity %d (%s) key %s: %r vs %r" % (i, x.get("classname"), k, x.get(k), y.get(k)))
            bad = True
print("bwscene: %d entities, %s" % (len(a), "different" if bad else "identical"))
sys.exit(1 if bad else 0)
PY
