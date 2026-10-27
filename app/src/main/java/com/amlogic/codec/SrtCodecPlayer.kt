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
     * AC3/EAC3/DTS/TrueHD bitstream'i AudioTrack offload'a basar.
     * Offload desteklenmiyorsa PCM decode'a duser.
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
                handler.post {
                    try { track?.stop() } catch (_: Exception) {}
                    try { track?.release() } catch (_: Exception) {}
                    track = null
                }
            }
        }
    }

    /**
     * Root fallback: Amlogic /dev/audiodsp0 raw node.
     * Offload AudioTrack kapali kutularda kullanilir.
     */
    fun startPassThroughRaw() {
        try {
            File("/sys/class/amaudio/output_enable").writeText("1\n")
        } catch (_: Exception) {}
        try {
            File("/sys/class/audiodsp/digital_raw").writeText("1\n")
        } catch (_: Exception) {}

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
        CodecBridge.nativeClose()
    }

    companion object {
        private const val TAG = "S912Codec"
    }
}
