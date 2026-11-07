#!/bin/sh
# Point the games here at a Nuclide checkout, so their progs build.
#
#   games/link-nuclide.sh /path/to/nuclide
#
# The games include Nuclide's framework by relative path: coop and
# tftemplate use ../../../src, ../../../valve and ../../../base (from
# games/<game>/src/<progs>/), CSNZ uses games/csnz/src/nuclide. This makes
# those paths gitignored symlinks into the checkout; nothing from Nuclide is
# copied into this repo. Re-run it to switch checkouts.
set -e
here=$(cd "$(dirname "$0")" && pwd)
nuc=${1:?usage: $0 /path/to/nuclide}
nuc=$(cd "$nuc" && pwd)
[ -f "$nuc/src/rules.src" ] || { echo "$nuc is not a Nuclide checkout" >&2; exit 1; }
for d in src valve base; do
	ln -sfn "$nuc/$d" "$here/$d"
done
if [ -d "$here/csnz/src" ]; then
	ln -sfn "$nuc/src" "$here/csnz/src/nuclide"
fi
echo "games/ linked to $nuc"
