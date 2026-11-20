#!/usr/bin/env bash
# TouchEQ 8 for MPC: test, build and package with Akai's MPC Plugin SDK.
#
#   ./build.sh          DSP tests, plugin (native + arm32 + arm64), skin, validation, host_sim, zip
#   ./build.sh test     DSP tests only
#
# Needs (Linux or WSL): g++, python3 with Pillow, zig (the SDK installs it on first use, or point ZIG=/path/to/zig;
# PYLIB=/dir/with/PIL is added to PYTHONPATH). The SDK lives in .deps/mpc-plugin-builder-sdk (cloned on first run).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
SDK="$HERE/.deps/mpc-plugin-builder-sdk"
SDK_COMMIT=a5949cf6fe2cbfb95bcbe0ddf1f7ef555e71f2be   # inmusic-software/mpc-plugin-builder-sdk, the version this was built against
ZIG="${ZIG:-$(command -v zig || true)}"
[ -n "${PYLIB:-}" ] && export PYTHONPATH="$PYLIB${PYTHONPATH:+:$PYTHONPATH}"
ZIGARG=(); [ -x "$ZIG" ] && ZIGARG=(--zig "$ZIG")

if [ ! -d "$SDK/framework" ]; then
  echo "== fetching the MPC Plugin SDK @ ${SDK_COMMIT:0:12}"
  git clone -q https://github.com/inmusic-software/mpc-plugin-builder-sdk.git "$SDK"
  git -C "$SDK" checkout -q "$SDK_COMMIT"
fi

echo "== DSP tests (ASan/UBSan, then optimised)"
mkdir -p build
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wshadow -Werror \
    -Icommon tests/dsp_test.cpp -o build/dsp_test_asan
./build/dsp_test_asan > build/dsp_test_asan.log || { cat build/dsp_test_asan.log; exit 1; }
g++ -std=c++17 -O2 -Wall -Wextra -Wshadow -Werror -Icommon tests/dsp_test.cpp -o build/dsp_test
./build/dsp_test
[ "${1:-}" = test ] && exit 0

PROJ="$HERE/plugins/TouchEQ8"
SLUG=toucheq-8
echo "== native build (parameter list for the skin)"
rm -rf "$PROJ/skins"
(cd "$SDK" && python3 tools/build.py "$PROJ" --target native "${ZIGARG[@]}" >/dev/null)
echo "== skin: graph, knobs, points, TUI.json, Q-Links.json"
python3 tools/make_skin.py "$PROJ/build/params.json" "$PROJ/skins" --preview docs/skin-preview
echo "== native + arm32 + arm64, validate"
(cd "$SDK" && python3 tools/build.py "$PROJ" "${ZIGARG[@]}" | grep -E "errors|warnings|Build finished|WARN|ERROR")
echo "== host_sim (the MPC host's load sequence and quirks)"
"$SDK/.sdk-tools/host_sim" "$PROJ/build/native/$SLUG.so" test --wav "$PROJ/build/test.wav" | grep -E "^(FAIL|WARN|Result)"
echo "== arm32 imports that pass structs to libc (the DualBank fstat lesson)"
ARM32=$(find "$PROJ/build" -name "$SLUG-arm32.so" | head -1)
if readelf --dyn-syms "$ARM32" | grep -E ' (fstat|stat|lstat|__fxstat|__xstat)' ; then echo "WARNING: struct-passing libc call in arm32 build"; else echo "none"; fi

echo "== end-to-end: the real plugin library publishes the curve and analyser the screen draws"
g++ -std=c++17 -O2 -Wall -I"$SDK/framework" -Icommon tests/plugin_e2e.cpp -ldl -lpthread -o build/plugin_e2e
./build/plugin_e2e "$PROJ/build/native/$SLUG.so"

echo "== package"
rm -rf dist && mkdir -p dist
(cd "$SDK" && python3 tools/package.py "$PROJ" --out "$HERE/dist" >/dev/null)
ls -la dist
