#!/bin/sh
# Builds FFmpeg 7.1.1 (LGPL 2.1+) for the desktop game module (native/guest)
# with wasi-sdk: only the Bink demuxer and decoders, like the browser's
# bink.wasm (src/install/build-bink.sh). Output: build/ffmpeg-wasi/out.
set -e
cd "$(dirname "$0")/../.."
root=$(pwd)
V=7.1.1
WASI_SDK=${WASI_SDK:-$HOME/projects/wasi-sdk-34.0-arm64-macos}
dir=build/ffmpeg-wasi
[ -f $dir/out/lib/libavcodec.a ] && exit 0
mkdir -p $dir
[ -d $dir/ffmpeg-$V ] || curl -sL https://ffmpeg.org/releases/ffmpeg-$V.tar.xz | tar xJ -C $dir
cd $dir/ffmpeg-$V
./configure --prefix="$root/$dir/out" \
  --target-os=none --arch=x86_32 --enable-cross-compile \
  --cc="$WASI_SDK/bin/clang" --ar="$WASI_SDK/bin/llvm-ar" --ranlib="$WASI_SDK/bin/llvm-ranlib" --nm="$WASI_SDK/bin/llvm-nm" \
  --extra-cflags="--target=wasm32-wasip1 -O2 -D_WASI_EMULATED_SIGNAL" --extra-ldflags="--target=wasm32-wasip1" \
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-asm --disable-pthreads --disable-x86asm --disable-inline-asm \
  --disable-avdevice --disable-swscale --disable-swresample --disable-avfilter --disable-postproc \
  --enable-decoder=bink,binkaudio_rdft,binkaudio_dct --enable-demuxer=bink --enable-protocol=file >/dev/null
make -j8 >/dev/null
make install >/dev/null
ls -la "$root/$dir/out/lib"
