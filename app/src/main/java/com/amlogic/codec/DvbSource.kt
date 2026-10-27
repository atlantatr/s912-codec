package com.amlogic.codec

import android.util.Log
import kotlin.concurrent.thread

/**
 * DVB / CIx besleme -> FFmpeg demuxer'a TS byte besleme katmani.
 *
 * Iki ihtimali birlestirir:
 *   1. /dev/dvb (tuner karti, DVB-S2)   -> DvbFrontend.nativeDvbPollTS
 *   2. CIx (CI slotundan TS)            -> DvbFrontend.nativeCixPoll
 *
 * Cikan 188-byte TS paketleri, codec_core.cpp'deki custom avio (TS feed
 * modu) uzerinden FFmpeg mpegts demuxer'a basilir. Boylece audio /
 * passthrough / video katmanlari DVB kaynagindan da calisir.
 *
 * Kullanilim:
 *   val src = DvbSource()
 *   src.openTuner(frontend=0, freqHz=11330_000_000, polH=true, sr=29700)
 *   src.start()           // -> CodecBridge TS feed dongusu
 *   ... src.stop(); src.release()
 */
class DvbSource {

    enum class Kind { TUNER, CIX }

    private var kind = Kind.TUNER
    private var ciFd = -1
    private var pumpThread: Thread? = null
    private var running = false
    private val buf = ByteArray(188 * 188) // ~32KB, bol

    /**
     * Tuner'i ac + tune et. SRT/CIx demuxer'a TS feed modu acilir.
     * @return 0 baarili, <0 basarisiz
     */
    fun openTuner(frontend: Int = 0, freqHz: Long, polH: Boolean,
                  loFreq: Long = 9_750_000, hiFreq: Long = 10_600_000,
                  sr: Int = 29700, fecNum: Int = 7, fecDen: Int = 8): Boolean {
        kind = Kind.TUNER
        if (DvbFrontend.nativeDvbOpen(frontend) != 0) return false
        if (DvbFrontend.nativeDvbTuneS2(freqHz, polH, loFreq, hiFreq, sr, fecNum, fecDen) != 0)
            return false
        CodecBridge.nativeOpenTSFeed("mpegts") // demuxer'i feed moduna al
        return true
    }

    /**
     * CIx beslemesini ac (CI slotundan TS).
     */
    fun openCix(slot: Int = 0): Boolean {
        kind = Kind.CIX
        ciFd = DvbFrontend.nativeCixOpen(slot)
        if (ciFd < 0) return false
        CodecBridge.nativeOpenTSFeed("mpegts")
        return true
    }

    /** DVB/CIx'den TS cek, FFmpeg demuxer'a bas (pump dongusu). */
    fun start() {
        if (running) return
        running = true
        pumpThread = thread(name = "dvb-feed") {
            while (running) {
                val n = if (kind == Kind.TUNER)
                    DvbFrontend.nativeDvbPollTS(buf)
                else
                    DvbFrontend.nativeCixPoll(ciFd, buf)
                if (n <= 0) {
                    // kaynak hicbir sey vermiyor; kisa bekle, henuz sinyal yok
                    try { Thread.sleep(50) } catch (_: InterruptedException) { break }
                    continue
                }
                CodecBridge.nativeFeedTSBytes(buf.copyOfRange(0, n.toInt()))
            }
            running = false
        }
    }

    fun stop() {
        running = false
        pumpThread?.join()
        pumpThread = null
    }

    fun release() {
        stop()
        if (kind == Kind.CIX && ciFd >= 0) {
            DvbFrontend.nativeCixClose(ciFd); ciFd = -1
        } else {
            DvbFrontend.nativeDvbClose()
        }
        CodecBridge.nativeCloseTSFeed()
    }
}
