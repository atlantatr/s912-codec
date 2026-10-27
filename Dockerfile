# Opsiyonel: NDK'li APK build imaji (CI'dan bağımsız, tek-container build)
# GitHub'da asıl yol .github/workflows/build-apk.yml'dir.
#
# Yerel kullanim:
#   docker build -t s912codec .
#   docker run --rm -v "$PWD:/work" -w /work s912codec
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        openjdk-17-jdk wget unzip git cmake ninja-build \
        build-essential ca-certificates git-lfs \
    && rm -rf /var/lib/apt/lists/*

# NDK + Gradle
ENV NDK_VERSION=25.1.8937325
ENV GRADLE_VERSION=8.5
ENV ANDROID_NDK=/opt/android/ndk/${NDK_VERSION}
RUN wget -q "https://dl.google.com/android/repository/android-ndk-${NDK_VERSION}-linux.zip" -O /tmp/n.zip \
    && unzip -q /tmp/n.zip -d /opt/android \
    && mv /opt/android/android-ndk-${NDK_VERSION} "${ANDROID_NDK}" \
    && rm /tmp/n.zip \
    && wget -q "https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip" -O /tmp/g.zip \
    && unzip -q /tmp/g.zip -d /opt \
    && ln -s /opt/gradle-${GRADLE_VERSION}/bin/gradle /usr/local/bin/gradle \
    && rm /tmp/g.zip

WORKDIR /work
COPY . /work
RUN echo "ndk.dir=${ANDROID_NDK}" > local.properties

# Kaynak clone + prebuilt + APK
CMD ["/bin/bash","-c", \
 "git clone --depth 1 --branch v1.5.3 https://github.com/Haivision/srt scripts/src/libsrt && \
  git clone --depth 1 --branch n6.1 https://git.ffmpeg.org/ffmpeg.git scripts/src/ffmpeg-6.1 && \
  export ANDROID_NDK=${ANDROID_NDK} && \
  chmod +x scripts/*.sh && \
  ./scripts/build_libsrt.sh && \
  ./scripts/build_ffmpeg.sh && \
  mkdir -p app/jniLibs/arm64-v8a && cp prebuilt/lib/libsrt.so app/jniLibs/arm64-v8a/ && \
  gradle :app:assembleRelease --no-daemon"]
