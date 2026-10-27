# S912 / Android 7.1.2 arm64 - SRT + AC3/EAC3/DTS/TrueHD codec app iskeleti

Bu klasor, yuklenmis `ses (1).apk` (com.android.audiopatch) setinin **guncel,
platform-imzasiz, kosesi kopuk 5.1 build'i libamplayer.so seti yerine**,
S912 + Android 7.1.2 (arm64) uzerinde calisacak bir **FFmpeg 6.x + libsrt 1.5.x**
tabanli JNI codec app iskeletidir.

## Neden bu yapi?

Eski setin kritik zayifliklari (ve bu projedeki cozum):

| Eski set | Bu proje |
|---|---|
| `libbinder.so` / `libstagefright` ABI sinari 5.1 build'ine (7.1.2'de crash riski) | Kosesi tamamen kosa: app process'i icinde MediaCodec HW + AudioTrack + FFmpeg (binder/stagefright'a bagimlilik yok) |
| 32-bit ARMv7/AArch32 ELF (S912 arm64 uyumsuz) | NDK `arm64-v8a` derleme |
| 10 yil eski FFmpeg libleri | FFmpeg 6.1 guncel |
| `android.uid.system` platform imzasi (kutudan platform key alinamadi) | Normal 3rd-party imza, root gerektirmez |

## Desteklenen audio codec (eski setin birebir kutusu)

- **Audio decode**: AC3, EAC3, DTS, DTS-HD, TrueHD, FLAC, WMA, WMAPro, Opus, AAC, MP3
- **Audio out**: PCM s16 (AudioTrack) + passthrough (bitstream, sonraki adim)
- **Demuxer**: AC3, DTS, FLAC, WMA, Matroska, MP4/MOV, MPEG-TS, MP3, **SRT**
- **SRT**: libsrt 1.5.x (listener/caller mod)

## Yapi (dosya agaci)

```
s912-codec/
├── README.md
├── settings.gradle
├── scripts/
│   ├── build_libsrt.sh     # libsrt 1.5.x -> prebuilt/lib + include
│   ├── build_ffmpeg.sh     # FFmpeg 6.1  -> prebuilt/ffmpeg
│   └── src/                # libsrt-1.5.3 + ffmpeg-6.1 kaynak (sen kosesin)
└── app/
    ├── build.gradle
    └── src/main/
        ├── AndroidManifest.xml
        ├── cpp/
        │   ├── CMakeLists.txt
        │   └── native/codec_core.cpp    # JNI + FFmpeg demux/decode + swr
        └── java/com/amlogic/codec/
            ├── CodecBridge.java         # native methodlar
            ├── SrtCodecPlayer.kt        # AudioTrack + poll dongusu
            └── CodecActivity.kt         # SRT URL girisi + play/stop
```

## Kurulum - 4 adim

### 1) Kaynagi cagi

```bash
mkdir -p scripts/src
git clone -b v1.5.3 https://github.com/Haivision/srt scripts/src/libsrt
git clone -b n6.1  https://git.ffmpeg.org/ffmpeg.git scripts/src/ffmpeg-6.1
```

### 2) libsrt + FFmpeg'i arm64 icin derle

```bash
export ANDROID_NDK=$HOME/Android/Sdk/ndk/25.1.8937325   # kendi NDK yolun
chmod +x scripts/build_libsrt.sh scripts/build_ffmpeg.sh
./scripts/build_libsrt.sh
./scripts/build_ffmpeg.sh
```

Ciktilari `prebuilt/` icine koyar:
- `prebuilt/lib/libsrt.so`
- `prebuilt/include/srt/*.h`
- `prebuilt/ffmpeg/lib/*.a` (static)

**jniLibs'a libsrt.so'yu koy:**
```bash
mkdir -p app/jniLibs/arm64-v8a
cp prebuilt/lib/libsrt.so app/jniLibs/arm64-v8a/
```

### 3) Android Studio'da ac

- `s912-codec/` dizinini ac
- "Sync" (gradle) -> CMake arm64-v8a derlenir
- `app/build.gradle` icindeki NDK versiyonunu kendi NDK'niza esit tut
  (varsayilan 25.1.8937325; farkli NDK kosesiniz icin `android.ndkVersion` ekle)

### 4) Kutuya at

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -s S912Codec
```

## SRT URL secenegi

| Mod | URL | Aciklama |
|---|---|---|
| caller (istemci) | `srt://HOST:PORT?mode=caller` | Remote listener'a baglanir |
| listener | `srt://:9000?mode=listener` | Bu kutu listener, baskasi baglanir |
| local dosya | `/sdcard/video.mkv` | SRT yerine FFmpeg demuxer |

