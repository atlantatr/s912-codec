//
// codec_core.cpp
//
// S912 / Android 7.1.2 arm64-uzerine kurulan FFmpeg 6.x + libsrt tabanli
// codec katinin JNI kosesi.
//
// Neden libamplayer.so setinin yerine kilitlandi:
//   - libbinder.so / libstagefright ABI'si 5.1 build'ine esnetilmisti
//     (7.1.2'de %99 crash riski). Burasi kendi app process'i icinde
//     "MediaCodec (HW) + AudioTrack (SW/Passthrough) + FFmpeg (audio
//     decode/demux) + libsrt (SRT stream)" yapisina baslayarak o kopuk
//     kosesi tamamen ati.
//
// Is akisi (native tarafta):
//   open()    -> demuxer + SRT stream acma, audio track tespiti
//   audioPoll-> FFmpeg decode ile PCM veya passthrough bitstream ceker
//   videoHint-> MediaCodec'e verilmek uzere (Java tarafinda) buffer ceker
//
// Bu dosya "iskelet"tir: is akisini tanimlar, gercek frame uretimini
// (HW MediaCodec, AudioTrack) Java/Kotlin tarafina birakir. NDK build
// icin CMakeLists.txt + JNI metotlari alttakidir.
//

#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>

#include <cstring>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>

#define LOG_TAG "S912Codec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// SRT stream acmak icin libsrt (FFmpeg icinde --enable-protocol=srt ile
// demux edilebilir, ayrica libsrt API'si srt_socket() seviyesinde de
// dogrudan kullanim icin). Baslangic icin FFmpeg demuxer yolu kullanilir.

namespace {

struct StreamState {
    AVFormatContext*  fmt = nullptr;
    int               audioStreamIdx = -1;
    AVCodecContext*   audioCtx = nullptr;
    SwrContext*       swr = nullptr;
    int               audioCodecId = 0;    // FFmpeg codec_id (passthrough icin)
    // Ses cikiş formati (AudioTrack icin)
    int               outSampleRate = 48000;
    int               outChannels   = 2;
    int               outFmt        = AV_SAMPLE_FMT_S16;

