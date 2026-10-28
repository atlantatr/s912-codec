//
// codec_core.cpp
//
// S912 / Android 7.1.2 (arm64-v8a) icin FFmpeg 6.1 + libsrt tabanli JNI katmani.
//
// Akis:
//   - Tek bir demux thread'i AVFormatContext'ten paket okur ve audio/video
//     kuyruklarina koyar. Boylece audio ve video ayni anda beslenirken
//     birbirinin paketlerini yutmaz.
//   - Audio: FFmpeg decode -> swresample -> s16 PCM (AudioTrack) veya
//     ham AC3/EAC3/DTS/TrueHD bitstream (AudioTrack offload / raw node).
//   - Video: ham H.264/HEVC paketleri + PTS (Kotlin MediaCodec tarafina).
//   - DVB/CIx: custom AVIOContext uzerinden mpegts demuxer'a canli TS besleme.
//
// Not: Video icin MediaCodec'e verilen paketler container'dan geldigi gibi
// (TS'de Annex-B, MKV/MP4'te length-prefixed) aktarilir. Bazi container'larda
// ayrica h264_mp4toannexb / hevc_mp4toannexb bitstream filter gerekir; bu
// iskelet ilk calisan APK icin bunu MediaCodec tarafinda SPS/PPS ile cozer.
//

#include <jni.h>
#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// FFmpeg 6.1 public headers are plain C and do NOT carry extern "C" guards.
// Without this block the C++ compiler emits mangled symbols (e.g.
// _Z14av_frame_allocv) and linking against the C static libs fails.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#define LOG_TAG "S912Codec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kOutSampleRate = 48000;
constexpr int kOutChannels = 2;
constexpr int kFeedBufferSize = 32 * 1024;
constexpr size_t kFeedCapacity = 2 * 1024 * 1024;
constexpr size_t kMaxQueuedPackets = 256;

struct StreamState {
    AVFormatContext* fmt = nullptr;

    int audioStreamIdx = -1;
    int videoStreamIdx = -1;

    AVCodecContext* audioCtx = nullptr;
    SwrContext* swr = nullptr;
    AVCodecID audioCodecId = AV_CODEC_ID_NONE;
    AVCodecID videoCodecId = AV_CODEC_ID_NONE;

    int outSampleRate = kOutSampleRate;
    int outChannels = kOutChannels;
    AVSampleFormat outFmt = AV_SAMPLE_FMT_S16;

    int videoWidth = 0;
    int videoHeight = 0;

    std::deque<AVPacket*> audioQ;
    std::deque<AVPacket*> videoQ;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread demuxThread;
    std::atomic<bool> running{false};
    bool eof = false;

    bool feedMode = false;
    std::string inputFormat = "mpegts";
    std::string url;

    AVIOContext* avio = nullptr;
    std::mutex feedMutex;
    std::condition_variable feedCv;
    std::vector<uint8_t> feedData;
    size_t feedReadPos = 0;
    bool feedClosed = false;
};

StreamState g_state;

int interruptCallback(void* opaque) {
    auto* s = static_cast<StreamState*>(opaque);
    return (s && !s->running.load()) ? 1 : 0;
}

int feedRead(void* opaque, uint8_t* buf, int size) {
    auto* s = static_cast<StreamState*>(opaque);
    if (!s || !buf || size <= 0) return AVERROR(EINVAL);

    std::unique_lock<std::mutex> lk(s->feedMutex);
    s->feedCv.wait_for(lk, std::chrono::seconds(10), [&] {
        return !s->feedData.empty() || s->feedClosed || !s->running.load();
    });

    if (!s->running.load() || s->feedClosed) {
        return AVERROR_EOF;
    }
    if (s->feedReadPos >= s->feedData.size()) {
        return AVERROR(EAGAIN);
    }

    const size_t available = s->feedData.size() - s->feedReadPos;
    const int n = static_cast<int>(std::min<size_t>(available, static_cast<size_t>(size)));
    memcpy(buf, s->feedData.data() + s->feedReadPos, n);
    s->feedReadPos += static_cast<size_t>(n);

    // Buffer'in basini periyodik olarak temizle.
    if (s->feedReadPos > 1024 * 1024) {
        s->feedData.erase(s->feedData.begin(),
                          s->feedData.begin() + static_cast<std::ptrdiff_t>(s->feedReadPos));
        s->feedReadPos = 0;
    }
    return n;
}

