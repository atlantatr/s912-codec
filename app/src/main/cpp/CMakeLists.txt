#
# CMakeLists.txt - S912 / Android 7.1.2 arm64 JNI codec katini
#
# Diksiyon:
#   - FFmpeg 6.x static libleri:  prebuilt/ffmpeg/lib  (build_ffmpeg.sh)
#   - libsrt shared lib:          prebuilt/lib         (build_libsrt.sh)
#   - app JNI:  bu dosyadan
#
# NDK tarafinda gradle icinde kullanim (build.gradle - externalNativeBuild).
# API min 21, target 25 (S912 7.1.2).
#
cmake_minimum_required(VERSION 3.22)
project(s912codec)

set(PREBUILT ${CMAKE_SOURCE_DIR}/../../../../prebuilt)
# gradle/cmake cagirisinda PREBUILT, proje kosesinden hesapla
set(PREBUILT_ROOT ${CMAKE_SOURCE_DIR}/../../../../../prebuilt)

add_library(codec_core SHARED
    native/codec_core.cpp
    native/dvb_frontend.cpp
)

# Prebuilt FFmpeg + libsrt
target_include_directories(codec_core PRIVATE
    ${PREBUILT_ROOT}/ffmpeg/include
    ${PREBUILT_ROOT}/include
)
target_link_directories(codec_core PRIVATE
    ${PREBUILT_ROOT}/ffmpeg/lib
    ${PREBUILT_ROOT}/lib
)

# Static FFmpeg libleri (build_ffmpeg.sh --enable-static sonucunda)
find_library(log-lib log)
find_library(z-lib z)

target_link_libraries(codec_core
    -Wl,--whole-archive
    ${PREBUILT_ROOT}/ffmpeg/lib/libavcodec.a
    ${PREBUILT_ROOT}/ffmpeg/lib/libavformat.a
    ${PREBUILT_ROOT}/ffmpeg/lib/libavutil.a
    ${PREBUILT_ROOT}/ffmpeg/lib/libswresample.a
    ${PREBUILT_ROOT}/ffmpeg/lib/libswscale.a
    ${PREBUILT_ROOT}/ffmpeg/lib/libavdevice.a
    -Wl,--no-whole-archive
    ${PREBUILT_ROOT}/lib/libsrt.so
    ${log-lib}
    ${z-lib}
    dl
)