## DVB / Dolby-DTS Passthrough katmani (ekli)

Kutuda DVB (tuner/CI) oldugu icin es setin asil islevi burada: DVB stream'inde
gelen **AC3/EAC3/DTS/TrueHD bitstream'i PCM'e cevirmeden, HDMI/Amlogic audio
DSP'a dogrudan basma** (Dolby/DTS surround sarti). Yeni sette de bu korundu:

- **`codec_core.cpp`**: `pollAudioPassThrough()` + `canPassThrough()` +
  `audioCodecId()` — FFmpeg demuxer'dan ham audio bitstream'i ceker (AC3/EAC3/
  DTS/TrueHD), PCM decode yapmaz.
- **`SrtCodecPlayer.startPassThrough()`** — `AudioTrack` offload format
  (`ENCODING_AC3` / `ENCODING_E_AC3` / `ENCODING_DTS` / `ENCODING_TRUEHD`)
  ile ham bitstream'i yazar. Amlogic `audiodsp0` / `astream` node'lari bu
  ofload format'i dogrudan yutar (eski setin yaptigi seyin tam karsi
  yantisi).
- **UI**: `CodecActivity`'de "Passthrough / PCM" butonu ile rejim degisir;
  passthrough yapilamayan audio (sadece PCM codec) otomatik PCM'e doner.

### Es set ve yeni set arasi passthrough esdegerligi

| Es set (libamplayer) | Bu set |
|---|---|
| `/sys/class/audiodsp/digital_raw`, `digital_codec` | Offload AudioTrack (platform'a bagli, root gerektirmez) |
| `/sys/class/astream/format` | `AudioFormat.ENCODING_AC3/EAC3/DTS/TRUEHD` |
| `/dev/audiodsp0` (dogrudan ioctl) | `AudioTrack` (framework kosesi) |
| `digital_raw is passthrough mode` logi | logcat'te `ENCODING_AC3` offload onaylari |

> Eski set `platform key` + `amhdmitx`/`astream` node'lari icin
> `android.uid.system` istemisti. Bu set 3rd-party imzayla calisir,
> `AudioTrack` ofload yoluyla ayni donanimi platform'siz kullanir — S912 +
> 7.1.2'de offload AC3/EAC3/DTS/TrueHD `media.audiocodec` policy'si ile
> desteklenir. Eger kutunda ofload format'lari `AudioPolicy` tarafinda
> kilitliyse (bazi firmalari kilitlemis), o zaman `/dev/audiodsp0`'a
> root ile dogrudan yazma modu eklemek gerekir (sonraki adim, opsiyonel).

## Video / 4K HEVC katmani (ekli - 2. adim)

Ses iskeletine ek olarak S912 4K HW decode katmani da tamam:

### Native (codec_core.cpp)
- **Video stream tespiti** (`openSrt` icinde `AVMEDIA_TYPE_VIDEO` arama)
- **`pollVideoPkt()`** — `av_read_packet` ile tek video AVPacket'i ceker,
  MediaCodec input'a verilmek uzere doner (SPS/VPS/PPS korunur)
- **JNI accessor'lar**: `nativeHasVideo`, `nativeVideoCodecId` (131=AVC,
  173=HEVC), `nativeVideoWidth`, `nativeVideoHeight`, `nativePollVideoPkt`

### Kotlin
- **`VideoDecoder.kt`** — MediaCodec HW decode:
  - `createDecoderByType("video/hevc")` / `"video/avc"` (7.1.2'te c2.* /
    OMX.AMLogic.* otomatik secilir)
  - S912 HW: 4K HEVC 10-bit + Dolby Vision (prof 5/8) destegi
  - `pump()` dongusu: `nativePollVideoPkt` -> `queueInputBuffer` -> Surface'e decode
- **`SrtCodecPlayer.startVideo(surface)` / `stopVideo()`** — audio dongusuyla
  paralel calisan video katmani
- **`CodecActivity.kt`** — SurfaceView + alt URL/Play/Stop paneli;
  4K HEVC HW decode dogrudan Surface'e basilir

