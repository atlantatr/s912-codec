package com.amlogic.codec

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Handler
import android.os.Looper
import android.view.Surface
import java.io.File
import java.io.FileOutputStream
import kotlin.concurrent.thread

/**
 * Audio cikiş rejimi - DVB/Dolby-DTS acgi icin kritik:
 *   - PCM_DECODE : FFmpeg decode -> s16 PCM -> AudioTrack (default, her zaman calisir)
 *   - PASS_THROUGH : ham AC3/EAC3/DTS/TrueHD bitstream -> AudioTrack offload
 *     (ENCODING_AC3 / DTS / EAC3 / TRUEHD). HDMI alici / Amlogic DSP
 *     bitstream'i dogrudan yutur; Dolby/DTS surunde bozulur.
 */
enum class AudioMode { PCM_DECODE, PASS_THROUGH }

/**
 * S912 / Android 7.1.2 icin SRT + AC3/EAC3/DTS/TrueHD/FLAC/Opus/WMA
 * audio player iskeleti.
 *
 * - audio decode: native FFmpeg (codec_core.cpp)
 * - audio out:   AudioTrack (PCM, s16, stereo veya kaynagindaki layout)
 * - video decode: MediaCodec HW (S912 4K HEVC/H264) -> SurfaceView
 *
 * Kullanilimi:
 *   val p = SrtCodecPlayer()
 *   p.open("srt://192.168.1.50:9000?mode=caller")
 *   p.startVideo(surface)   // 4K HEVC HW decode Surface'e baslar
 *   p.start()               // audio dongusu
 *   ... p.stopVideo(); p.stop(); p.release()
 */
class SrtCodecPlayer {

    private val handler = Handler(Looper.getMainLooper())
    private var playerThread: Thread? = null
    private var running = false
    private var track: AudioTrack? = null
    private var video: VideoDecoder? = null

    private val pcmBuf = ByteArray(48000 * 2 * 2) // ~200ms stereo s16@48k

    fun open(url: String, sampleRate: Int = 0, channels: Int = 0): Boolean {
        val rc = CodecBridge.nativeOpen(url, sampleRate, channels)
        return rc == 0
    }

