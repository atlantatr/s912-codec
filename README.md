# S912 / Android 7.1.2 (arm64) — SRT + AC3/EAC3/DTS/TrueHD codec uygulaması

S912 kutularda (Android 7.1.2, arm64-v8a) çalışan, **FFmpeg 6.1 + libsrt 1.5.3**
tabanlı bir medya oynatıcı iskeleti.

Eski `com.android.audiopatch` / `libamplayer.so` setinin yerini alacak şekilde
tasarlanmıştır:

| Eski set | Bu proje |
|---|---|
| Android 5.1 ABI'ına bağlı `libbinder` / `libstagefright` | Framework'e bağımlılık yok; MediaCodec + AudioTrack + FFmpeg |
| 32-bit ARMv7 ELF (S912 arm64'e uymaz) | NDK `arm64-v8a` |
| 10 yıllık FFmpeg | FFmpeg 6.1 |
| `android.uid.system` + platform imzası | Normal 3. parti imza, root gerekmez |

## Özellikler

- **SRT**: libsrt 1.5.3 (caller / listener), `srt://` URL'leri
- **Demux**: MPEG-TS, Matroska, MP4/MOV, AC3, EAC3, DTS, TrueHD/MLP, FLAC, ASF/WMA, MP3
- **Ses decode**: AC3, EAC3, DTS (DTS-HD dahil `dca`), TrueHD, FLAC, WMA/WMAPro, Opus, AAC, MP3
- **Ses çıkışı**: PCM (AudioTrack) veya **bitstream passthrough** (AC3/EAC3/DTS/TrueHD)
- **Video**: MediaCodec HW decode → Surface (H.264 + 4K HEVC)
- **DVB**: DVB v5 frontend (`/dev/dvb/adapterN/frontend0`) ve CIx beslemesi
- **TS feed**: Tuner/CI'dan gelen TS byte'ları FFmpeg'e custom AVIO ile beslenir

## Gereksinimler

| Araç | Sürüm |
|---|---|
| JDK | 17 |
| Android SDK | platform 33, build-tools 34.0.0 |
| NDK | 25.1.8937325 |
| CMake | 3.22.1 |
| Gradle | 8.5 (wrapper dahil) |
| AGP / Kotlin | 8.2.2 / 1.9.22 |

`minSdk 21`, `targetSdk 25`, `compileSdk 33`.

## GitHub Actions ile APK üretimi (önerilen yol)

Depo hazır olduğunda `.github/workflows/build-apk.yml` her push'ta çalışır ve
**imzalı (debug anahtarı), kurulabilir** bir release APK üretir. Secret gerekmez.

1. GitHub'da boş bir depo oluşturun (public/private fark etmez).
2. Bu klasörü o depoya gönderin:

   ```bash
   cd s912-codec
   git add -A
   git commit -m "S912 codec app: FFmpeg 6.1 + libsrt 1.5.3, GitHub CI"
   git branch -M main
   git remote add origin https://github.com/KULLANICI/REPO.git
   git push -u origin main
   ```

3. Depo → **Actions** → **Build APK** işi otomatik başlar (ilk çalışma ~30-50 dk;
   FFmpeg derlemesi uzun sürer, sonrakiler cache ile hızlanır).
4. İş bitince sayfanın altındaki **Artifacts** bölümünden `s912-codec-apk`
   dosyasını indirin. İçinde `app-release.apk` bulunur.

> `workflow_dispatch` eklendiği için Actions sekmesinden **Run workflow** ile
> elle de tetikleyebilirsiniz.

### APK'yı kutuya kurma

```bash
# Kutuyu USB ile bağlayın (veya adb connect IP:5555)
adb install -r app-release.apk
adb shell am start -n com.amlogic.codec/.CodecActivity
```

Kurulum `INSTALL_FAILED_OLDER_SDK` verirse cihazın `ro.build.version.sdk`
değeri 21'in altındadır; `INSTALL_FAILED_NO_MATCHING_ABIS` verirse cihaz arm64
değildir (bu APK yalnızca `arm64-v8a` içerir).

## SRT kullanımı

Uygulama açıldığında URL alanına adresi yazıp **Play**'e basın:

| Mod | URL |
|---|---|
| Caller (istemci) | `srt://HOST:PORT?mode=caller` |
| Listener (sunucu) | `srt://:9000?mode=listener` |
| Yerel dosya | `/sdcard/test.mkv` |

**Passthrough / PCM** düğmesi ses yolunu değiştirir. Passthrough yalnızca
AC3/EAC3/DTS/TrueHD kaynaklarda ve cihazın audio policy'si offload'a izin
veriyorsa çalışır; aksi halde otomatik PCM'e döner.

## Yerel derleme

```bash
export ANDROID_NDK=$HOME/Android/Sdk/ndk/25.1.8937325

# 1) Kaynaklar
mkdir -p scripts/src
git clone --depth 1 -b v1.5.3 https://github.com/Haivision/srt scripts/src/libsrt
git clone --depth 1 -b n6.1  https://github.com/FFmpeg/FFmpeg.git scripts/src/ffmpeg-6.1

# 2) Bağımlılıklar (statik)
./scripts/build_libsrt.sh    # -> prebuilt/lib/libsrt.a, prebuilt/include/srt
./scripts/build_ffmpeg.sh    # -> prebuilt/ffmpeg/{include,lib}

# 3) APK
./gradlew :app:assembleDebug
# veya release (debug anahtarıyla imzalı):
./gradlew :app:assembleRelease
```

Çıktılar: `app/build/outputs/apk/debug/app-debug.apk`,
`app/build/outputs/apk/release/app-release.apk`.

Farklı bir NDK kullanıyorsanız:

```bash
./gradlew :app:assembleDebug "-Ps912NdkVersion=28.2.13676358"
```

`local.properties` içine `sdk.dir=...` yazmayı unutmayın (Android Studio
otomatik oluşturur; git'e girmez).

### Docker ile derleme (opsiyonel)

```bash
docker build -t s912codec .
docker run --rm -v "$PWD/out:/out" s912codec
# APK: out/app-release.apk
```

## FFmpeg codec ID'leri

`libavcodec/codec_id.h` (n6.1) gerçek değerleri — `CodecBridge.java` ile birebir:

| Codec | Sabit | Değer |
|---|---|---|
| AC3 | `AV_CODEC_ID_AC3` | `0x15003` (86019) |
| DTS | `AV_CODEC_ID_DTS` | `0x15004` (86020) |
| EAC3 | `AV_CODEC_ID_EAC3` | `0x15028` (86056) |
| TrueHD | `AV_CODEC_ID_TRUEHD` | `0x1502C` (86060) |
| H.264 | `AV_CODEC_ID_H264` | `27` |
| HEVC | `AV_CODEC_ID_HEVC` | `173` |

## Dosya yapısı

```
s912-codec/
├─ .github/workflows/build-apk.yml   # CI: bağımlılıklar + APK + artifact
├─ scripts/
│  ├─ build_libsrt.sh                # libsrt 1.5.3 -> prebuilt/ (statik)
│  ├─ build_ffmpeg.sh                # FFmpeg 6.1  -> prebuilt/ffmpeg (statik)
│  └─ src/                           # klonlanan kaynaklar (git'e girmez)
├─ app/
│  ├─ build.gradle
│  └─ src/main/
│     ├─ AndroidManifest.xml
│     ├─ cpp/
│     │  ├─ CMakeLists.txt
│     │  └─ native/
│     │     ├─ codec_core.cpp        # JNI, demux kuyrukları, decode, TS feed
│     │     └─ dvb_frontend.cpp      # DVB v5 + CIx
│     └─ java/com/amlogic/codec/
│        ├─ CodecBridge.java         # native metotlar + codec sabitleri
│        ├─ SrtCodecPlayer.kt        # AudioTrack PCM / passthrough
│        ├─ VideoDecoder.kt          # MediaCodec HW decode
│        ├─ DvbSource.kt             # tuner / CIx -> TS feed
│        └─ CodecActivity.kt         # SurfaceView + kontrol paneli
└─ README.md
```

## Bilinen sınırlar

- Video/audio senkronu PTS tabanlıdır; uzun süreli oynatmada drift için
  `MediaSync`/`AudioTrack.getTimestamp()` ince ayarı eklenebilir.
- Passthrough, cihazın `audio_policy` offload desteğine bağlıdır. Kapalıysa
  root gerektiren `/dev/audiodsp0` yolu `SrtCodecPlayer.startPassThroughRaw()`
  içinde opsiyonel fallback olarak durur.
- APK yalnızca `arm64-v8a` içerir.
