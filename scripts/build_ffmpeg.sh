#!/usr/bin/env bash
#
# build_ffmpeg.sh - FFmpeg 6.1'i S912 / Android 7.1.2 (arm64-v8a) icin derler.
#
# Onkosul:
#   scripts/src/ffmpeg-6.1  (git clone -b n6.1 https://github.com/FFmpeg/FFmpeg.git)
#   ./scripts/build_libsrt.sh (prebuilt/lib/libsrt.a + srt.pc)
#
# Cikti:
#   prebuilt/ffmpeg/{include,lib}
#
set -euo pipefail

NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/25.1.8937325}}"
if [ ! -d "$NDK" ]; then
  echo "HATA: NDK bulunamadi. ANDROID_NDK degiskenini kur."
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$ROOT/scripts/src/ffmpeg-6.1"
PREBUILT="$ROOT/prebuilt"
API=21
ARCH="aarch64-linux-android"
case "$(uname -s)" in
  Linux*)   HOST_TAG="linux-x86_64"; EXE="" ;;
  Darwin*)  HOST_TAG="darwin-x86_64"; EXE="" ;;
  MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64"; EXE=".cmd" ;;
  *)        HOST_TAG="$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64"; EXE="" ;;
esac
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
CC="$TOOLCHAIN/bin/${ARCH}${API}-clang${EXE}"
CXX="$TOOLCHAIN/bin/${ARCH}${API}-clang++${EXE}"

if [ ! -d "$SRC" ]; then
  echo "HATA: $SRC bulunamadi."
  echo "git clone --depth 1 -b n6.1 https://github.com/FFmpeg/FFmpeg.git $SRC"
  exit 1
fi
if [ ! -f "$PREBUILT/lib/libsrt.a" ]; then
  echo "HATA: $PREBUILT/lib/libsrt.a yok. Once ./scripts/build_libsrt.sh calistir."
  exit 1
fi
if ! command -v pkg-config >/dev/null 2>&1; then
  echo "HATA: pkg-config gerekli (FFmpeg libsrt kontrolu icin)."
  exit 1
fi

export PKG_CONFIG_PATH="$PREBUILT/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_LIBDIR="$PREBUILT/lib/pkgconfig"

cd "$SRC"
make distclean >/dev/null 2>&1 || true

./configure \
  --prefix="$PREBUILT/ffmpeg" \
  --target-os=android \
  --arch=aarch64 \
  --cpu=armv8-a \
  --enable-cross-compile \
  --sysroot="$TOOLCHAIN/sysroot" \
  --cross-prefix="$TOOLCHAIN/bin/llvm-" \
  --cc="$CC" \
  --cxx="$CXX" \
  --ar="$TOOLCHAIN/bin/llvm-ar" \
  --nm="$TOOLCHAIN/bin/llvm-nm" \
  --ranlib="$TOOLCHAIN/bin/llvm-ranlib" \
  --strip="$TOOLCHAIN/bin/llvm-strip" \
  --pkg-config="$(command -v pkg-config)" \
  --enable-static \
  --disable-shared \
  --enable-pic \
  --disable-symver \
  --disable-programs \
  --disable-doc \
  --disable-debug \
  --disable-autodetect \
  --disable-avdevice \
  --disable-avfilter \
  --disable-postproc \
  --disable-swscale \
  --disable-bzlib \
  --disable-lzma \
  --disable-iconv \
  --enable-zlib \
  --enable-avcodec \
  --enable-avformat \
  --enable-avutil \
  --enable-swresample \
  --disable-everything \
  --enable-protocol=file \
  --enable-protocol=pipe \
  --enable-protocol=libsrt \
  --enable-libsrt \
  --enable-demuxer=ac3 \
  --enable-demuxer=eac3 \
  --enable-demuxer=dts \
  --enable-demuxer=truehd \
  --enable-demuxer=mlp \
  --enable-demuxer=flac \
  --enable-demuxer=asf \
  --enable-demuxer=matroska \
  --enable-demuxer=mov \
  --enable-demuxer=mpegts \
  --enable-demuxer=mp3 \
  --enable-demuxer=srt \
  --enable-decoder=ac3 \
  --enable-decoder=eac3 \
  --enable-decoder=dca \
  --enable-decoder=truehd \
  --enable-decoder=mlp \
  --enable-decoder=flac \
  --enable-decoder=wmav1 \
  --enable-decoder=wmav2 \
  --enable-decoder=wmapro \
  --enable-decoder=opus \
  --enable-decoder=aac \
  --enable-decoder=mp3 \
  --enable-decoder=pcm_s16le \
  --enable-decoder=pcm_s24le \
  --extra-cflags="-fPIC -D__ANDROID__ -I$PREBUILT/include" \
  --extra-ldflags="-L$PREBUILT/lib" \
  --extra-libs="-lc++_shared -lm"

make -j"$(nproc 2>/dev/null || echo 4)"
make install

echo ""
echo "OK -> FFmpeg: $PREBUILT/ffmpeg"
ls -l "$PREBUILT/ffmpeg/lib" 2>/dev/null | grep -E 'libav(codec|format|util)|libswresample' || true
