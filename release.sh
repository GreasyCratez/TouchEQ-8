#!/usr/bin/env bash
# TouchEQ 8: make the GitHub release files for the open-mpc catalog (https://open-mpc.github.io/workflow.html).
#
#   ./release.sh <owner/repo>          e.g. ./release.sh greasycratez/toucheq-mpc
#
# Run ./build.sh first (it builds and tests the Akai SDK folder). This then uses open-mpc's own tools, pinned below:
#   tools/sdk_release.py   SDK folder -> TouchEQ-8-<v>-mpc-armv7.zip (Gen1: Key 61, Live, One, X, Force)
#                                       + TouchEQ-8-<v>-mpc-aarch64.zip (Gen2: 64-bit models)
#   tools/catalog_check.py --catalog   on both (the check the catalog runs; must say OK)
#   tools/sdk_zip.py       TouchEQ-8-<v>-mpc-sdk.zip: the one folder to copy into "VST2 Plugins" by hand
# and writes release/SHA256SUMS.txt and release/catalog_check.txt for the catalog pull request.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
REPO="${1:?usage: ./release.sh <owner/repo>}"
OPENMPC="$HERE/.deps/mpc-vst-plugins"
OPENMPC_COMMIT=1af05b911e8580c8c2e473a1cc10119132e06eca   # open-mpc/mpc-vst-plugins, the version these steps were checked with
ID=toucheq-8
FOLDER="plugins/TouchEQ8/build/GreasyCratez - VST - TouchEQ 8"
VERSION=$(python3 -c 'import json;print(json.load(open("plugins/TouchEQ8/plugin.json"))["version"])')

[ -f "$FOLDER/plugin.json" ] || { echo "no build: run ./build.sh first"; exit 1; }
if [ ! -d "$OPENMPC/tools" ]; then
  echo "== fetching open-mpc tools @ ${OPENMPC_COMMIT:0:12}"
  git clone -q --filter=blob:limit=2m https://github.com/open-mpc/mpc-vst-plugins.git "$OPENMPC"
  git -C "$OPENMPC" checkout -q "$OPENMPC_COMMIT"
fi

rm -rf release && mkdir -p release
echo "== open-mpc zips for $ID $VERSION ($REPO)"
python3 "$OPENMPC/tools/sdk_release.py" "$FOLDER" --version "$VERSION" --id "$ID" --repo "$REPO" --license MIT -o release
A32="release/TouchEQ-8-$VERSION-mpc-armv7.zip"
A64="release/TouchEQ-8-$VERSION-mpc-aarch64.zip"
echo "== catalog check"
for z in "$A32" "$A64"; do
  python3 "$OPENMPC/tools/catalog_check.py" "$z" --catalog --expect-id "$ID" --expect-repo "$REPO" | tee -a release/catalog_check.txt
done
echo "== hand-copy SDK zip"
python3 "$OPENMPC/tools/sdk_zip.py" "release/TouchEQ-8-$VERSION-mpc-sdk.zip" "$A32" "$A64"
(cd release && sha256sum *.zip | tee SHA256SUMS.txt)