### Audio + Video senkronizasyonu (3. adim, sonrakiler)
- Video packet'in `AVPacket.pts`'i `MediaCodec.queueInputBuffer`'un
  `presentationTimeUs` parameterine verilir (iskelette 0L; 3. adimda
  `nativePollVideoPkt` -> `long[] ptsOut` signature'i eklenir)
- Audio dongusu `AVRational.time_base` ile PCM PTS uretir, video + audio
  ayni `pts` zaman cizgisine konur, `Surface.setFrameRate` veya `MediaSync`
  ile ayni kare aktiligi saglanir

## Kalan 5 maddede durum (hepsi eklendi)

### 1) FFmpeg 6.1 codec_id enum degerleri
FFmpeg 6.1 libavcodec/codec_id.h icin dogru degerler:
- AC3 = 87, EAC3 = 88, DTS = 14, TRUEHD = 13

Kodda (codec_core.cpp + SrtCodecPlayer.startPassThrough()) bu degerler
birebir kullanilir.

### 2) CIx / CI besleme JNI
dvb_frontend.cpp icine eklendi:
- nativeCixOpen(ciSlot) — /dev/ci0 acar, CI_CAPABILITIES ioctl
- nativeCixPoll(ciFd, buf) — CIx'ten 188-byte TS paketi okur
- nativeCixClose(ciFd) — kapatir

### 3) DVB/CIx to FFmpeg pipe
codec_core.cpp icine TS feed modu eklendi:
- nativeOpenTSFeed("mpegts") — custom AVIOContext ile demuxer'a besleme
- nativeFeedTSBytes(buf) — DVB/CIx cikan TS byte'larini demuxer'a basar
- nativeCloseTSFeed() — kapatir

Kotlin tarafinda DvbSource.kt:
- openTuner(frontend, freqHz, polH, ...) -> DVB-S2 tuner + FFmpeg feed modu
- openCix(slot) -> CIx beslemesi + FFmpeg feed modu
- start() -> pump dongusu (DVB/CIx poll to FFmpeg feed)

### 4) Video + audio PTS senkronizasyonu
nativePollVideoPkt(buf, ptsOutUs):
- Cikan video paketi PTS'i (mikrosecond) doner
- VideoDecoder.pump(): 1. video frame'in PTS'i firstPtsUs olarak sabitlenir,
  suresinden sonler relative PTS ile queueInputBuffer icin kullanilir.
  Audio dongusu da ayni cizgiyi kullanir (0'dan baslar).

### 5) /dev/audiodsp0 root raw write fallback
SrtCodecPlayer.startPassThroughRaw():
1. echo 1 > /sys/class/amaudio/output_enable (root)
2. echo 1 > /sys/class/audiodsp/digital_raw (root)
3. FileOutputStream("/dev/audiodsp0") ile binary write
4. Offload AudioTrack calismiyorsa (bazi kutularda audio_policy offload
   kapali), bu yol kullanilir. Root / su ile calisir, platform imzasi
   gerektirmez.

---

## GitHub'da derleme (otomatik APK)

Bu proje GitHub'a itildiginde kendiliğinden derlenir:

1. Bu klasoru bir GitHub repo olarak olustur / push et
2. GitHub her push/PR'da `.github/workflows/build-apk.yml` calistirir:
   - Android NDK r25.1 (arm64-v8a) kur
   - libsrt 1.5.3 + FFmpeg 6.1'i arm64 icin derle
   - `libsrt.so` -> `app/jniLibs/arm64-v8a/` kopyala
   - `gradle :app:assembleRelease` -> **unsigned release APK**
   - APK'yiy "s912-codec-apk" artifact olarak yukler
3. Repo > Actions > calisan isin altindan `s912-codec-apk` artifact'i indir
   -> `app-build/outputs/apk/release/app-release-unsigned.apk`

### APK'yiy kutuya kur

Release APK **imzasizdir** (signingConfig=debug). Kendi imzasiyla yurutmek icin:

```bash
# apksigner (Android build-tools) ile imzala, sonra install
apksigner sign --ks my.keystore --ks-key-alias s912 \
    app-release-unsigned.apk s912-codec-signed.apk
adb install s912-codec-signed.apk
```

### Opsiyonel: Docker ile yerel build

```bash
docker build -t s912codec .
docker run --rm -v "$PWD:/work" -w /work s912codec
# -> app/build/outputs/apk/release/app-release-unsigned.apk
```

### Lokaliniz (Android Studio) build

```bash
# 1) NDK + FFmpeg + libsrt kaynagi
export ANDROID_NDK=$HOME/Android/Sdk/ndk/25.1.8937325
git clone --depth 1 -b v1.5.3 https://github.com/Haivision/srt scripts/src/libsrt
git clone --depth 1 -b n6.1  https://git.ffmpeg.org/ffmpeg.git scripts/src/ffmpeg-6.1
./scripts/build_libsrt.sh && ./scripts/build_ffmpeg.sh
mkdir -p app/jniLibs/arm64-v8a && cp prebuilt/lib/libsrt.so app/jniLibs/arm64-v8a/
# 2) APK
./gradlew :app:assembleDebug
```