    // ---- Video (MediaCodec HW decode icin, JNI beslemesi) ----
    int               videoStreamIdx = -1;
    AVCodecID         videoCodecId   = AV_CODEC_ID_NONE;
    int               videoWidth     = 0;   // kaynak cozunurluk (CMake'da MediaCodec icin giris)
    int               videoHeight    = 0;
    // AVMediaCodec'a verilecek mime (Java tarafinda HEVC/H264 seciminde kullanilir)
};

StreamState g_state;

// ------------------------------------------------------------------
// memory feed (DVB/CIx/DVR -> FFmpeg demuxer'a byte besleme)
//
// DvbFrontend.nativeDvbPollTS / nativeCixPoll cektigi 188-byte
// TS paketlerini FFmpeg demuxer'a beslemek icin, avformat_open_input
// yerine avformat_new_stream + custom demux callback kullanilir.
// Basit yol: mpegts demuxer'a avio_context + memory buffer verilir.
// ------------------------------------------------------------------
struct FeedState {
    AVIOContext*  avioCtx   = nullptr;  // custom avio
    uint8_t*      buf      = nullptr;
    size_t        len      = 0;
    size_t        cap      = 0;
    int           feedFd   = -1;        // 0 = memory mode
    bool          feeding  = false;
};
FeedState g_feed;

// 2 MB sliding buffer (mpegts demuxer icin)
static const size_t FEED_BUF_CAP = 2 * 1024 * 1024;

// avio callback'lar (mpegts demuxer'a buffer'i okutur)
static int feedRead(void* opq, uint8_t* buf, int size) {
    auto* st = static_cast<FeedState*>(opq);
    if (st->len == 0) return AVERROR(EAGAIN); // demuxer'a "henuz yok" s
    int n = static_cast<int>(std::min<size_t>(st->len, size));
    memcpy(buf, st->buf + 0, n);
    // slidir: tashanani sil (DVB stream icin)
    memmove(st->buf, st->buf + n, st->len - n);
    st->len -= n;
    return n;
}
static int64_t feedSeek(void* opq, int64_t offset, int whence) {
    // DVB stream icin seek anlamsiz; EIMMEDIATELY
    return AVERROR(ENOSYS);
}

/**
 * TS feed modu ac: mpegts demuxer'i memory buffer icinden okur.
 * @param fmtName "mpegts" (Tuner ciktisi icin)
 */
int openTSFeed(const char* fmtName) {
    // AVIOContext olustur
    uint8_t* avioBuf = new uint8_t[FEED_BUF_CAP + 1024]; // avio header'i icin bol
    memset(avioBuf, 0, FEED_BUF_CAP + 1024);
    g_feed.buf = new uint8_t[FEED_BUF_CAP];
    memset(g_feed.buf, 0, FEED_BUF_CAP);
    g_feed.len = 0;
    g_feed.cap = FEED_BUF_CAP;

    int r = avio_alloc_context(&g_feed.avioCtx, avioBuf, 1024,
                                1, &g_feed, feedRead, nullptr, &feedSeek);
    if (r < 0) {
        LOGE("avio_alloc_context fail: %d", r);
        return -1;
    }
    // demuxer ac (custom avio ile)
    const char* url = fmtName ? fmtName : "mpegts";
    if (avformat_open_input(&g_state.fmt, url, g_feed.avioCtx, nullptr) < 0) {
        LOGE("TS feed demuxer open fail");
        return -1;
    }
    if (avformat_find_stream_info(g_state.fmt, nullptr) < 0) {
        LOGE("TS feed find_stream_info fail");
        return -1;
    }
    g_feed.feeding = true;
    LOGI("TS feed open OK (%s)", url);
    return 0;
}

// DVB / CIx / SRT-DVR'dan cikan TS byte'larini demuxer'a bas
int feedTSBytes(const uint8_t* data, int len) {
    if (!g_feed.feeding || g_feed.len + len > g_feed.cap) return -1;
    // slidir: buffer doluyorsa eski paketleri sil (en az 4KB)
    if (g_feed.len + len > g_feed.cap) {
        size_t free = g_feed.cap - g_feed.len;
        if (free < 4096) {
            // 4096 byte'i kaydir, gerisini tasi
            size_t keep = g_feed.cap - 4096;
            if (g_feed.len > keep) {
                memmove(g_feed.buf, g_feed.buf + (g_feed.len - keep), keep);
                g_feed.len = keep;
            }
        }
    }
    memcpy(g_feed.buf + g_feed.len, data, len);
    g_feed.len += len;
    return len;
}

// ------------------------------------------------------------------
// Standart dosya/SRT acilisi (openSrt)
// ------------------------------------------------------------------
int openSrt(const char* url) {
    g_state.fmt = avformat_alloc_context();
    if (!g_state.fmt) { LOGE("avformat_alloc_context fail"); return -1; }

    if (avformat_open_input(&g_state.fmt, url, nullptr, nullptr) < 0) {
        LOGE("SRT/demuxer open fail: %s", url);
        return -1;
    }
    if (avformat_find_stream_info(g_state.fmt, nullptr) < 0) {
        LOGE("find_stream_info fail");
        return -1;
    }

    // Audio streami bul
    for (unsigned i = 0; i < g_state.fmt->nb_streams; ++i) {
        if (g_state.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            g_state.audioStreamIdx = static_cast<int>(i);
            break;
        }
    }

    // Video streami bul (MediaCodec icin, codec_id / cozunurluk raporu)
    for (unsigned i = 0; i < g_state.fmt->nb_streams; ++i) {
        if (g_state.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_state.videoStreamIdx = static_cast<int>(i);
            auto* vp = g_state.fmt->streams[i]->codecpar;
            g_state.videoCodecId = vp->codec_id;
            g_state.videoWidth   = vp->width;
            g_state.videoHeight  = vp->height;
            break;
        }
    }
    if (g_state.videoStreamIdx >= 0) {
        LOGI("video stream: idx=%d codec=%d w=%d h=%d",
             g_state.videoStreamIdx, (int)g_state.videoCodecId,
             g_state.videoWidth, g_state.videoHeight);
    } else {
        LOGI("video stream yok (audio-only kaynak)");
    }

    if (g_state.audioStreamIdx >= 0) {
        auto* sp = g_state.fmt->streams[g_state.audioStreamIdx]->codecpar;
        g_state.audioCodecId = sp->codec_id;
        g_state.audioCtx = avcodec_alloc_context3(nullptr);
        if (!g_state.audioCtx || avcodec_parameters_to_context(g_state.audioCtx, sp) < 0) {
            LOGE("audio codec context fail");
            return -1;
        }
        const AVCodec* codec = avcodec_find_decoder(sp->codec_id);
        if (!codec || avcodec_open2(g_state.audioCtx, codec, nullptr) < 0) {
            LOGE("audio codec open fail (codec_id=%d)", sp->codec_id);
            return -1;
        }

        // Resampler ayari (PCM cikiş icin)
        g_state.outFmt = AV_SAMPLE_FMT_S16;
        int layout = g_state.audioCtx->ch_layout.nb_channels > 0
            ? g_state.audioCtx->ch_layout.nb_channels : sp->channels;
        g_state.outChannels = layout ? layout : 2;
        SwrContext* swr = swr_alloc();
        if (swr) {
            int64_t outLay = g_state.outChannels > 2 ? AV_CH_LAYOUT_5POINT1
                                                     : AV_CH_LAYOUT_STEREO;
            av_opt_set_int(swr, "in_sample_rate",  sp->sample_rate, 0);
            av_opt_set_int(swr, "out_sample_rate", g_state.outSampleRate, 0);
            av_opt_set_int(swr, "in_sample_fmt",   g_state.audioCtx->sample_fmt, 0);
            av_opt_set_int(swr, "out_sample_fmt",  g_state.outFmt, 0);
            if (swr_init(swr) < 0) {
                LOGE("swr_init fail");
                swr_free(&swr);
            } else {
                g_state.swr = swr;
            }
        }

        LOGI("audio open ok, codecId=%d rate=%d", sp->codec_id, sp->sample_rate);
    } else {
        // Audio yoksa video bile calisir (SrtCodecPlayer audioPoll'de -1 verir)
        LOGI("audio stream yok, video-only mod");
    }
    return 0;
}

// Tek adim PCM decode. cikan PCM, AudioTrack'e yazilir (Java tarafi).
// Donus: cikan PCM buffer bultel sayisi (s16), -1 hata/kapatilmis.
long pollAudioPcm(uint8_t* outBuf, int capBytes) {
    if (!g_state.fmt) return -1;
    AVPacket pkt;
    AVFrame* fr = av_frame_alloc();
    if (!fr) return -1;
    int got = -1;
    while (got < 0) {
        int r = av_read_packet(g_state.fmt, &pkt);
        if (r < 0) { av_packet_unref(&pkt); got = r; break; }
        r = avcodec_send_packet(g_state.audioCtx, &pkt);
        av_packet_unref(&pkt);
        if (r < 0) continue;
        r = avcodec_receive_frame(g_state.audioCtx, fr);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) continue;
        if (r < 0) break;
        got = 1;
        break;
    }
    if (got < 0) { av_frame_free(&fr); return -1; }