    /**
     * Video katmanini baslat: HEVC/H.264 detektasyonu + MediaCodec HW decode
     * + SurfaceView'a baslama. 4K 10-bit / Dolby Vision profilleri S912 HW'de
     * dekod edilir (OMX.AMLogic / c2.hevc).
     */
    /**
     * Passthrough dongusu: audio, ffmpeg decode edilmez; AC3/EAC3/DTS/TrueHD
     * bitstream'i dogrudan AudioTrack offload'a basilir (HDMI/Amlogic DSP).
     * Dolby/DTS surround icin kullanilir.
     */
    fun startPassThrough() {
        if (CodecBridge.nativeCanPassThrough() == 0) {
            // Passthrough yapilamaz, PCM'e don
            start(); return
        }
        // FFmpeg 6.1 codec_id degerleri (libavcodec/codec_id.h):
        //   AC3=87, EAC3=88, DTS=14, TRUEHD=13
        val enc = when (CodecBridge.nativeAudioCodecId()) {
            87 -> AudioFormat.ENCODING_AC3       // AC3
            88 -> AudioFormat.ENCODING_E_AC3     // EAC3
            14 -> AudioFormat.ENCODING_DTS       // DTS / DTS-HD
            13 -> AudioFormat.ENCODING_TRUEHD    // TrueHD
            else -> AudioFormat.ENCODING_AC3
        }
        val cfg = AudioFormat.Builder()
            .setSampleRate(48000)
            .setEncoding(enc)
            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
            .build()
        val at = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.DEFAULT)
            .setAudioFormat(cfg)
            .setBufferSizeInBytes(4 * 1024 * 1024) // bol, offload icin
            .build()
        at.play()
        this.track = at
        running = true
        val buf = ByteArray(4 * 1024 * 1024)
        playerThread = thread(name = "srt-passthrough") {
            while (running) {
                val n = CodecBridge.nativePollAudioPassThrough(buf)
                if (n < 0) break
                at.write(buf, 0, n.toInt())
            }
            running = false
            handler.post { at.release() }
        }
    }

    /**
     * /dev/audiodsp0 fallback — 7.1.2 S912 kutusunda offload format
     * (ENCODING_AC3/EAC3/DTS/TRUEHD) audio_policy icin kapaliysa,
     * PCM offload da calismayabilir. Bu durumda Amlogic audio DSP
     * dogrudan basilir (root / su ile):
     *
     *   1. audio out device init: echo 1 > /sys/class/amaudio/output_enable
     *   2. raw out mode:    echo 1 > /sys/class/audiodsp/digital_raw
     *   3. raw write:       > /dev/audiodsp0  (binary write, AC3/DTS/TrueHD
     *                       bitstream boylu; 7.1'de kernel amaudio raw node
     *                       bunu HDMI'e basar)
     *
     * Bu adim root / platform imzasi gerektirir (eski setin yaptigi seyin
     * birebir yantisi). Offload calisiyorsa bu metodu cagirmayin,
     * startPassThrough() kullanin.
     */
    fun startPassThroughRaw() {
        // 1. output_enable (root)
        try {
            File("/sys/class/amaudio/output_enable").writeText("1\n")
        } catch (_: Exception) {
            // root/devel cagrisi; izin yoksa atla
        }
        // 2. digital_raw mod (root)
        try {
            File("/sys/class/audiodsp/digital_raw").writeText("1\n")
        } catch (_: Exception) {}
        // 3. /dev/audiodsp0'a binary write (root gerekli; su ile calisir)
        running = true
        val buf = ByteArray(4 * 1024 * 1024)
        playerThread = thread(name = "srt-passthrough-raw") {
            try {
                val rawOut = FileOutputStream(File("/dev/audiodsp0"))
                while (running) {
                    val n = CodecBridge.nativePollAudioPassThrough(buf)
                    if (n < 0) break
                    rawOut.write(buf, 0, n.toInt())
                    rawOut.flush()
                }
                rawOut.close()
            } catch (e: Exception) {
                Log.e("S912Codec", "raw passthrough /dev/audiodsp0 hata: ${e.message}")
            }
            running = false
        }
    }

    fun startVideo(surface: Surface) {
        if (CodecBridge.nativeHasVideo() == 0) {
            // audio-only kaynak, hic bir sey yapma
            return
        }
        val cid = CodecBridge.nativeVideoCodecId()
        val w = CodecBridge.nativeVideoWidth()
        val h = CodecBridge.nativeVideoHeight()
        val vd = VideoDecoder()
        vd.open(cid, w, h, surface)
        this.video = vd
    }

    fun stopVideo() {
        video?.stop()
        video = null
    }

    fun start() {
        if (running) return
        running = true
        val rate = if (CodecBridge.nativeAudioRate() == 0) 48000 else CodecBridge.nativeAudioRate()
        val ch = if (CodecBridge.nativeAudioChannels() == 0) 2 else CodecBridge.nativeAudioChannels()
        val chMask = if (ch > 2) AudioFormat.CHANNEL_MASK_5POINT1 else AudioFormat.CHANNEL_MASK_STEREO
        val track = AudioTrack(
            AudioManager.STREAM_MUSIC,
            rate, AudioFormat.CHANNEL_OUT_MONO.let { chMask },
            AudioFormat.ENCODING_PCM_16BIT,
            pcmBuf.size, AudioTrack.MODE_STREAM,
            AudioAttributes.DEFAULT
        )
        track.play()
        this.track = track
        playerThread = thread(name = "srt-audio") {
            while (running) {
                val n = CodecBridge.nativePollAudioPcm(pcmBuf)
                if (n < 0) break
                track.write(pcmBuf, 0, n.toInt())
            }
            running = false
            handler.post { track?.stop(); track?.release(); track = null }
        }
    }

    fun stop() {
        running = false
        playerThread?.join()
        playerThread = null
    }

    fun release() {
        stop()
        stopVideo()
        CodecBridge.nativeClose()
    }
}