void freeQueuesLocked() {
    for (AVPacket* p : g_state.audioQ) av_packet_free(&p);
    for (AVPacket* p : g_state.videoQ) av_packet_free(&p);
    g_state.audioQ.clear();
    g_state.videoQ.clear();
}

void closeAll() {
    g_state.running.store(false);

    {
        std::lock_guard<std::mutex> lk(g_state.feedMutex);
        g_state.feedClosed = true;
        g_state.feedData.clear();
        g_state.feedReadPos = 0;
    }
    g_state.feedCv.notify_all();
    g_state.cv.notify_all();

    if (g_state.demuxThread.joinable()) {
        g_state.demuxThread.join();
    }

    std::lock_guard<std::mutex> lk(g_state.mutex);

    freeQueuesLocked();

    if (g_state.swr) {
        swr_free(&g_state.swr);
    }
    if (g_state.audioCtx) {
        avcodec_free_context(&g_state.audioCtx);
    }
    if (g_state.fmt) {
        avformat_close_input(&g_state.fmt);
    }
    if (g_state.avio) {
        // avio_context_free, avio_alloc_context'in ayirdigi buffer'i da serbest birakir.
        avio_context_free(&g_state.avio);
    }

    g_state.audioStreamIdx = -1;
    g_state.videoStreamIdx = -1;
    g_state.audioCodecId = AV_CODEC_ID_NONE;
    g_state.videoCodecId = AV_CODEC_ID_NONE;
    g_state.videoWidth = 0;
    g_state.videoHeight = 0;
    g_state.eof = false;
    g_state.feedMode = false;
    g_state.url.clear();
    g_state.feedClosed = false;
}

void setupSwrLocked() {
    if (!g_state.audioCtx) return;

    AVChannelLayout inLayout = g_state.audioCtx->ch_layout;
    if (inLayout.nb_channels <= 0) {
        av_channel_layout_default(&inLayout, 2);
    }
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, g_state.outChannels);

    SwrContext* swr = nullptr;
    int r = swr_alloc_set_opts2(&swr,
                                &outLayout, g_state.outFmt, g_state.outSampleRate,
                                &inLayout, g_state.audioCtx->sample_fmt,
                                g_state.audioCtx->sample_rate,
                                0, nullptr);
    if (r < 0) {
        LOGE("swr_alloc_set_opts2 fail: %d", r);
        return;
    }
    if (swr_init(swr) < 0) {
        LOGE("swr_init fail (in_fmt=%d, rate=%d, ch=%d)",
             g_state.audioCtx->sample_fmt, g_state.audioCtx->sample_rate,
             inLayout.nb_channels);
        swr_free(&swr);
        return;
    }
    g_state.swr = swr;
}

void setupStreamsLocked() {
    if (!g_state.fmt) return;

    g_state.audioStreamIdx = -1;
    g_state.videoStreamIdx = -1;
    g_state.audioCodecId = AV_CODEC_ID_NONE;
    g_state.videoCodecId = AV_CODEC_ID_NONE;

    for (unsigned i = 0; i < g_state.fmt->nb_streams; ++i) {
        AVCodecParameters* par = g_state.fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO && g_state.audioStreamIdx < 0) {
            g_state.audioStreamIdx = static_cast<int>(i);
        } else if (par->codec_type == AVMEDIA_TYPE_VIDEO && g_state.videoStreamIdx < 0) {
            g_state.videoStreamIdx = static_cast<int>(i);
        }
    }

    if (g_state.videoStreamIdx >= 0) {
        AVCodecParameters* vp = g_state.fmt->streams[g_state.videoStreamIdx]->codecpar;
        g_state.videoCodecId = vp->codec_id;
        g_state.videoWidth = vp->width;
        g_state.videoHeight = vp->height;
        LOGI("video: idx=%d codec=%d %dx%d", g_state.videoStreamIdx,
             static_cast<int>(g_state.videoCodecId), g_state.videoWidth,
             g_state.videoHeight);
    } else {
        LOGI("video stream yok (audio-only)");
    }

    if (g_state.audioStreamIdx >= 0) {
        AVStream* st = g_state.fmt->streams[g_state.audioStreamIdx];
        AVCodecParameters* ap = st->codecpar;
        g_state.audioCodecId = ap->codec_id;
        g_state.audioCtx = avcodec_alloc_context3(nullptr);
        if (g_state.audioCtx &&
            avcodec_parameters_to_context(g_state.audioCtx, ap) >= 0) {
            g_state.audioCtx->pkt_timebase = st->time_base;
            const AVCodec* codec = avcodec_find_decoder(ap->codec_id);
            if (!codec) {
                LOGI("audio decoder yok (codec_id=%d); passthrough denenebilir",
                     static_cast<int>(ap->codec_id));
                avcodec_free_context(&g_state.audioCtx);
            } else if (avcodec_open2(g_state.audioCtx, codec, nullptr) < 0) {
                LOGE("audio decoder open fail (codec_id=%d)",
                     static_cast<int>(ap->codec_id));
                avcodec_free_context(&g_state.audioCtx);
            } else {
                LOGI("audio: idx=%d codec=%d rate=%d ch=%d",
                     g_state.audioStreamIdx, static_cast<int>(ap->codec_id),
                     g_state.audioCtx->sample_rate,
                     g_state.audioCtx->ch_layout.nb_channels);
                setupSwrLocked();
            }
        } else {
            LOGE("audio codec context fail (codec_id=%d)",
                 static_cast<int>(ap->codec_id));
            avcodec_free_context(&g_state.audioCtx);
        }
    } else {
        LOGI("audio stream yok");
    }
}

