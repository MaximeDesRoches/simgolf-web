#!/bin/sh
# Cross-builds the Windows desktop app with mingw-w64 (brew install
# mingw-w64) into build/native-win and zips it as
# build/native-win/SimGolf-<version>-windows.zip. Needs golf-host.wasm
# (tools/native/build-mac.sh builds it, or set SIMGOLF_WASM).
set -e
cd "$(dirname "$0")/../.."
VERSION=${SIMGOLF_VERSION:-0.1.0}
WASM=${SIMGOLF_WASM:-build/guest/golf-host.wasm}
OUT=build/native-win
cmake -G Ninja -B "$OUT" -S native/host -DSIMGOLF_WASM="$WASM" -DCMAKE_BUILD_TYPE=Release \
  -DSIMGOLF_BUNDLED_SDL=ON -DSIMGOLF_BUNDLED_ZLIB=ON -DSIMGOLF_VERSION="$VERSION" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/native/mingw-toolchain.cmake" >/dev/null
cmake --build "$OUT" --target SimGolf
stage="$OUT/SimGolf"
rm -rf "$stage"; mkdir -p "$stage"
cp "$OUT/SimGolf.exe" "$stage/"
cp -R "$OUT/resources" "$stage/"
cp native/README-players.txt "$stage/Read me first.txt"
zip="SimGolf-$VERSION-windows.zip"
(cd "$OUT" && rm -f "$zip" && zip -qr "$zip" SimGolf)
rm -rf "$stage"
ls -la "$OUT/$zip"
