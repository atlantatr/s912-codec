package com.amlogic.codec;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import java.io.File;
import java.io.FileOutputStream;

import kotlin.concurrent.thread;

/**
 * S912 / Android 7.1.2 icin SRT + AC3/EAC3/DTS/TrueHD/FLAC/Opus/WMA oynatici.
 *
 * - Demux/decode: native FFmpeg (codec_core.cpp)
 * - Audio out: AudioTrack (PCM) veya offload bitstream (AC3/EAC3/DTS/TrueHD)
 * - Video: MediaCodec HW decode -> Surface
 */
class SrtCodecPlayer {

    private val handler = Handler(Looper.getMainLooper())
    private var playerThread: Thread? = null
    private var running = false
    private var track: AudioTrack? = null
    private var video: VideoDecoder? = null
    private var dvb: DvbSource? = null

    /** digital_raw'in bizim degistirmeden onceki degeri (-1 = bilinmiyor). */
    private var rawPrevious = -1

    // 1 saniyelik stereo s16 @48k tampon.
    private val pcmBuf = ByteArray(48000 * 2 * 2)

    private fun audioAttributes(): AudioAttributes =
        AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
            .build()

    /**
     * API 23+ icin AudioTrack.Builder, API 21/22 icin eski constructor.
     * minSdk 21 korunur ve lint NewApi hatasi cikmaz.
     */
    @Suppress("DEPRECATION")
    private fun createAudioTrack(
        sampleRate: Int,
        channelMask: Int,
        encoding: Int,
        bufferSize: Int
    ): AudioTrack {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            val cfg = AudioFormat.Builder()
                .setSampleRate(sampleRate)
                .setEncoding(encoding)
                .setChannelMask(channelMask)
                .build()
            return AudioTrack.Builder()
                .setAudioAttributes(audioAttributes())
                .setAudioFormat(cfg)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(bufferSize)
                .build()
        }
        return AudioTrack(
            AudioManager.STREAM_MUSIC,
            sampleRate,
            channelMask,
            encoding,
            bufferSize,
            AudioTrack.MODE_STREAM
        )
    }

    fun open(url: String, sampleRate: Int = 0, channels: Int = 0): Boolean {
        return CodecBridge.nativeOpen(url, sampleRate, channels) == 0
    }

    /**
     * DVB-S/S2 kanalini ac: tuner -> TS feed -> FFmpeg demux.
     * Akis gelene kadar (PAT/PMT + ilk paketler) bekler.
     *
     * @param freqHz transponder frekansi (Hz), ornek 11_767_000_000
     * @param srKsym symbol rate (kSym/s), ornek 15000
     */
    fun openDvb(
        freqHz: Long,
        polH: Boolean,
        srKsym: Int,
        adapter: Int = 0,
        fecNum: Int = 2,
        fecDen: Int = 3
    ): Boolean {
        releaseDvb()
        val d = DvbSource()
        dvb = d
        if (!d.openTuner(
                frontend = adapter,
                freqHz = freqHz,
                polH = polH,
                sr = srKsym,
                fecNum = fecNum,
                fecDen = fecDen
        )) {
            Log.e(TAG, "DVB acilamadi (freq=$freqHz sr=$srKsym adapter=$adapter)")
            dvb = null
            return false
        }
        d.start()

        // FFmpeg'in PAT/PMT okuyup codec'leri bulmasi icin bekle.
        var waited = 0
        while (waited < 8000) {
            if (CodecBridge.nativeAudioRate() > 0 || CodecBridge.nativeHasVideo() != 0) {
                Log.i(TAG, "DVB akis hazir (${waited} ms): video=${CodecBridge.nativeHasVideo()} " +
                        "audio=${CodecBridge.nativeAudioRate()}Hz/${CodecBridge.nativeAudioChannels()}ch " +
                        "codec=${CodecBridge.nativeAudioCodecId()}")
                return true
            }
            Thread.sleep(100)
            waited += 100
        }
        Log.e(TAG, "DVB akis hazir olmadi (${waited} ms)")
        return false
    }

    private fun releaseDvb() {
        dvb?.release()
        dvb = null
    }

    /**
     * AC3/EAC3/DTS/TrueHD bitstream'i AudioTrack offload'a basar.
     * Offload desteklenmiyorsa PCM decode'a duser.
     *
     * Amlogic kutularda bitstream icin /sys/class/audiodsp/digital_raw=1
     * olmasi gerekir; asagida AudioTrack'ten ONCE set edilir, is bitince
     * geri alinir.
     */
    fun startPassThrough() {
        if (CodecBridge.nativeCanPassThrough() == 0) {
            start()
            return
        }

        val enc = when (CodecBridge.nativeAudioCodecId()) {
            CodecBridge.CODEC_ID_AC3 -> AudioFormat.ENCODING_AC3
            CodecBridge.CODEC_ID_EAC3 -> AudioFormat.ENCODING_E_AC3
            CodecBridge.CODEC_ID_DTS -> AudioFormat.ENCODING_DTS
            CodecBridge.CODEC_ID_TRUEHD -> AudioFormat.ENCODING_DOLBY_TRUEHD
            else -> AudioFormat.ENCODING_AC3
        }

        // Bitstream yolu: kutu raw moda alinir (root gerekmez).
        applyRawMode()

        try {
            val at = createAudioTrack(
                sampleRate = 48000,
                channelMask = AudioFormat.CHANNEL_OUT_5POINT1,
                encoding = enc,
                bufferSize = 1024 * 1024
            )

            at.play()
            track = at
        } catch (e: Exception) {
            Log.e(TAG, "passthrough AudioTrack kurulamadi: ${e.message}")
            restoreRawMode()
            start()
            return
        }

        running = true
        val buf = ByteArray(1024 * 1024)
        playerThread = thread(name = "srt-passthrough") {
            try {
                while (running) {
                    val n = CodecBridge.nativePollAudioPassThrough(buf)
                    if (n < 0) break
                    if (n == 0L) {
                        Thread.sleep(5)
                        continue
                    }
                    track?.write(buf, 0, n.toInt())
                }
            } catch (e: Exception) {
                Log.e(TAG, "passthrough dongu hatasi: ${e.message}")
            } finally {
                running = false
                restoreRawMode()
                handler.post {
                    try { track?.stop() } catch (_: Exception) {}
                    try { track?.release() } catch (_: Exception) {}
                    track = null
                }
            }
        }
    }

    /**
     * Bitstream icin digital_raw=1 yapar ve onceki degeri saklar.
     * Onceki deger 1 ise (baska bir uygulama/kullanici zaten raw modda)
     * cikista tekrar 1'e donulur; boylece disaridan yapilmis ayar bozulmaz.
     */
    private fun applyRawMode() {
        rawPrevious = AmlogicRawAudio.readRawMode()
        AmlogicRawAudio.setRawMode(true)
        Log.i(TAG, "raw mod acildi (onceki digital_raw=$rawPrevious)")
    }

    /** digital_raw'i bizim buldugumuz degerine dondurur. */
    private fun restoreRawMode() {
        if (rawPrevious < 0) return
        val target = rawPrevious
        rawPrevious = -1
        AmlogicRawAudio.setRawMode(target != 0)
        Log.i(TAG, "raw mod geri alindi (digital_raw=$target)")
    }

    /**
     * Root fallback: Amlogic /dev/audiodsp0 raw node.
     * Offload AudioTrack kapali kutularda kullanilir.
     */
    fun startPassThroughRaw() {
        try {
            File("/sys/class/amaudio/output_enable").writeText("1\n")
        } catch (_: Exception) {}
        applyRawMode()

        running = true
        val buf = ByteArray(1024 * 1024)
        playerThread = thread(name = "srt-passthrough-raw") {
            try {
                FileOutputStream(File("/dev/audiodsp0")).use { rawOut ->
                    while (running) {
                        val n = CodecBridge.nativePollAudioPassThrough(buf)
                        if (n < 0) break
                        if (n == 0L) {
                            Thread.sleep(5)
                            continue
                        }
                        rawOut.write(buf, 0, n.toInt())
                        rawOut.flush()
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "raw passthrough /dev/audiodsp0: ${e.message}")
            } finally {
                running = false
                restoreRawMode()
            }
        }
    }

    fun startVideo(surface: Surface) {
        if (CodecBridge.nativeHasVideo() == 0) return

        val vd = VideoDecoder()
        try {
            vd.open(
                CodecBridge.nativeVideoCodecId(),
                CodecBridge.nativeVideoWidth(),
                CodecBridge.nativeVideoHeight(),
                surface
            )
            video = vd
        } catch (e: Exception) {
            Log.e(TAG, "video decoder acilamadi: ${e.message}")
            vd.release()
        }
    }

    fun stopVideo() {
        video?.stop()
        video = null
    }

    /** FFmpeg decode -> s16 PCM -> AudioTrack. */
    fun start() {
        if (running) return

        // PCM yolu: onceki oturumdan raw mod kalmis olabilir, kapat.
        restoreRawMode()
        AmlogicRawAudio.setRawMode(false)

        val rate = CodecBridge.nativeAudioRate().let { if (it <= 0) 48000 else it }
        val ch = CodecBridge.nativeAudioChannels().let { if (it <= 0) 2 else it }
        val chMask = when {
            ch >= 8 -> AudioFormat.CHANNEL_OUT_7POINT1_SURROUND
            ch >= 6 -> AudioFormat.CHANNEL_OUT_5POINT1
            ch == 1 -> AudioFormat.CHANNEL_OUT_MONO
            else -> AudioFormat.CHANNEL_OUT_STEREO
        }

        val minBuf = AudioTrack.getMinBufferSize(
            rate, chMask, AudioFormat.ENCODING_PCM_16BIT
        )
        val bufSize = if (minBuf > 0) maxOf(minBuf, pcmBuf.size) else pcmBuf.size

        val at = try {
            createAudioTrack(
                sampleRate = rate,
                channelMask = chMask,
                encoding = AudioFormat.ENCODING_PCM_16BIT,
                bufferSize = bufSize
            )
        } catch (e: Exception) {
            Log.e(TAG, "PCM AudioTrack kurulamadi: ${e.message}")
            return
        }

        at.play()
        track = at
        running = true

        playerThread = thread(name = "srt-audio") {
            try {
                while (running) {
                    val n = CodecBridge.nativePollAudioPcm(pcmBuf)
                    if (n < 0) break
                    if (n == 0L) {
                        Thread.sleep(5)
                        continue
                    }
                    at.write(pcmBuf, 0, n.toInt())
                }
            } catch (e: Exception) {
                Log.e(TAG, "audio dongu hatasi: ${e.message}")
            } finally {
                running = false
                handler.post {
                    try { track?.stop() } catch (_: Exception) {}
                    try { track?.release() } catch (_: Exception) {}
                    track = null
                }
            }
        }
    }

    fun stop() {
        running = false
        playerThread?.join(2000)
        playerThread = null
    }

    fun release() {
        stop()
        stopVideo()
        releaseDvb()
        CodecBridge.nativeClose()
    }

    companion object {
        private const val TAG = "S912Codec"
    }
}