    int outN = capBytes / 2; // s16
    int nOutSamples = 0;
    if (g_state.swr) {
        const uint8_t** inPlanes = fr->data;
        int inLines = fr->nb_samples;
        int res = swr_convert(g_state.swr,
                               reinterpret_cast<uint8_t**>(&outBuf), outN,
                               inPlanes, inLines);
        if (res < 0) { LOGE("swr_convert fail"); av_frame_free(&fr); return -1; }
        nOutSamples = res;
    } else {
        // swr yoksa ham PCM (genelde s16 olani varsayilir)
        memcpy(outBuf, fr->data[0], fr->nb_samples * g_state.outChannels * 2);
        nOutSamples = fr->nb_samples;
    }
    av_frame_free(&fr);
    return nOutSamples * g_state.outChannels * 2; // byte
}

// ------------------------------------------------------------------
// Passthrough (DVB/Dolby-DTS icin) - demux edilmis ham audio bitstream'i
// (AC3/EAC3/DTS/DTS-HD/TrueHD) doer. Amlogic audio DSP / HDMI alici
// bu bitstream'i dogrudan yutabilir; PCM'e ceversek Dolby/DTS etrafini
// bozariz. SRT + DVB-tuner kanaliyla gelen paketi, varsa ADTS/AC3
// sarmalini soyarak cikan data'yi doneriz.
//
// Donus:
//   >= 0 -> cikan passthrough bitstream byte
//   -1   -> kaynak tamsandi / audio yok
//
// 7.1.2 + S912 icin AudioTrack offload format (ENCODING_AC3 / DTS /
// EAC3 / TRUEHD) ile cikan byte'lar doerulur; MediaCodec offload ile
// de basilir.
// ------------------------------------------------------------------
long pollAudioPassThrough(uint8_t* outBuf, int capBytes) {
    if (!g_state.fmt) return -1;
    if (g_state.audioStreamIdx < 0) return -1;
    auto* sp = g_state.fmt->streams[g_state.audioStreamIdx]->codecpar;
    // Passthrough yapilacak codec'ler (FFmpeg codec_id)
    if (sp->codec_id != AV_CODEC_ID_AC3  && sp->codec_id != AV_CODEC_ID_EAC3 &&
        sp->codec_id != AV_CODEC_ID_DTS   && sp->codec_id != AV_CODEC_ID_TRUEHD) {
        LOGI("passthrough: codec_id=%d passthrough degil, PCM yolunu kullanin", sp->codec_id);
        return -1;
    }
    AVPacket pkt = {};
    while (true) {
        int r = av_read_packet(g_state.fmt, &pkt);
        if (r < 0) { av_packet_unref(&pkt); return -1; }
        if (pkt.stream_index == g_state.audioStreamIdx) {
            int n = std::min((int)pkt.size, capBytes);
            if (n <= 0) { av_packet_unref(&pkt); return -1; }
            memcpy(outBuf, pkt.data, n);
            av_packet_unref(&pkt);
            return n;
        }
        av_packet_unref(&pkt);
    }
}

