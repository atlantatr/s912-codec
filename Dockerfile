# S912 codec app - tek container'lik yerel build (opsiyonel)
#
# GitHub icin asil yol .github/workflows/build-apk.yml'dir; bu dosya sadece
# Docker'i olan makinelerde CI'siz derleme icin.
#
# Kullanim:
#   docker build -t s912codec .
#   docker run --rm -v "$PWD/out:/out" s912codec
#   # -> out/app-release.apk
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        openjdk-17-jdk \
        wget unzip curl git \
        cmake ninja-build build-essential pkg-config \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# --- Android NDK ---
ENV NDK_VERSION=25.1.8937393
ENV NDK_ZIP=android-ndk-r25b-linux.zip
ENV ANDROID_NDK=/opt/android/ndk/${NDK_VERSION}
RUN wget -q "https://dl.google.com/android/repository/${NDK_ZIP}" -O /tmp/ndk.zip \
    && unzip -q /tmp/ndk.zip -d /opt/android \
    && mv /opt/android/android-ndk-r25b "${ANDROID_NDK}" \
    && rm /tmp/ndk.zip

# --- Android SDK (cmdline-tools + platform + build-tools) ---
ENV ANDROID_SDK_ROOT=/opt/android/sdk
ENV ANDROID_HOME=/opt/android/sdk
RUN mkdir -p "${ANDROID_SDK_ROOT}/cmdline-tools" \
    && wget -q "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip" -O /tmp/cmdline.zip \
    && unzip -q /tmp/cmdline.zip -d /tmp/cmdline \
    && mv /tmp/cmdline/cmdline-tools "${ANDROID_SDK_ROOT}/cmdline-tools/latest" \
    && rm -rf /tmp/cmdline /tmp/cmdline.zip \
    && yes | "${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin/sdkmanager" --sdk_root="${ANDROID_SDK_ROOT}" --licenses >/dev/null \
    && "${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin/sdkmanager" --sdk_root="${ANDROID_SDK_ROOT}" \
        "platform-tools" "platforms;android-33" "build-tools;34.0.0" "cmake;3.22.1"

ENV PATH="${ANDROID_SDK_ROOT}/platform-tools:${PATH}"

WORKDIR /work
COPY . /work
RUN printf 'sdk.dir=%s\nndk.dir=%s\n' "${ANDROID_SDK_ROOT}" "${ANDROID_NDK}" > local.properties

# Kaynaklari klonla, statik bagimliliklari derle, APK uret.
CMD ["/bin/bash","-c", \
 "set -euo pipefail && \
  git clone --depth 1 --branch v1.5.3 https://github.com/Haivision/srt.git scripts/src/libsrt && \
  git clone --depth 1 --branch n6.1 https://github.com/FFmpeg/FFmpeg.git scripts/src/ffmpeg-6.1 && \
  chmod +x scripts/*.sh && \
  ./scripts/build_libsrt.sh && \
  ./scripts/build_ffmpeg.sh && \
  bash ./gradlew :app:assembleRelease --no-daemon && \
  mkdir -p /out && cp app/build/outputs/apk/release/app-release.apk /out/ && \
  ls -lh /out"]
