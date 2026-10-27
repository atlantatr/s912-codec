package com.amlogic.codec

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Handler
import android.os.Looper
import android.view.Surface
import java.nio.ByteBuffer
import kotlin.concurrent.thread

/**
 * 4K HEVC / H.264 video decode katmani - MediaCodec HW.
 *
 * S912 / 7.1.2'te:
 *   - HEVC: "c2.hevc" (Codec2) veya "OMX.AMLogic.hevc.decode"
 *            S912 HW: 4K HEVC 10-bit, Dolby Vision (prof 5/8)
 *   - H.264: "c2.avc" / "OMX.AMLogic.avc.decode"
 *
 * Iskelet:
 *   open(codecId, w, h, surface)  -> MediaCodec olustur, surface bagla, start
 *   pump()                         -> nativePollVideoPkt ile packet cek,
 *                                     MediaCodec input'a yaz (HW decode -> surface)
 *
 * Codec id mapping (CodecBridge.nativeVideoCodecId()):
 *   131 = H.264 -> "video/avc"
 *   173 = HEVC  -> "video/hevc"
 */
class VideoDecoder {

    private var codec: MediaCodec? = null
    private var running = false
    private val looperHandler = Handler(Looper.getMainLooper())

    /**
     * MediaCodec'i hazirla. HEVC icin `codecId == 173`
     * (AV_CODEC_ID_HEVC), H.264 icin `codecId == 131`.
     */
    fun open(codecId: Int, width: Int, height: Int, surface: Surface) {
        val mime = when (codecId) {
            173 -> "video/hevc"
            131 -> "video/avc"
            else -> error("desteklenmeyen video codec id=$codecId (sadece HEVC/H264 iskelet)")
        }
        val fmt = MediaFormat.createVideoFormat(mime, width, height)
        // HW HW-accelerated HW HW HW
        fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4 * 1024 * 1024) // S912 4K 10bit icin bol
        val c = MediaCodec.createDecoderByType(mime) // 7.1'de codec2/OMX otomatik secer
        c.configure(fmt, surface, null, 0)
        c.start()
        this.codec = c
        this.running = true

        // HW decode besleme dongusu (audio dongusuyla paralel)
        thread(name = "srt-video") { pump() }
    }

    /**
     * Video pipeline beslemesi: native demuxer'dan packet cek,
     * MediaCodec input'a yaz. HW cikan Surface'e basilir (4K HEVC).
     *
     * Senkronizasyon: FFmpeg cizilen video paketi PTS'i (mikrosecond)
     * queueInputBuffer(presentationTimeUs) olarak verilir. Audio dongusu
     * da ayni cizgiyi kullandiginda, HW decoder'inin gecikme telafisi
     * (S912 4K HEVC ~3-5 frame) icinde video+audio senkron kalir.
     *
     * 7.1.2'te MediaCodec HW, presentationTimeUs'u absolute cizgiye
     * degil, ilk frame'e gore relative cizgiye yorabilir (bazi
     * AMLogic derlemeleri). Bu yuzden ilk PTS'i sabitleyip
     * (firstPtsUs) tum PTS'leri (pts - firstPtsUs) ile relative'e
     * ceviririz. Audio dongusu da ayni cizgiyi kullanir.
     */
    private fun pump() {
        val buf = ByteArray(4 * 1024 * 1024)
        val ptsOut = LongArray(1)
        val c = codec ?: return
        val inBufs = arrayOfNulls<ByteBuffer>(c.inputBufferCount)
        for (i in 0 until c.inputBufferCount) inBufs[i] = c.getInputBuffer(i)
        var firstPtsUs: Long? = null
        while (running) {
            val n = CodecBridge.nativePollVideoPkt(buf, ptsOut)
            if (n < 0) break
            val inIdx = c.dequeueInputBuffer(10_000)
            if (inIdx < 0) continue
            val inBuf = inBufs[inIdx]!!
            inBuf.clear()
            inBuf.put(buf, 0, n.toInt())
            val absPts = ptsOut[0]
            val relPts = if (firstPtsUs == null) {
                firstPtsUs = absPts
                0L
            } else {
                absPts - (firstPtsUs ?: 0L)
            }
            c.queueInputBuffer(inIdx, 0, n.toInt(), relPts, 0)
        }
    }

    fun stop() {
        running = false
        codec?.let {
            try { it.stop() } catch (_: Exception) {}
            try { it.release() } catch (_: Exception) {}
        }
        codec = null
    }

    fun release() {
        stop()
    }
}