void demuxLoop(bool feedMode) {
    if (feedMode) {
        AVFormatContext* fmt = avformat_alloc_context();
        if (!fmt) {
            LOGE("avformat_alloc_context fail (feed)");
            std::lock_guard<std::mutex> lk(g_state.mutex);
            g_state.eof = true;
            g_state.cv.notify_all();
            return;
        }

        fmt->pb = g_state.avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        fmt->interrupt_callback.callback = interruptCallback;
        fmt->interrupt_callback.opaque = &g_state;

        const AVInputFormat* ifmt = av_find_input_format(g_state.inputFormat.c_str());
        int r = avformat_open_input(&fmt, "", ifmt, nullptr);
        if (r < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(r, err, sizeof(err));
            LOGE("TS feed open fail: %s", err);
            avformat_close_input(&fmt);
            std::lock_guard<std::mutex> lk(g_state.mutex);
            g_state.eof = true;
            g_state.cv.notify_all();
            return;
        }

        r = avformat_find_stream_info(fmt, nullptr);
        if (r < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(r, err, sizeof(err));
            LOGE("TS feed find_stream_info fail: %s", err);
            avformat_close_input(&fmt);
            std::lock_guard<std::mutex> lk(g_state.mutex);
            g_state.eof = true;
            g_state.cv.notify_all();
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_state.mutex);
            g_state.fmt = fmt;
            setupStreamsLocked();
        }
    } else if (!g_state.fmt) {
        std::lock_guard<std::mutex> lk(g_state.mutex);
        g_state.eof = true;
        g_state.cv.notify_all();
        return;
    }

    while (g_state.running.load()) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) break;

        int r = av_read_frame(g_state.fmt, pkt);
        if (r < 0) {
            av_packet_free(&pkt);
            if (r == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (g_state.feedMode && g_state.running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            std::lock_guard<std::mutex> lk(g_state.mutex);
            g_state.eof = true;
            g_state.cv.notify_all();
            break;
        }

        bool queued = false;
        {
            std::lock_guard<std::mutex> lk(g_state.mutex);
            std::deque<AVPacket*>* q = nullptr;
            if (pkt->stream_index == g_state.audioStreamIdx) {
                q = &g_state.audioQ;
            } else if (pkt->stream_index == g_state.videoStreamIdx) {
                q = &g_state.videoQ;
            }
            if (q) {
                if (q->size() >= kMaxQueuedPackets) {
                    av_packet_free(&q->front());
                    q->pop_front();
                }
                q->push_back(pkt);
                queued = true;
                g_state.cv.notify_all();
            }
        }
        if (!queued) {
            av_packet_free(&pkt);
        }
    }

    std::lock_guard<std::mutex> lk(g_state.mutex);
    g_state.eof = true;
    g_state.cv.notify_all();
    LOGI("demux thread bitti");
}

