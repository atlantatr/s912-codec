package com.amlogic.codec

import android.util.Log
import java.io.File

/**
 * Amlogic dijital ses yolu anahtari.
 *
 * /sys/class/audiodsp/digital_raw:
 *   0 = PCM      (kutu AC3/DTS'i cozer, cok kanalli PCM verir)
 *   1 = RAW      (IEC61937 bitstream - AC3/EAC3/DTS dogrudan HDMI'a)
 *
 * Bu dugum Amlogic 7.1 kutularinda rw-rw-rw- (dunya yazilabilir) olur,
 * yani root GEREKMEZ. Kodi benzeri oynaticilar da ayni dugumu kullanir.
 *
 * Not: bitstream'in TV/AVR tarafindan cozulebilmesi icin HDMI EDID'inde
 * ilgili ses formatinin (AC3/DTS) bildirilmis olmasi gerekir. Bildirilmemisse
 * kutu raw moda gecse bile karsi taraf sessiz kalir; bu yuzden mod her
 * durumda geri alinir (0).
 */
object AmlogicRawAudio {

    private const val TAG = "S912Codec"

    private val NODES = arrayOf(
        "/sys/class/audiodsp/digital_raw",
        "/sys/class/amaudio/digital_raw"
    )

    /** @return true ise en az bir dugum yazildi. */
    fun setRawMode(raw: Boolean): Boolean {
        val value = if (raw) "1" else "0"
        var written = false
        for (path in NODES) {
            val f = File(path)
            if (!f.exists()) continue
            try {
                f.writeText(value)
                written = true
                Log.i(TAG, "digital_raw <- $value ($path)")
            } catch (e: Exception) {
                Log.w(TAG, "digital_raw yazilamadi ($path): ${e.message}")
            }
        }
        if (!written) Log.w(TAG, "digital_raw dugumu bulunamadi/yazilamadi")
        return written
    }

    /** @return 0 = PCM, 1 = RAW, -1 = okunamadi. */
    fun readRawMode(): Int {
        for (path in NODES) {
            val f = File(path)
            if (!f.exists()) continue
            try {
                return f.readText().trim().toInt()
            } catch (_: Exception) {
            }
        }
        return -1
    }

    /** Bitstream icin kutu raw moda alinabiliyor mu? */
    fun isAvailable(): Boolean {
        for (path in NODES) {
            val f = File(path)
            if (f.exists() && f.canWrite()) return true
        }
        return false
    }
}
