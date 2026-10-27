#!/usr/bin/env bash
#
# build_libsrt.sh - libsrt 1.5.x'i S912 / Android 7.1.2 (arm64-v8a) icin derler.
#
# Kullanim:
#   git clone --depth 1 -b v1.5.3 https://github.com/Haivision/srt scripts/src/libsrt
#   export ANDROID_NDK=$HOME/Android/Sdk/ndk/25.1.8937393
#   ./scripts/build_libsrt.sh
#
# Cikti:
#   prebuilt/lib/libsrt.a
#   prebuilt/include/srt/*.h
#   prebuilt/lib/pkgconfig/srt.pc
#
set -euo pipefail

NDK="${ANDROID_NDK:-${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/25.1.8937393}}"
if [ ! -d "$NDK" ]; then
  echo "HATA: NDK bulunamadi. ANDROID_NDK degiskenini kendi NDK yoluna ayarla."
  echo "Ornek: export ANDROID_NDK=\$HOME/Android/Sdk/ndk/25.1.8937393"
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$ROOT/scripts/src/libsrt"
OUT="$ROOT/prebuilt"

API=21
ARCH="aarch64-linux-android"
case "$(uname -s)" in
  Linux*)   HOST_TAG="linux-x86_64" ;;
  Darwin*)  HOST_TAG="darwin-x86_64" ;;
  MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64" ;;
  *)        HOST_TAG="$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64" ;;
esac
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
CC="$TOOLCHAIN/bin/${ARCH}${API}-clang"
CXX="$TOOLCHAIN/bin/${ARCH}${API}-clang++"

if [ ! -d "$SRC" ]; then
  echo "HATA: $SRC bulunamadi."
  echo "git clone --depth 1 -b v1.5.3 https://github.com/Haivision/srt $SRC"
  exit 1
fi
if [ ! -x "$CC" ]; then
  echo "HATA: NDK clang bulunamadi: $CC"
  exit 1
fi

rm -rf "$OUT/build/libsrt"
mkdir -p "$OUT/build/libsrt" "$OUT/lib" "$OUT/include"

cd "$OUT/build/libsrt"
cmake "$SRC" \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-$API \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$OUT" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DENABLE_SHARED=OFF \
  -DENABLE_STATIC=ON \
  -DENABLE_APPS=OFF \
  -DENABLE_TESTING=OFF \
  -DENABLE_UNITTESTS=OFF \
  -DENABLE_ENCRYPTION=OFF \
  -DENABLE_CXX_DEPS=OFF \
  -DUSE_STATIC_LIBSTDCXX=OFF

cmake --build . --parallel "$(nproc 2>/dev/null || echo 4)"
cmake --install .

echo ""
echo "OK -> libsrt: $OUT/lib/libsrt.a"
ls -l "$OUT/lib" | grep -i srt || true
echo "Sonraki adim: ./scripts/build_ffmpeg.sh"
