package com.amlogic.codec;

/**
 * /dev/dvb/frontend0 (DVB-S2 tuner) JNI kosesi.
 *
 * S912 CI/DVB kutularinda, modulu takiliysa /dev/dvb/frontend0 +
 * /dev/dvr0/dvr0 vardir. Bu sinif:
 *   - dvbOpen(0)                  -> frontend + dvr ac
 *   - dvbTuneS2(freq, pol, lo, hi, sr, num, den) -> LNB ayari
 *   - dvbPollTS(buf)              -> 188-byte TS paketi ceker (FFmpeg'e /
 *                                     SRT sunucusuna basilir)
 *
 * Kotlin tarafinda, SRT-TS besleyicisi (SrtCodecPlayer / codec_core.cpp)
 * bu TS ciktisini avformat'a baglayarak kullanir.
 */
public final class DvbFrontend {

    static { System.loadLibrary("codec_core"); }

    private DvbFrontend() {}

    /**
     * Frontend + DVR'i acar.
     * @param adapter Genelde 0
     * @return 0 baarili, <0 basarisiz
     */
    public static native int  nativeDvbOpen(int adapter);

    /**
     * DVB-S2 tuning (LNB universal).
     * @param freqHz  transponder merkezi frekansi (Hz)
     * @param polH    true=H, false=V
     * @param loFreq  LNB Lo bant frekansi (Hz) (varsayilan 9750000)
     * @param hiFreq  LNB Hi bant frekansi (Hz) (varsayilan 10600000)
     * @param sr      symbol rate (kSym/s)
     * @param num/den FEC oran (7/8)
     * @return 0 baarili
     */
    public static native int  nativeDvbTuneS2(long freqHz, boolean polH,
                                               long loFreq, long hiFreq,
                                               int sr, int num, int den);

    /**
     * TS paketleri oku.
     * @param out Buffer (en az 188 byte)
     * @return Okunan byte sayisi (>0), -1 kaynak tamsandi / hata
     */
    public static native long nativeDvbPollTS(byte[] out);

    /** Fe + dvr close */
    public static native void nativeDvbClose();

    // ---- CIx / CI besleme (tuner kartinin TS ciktisini FFmpeg'e basmak) ----

    /**
     * CIx beslemesini acar (tuner kartin CI slotundan gelen TS'yi okur).
     * @param ciSlot CI slot index (genelde 0)
     * @return fd (>0), -1 hata
     */
    public static native int  nativeCixOpen(int ciSlot);

    /**
     * CIx'ten 188-byte TS paketleri oku.
     * @param ciFd nativeCixOpen dondugu fd
     * @param out  Buffer (en az 188)
     * @return byte sayisi (>0), -1 yok/hata
     */
    public static native long nativeCixPoll(int ciFd, byte[] out);

    /** CIx beslemesini kaptirir. */
    public static native void nativeCixClose(int ciFd);
}