int openSrt(const char* url) {
    closeAll();
    if (!url || !*url) return AVERROR(EINVAL);

    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return AVERROR(ENOMEM);
    fmt->interrupt_callback.callback = interruptCallback;
    fmt->interrupt_callback.opaque = &g_state;

    // ONEMLI: interruptCallback, running=false gorurse acmayi AVERROR_EXIT ile
    // iptal eder. closeAll() running'i false yaptigi icin avformat_open_input
    // cagrisindan ONCE true yapilmalidir; aksi halde her acilis "Immediate exit
    // requested" ile basarisiz olur.
    g_state.running.store(true);

    int r = avformat_open_input(&fmt, url, nullptr, nullptr);
    if (r < 0) {
        g_state.running.store(false);
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(r, err, sizeof(err));
        LOGE("open fail (%s): %s", url, err);
        avformat_close_input(&fmt);
        return r;
    }
    r = avformat_find_stream_info(fmt, nullptr);
    if (r < 0) {
        g_state.running.store(false);
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(r, err, sizeof(err));
        LOGE("find_stream_info fail: %s", err);
        avformat_close_input(&fmt);
        return r;
    }

    {
        std::lock_guard<std::mutex> lk(g_state.mutex);
        g_state.fmt = fmt;
        g_state.feedMode = false;
        g_state.eof = false;
        setupStreamsLocked();
    }

    g_state.running.store(true);
    g_state.demuxThread = std::thread(demuxLoop, false);
    LOGI("opened: %s", url);
    return 0;
}

int openTSFeed(const char* fmtName) {
    closeAll();

    {
        std::lock_guard<std::mutex> lk(g_state.feedMutex);
        g_state.feedData.clear();
        g_state.feedReadPos = 0;
        g_state.feedClosed = false;
    }

    std::lock_guard<std::mutex> lk(g_state.mutex);
    g_state.feedMode = true;
    g_state.inputFormat = (fmtName && *fmtName) ? fmtName : "mpegts";
    g_state.eof = false;

    uint8_t* avioBuf = static_cast<uint8_t*>(av_malloc(kFeedBufferSize));
    if (!avioBuf) {
        LOGE("av_malloc avio buffer fail");
        return AVERROR(ENOMEM);
    }
    g_state.avio = avio_alloc_context(avioBuf, kFeedBufferSize, 0, &g_state,
                                      feedRead, nullptr, nullptr);
    if (!g_state.avio) {
        av_free(avioBuf);
        LOGE("avio_alloc_context fail");
        return AVERROR(ENOMEM);
    }

    g_state.running.store(true);
    g_state.demuxThread = std::thread(demuxLoop, true);
    LOGI("TS feed modu acildi (fmt=%s)", g_state.inputFormat.c_str());
    return 0;
}

long pollAudioPcm(uint8_t* out, int capBytes) {
    if (!out || capBytes <= 0) return -1;

    std::unique_lock<std::mutex> lk(g_state.mutex);
    if (!g_state.audioCtx || !g_state.fmt) return -1;

    AVFrame* frame = av_frame_alloc();
    if (!frame) return -1;

    long result = 0;
    while (g_state.running.load() || !g_state.audioQ.empty()) {
        if (g_state.audioQ.empty()) {
            if (g_state.eof || !g_state.running.load()) break;
            g_state.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !g_state.audioQ.empty() || g_state.eof || !g_state.running.load();
            });
            if (g_state.audioQ.empty()) {
                result = 0;
                break;
            }
        }

        AVPacket* pkt = g_state.audioQ.front();
        g_state.audioQ.pop_front();

        int send = avcodec_send_packet(g_state.audioCtx, pkt);
        av_packet_free(&pkt);
        if (send < 0 && send != AVERROR(EAGAIN)) {
            continue;
        }

        int recv = avcodec_receive_frame(g_state.audioCtx, frame);
        if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) {
            av_frame_unref(frame);
            continue;
        }
        if (recv < 0) {
            av_frame_unref(frame);
            break;
        }

        const int outSamples = capBytes / (static_cast<int>(sizeof(int16_t)) * g_state.outChannels);
        int got = 0;
        if (g_state.swr) {
            const uint8_t* inPlanes[AV_NUM_DATA_POINTERS] = {nullptr};
            for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
                inPlanes[i] = frame->extended_data[i];
            }
            got = swr_convert(g_state.swr,
                              reinterpret_cast<uint8_t**>(&out), outSamples,
                              inPlanes, frame->nb_samples);
        } else if (g_state.audioCtx->sample_fmt == AV_SAMPLE_FMT_S16 &&
                   g_state.audioCtx->ch_layout.nb_channels == g_state.outChannels) {
            const int bytes = std::min(capBytes,
                                       frame->nb_samples * g_state.outChannels *
                                       static_cast<int>(sizeof(int16_t)));
            memcpy(out, frame->data[0], static_cast<size_t>(bytes));
            got = bytes / (g_state.outChannels * static_cast<int>(sizeof(int16_t)));
        }
        av_frame_unref(frame);

        if (got < 0) {
            LOGE("swr_convert fail: %d", got);
            break;
        }
        if (got > 0) {
            result = static_cast<long>(got) * g_state.outChannels *
                     static_cast<long>(sizeof(int16_t));
            break;
        }
    }

    av_frame_free(&frame);
    return result;
}

