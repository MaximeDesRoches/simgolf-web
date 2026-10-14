#!/bin/sh
# Builds the macOS desktop app and its .dmg (native/README.md):
#   1. the game module, native/guest -> build/guest/golf-host.wasm (needs the
#      translated game in build/gen and wasi-sdk), unless SIMGOLF_WASM is set
#   2. the app, native/host -> build/native-release/SimGolf.app (universal,
#      SDL2 linked in, ad-hoc signed)
#   3. build/native-release/SimGolf-<version>-mac.dmg
# Leaves build/web and build/pages alone.
set -e
cd "$(dirname "$0")/../.."
VERSION=${SIMGOLF_VERSION:-0.1.0}
WASI_SDK=${WASI_SDK:-$HOME/projects/wasi-sdk-34.0-arm64-macos}
OUT=build/native-release

if [ -z "$SIMGOLF_WASM" ]; then
  tools/native/build-ffmpeg-wasi.sh
  [ -f build/guest/build.ninja ] || cmake -G Ninja -B build/guest -S native/guest \
    -DCMAKE_TOOLCHAIN_FILE="$WASI_SDK/share/cmake/wasi-sdk-p1.cmake" -DWASI_SDK_PREFIX="$WASI_SDK" \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build/guest 2>&1 | grep -E "error" || true
  SIMGOLF_WASM=build/guest/golf-host.wasm
fi

cmake -G Ninja -B "$OUT" -S native/host -DSIMGOLF_WASM="$SIMGOLF_WASM" -DCMAKE_BUILD_TYPE=Release \
  -DSIMGOLF_BUNDLED_SDL=ON -DSIMGOLF_VERSION="$VERSION" \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 >/dev/null
cmake --build "$OUT" --target SimGolf
codesign --force --deep -s - "$OUT/SimGolf.app"

stage="$OUT/dmg"
rm -rf "$stage"; mkdir -p "$stage"
cp -R "$OUT/SimGolf.app" "$stage/"
ln -s /Applications "$stage/Applications"
cp native/README-players.txt "$stage/Read me first.txt"
dmg="$OUT/SimGolf-$VERSION-mac.dmg"
rm -f "$dmg"
hdiutil create -volname "SimGolf" -srcfolder "$stage" -ov -format UDZO "$dmg" >/dev/null
rm -rf "$stage"
ls -la "$dmg"
