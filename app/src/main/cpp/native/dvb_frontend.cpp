//
// dvb_frontend.cpp
//
// DVB-S2 tuner (/dev/dvb/adapterN/frontend0) + DVR (/dev/dvb/adapterN/dvr0)
// ve Amlogic CIx (/dev/ciN) TS kaynagi icin JNI katmani.
//
// Akis:
//   DvbFrontend.nativeDvbOpen(0)
//   DvbFrontend.nativeDvbTuneS2(freqHz, polH, loHz, hiHz, srKsym, fecNum, fecDen)
//   DvbFrontend.nativeDvbPollTS(buf)  -> 188-byte TS paketleri
//   CodecBridge.nativeFeedTSBytes(buf) -> FFmpeg mpegts demuxer
//
// Not: Bu katman root gerektirmez; /dev/dvb ve /dev/ci node'lari icin
// uygulamanin okuma izni olmasi gerekir (bazi kutularda 0660 root:system).
//

#include <jni.h>
#include <android/log.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <linux/dvb/frontend.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_TAG "S912Dvb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct DvbState {
    int feFd = -1;
    int dvrFd = -1;
    bool s2 = true;
    bool polH = true;
    long loFreqHz = 9750000000L;
    long hiFreqHz = 10600000000L;
    int srKsym = 27500;
    int fecNum = 5;
    int fecDen = 6;
};

DvbState g_state;

int openFirst(const char* path1, const char* path2, int flags) {
    int fd = open(path1, flags);
    if (fd < 0 && path2 && *path2) {
        fd = open(path2, flags);
    }
    return fd;
}

int dvbOpen(int adapterId) {
    char fe1[128] = {0};
    char fe2[128] = {0};
    char dvr1[128] = {0};
    char dvr2[128] = {0};

    snprintf(fe1, sizeof(fe1), "/dev/dvb/adapter%d/frontend0", adapterId);
    snprintf(fe2, sizeof(fe2), "/dev/dvb/frontend%d", adapterId);
    g_state.feFd = openFirst(fe1, fe2, O_RDWR | O_CLOEXEC);
    if (g_state.feFd < 0) {
        LOGE("frontend open fail (%s / %s): %s", fe1, fe2, strerror(errno));
        return -1;
    }

    snprintf(dvr1, sizeof(dvr1), "/dev/dvb/adapter%d/dvr0", adapterId);
    snprintf(dvr2, sizeof(dvr2), "/dev/dvr0/dvr%d", adapterId);
    g_state.dvrFd = openFirst(dvr1, dvr2, O_RDONLY | O_CLOEXEC);
    if (g_state.dvrFd < 0) {
        LOGE("dvr open fail (%s / %s): %s", dvr1, dvr2, strerror(errno));
    }

    LOGI("DVB open: adapter=%d fe=%d dvr=%d", adapterId, g_state.feFd,
         g_state.dvrFd);
    return 0;
}

fe_code_rate fecFromRatio(int num, int den) {
    if (num <= 0 || den <= 0) return FEC_AUTO;
    if (num == 1 && den == 2) return FEC_1_2;
    if (num == 2 && den == 3) return FEC_2_3;
    if (num == 3 && den == 4) return FEC_3_4;
    if (num == 3 && den == 5) return FEC_3_5;
    if (num == 4 && den == 5) return FEC_4_5;
    if (num == 5 && den == 6) return FEC_5_6;
    if (num == 7 && den == 8) return FEC_7_8;
    if (num == 8 && den == 9) return FEC_8_9;
    if (num == 9 && den == 10) return FEC_9_10;
    return FEC_AUTO;
}

long normalizeLnbHz(long value) {
    // Eski Kotlin kodu kHz (9_750_000) gonderiyordu; Hz'e cevir.
    return (value > 0 && value < 1000000L) ? value * 1000L : value;
}