long pollAudioPassThrough(uint8_t* out, int capBytes) {
    if (!out || capBytes <= 0) return -1;

    std::unique_lock<std::mutex> lk(g_state.mutex);
    if (!g_state.fmt || g_state.audioStreamIdx < 0) return -1;

    while (g_state.running.load() || !g_state.audioQ.empty()) {
        if (g_state.audioQ.empty()) {
            if (g_state.eof || !g_state.running.load()) break;
            g_state.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !g_state.audioQ.empty() || g_state.eof || !g_state.running.load();
            });
            if (g_state.audioQ.empty()) return 0;
        }

        AVPacket* pkt = g_state.audioQ.front();
        g_state.audioQ.pop_front();
        if (pkt->size <= 0 || pkt->size > capBytes) {
            av_packet_free(&pkt);
            continue;
        }
        const int n = pkt->size;
        memcpy(out, pkt->data, static_cast<size_t>(n));
        av_packet_free(&pkt);
        return n;
    }
    return -1;
}

long pollVideoPkt(uint8_t* out, int capBytes, int64_t* ptsOutUs) {
    if (ptsOutUs) *ptsOutUs = 0;
    if (!out || capBytes <= 0) return -1;

    std::unique_lock<std::mutex> lk(g_state.mutex);
    if (!g_state.fmt || g_state.videoStreamIdx < 0) return -1;

    while (g_state.running.load() || !g_state.videoQ.empty()) {
        if (g_state.videoQ.empty()) {
            if (g_state.eof || !g_state.running.load()) break;
            g_state.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !g_state.videoQ.empty() || g_state.eof || !g_state.running.load();
            });
            if (g_state.videoQ.empty()) return 0;
        }

        AVPacket* pkt = g_state.videoQ.front();
        g_state.videoQ.pop_front();
        if (pkt->size <= 0 || pkt->size > capBytes) {
            av_packet_free(&pkt);
            continue;
        }

        const int n = pkt->size;
        memcpy(out, pkt->data, static_cast<size_t>(n));
        if (ptsOutUs) {
            AVStream* vs = g_state.fmt->streams[g_state.videoStreamIdx];
            int64_t pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            *ptsOutUs = (pts != AV_NOPTS_VALUE)
                        ? av_rescale_q(pts, vs->time_base, AV_TIME_BASE_Q)
                        : 0;
        }
        av_packet_free(&pkt);
        return n;
    }
    return -1;
}

