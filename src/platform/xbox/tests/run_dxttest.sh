#!/bin/sh
# Builds and runs the host tests of the Xbox texture compression:
# dxttest (the DXT codec) and, when NXDK_DIR is set (for nv_regs.h),
# gltextest (gl_xbox.c's texture storage).
#   src/platform/xbox/tests/run_dxttest.sh [folder of texture dumps]
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/bw_xboxtest.$$
cc=${CC:-cc}
$cc -O2 -Wall -Wextra -o "$out" "$here/dxttest.c" "$here/../dxt_xbox.c" -lm
status=0
"$out" "$@" || status=1
if [ -n "$NXDK_DIR" ]; then
	$cc -O1 -g -w -fsanitize=address,undefined -I"$here/stub" -I"$NXDK_DIR/lib/pbkit" -o "$out" \
		"$here/gltextest.c" "$here/../dxt_xbox.c" -lm
	"$out" "$@" || status=1
else
	echo "gltextest skipped: set NXDK_DIR for nv_regs.h"
fi
rm -f "$out"
exit $status
