package com.amlogic.codec;

/**
 * JNI koprusu - codec_core.cpp.
 *
 * Tum metotlar static'tir; native taraf da jclass alir.
 */
public final class CodecBridge {

    static {
        System.loadLibrary("codec_core");
    }

    private CodecBridge() {}

    // FFmpeg 6.1 AVCodecID degerleri (libavcodec/codec_id.h).
    // AudioFormat encoding secimi ve MediaCodec mime secimi icin.
    public static final int CODEC_ID_H264   = 27;      // 0x1B
    public static final int CODEC_ID_HEVC   = 173;     // 0xAD
    public static final int CODEC_ID_AC3    = 0x15003; // 86019
    public static final int CODEC_ID_DTS    = 0x15004; // 86020
    public static final int CODEC_ID_EAC3   = 0x15028; // 86056
    public static final int CODEC_ID_TRUEHD = 0x1502C; // 86060

    /** SRT/dosya demuxer'ini acar. 0 basarili, <0 hata. */
    public static native int nativeOpen(String url, int sampleRate, int channels);

    /** FFmpeg decode -> s16 PCM. Cikan byte sayisi, 0 veri yok, -1 bitti/hata. */
    public static native long nativePollAudioPcm(byte[] out);

    public static native int nativeAudioRate();

    public static native int nativeAudioChannels();

    /** Demuxer + decoder + resampler kapatir. */
    public static native void nativeClose();

    /** AC3/EAC3/DTS/TrueHD ise 1, degilse 0. */
    public static native int nativeCanPassThrough();

    /** Kaynak audio AVCodecID (CODEC_ID_* sabitleri). */
    public static native int nativeAudioCodecId();

    /** Ham AC3/EAC3/DTS/TrueHD bitstream. Cikan byte sayisi, 0 veri yok, -1 bitti. */
    public static native long nativePollAudioPassThrough(byte[] out);

    public static native int nativeHasVideo();

    /** Kaynak video AVCodecID (CODEC_ID_H264 / CODEC_ID_HEVC). */
    public static native int nativeVideoCodecId();

    public static native int nativeVideoWidth();

    public static native int nativeVideoHeight();

    /**
     * Tek video paketi + PTS (mikrosecond).
     * @return byte sayisi, 0 veri yok, -1 bitti/hata
     */
    public static native long nativePollVideoPkt(byte[] out, long[] ptsOutUs);

    /** DVB/CIx icin mpegts feed modu acar. */
    public static native int nativeOpenTSFeed(String fmtName);

    /** DVB/CIx TS byte'larini FFmpeg demuxer'a basar. */
    public static native long nativeFeedTSBytes(byte[] data);

    /** TS feed modunu kapatir. */
    public static native void nativeCloseTSFeed();
}
