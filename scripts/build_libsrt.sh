#!/usr/bin/env bash
#
# build_libsrt.sh - libsrt 1.5.x'i S912 / Android 7.1.2 (arm64-v8a) için derler
#
# Kullanım:
#   1) scripts/indirs/ klasorune libsrt-1.5.3.tar.gz indir
#      (git clone https://github.com/Haivision/srt || tag v1.5.3)
#   2) ./build_libsrt.sh
#
# Gerekli ortam (Android Studio ayarladıysan genelde otomatik):
#   ANDROID_NDK  -> NDK dizini (r23+ veya r25+ / 23.x / 25.x)
# NDK r25+ ise API 21+ icin cicc yerine clang kullanilir, bu sorun degil.
# min API 21 tutulur cunku S912 kutulari standart su 7.1 (API 25).
#
set -euo pipefail

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/25.1.8937325}"
if [ ! -d "$NDK" ]; then
  echo "HATA: NDK bulunamadi. ANDROID_NDK degiskenini kendi NDK yoluna ayarla."
  echo "Ornek: export ANDROID_NDK=\$HOME/Android/Sdk/ndk/25.1.8937325"
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$ROOT/scripts/src/libsrt"          # libsrt-1.5.3 kaynak dizini
OUT="$ROOT/prebuilt"                    # cikiş: prebuilt/{libsrt,ffmpeg,include,lib}

API=21
ARCH="aarch64-linux-android"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$(uname -s | tr 'A-Z' 'a-z')-x86_64"
CC="$TOOLCHAIN/bin/${ARCH}${API}-clang"
CXX="$TOOLCHAIN/bin/${ARCH}${API}-clang++"

if [ ! -d "$SRC" ]; then
  echo "HATA: $SRC bulunamadi."
  echo "libsrt-1.5.3 kaynagini oraya koy (git clone -b v1.5.3 https://github.com/Haivision/srt $SRC)"
  exit 1
fi

rm -rf "$OUT/build"
mkdir -p "$OUT/build/libsrt" "$OUT/lib" "$OUT/include"

cd "$OUT/build/libsrt"
cmake "$SRC" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-$API \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$OUT" \
  -DSRT_ENABLE_ENCRYPTION=ON \
  -DSRT_ENABLE_UHDMABUF=OFF \
  -DSRT_TRANSMIT_DATA_EXT=ON \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_SHARED_LIBRARY_RPATH_LINK_FLAG="" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_INSTALL_LIBDIR=lib
make -j"$(nproc)"
make install

echo ""
echo "OK -> libsrt:  $OUT/lib/libsrt$(command -v go >/dev/null 2>&1 && echo .so || echo .so)"
ls -l "$OUT/lib" | grep -i srt || true
echo "Libs srt kuruldu. Sonraki adim: ./build_ffmpeg.sh"
