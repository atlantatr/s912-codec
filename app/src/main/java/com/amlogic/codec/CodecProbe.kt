package com.amlogic.codec

import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaFormat
import android.util.Log

/**
 * Sistemin gercekten hangi ses codec'lerini cozebildigini olcer.
 *
 * Amlogic imajlarinda media_codecs.xml'de codec tanimli olsa bile
 * kutuphane eslemesi bozuk olabiliyor (or. AC3 -> libstagefright_soft_dcvdec.so).
 * Bu sinif hem tanimi hem de codec'in gercekten olusturulabildigini kontrol eder.
 */
object CodecProbe {

    private const val TAG = "S912Codec"

    private val mimes = listOf(
        "audio/ac3" to "AC3 (Dolby Digital)",
        "audio/eac3" to "EAC3 (Dolby Digital Plus)",
        "audio/vnd.dts" to "DTS",
        "audio/dtshd" to "DTS-HD",
        "audio/truehd" to "TrueHD",
        "audio/mpeg-L2" to "MP2 (DVB)",
        "audio/mp4a-latm" to "AAC"
    )

    /** @return insan tarafindan okunabilir, satir satir rapor. */
    fun report(): String {
        val sb = StringBuilder()
        val list = MediaCodecList(MediaCodecList.REGULAR_CODECS)

        for ((mime, label) in mimes) {
            val name = try {
                list.findDecoderForFormat(MediaFormat.createAudioFormat(mime, 48000, 2))
            } catch (e: Exception) {
                null
            }
            if (name == null) {
                sb.append("$label: YOK\n")
                Log.w(TAG, "kodek $mime -> tanimli degil")
                continue
            }
            val ok = try {
                val c = MediaCodec.createByCodecName(name)
                c.release()
                true
            } catch (e: Exception) {
                Log.e(TAG, "kodek $mime -> $name OLUSTURULAMADI: ${e.message}")
                false
            }
            sb.append("$label: ${if (ok) "OK" else "BOZUK"} ($name)\n")
            if (ok) Log.i(TAG, "kodek $mime -> $name OK")
        }
        return sb.toString().trimEnd()
    }
}
