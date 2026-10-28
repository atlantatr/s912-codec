# S912 Codec 1.1 (2026.10.28)

APK: `S912Codec-1.1.2026.10.28.apk` (4,47 MB)
İmza: `CN=S912 Codec, O=atlantatr, C=TR` (sabit — güncellemeler üzerine kurulur)
Kaynak: https://github.com/atlantatr/s912-codec (dal: `apk-latest` her derlemede güncellenir)

## Bu sürümde değişenler

| # | Değişiklik | Kanıt |
|---|---|---|
| 1 | **Açılış hatası düzeltildi.** `avformat_open_input` her dosyada `Immediate exit requested` veriyordu (interrupt callback `running=false` görüyordu) → uygulama hiçbir dosyayı açamıyordu. | logcat: `opened: /sdcard/ac3_test.mkv`, `audio: idx=1 codec=86019 rate=48000 ch=6` |
| 2 | **RAW (bitstream) modu.** AC3/EAC3/DTS/TrueHD çalarken `/sys/class/audiodsp/digital_raw=1` yazılır, önceki değer saklanır, iş bitince geri konur. Root gerekmez (düğüm `rw-rw-rw-`). | logcat: `digital_raw <- 1`, `raw mod acildi (onceki digital_raw=0)`, sonra `raw mod geri alindi (digital_raw=0)`; sysfs'te doğrulandı |
| 3 | **Dolby Pro Logic II downmix.** 5.1 kaynak 2 kanala indirilirken `swr matrix_encoding=dolby` kullanılır; böylece 2 kanallı HDMI'da bile surround bilgisi korunur ve Pro Logic II destekli TV/AVR 5.1'e geri açar. | logcat: `swr: 6 -> 2 kanal, Dolby Pro Logic II matris kodlamasi acik` |
| 4 | **Sabit imza anahtarı.** CI her derlemede yeni debug anahtarı üretiyordu → `INSTALL_FAILED_UPDATE_INCOMPATIBLE`. Artık `keystore/s912-codec.jks` kullanılıyor. | `apksigner verify` → `CN=S912 Codec` |
| 5 | **Teşhis satırı.** Ekranda `Dijital cikis: PCM / RAW (bitstream)` canlı gösterilir. | ekran görüntüsü: `ekran-107.png` |
| 6 | **`url` intent extra'sı.** `am start -n com.amlogic.codec/.CodecActivity -e url /sdcard/x.mkv` ile otomatik oynatma. | testlerde kullanıldı |

## Neden hâlâ bitstream ses yok (ölçüm)

Bu firmware'de AC3/DTS bitstream **yazılımla** mümkün değil. Üç bağımsız kanıt:

1. `dumpsys media.audio_policy` → HDMI çıkış profili `[dynamic format]`, yalnızca
   `AUDIO_FORMAT_PCM_16_BIT`. AC3 profili yok ⇒ `AudioTrack(ENCODING_AC3)` →
   `Cannot create AudioTrack`.
2. `/sys/class/amhdmitx/amhdmitx0/aud_ch` = `hdmi_channel = 2 ch`; EDID'de ses
   bloğu (SAD) boş.
3. `tinyplay` + `digital_raw=1` ile doğrudan ALSA (`hw:0,1`) denendi:
   `audio_dsp: IEC958_mode_raw=1` **ama** `aml_spdif_dai: iec958 mode PCM16`.
   `tinymix`'te non-audio/IEC61937 anahtarı yok ⇒ çekirdek AC3 baytlarını PCM16
   çerçevesine sarıyor.

Yani tek çözüm HDMI zinciri: geçerli EDID (TV/AVR AC3 bildirsin) → o anda
`AudioTrack(AC3)` açılır ve RAW modu kendiliğinden çalışır.

## Kurulum

```
adb install -r S912Codec-1.1.2026.10.28.apk
adb shell pm grant com.amlogic.codec android.permission.READ_EXTERNAL_STORAGE
adb shell am start -n com.amlogic.codec/.CodecActivity -e url /sdcard/film.mkv
```
