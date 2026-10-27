#!/usr/bin/env bash
#
# build_ffmpeg.sh - FFmpeg 6.x'i S912 / Android 7.1.2 (arm64-v8a) icin derler.
#
# Hedef: eski com.android.audiopatch setindaki codec/muxer/demuxer kutu setini
# birebir koruyarak, 10 yil eski 32-bit derlemeye karsi S912 arm64 + FFmpeg 6.x
# uzerinden yeniden uretmek.
#
# Kullanim:
#   1) scripts/src/ffmpeg-6.1 (git clone -b n6.1 https://git.ffmpeg.org/ffmpeg.git)
#   2) once ./build_libsrt.sh calistir (libsrt srt demuxer icin gerekli)
#   3) ./build_ffmpeg.sh
#
set -euo pipefail

NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/25.1.8937325}"
if [ ! -d "$NDK" ]; then
  echo "HATA: NDK bulunamadi. ANDROID_NDK degiskenini kur (bkz. build_libsrt.sh)."
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$ROOT/scripts/src/ffmpeg-6.1"
PREBUILT="$ROOT/prebuilt"
API=21
ARCH="aarch64-linux-android"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$(uname -s | tr 'A-Z' 'a-z')-x86_64"
CC="$TOOLCHAIN/bin/${ARCH}${API}-clang"
CXX="$TOOLCHAIN/bin/${ARCH}${API}-clang++"

if [ ! -d "$SRC" ]; then
  echo "HATA: $SRC bulunamadi. ffmpeg 6.1 kaynagini oraya koy."
  exit 1
fi

# SRT destegi icin libsrt konumu (build_libsrt.sh ciktisi)
SRT_CFLAGS="-I$PREBUILT/include"
SRT_LIBS="-L$PREBUILT/lib -lsrt"

# ---- Codec/canli demuxer/muxer seti ----
# Eski audoipatch setinin tam karseti:
#   decoders : ac3, eac3, dca(dts), dts, truehd, flac, wma, opus, aac, mp3
#   demuxers : ac3, dts, flac, wma, srt, mpegts, matroska, mov, mp4
#   muxers   : mkv, mp4, srt, adts, ogv, mp3
#   audio out: pcm (sdaudio/pcm) - AudioTrack icin
FFMPEG_CFG=(
  --prefix="$PREBUILT/ffmpeg"
  --cross-prefix="$TOOLCHAIN/bin/${ARCH}${API}-"
  --target-os=android
  --arch=aarch64
  --cc="$CC"
  --cxx="$CXX"
  --enable-static
  --disable-shared
  --disable-doc
  --disable-network
  --enable-gpl
  # Audio decode (birebir eski set)
  --enable-decoder=ac3 --enable-decoder=eac3
  --enable-decoder=dca --enable-decoder=dts --enable-decoder=truehd
  --enable-decoder=flac --enable-decoder=wma --enable-decoder=wmapro
  --enable-decoder=opus --enable-decoder=aac --enable-decoder=mp3
  # Audio encode (passthrough out, AudioTrack PCM + bitstream mux)
  --enable-encoder=pcm_s16le --enable-encoder=aac --enable-encoder=libopus
  # Demuxers (eski set + srt)
  --enable-demuxer=ac3 --enable-demuxer=dts --enable-demuxer=flac
  --enable-demuxer=wma --enable-demuxer=matroska --enable-demuxer=mov
  --enable-demuxer=mpegts --enable-demuxer=mp3 --enable-demuxer=srt
  # Muxers
  --enable-muxer=matroska --enable-muxer=mp4 --enable-muxer=adts
  --enable-muxer=ogg --enable-muxer=mp3 --enable-muxer=srt
  # SRT protocol (libsrt)
  --enable-protocol=srt
  --enable-libx264 --enable-libvpx  # video mux (isemleme/gerektirse)
  --extra-cflags="-I$PREBUILT/include -D__ANDROID__"
  --extra-cflags="$SRT_CFLAGS"
  --extra-ldflags="-L$PREBUILT/lib -llog -lz $SRT_LIBS"
)

# libx264/libvpx icinde NDK icin derlenmemis olabilir; siklikla istenmez.
# Gerekirse satirlari sil. Baslangic icin yorumlandi:
# (libx264/libvpx opsiyonel, bu projede video decode HW MediaCodec ile oldugu
#  icin libx264/vpx zorunlu degil - yalnizca SRT encoder icin opsiyonel)

cd "$SRC"
make distclean >/dev/null 2>&1 || true
./configure "${FFMPEG_CFG[@]}"
make -j"$(nproc)"
make install

echo ""
echo "OK -> ffmpeg:  $PREBUILT/ffmpeg"
ls -l "$PREBUILT/ffmpeg/lib" 2>/dev/null | grep -E 'libav(.*|ffmpeg)' || true
echo "FFmpeg kuruldu. Sonraki adim: NDK/CMake JNI iskeletini app/ icinde derle."