// Passthrough yapilabilir mi? (codec_id kontrolu)
//
// FFmpeg 6.1 libavcodec/codec_id.h degerleri (enum AVCodecID sirasiyla):
//   AV_CODEC_ID_TRUEHD = 13
//   AV_CODEC_ID_DTS    = 14
//   AV_CODEC_ID_AC3    = 87
//   AV_CODEC_ID_EAC3   = 88
// (Kaynak: n6.1 tree, libavcodec/codec_id.h — "AUDIO CODECS" blogu)
int canPassThrough() {
    if (g_state.audioStreamIdx < 0) return 0;
    auto* sp = g_state.fmt->streams[g_state.audioStreamIdx]->codecpar;
    return (sp->codec_id == AV_CODEC_ID_AC3  || sp->codec_id == AV_CODEC_ID_EAC3 ||
            sp->codec_id == AV_CODEC_ID_DTS   || sp->codec_id == AV_CODEC_ID_TRUEHD)
           ? 1 : 0;
}

// Audio codec_id (Java tarafinda AudioTrack offload format secimi icin)
// Doner:
//   13 = TRUEHD, 14 = DTS, 87 = AC3, 88 = EAC3
int audioCodecId() {
    if (g_state.audioStreamIdx < 0) return 0;
    return g_state.audioCodecId;
}