int dvbTuneS2(long freqHz, bool polH, long loFreq, long hiFreq,
              int srKsym, int fecNum, int fecDen) {
    if (g_state.feFd < 0) {
        LOGE("tune: frontend acik degil");
        return -1;
    }

    g_state.polH = polH;
    g_state.loFreqHz = normalizeLnbHz(loFreq);
    g_state.hiFreqHz = normalizeLnbHz(hiFreq);
    g_state.srKsym = srKsym;
    g_state.fecNum = fecNum;
    g_state.fecDen = fecDen;

    long ifFreq = freqHz;
    bool highBand = false;

    // Uydu frekansi (yaklasik 10.7-12.75 GHz) verildiyse LNB IF'e cevir.
    if (freqHz > 2000000000L) {
        highBand = freqHz >= 11700000000L;
        const long lo = highBand ? g_state.hiFreqHz : g_state.loFreqHz;
        ifFreq = freqHz - lo;
        if (ifFreq < 950000000L || ifFreq > 2150000000L) {
            LOGE("tune: LNB IF araligi disinda (freq=%ld, LO=%ld, IF=%ld)",
                 freqHz, lo, ifFreq);
        }
    }

    struct dtv_property props[8] = {};
    int n = 0;

    props[n].cmd = DTV_DELIVERY_SYSTEM;
    props[n++].u.data = SYS_DVBS2;

    props[n].cmd = DTV_FREQUENCY;
    props[n++].u.data = static_cast<uint32_t>(ifFreq);

    props[n].cmd = DTV_SYMBOL_RATE;
    props[n++].u.data = static_cast<uint32_t>(srKsym) * 1000U;

    props[n].cmd = DTV_INNER_FEC;
    props[n++].u.data = fecFromRatio(fecNum, fecDen);

    props[n].cmd = DTV_INVERSION;
    props[n++].u.data = INVERSION_AUTO;

    props[n].cmd = DTV_PILOT;
    props[n++].u.data = PILOT_AUTO;

    props[n].cmd = DTV_ROLLOFF;
    props[n++].u.data = ROLLOFF_AUTO;

    props[n].cmd = DTV_TONE;
    props[n++].u.data = highBand ? SEC_TONE_ON : SEC_TONE_OFF;

    struct dtv_properties seq = {};
    seq.num = static_cast<unsigned int>(n);
    seq.props = props;

    if (ioctl(g_state.feFd, FE_SET_PROPERTY, &seq) < 0) {
        LOGE("FE_SET_PROPERTY fail: %s", strerror(errno));
        return -1;
    }

    LOGI("DVB tune: freq=%ld IF=%ld LNB=%s pol=%s SR=%d FEC=%d/%d",
         freqHz, ifFreq, highBand ? "HI" : "LO", polH ? "H" : "V", srKsym,
         fecNum, fecDen);
    return 0;
}

long dvbPollTSPackets(uint8_t* out, int capBytes) {
    if (g_state.dvrFd < 0) return -1;
    const int aligned = capBytes - (capBytes % 188);
    if (aligned <= 0) return -1;

    ssize_t n = read(g_state.dvrFd, out, static_cast<size_t>(aligned));
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        LOGE("DVB read fail: %s", strerror(errno));
        return -1;
    }
    if (n == 0) return 0;
    return static_cast<long>(n);
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbOpen(
        JNIEnv*, jclass, jint adapter) {
    if (g_state.feFd >= 0) close(g_state.feFd);
    if (g_state.dvrFd >= 0) close(g_state.dvrFd);
    g_state = DvbState();
    return dvbOpen(static_cast<int>(adapter));
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbTuneS2(
        JNIEnv*, jclass, jlong freq, jboolean polH, jlong lo, jlong hi,
        jint sr, jint num, jint den) {
    return dvbTuneS2(static_cast<long>(freq), polH == JNI_TRUE,
                     static_cast<long>(lo), static_cast<long>(hi),
                     static_cast<int>(sr), static_cast<int>(num),
                     static_cast<int>(den));
}

JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbPollTS(
        JNIEnv* env, jclass, jbyteArray out) {
    if (!out) return -1;
    const jsize len = env->GetArrayLength(out);
    std::vector<uint8_t> buf(static_cast<size_t>(std::max<jsize>(len, 188)));
    const long n = dvbPollTSPackets(buf.data(), len);
    if (n > 0) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                                reinterpret_cast<const jbyte*>(buf.data()));
    }
    return n;
}

JNIEXPORT void JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbClose(JNIEnv*, jclass) {
    if (g_state.feFd >= 0) close(g_state.feFd);
    if (g_state.dvrFd >= 0) close(g_state.dvrFd);
    g_state = DvbState();
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeCixOpen(
        JNIEnv*, jclass, jint ciSlot) {
    char path1[128] = {0};
    char path2[128] = {0};
    snprintf(path1, sizeof(path1), "/dev/ci%d", static_cast<int>(ciSlot));
    snprintf(path2, sizeof(path2), "/dev/dvb/adapter%d/ca0",
             static_cast<int>(ciSlot));

    int fd = openFirst(path1, path2, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fd = openFirst(path1, path2, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        LOGE("CIx open fail (%s / %s): %s", path1, path2, strerror(errno));
        return -1;
    }
    LOGI("CIx open: %s fd=%d", path1, fd);
    return fd;
}

JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeCixPoll(
        JNIEnv* env, jclass, jint ciFd, jbyteArray out) {
    if (!out || ciFd < 0) return -1;
    const jsize len = env->GetArrayLength(out);
    const int aligned = len - (len % 188);
    if (aligned <= 0) return -1;

    std::vector<uint8_t> buf(static_cast<size_t>(aligned));
    ssize_t n = read(ciFd, buf.data(), static_cast<size_t>(aligned));
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        return -1;
    }
    if (n > 0) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                                reinterpret_cast<const jbyte*>(buf.data()));
    }
    return static_cast<long>(n);
}

JNIEXPORT void JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeCixClose(JNIEnv*, jclass, jint ciFd) {
    if (ciFd >= 0) close(ciFd);
}

} // extern "C"
