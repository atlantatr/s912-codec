package com.amlogic.codec;

/**
 * JNI kosesi - codec_core.cpp'ye bir kapi.
 *
 * SRT URL'si acilir, audio PCM (veya passthrough bitstream) native'dan cekilir,
 * MediaCodec/AudioTrack Java (Kotlin) tarafinda yonetilir.
 *
 * Kullanma oncesi:
 *   1. prebuilt/  klasorune build_libsrt.sh + build_ffmpeg.sh ciktisini koy
 *   2. app/ build.gradle -> externalNativeBuild + jniLibs ayari
 */
public final class CodecBridge {

    static {
        System.loadLibrary("codec_core");
    }

    private CodecBridge() {}

    /**
     * SRT (veya dogrudan dosya/URL) demuxer'ini acar.
     * @param url        srt://host:port?mode=listener  /  srt://host:port?mode=caller  /  dosya yolu
     * @param sampleRate 0 = native duzenleme; yoksa PCM cikiş sample rate
     * @param channels   0 = duzenleme; yoksa PCM cikiş kanal sayisi
     * @return 0 baarili, <0 basarisiz
     */
    public static native int    nativeOpen(String url, int sampleRate, int channels);

    /**
     * Tek PCM adimi cek (s16, stereo cikiş).
     * @param out Buffer (ses icin en az 4 byte)
     * @return Cikan PCM byte sayisi, -1 hata / kaynak tamsandi
     */
    public static native long   nativePollAudioPcm(byte[] out);

    /** PCM cikiş sample rate */
    public static native int    nativeAudioRate();

    /** PCM cikiş kanal sayisi */
    public static native int    nativeAudioChannels();

    /** Kaynaklari kaptirir (avformat_close + codec + swr) */
    public static native void   nativeClose();

    // ---- Passthrough JNI (DVB/Dolby-DTS) ----

    /** Audio codec, passthrough yapilabilir mi? 1 = evet, 0 = hayir (PCM) */
    public static native int    nativeCanPassThrough();

    /**
     * Audio codec_id (FFmpeg 6.1 AVCodecID, libavcodec/codec_id.h):
     *   13 = TRUEHD, 14 = DTS, 87 = AC3, 88 = EAC3
     * Java tarafinda AudioFormat.ENCODING_AC3/EAC3/DTS/TRUEHD secimi icin.
     */
    public static native int    nativeAudioCodecId();

    /**
     * Passthrough bitstream adimi cek (ham AC3/EAC3/DTS/TrueHD).
     * AudioTrack offload (ENCODING_AC3/DTS/EAC3/TRUEHD) veya
     * /dev/audiodsp0 raw node'a basilir.
     * @return byte sayisi (>0), -1 kaynak tamsandi / passthrough yok
     */
    public static native long   nativePollAudioPassThrough(byte[] out);

    // ---- Video JNI (MediaCodec HW decode beslemesi) ----

    /** Kaynakta video stream var mi? 1 = var, 0 = yok (audio-only) */
    public static native int    nativeHasVideo();

    /**
     * Video codec_id (FFmpeg AVCodecID).
     *   131 = H.264/AVC  -> MediaCodec mime "video/avc"
     *   173 = HEVC       -> MediaCodec mime "video/hevc"  (S912 4K 10-bit DV)
     */
    public static native int    nativeVideoCodecId();

    /** Kaynak cozunurlugu (MediaCodec create ile giris; 0 = belirsiz) */
    public static native int    nativeVideoWidth();

    /** Kaynak cozunurlugu (yukseklik) */
    public static native int    nativeVideoHeight();

    /**
     * Tek video bitstream paketi cek (MediaCodec input'a yazilir).
     * @param out      Buffer
     * @param ptsOutUs [1] cikan pakete ait PTS (mikrosecond) —
     *                 MediaCodec.queueInputBuffer presentationTimeUs icin
     * @return byte sayisi (>0), -1 kaynak tamsandi / video yok
     */
    public static native long   nativePollVideoPkt(byte[] out, long[] ptsOutUs);

    // ---- TS Feed JNI (DVB/CIx -> FFmpeg demuxer'a byte besleme) ----

    /**
     * TS feed modu ac (mpegts demuxer'i memory buffer'dan okur).
     * @param fmtName "mpegts"
     * @return 0 baarili
     */
    public static native int    nativeOpenTSFeed(String fmtName);

    /**
     * TS byte'larini demuxer'a bas (feed modunda).
     * @param data 188-multiples TS byte'lari
     * @return basilan byte (>0), -1 buffer tami / hata
     */
    public static native long   nativeFeedTSBytes(byte[] data);

    /** TS feed modu kapati (buffer + avio free) */
    public static native void   nativeCloseTSFeed();
}