int canPassThroughLocked() {
    if (!g_state.fmt || g_state.audioStreamIdx < 0) return 0;
    AVCodecID id = g_state.audioCodecId;
    return (id == AV_CODEC_ID_AC3 || id == AV_CODEC_ID_EAC3 ||
            id == AV_CODEC_ID_DTS || id == AV_CODEC_ID_TRUEHD)
           ? 1 : 0;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    avformat_network_init();
    av_log_set_level(AV_LOG_INFO);
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeOpen(
        JNIEnv* env, jclass, jstring jurl, jint sampleRate, jint channels) {
    if (!jurl) return AVERROR(EINVAL);
    const char* url = env->GetStringUTFChars(jurl, nullptr);
    if (!url) return AVERROR(ENOMEM);

    if (sampleRate > 0) g_state.outSampleRate = static_cast<int>(sampleRate);
    if (channels > 0) g_state.outChannels = static_cast<int>(channels);
    const int rc = openSrt(url);
    env->ReleaseStringUTFChars(jurl, url);
    return rc;
}

JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_CodecBridge_nativePollAudioPcm(
        JNIEnv* env, jclass, jbyteArray out) {
    if (!out) return -1;
    const jsize len = env->GetArrayLength(out);
    std::vector<uint8_t> buf(static_cast<size_t>(std::max<jsize>(len, 4)));
    const long n = pollAudioPcm(buf.data(), len);
    if (n > 0) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                                reinterpret_cast<const jbyte*>(buf.data()));
    }
    return n;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeAudioRate(JNIEnv*, jclass) {
    return g_state.outSampleRate;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeAudioChannels(JNIEnv*, jclass) {
    return g_state.outChannels;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeCanPassThrough(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_state.mutex);
    return canPassThroughLocked();
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeAudioCodecId(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_state.mutex);
    return static_cast<jint>(g_state.audioCodecId);
}

JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_CodecBridge_nativePollAudioPassThrough(
        JNIEnv* env, jclass, jbyteArray out) {
    if (!out) return -1;
    const jsize len = env->GetArrayLength(out);
    std::vector<uint8_t> buf(static_cast<size_t>(std::max<jsize>(len, 4)));
    const long n = pollAudioPassThrough(buf.data(), len);
    if (n > 0) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                                reinterpret_cast<const jbyte*>(buf.data()));
    }
    return n;
}

JNIEXPORT void JNICALL
Java_com_amlogic_codec_CodecBridge_nativeClose(JNIEnv*, jclass) {
    closeAll();
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeHasVideo(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_state.mutex);
    return g_state.videoStreamIdx >= 0 ? 1 : 0;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeVideoCodecId(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_state.mutex);
    return static_cast<jint>(g_state.videoCodecId);
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeVideoWidth(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_state.mutex);
    return g_state.videoWidth;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeVideoHeight(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_state.mutex);
    return g_state.videoHeight;
}

JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_CodecBridge_nativePollVideoPkt(
        JNIEnv* env, jclass, jbyteArray out, jlongArray ptsOutUs) {
    if (!out) return -1;
    const jsize len = env->GetArrayLength(out);
    std::vector<uint8_t> buf(static_cast<size_t>(std::max<jsize>(len, 4)));
    int64_t pts = 0;
    const long n = pollVideoPkt(buf.data(), len, &pts);
    if (n > 0) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                                reinterpret_cast<const jbyte*>(buf.data()));
    }
    if (ptsOutUs && env->GetArrayLength(ptsOutUs) >= 1) {
        const jlong value = static_cast<jlong>(pts);
        env->SetLongArrayRegion(ptsOutUs, 0, 1, &value);
    }
    return n;
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_CodecBridge_nativeOpenTSFeed(
        JNIEnv* env, jclass, jstring jfmt) {
    const char* fmt = jfmt ? env->GetStringUTFChars(jfmt, nullptr) : nullptr;
    const int rc = openTSFeed(fmt);
    if (fmt) env->ReleaseStringUTFChars(jfmt, fmt);
    return rc;
}

JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_CodecBridge_nativeFeedTSBytes(
        JNIEnv* env, jclass, jbyteArray data) {
    if (!data) return -1;
    const jsize len = env->GetArrayLength(data);
    if (len <= 0) return 0;

    std::vector<uint8_t> buf(static_cast<size_t>(len));
    env->GetByteArrayRegion(data, 0, len,
                            reinterpret_cast<jbyte*>(buf.data()));

    std::lock_guard<std::mutex> lk(g_state.feedMutex);
    if (!g_state.running.load() || g_state.feedClosed) return -1;

    const size_t buffered = g_state.feedData.size() - g_state.feedReadPos;
    if (buffered + static_cast<size_t>(len) > kFeedCapacity) {
        size_t drop = buffered + static_cast<size_t>(len) - kFeedCapacity;
        drop = ((drop + 187) / 188) * 188; // TS paket sinirina hizala
        drop = std::min(drop, buffered);
        g_state.feedReadPos += drop;
    }
    g_state.feedData.insert(g_state.feedData.end(), buf.begin(), buf.end());
    g_state.feedCv.notify_one();
    return len;
}

JNIEXPORT void JNICALL
Java_com_amlogic_codec_CodecBridge_nativeCloseTSFeed(JNIEnv*, jclass) {
    closeAll();
}

} // extern "C"
