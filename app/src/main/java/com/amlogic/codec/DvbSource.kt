package com.amlogic.codec

import android.util.Log
import kotlin.concurrent.thread

/**
 * DVB / CIx -> FFmpeg mpegts feed.
 *
 * 188-byte TS paketleri /dev/dvb/.../dvr0 veya /dev/ciN'den okunur,
 * CodecBridge.nativeFeedTSBytes ile native demuxer'a basilir.
 */
class DvbSource {

    enum class Kind { TUNER, CIX }

    private var kind = Kind.TUNER
    private var ciFd = -1
    private var pumpThread: Thread? = null
    private var running = false
    private val buf = ByteArray(188 * 188)

    /**
     * Tuner'i ac + DVB-S2 tune et ve FFmpeg feed modunu baslat.
     *
     * @param freqHz  transponder frekansi (Hz, ornek 11330000000)
     * @param loFreq  LNB low-band LO (Hz, 9750000000)
     * @param hiFreq  LNB high-band LO (Hz, 10600000000)
     */
    fun openTuner(
        frontend: Int = 0,
        freqHz: Long,
        polH: Boolean,
        loFreq: Long = 9_750_000_000L,
        hiFreq: Long = 10_600_000_000L,
        sr: Int = 29700,
        fecNum: Int = 7,
        fecDen: Int = 8
    ): Boolean {
        kind = Kind.TUNER
        if (DvbFrontend.nativeDvbOpen(frontend) != 0) return false
        if (DvbFrontend.nativeDvbTuneS2(freqHz, polH, loFreq, hiFreq, sr, fecNum, fecDen) != 0) {
            DvbFrontend.nativeDvbClose()
            return false
        }
        return CodecBridge.nativeOpenTSFeed("mpegts") == 0
    }

    fun openCix(slot: Int = 0): Boolean {
        kind = Kind.CIX
        ciFd = DvbFrontend.nativeCixOpen(slot)
        if (ciFd < 0) return false
        return CodecBridge.nativeOpenTSFeed("mpegts") == 0
    }

    fun start() {
        if (running) return
        running = true

        pumpThread = thread(name = "dvb-feed") {
            try {
                while (running) {
                    val n = if (kind == Kind.TUNER) {
                        DvbFrontend.nativeDvbPollTS(buf)
                    } else {
                        DvbFrontend.nativeCixPoll(ciFd, buf)
                    }

                    if (n <= 0) {
                        Thread.sleep(50)
                        continue
                    }

                    CodecBridge.nativeFeedTSBytes(buf.copyOfRange(0, n.toInt()))
                }
            } catch (e: InterruptedException) {
                Log.i(TAG, "dvb feed durduruldu")
            } catch (e: Exception) {
                Log.e(TAG, "dvb feed hatasi: ${e.message}")
            } finally {
                running = false
            }
        }
    }

    fun stop() {
        running = false
        pumpThread?.join(2000)
        pumpThread = null
    }

    fun release() {
        stop()
        if (kind == Kind.CIX && ciFd >= 0) {
            DvbFrontend.nativeCixClose(ciFd)
            ciFd = -1
        } else {
            DvbFrontend.nativeDvbClose()
        }
        CodecBridge.nativeCloseTSFeed()
    }

    companion object {
        private const val TAG = "S912Dvb"
    }
}
