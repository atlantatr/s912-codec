#
# CMakeLists.txt - S912 / Android 7.1.2 arm64 JNI codec layer
#
# Prebuilt (scripts/build_*.sh):
#   prebuilt/ffmpeg/{include,lib}   FFmpeg 6.1 static libs
#   prebuilt/{include,lib}          libsrt 1.5.3 static lib + headers
#
cmake_minimum_required(VERSION 3.22)
project(s912codec CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# app/src/main/cpp -> app/src/main -> app/src -> app -> repo root
get_filename_component(PREBUILT_ROOT "${CMAKE_SOURCE_DIR}/../../../../prebuilt" ABSOLUTE)

if(NOT EXISTS "${PREBUILT_ROOT}/ffmpeg/lib/libavformat.a")
    message(FATAL_ERROR "FFmpeg prebuilt bulunamadi: ${PREBUILT_ROOT}/ffmpeg/lib. "
                        "Once scripts/build_ffmpeg.sh calistirin.")
endif()
if(NOT EXISTS "${PREBUILT_ROOT}/lib/libsrt.a")
    message(FATAL_ERROR "libsrt prebuilt bulunamadi: ${PREBUILT_ROOT}/lib/libsrt.a. "
                        "Once scripts/build_libsrt.sh calistirin.")
endif()

add_library(codec_core SHARED
    native/codec_core.cpp
    native/dvb_frontend.cpp
)

target_include_directories(codec_core PRIVATE
    ${PREBUILT_ROOT}/ffmpeg/include
    ${PREBUILT_ROOT}/include
)

find_library(log-lib log)
find_library(z-lib z)
find_library(m-lib m)

# Static FFmpeg + libsrt. --start-group cozer: avformat -> avcodec -> srt -> avutil
# arasindaki dairesel bagimliliklari.
target_link_libraries(codec_core
    -Wl,--start-group
    ${PREBUILT_ROOT}/ffmpeg/lib/libavformat.a
    ${PREBUILT_ROOT}/ffmpeg/lib/libavcodec.a
    ${PREBUILT_ROOT}/ffmpeg/lib/libswresample.a
    ${PREBUILT_ROOT}/ffmpeg/lib/libavutil.a
    ${PREBUILT_ROOT}/lib/libsrt.a
    -Wl,--end-group
    ${log-lib}
    ${z-lib}
    ${m-lib}
    dl
)

target_compile_options(codec_core PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)