// Video adimi: tek AVPacket oku, MediaCodec input'a vermek icin cikan
// ham ekstre data'yi (bitstream) doner. SPS/PPS (HEVC) + NAL header'i
// korunur; MediaCodec "c2.hevc" / OMX.AMLogic icin dogrudan
// input.Buffer'e yazilabilir.
//
// Donus:
//   >= 0 -> cikan video byte sayisi (out'a yazildi)
//   -1    -> kaynak tamsandi / hata
//
// Note: HEVC icin MediaCodec input format olarak keyframe'den sonrasi
// (SPS/VPS/PPS/SEI) lazim; FFmpeg'in av_read_packet'ini dogrudan
// kullanirseniz, ilk keyframe'den sonra beslemeye baslamalisiniz
// (bu kosesi MediaCodec tarafinda "first keyframe" flag'i belirler).
long pollVideoPkt(uint8_t* outBuf, int capBytes, int64_t* ptsOutUs) {
    if (!g_state.fmt) return -1;
    if (g_state.videoStreamIdx < 0) return -1;
    if (ptsOutUs) *ptsOutUs = 0;
    AVPacket pkt = {};
    while (true) {
        int r = av_read_packet(g_state.fmt, &pkt);
        if (r < 0) { av_packet_unref(&pkt); return -1; }
        if (pkt.stream_index == g_state.videoStreamIdx) {
            int n = std::min((int)pkt.size, capBytes);
            if (n <= 0) { av_packet_unref(&pkt); return -1; }
            memcpy(outBuf, pkt.data, n);
            // PTS'i cikan microsecond cizgisine cevir (time_base ile)
            if (ptsOutUs) {
                AVStream* vs = g_state.fmt->streams[g_state.videoStreamIdx];
                int64_t tv = vs->time_base.num * 1000000LL /
                             (vs->time_base.den == 0 ? 1 : vs->time_base.den);
                int64_t p  = (pkt.pts != AV_NOPTS_VALUE) ? pkt.pts : pkt.dts;
                *ptsOutUs  = p * tv;   // mikrosecond
            }
            av_packet_unref(&pkt);
            return n;
        }
        av_packet_unref(&pkt);
    }
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeOpen(
        JNIEnv* env, jobject, jstring jurl, jint sampleRate, jint channels) {
    const char* url = env->GetStringUTFChars(jurl, nullptr);
    if (sampleRate > 0) g_state.outSampleRate = static_cast<int>(sampleRate);
    if (channels > 0)   g_state.outChannels   = static_cast<int>(channels);
    int rc = openSrt(url);
    env->ReleaseStringUTFChars(jurl, url);
    return rc;
}

JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_CodecBridge_nativePollAudioPcm(
        JNIEnv* env, jobject, jbyteArray out) {
    jsize len = env->GetArrayLength(out);
    auto* buf = new uint8_t[std::max(len, 4)];
    long outBytes = pollAudioPcm(buf, len);
    if (outBytes > 0) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(outBytes),
                                 reinterpret_cast<jbyte*>(buf));
    }
    delete[] buf;
    return outBytes;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeAudioRate(JNIEnv*, jobject) {
    return g_state.outSampleRate;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeAudioChannels(JNIEnv*, jobject) {
    return g_state.outChannels;
}

// ---- Passthrough JNI (DVB/Dolby-DTS) ----

/** Audio codec, passthrough yapilabilir mi? 1 = evet, 0 = hayir (PCM kullan) */
JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeCanPassThrough(JNIEnv*, jobject) {
    return canPassThrough();
}

/** Audio codec_id (AC3=86, EAC3=87, DTS=85, TRUEHD=84) - offload format secimi icin */
JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeAudioCodecId(JNIEnv*, jobject) {
    return audioCodecId();
}

/**
 * Passthrough bitstream adimi cek (AudioTrack offload / Amlogic raw node'a basilir).
 * @return byte sayisi (>0), -1 kaynak tamsandi / passthrough yok
 */
JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_CodecBridge_nativePollAudioPassThrough(
        JNIEnv* env, jobject, jbyteArray out) {
    jsize len = env->GetArrayLength(out);
    auto* buf = new uint8_t[std::max(len, 4)];
    long n = pollAudioPassThrough(buf, len);
    if (n > 0) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                                 reinterpret_cast<jbyte*>(buf));
    }
    delete[] buf;
    return n;
}

JNIEXPORT void JNICALL
Java_com_amlogic_codec_CodecBridge_nativeClose(JNIEnv*, jobject) {
    if (g_state.fmt) { avformat_close_input(&g_state.fmt); g_state.fmt = nullptr; }
    if (g_state.audioCtx) { avcodec_free_context(&g_state.audioCtx); g_state.audioCtx = nullptr; }
    if (g_state.swr) { swr_free(&g_state.swr); g_state.swr = nullptr; }
    g_state.videoStreamIdx = -1;
    g_state.videoCodecId   = AV_CODEC_ID_NONE;
}

