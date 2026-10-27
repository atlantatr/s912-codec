package com.amlogic.codec;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.util.Log;
import android.view.Surface;

import kotlin.concurrent.thread;

/**
 * S912 4K HEVC / H.264 HW decode (MediaCodec -> Surface).
 *
 * FFmpeg demuxer'dan gelen ham video paketleri nativePollVideoPkt ile alinir,
 * MediaCodec input buffer'a yazilir. S912'de codec2/OMX Amlogic HW decoder
 * otomatik secilir.
 */
class VideoDecoder {

    private var codec: MediaCodec? = null
    private var running = false
    private var pumpThread: Thread? = null

    fun open(codecId: Int, width: Int, height: Int, surface: Surface) {
        val mime = when (codecId) {
            CodecBridge.CODEC_ID_HEVC -> "video/hevc"
            CodecBridge.CODEC_ID_H264 -> "video/avc"
            else -> throw IllegalArgumentException("desteklenmeyen video codec id=$codecId")
        }

        val w = if (width > 0) width else 1920
        val h = if (height > 0) height else 1080
        val fmt = MediaFormat.createVideoFormat(mime, w, h)
        fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 8 * 1024 * 1024)

        val c = MediaCodec.createDecoderByType(mime)
        c.configure(fmt, surface, null, 0)
        c.start()
        codec = c
        running = true

        pumpThread = thread(name = "srt-video") { pump(c) }
        Log.i(TAG, "MediaCodec acildi: $mime ${w}x$h")
    }

    private fun pump(c: MediaCodec) {
        val buf = ByteArray(8 * 1024 * 1024)
        val ptsOut = LongArray(1)
        val info = MediaCodec.BufferInfo()

        var firstPtsUs: Long? = null

        try {
            while (running) {
                // Decoder output'unu surekli bosalt; aksi halde MediaCodec kilitlenir.
                drainOutput(c, info)

                val n = CodecBridge.nativePollVideoPkt(buf, ptsOut)
                if (n < 0) {
                    val idx = c.dequeueInputBuffer(10_000)
                    if (idx >= 0) {
                        c.queueInputBuffer(
                            idx, 0, 0, 0,
                            MediaCodec.BUFFER_FLAG_END_OF_STREAM
                        )
                    }
                    break
                }
                if (n == 0L) {
                    Thread.sleep(2)
                    continue
                }

                val inIdx = c.dequeueInputBuffer(10_000)
                if (inIdx < 0) {
                    // Paket kaybolmasin diye kisa bekle; pratikte input buffer
                    // 8 MB oldugu icin bu dal nadir.
                    Thread.sleep(2)
                    continue
                }
                val inBuf = c.getInputBuffer(inIdx) ?: continue
                inBuf.clear()
                val size = n.toInt().coerceAtMost(inBuf.capacity())
                inBuf.put(buf, 0, size)

                val absPts = ptsOut[0]
                val relPts = if (firstPtsUs == null) {
                    firstPtsUs = absPts
                    0L
                } else {
                    (absPts - (firstPtsUs ?: 0L)).coerceAtLeast(0L)
                }

                c.queueInputBuffer(inIdx, 0, size, relPts, 0)
            }
        } catch (e: Exception) {
            Log.e(TAG, "video pump hatasi: ${e.message}")
        } finally {
            running = false
        }
    }

    private fun drainOutput(c: MediaCodec, info: MediaCodec.BufferInfo) {
        while (true) {
            val outIdx = c.dequeueOutputBuffer(info, 0)
            when {
                outIdx >= 0 -> c.releaseOutputBuffer(outIdx, true)
                outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED ->
                    Log.i(TAG, "output format: ${c.outputFormat}")
                outIdx == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                outIdx == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED -> Unit
                else -> return
            }
        }
    }

    fun stop() {
        running = false
        pumpThread?.join(2000)
        pumpThread = null
        codec?.let {
            try { it.stop() } catch (_: Exception) {}
            try { it.release() } catch (_: Exception) {}
        }
        codec = null
    }

    fun release() = stop()

    companion object {
        private const val TAG = "S912Video"
    }
}
