#!/bin/sh
# Build the web engine (bulwark.js + bulwark.wasm) into build-web/, or the
# folder given. Needs the Emscripten SDK on PATH (source emsdk_env.sh);
# docs/platforms/web.md has the setup.
#
#   tools/web/build.sh [build-dir] [extra cmake args...]
set -e
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-"$repo/build-web"}
[ $# -gt 0 ] && shift
if ! command -v emcmake >/dev/null 2>&1; then
	echo "build.sh: emcmake isn't on PATH: install the Emscripten SDK and source its emsdk_env.sh" >&2
	exit 1
fi
gen=
command -v ninja >/dev/null 2>&1 && gen="-G Ninja"
emcmake cmake -S "$repo" -B "$out" $gen -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$out" --target bulwark -j"$(nproc 2>/dev/null || echo 4)"
echo "build.sh: $out/bulwark.js and bulwark.wasm"