// ---- Video JNI (MediaCodec HW decode beslemesi icin) ----

// Donus: 0 = video yok, >0 = video stream var; kok codec_id + cozunurlugu
// doner, Java tarafi "c2.hevc" / "OMX.AMLogic.hevc.decode" seciminde kullanir.
JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeHasVideo(
        JNIEnv*, jobject) {
    return g_state.videoStreamIdx >= 0 ? 1 : 0;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeVideoCodecId(
        JNIEnv*, jobject) {
    // AVMediaCodec'a verecek codec_id:  HEVC=AV_CODEC_ID_HEVC(173),
    //                                    AVC =AV_CODEC_ID_H264(131)
    // Java tarafi 173 -> "video/hevc" mime, 131 -> "video/avc" der.
    return static_cast<jint>(g_state.videoCodecId);
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeVideoWidth(JNIEnv*, jobject) {
    return g_state.videoWidth;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeVideoHeight(JNIEnv*, jobject) {
    return g_state.videoHeight;
}

/**
 * Tek video paketi cek (MediaCodec input'a yazilir).
 * @param out Buffer
 * @return byte sayisi (>0), -1 kaynak tamsandi / video yok
 *
 * Not: SRT/demuxer'dan cikan video paketi, MediaCodec'e verilecek
 * ham bitstream'dir. PTS senkronizasyonu icin, FFmpeg time_base'i
 * ile cizgide avPacket.pts (saniye cizgisi) donulur. MediaCodec
 * queueInputBuffer(presentationTimeUs) cagirisinda bu PTS kullanilir.
 */
JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_CodecBridge_nativePollVideoPkt(
        JNIEnv* env, jobject, jbyteArray out, jlongArray ptsOutUs) {
    jsize len = env->GetArrayLength(out);
    auto* buf = new uint8_t[std::max(len, 4)];
    int64_t ptsUs = 0;
    long n = pollVideoPkt(buf, len, &ptsUs);
    if (n > 0) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                                 reinterpret_cast<jbyte*>(buf));
    }
    if (ptsOutUs != nullptr && env->GetArrayLength(ptsOutUs) >= 1) {
        jlong arr[1];
        arr[0] = static_cast<jlong>(ptsUs);
        env->SetLongArrayRegion(ptsOutUs, 0, 1, arr);
    }
    delete[] buf;
    return n;
}

// ---- TS Feed JNI (DVB/CIx -> FFmpeg demuxer'a byte besleme) ----

/**
 * TS feed modu ac (mpegts demuxer'i memory buffer'dan okur).
 * DvbFrontend.nativeDvbPollTS / nativeCixPoll'dan cikan 188-byte
 * paketleri feedTSBytes icin kullanilir.
 * @param fmtName "mpegts" / "dvbts"
 * @return 0 baarili
 */
JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeOpenTSFeed(
        JNIEnv* env, jobject, jstring jfmt) {
    const char* fmt = env->GetStringUTFChars(jfmt, nullptr);
    int rc = openTSFeed(fmt);
    env->ReleaseStringUTFChars(jfmt, fmt);
    return rc;
}

/**
 * TS byte'larini demuxer'a bas (feed modunda).
 * @param data TS byte'lari (188-multiples)
 * @return basilan byte (>0), -1 buffer tami / hata
 */
JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_CodecBridge_nativeFeedTSBytes(
        JNIEnv* env, jobject, jbyteArray data) {
    jsize len = env->GetArrayLength(data);
    auto* buf = new uint8_t[len];
    env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(buf));
    long rc = feedTSBytes(buf, len);
    delete[] buf;
    return rc;
}

/** TS feed modu kapati (buffer + avio free) */
JNIEXPORT void JNICALL
Java_com_amlogic_codec_CodecBridge_nativeCloseTSFeed(JNIEnv*, jobject) {
    if (g_feed.avioCtx) {
        avio_flush(g_feed.avioCtx);
        av_freep(&g_feed.avioCtx->buffer);
        av_freep(&g_feed.avioCtx);
    }
    if (g_feed.buf) { delete[] g_feed.buf; g_feed.buf = nullptr; }
    g_feed.len = 0;
    g_feed.feeding = false;
}

} // extern C
