//
// dvb_frontend.cpp
//
// DVB-S/S2 tuner + demux -> TS akisi (JNI).
//
// Amlogic S912 kutularinda aygit adlari standart DEGIL:
//   /dev/dvb0.frontend0   /dev/dvb0.demux0   /dev/dvb0.dvr0
// Bazi imajlarda standart yerlesim de olabilir:
//   /dev/dvb/adapter0/frontend0 /dev/dvb/adapter0/demux0 /dev/dvb/adapter0/dvr0
//   /dev/dvb/frontend0 ...
// Bu yuzden her dugum icin aday yollarin tamami denenir.
//
// Tam TS icin demux'a "tum PID'ler" filtresi (0x2000) kurulur; aksi halde
// /dev/dvb0.dvr0 bos kalir (filtre yoksa veri akmaz).
//

#include <jni.h>
#include <android/log.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_TAG "S912Dvb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// Amlogic 3.14 cekirdegi DMX_SET_SOURCE'u 65 numarali ioctl ile tanimlar;
// mainline cekirdekler 49 kullanir. Ikisi de denenir.
#define AML_DMX_SET_SOURCE _IOW('o', 65, int)

namespace {

struct DvbState {
    int feFd = -1;
    int dvrFd = -1;
    int dmxFd = -1;
    int adapter = -1;
    bool tuned = false;
};

DvbState g_state;

const char* kFePaths[] = {
    "/dev/dvb/adapter%d/frontend0",
    "/dev/dvb/frontend%d",
    "/dev/dvb0.frontend%d",
};

const char* kDvrPaths[] = {
    "/dev/dvb/adapter%d/dvr0",
    "/dev/dvr0/dvr%d",
    "/dev/dvb0.dvr%d",
};

const char* kDmxPaths[] = {
    "/dev/dvb/adapter%d/demux0",
    "/dev/dvb/demux%d",
    "/dev/dvb0.demux%d",
};

int openByTemplate(const char* const* paths, size_t n, int idx, int flags) {
    char buf[128];
    for (size_t i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), paths[i], idx);
        int fd = open(buf, flags);
        if (fd >= 0) {
            LOGI("acildi: %s (fd=%d)", buf, fd);
            return fd;
        }
        LOGW("acilamadi: %s (%s)", buf, strerror(errno));
    }
    return -1;
}

void closeAll() {
    if (g_state.dvrFd >= 0) close(g_state.dvrFd);
    if (g_state.dmxFd >= 0) close(g_state.dmxFd);
    if (g_state.feFd >= 0) close(g_state.feFd);
    g_state = DvbState();
}

// Ileri bildirim (tanim dosyanin sonunda).
void setDemuxSource(int adapter);

/**
 * Demux'a tam TS filtresi kur (pid 0x2000 = tum PID'ler).
 * Amlogic'te kaynak secimi (DMX_SET_SOURCE) zorunlu; olmadan dvr bos kalir.
 */
int setupDemux(int adapter) {
    g_state.dmxFd = openByTemplate(kDmxPaths, 3, adapter, O_RDWR | O_CLOEXEC);
    if (g_state.dmxFd < 0) {
        LOGE("demux acilamadi");
        return -1;
    }

    setDemuxSource(adapter);

    struct dmx_pes_filter_params f;
    memset(&f, 0, sizeof(f));
    f.pid = 0x2000;                 // tam TS
    f.input = DMX_IN_FRONTEND;
    f.output = DMX_OUT_TS_TAP;
    f.pes_type = DMX_PES_OTHER;
    f.flags = DMX_IMMEDIATE_START;

    if (ioctl(g_state.dmxFd, DMX_SET_PES_FILTER, &f) < 0) {
        LOGE("DMX_SET_PES_FILTER fail: %s", strerror(errno));
        return -1;
    }
    LOGI("demux filtre kuruldu: pid=0x2000 (tam TS)");
    return 0;
}

int dvbOpen(int adapterId) {
    closeAll();

    g_state.feFd = openByTemplate(kFePaths, 3, adapterId, O_RDWR | O_CLOEXEC);
    if (g_state.feFd < 0) {
        LOGE("frontend bulunamadi (adapter=%d)", adapterId);
        return -1;
    }

    if (setupDemux(adapterId) != 0) {
        closeAll();
        return -1;
    }

    g_state.dvrFd = openByTemplate(kDvrPaths, 3, adapterId, O_RDONLY | O_CLOEXEC);
    if (g_state.dvrFd < 0) {
        LOGE("dvr acilamadi");
        closeAll();
        return -1;
    }

    g_state.adapter = adapterId;
    LOGI("DVB hazir: adapter=%d fe=%d dmx=%d dvr=%d", adapterId, g_state.feFd,
         g_state.dmxFd, g_state.dvrFd);
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

/** LNB beslemesi: 18V = yatay (H), 13V = dikey (V). */
void setLnbVoltage(bool polH) {
    fe_sec_voltage_t v = polH ? SEC_VOLTAGE_18 : SEC_VOLTAGE_13;
    if (ioctl(g_state.feFd, FE_SET_VOLTAGE, v) < 0) {
        LOGW("FE_SET_VOLTAGE(%d) basarisiz: %s", v, strerror(errno));
    } else {
        LOGI("LNB beslemesi: %d V", polH ? 18 : 13);
    }
}

/** Kilit gelene kadar bekle. @return 0 = kilit var. */
int waitLock(int timeoutMs) {
    int waited = 0;
    fe_status_t last = static_cast<fe_status_t>(0);
    while (waited < timeoutMs) {
        fe_status_t st = static_cast<fe_status_t>(0);
        if (ioctl(g_state.feFd, FE_READ_STATUS, &st) == 0) {
            last = st;
            if (st & FE_HAS_LOCK) {
                uint16_t sig = 0, snr = 0;
                ioctl(g_state.feFd, FE_READ_SIGNAL_STRENGTH, &sig);
                ioctl(g_state.feFd, FE_READ_SNR, &snr);
                LOGI("kilit tamam (%d ms, status=0x%02x, sinyal=%u, snr=%u)",
                     waited, st, sig, snr);
                return 0;
            }
            if (waited % 1000 == 0) {
                uint16_t sig = 0;
                ioctl(g_state.feFd, FE_READ_SIGNAL_STRENGTH, &sig);
                LOGI("bekleniyor %d ms: status=0x%02x sinyal=%u", waited, st, sig);
            }
        }
        usleep(100 * 1000);
        waited += 100;
    }
    LOGE("kilit yok (%d ms, status=0x%02x)", timeoutMs, last);
    return -1;
}

/** Tek bir DTV özelliği yaz. @return 0 = kabul edildi. */
int setProp(int fd, uint32_t cmd, uint32_t data) {
    struct dtv_property p;
    memset(&p, 0, sizeof(p));
    p.cmd = cmd;
    p.u.data = data;
    struct dtv_properties seq;
    memset(&seq, 0, sizeof(seq));
    seq.num = 1;
    seq.props = &p;
    return ioctl(fd, FE_SET_PROPERTY, &seq);
}

/** Tek bir DTV özelliği oku. @return 0 = okundu. */
int getProp(int fd, uint32_t cmd, uint32_t* out) {
    struct dtv_property p;
    memset(&p, 0, sizeof(p));
    p.cmd = cmd;
    struct dtv_properties seq;
    memset(&seq, 0, sizeof(seq));
    seq.num = 1;
    seq.props = &p;
    if (ioctl(fd, FE_GET_PROPERTY, &seq) < 0) return -1;
    if (out) *out = p.u.data;
    return 0;
}

void logFrontendInfo() {
    struct dvb_frontend_info info;
    memset(&info, 0, sizeof(info));
    if (ioctl(g_state.feFd, FE_GET_INFO, &info) == 0) {
        LOGI("frontend: %s type=%d caps=0x%08x symbol_rate=%u-%u",
             info.name, info.type, info.caps,
             info.symbol_rate_min, info.symbol_rate_max);
    } else {
        LOGW("FE_GET_INFO fail: %s", strerror(errno));
    }
    uint32_t api = 0;
    if (getProp(g_state.feFd, DTV_API_VERSION, &api) == 0) {
        LOGI("DTV API surumu: %u.%u", api >> 16, api & 0xffff);
    } else {
        LOGW("DTV_API_VERSION okunamadi (legacy ioctl olabilir)");
    }
}

/**
 * Amlogic 3.14 cekirdekleri DMX_SET_SOURCE'u farkli ioctl imzalariyla tanimlar.
 * Hepsini sirayla dene; hicbiri tutmazsa filtre yine de varsayilan kaynakla
 * calisabilir, bu yuzden hata olumcul degil.
 */
void setDemuxSource(int adapter) {
    int src = adapter;
    if (ioctl(g_state.dmxFd, AML_DMX_SET_SOURCE, &src) == 0) {
        LOGI("DMX_SET_SOURCE _IOW(65) ok");
        return;
    }
    if (ioctl(g_state.dmxFd, _IO('o', 65), &src) == 0) {
        LOGI("DMX_SET_SOURCE _IO(65) ok");
        return;
    }
    if (ioctl(g_state.dmxFd, _IOW('o', 65, int), &src) == 0) {
        LOGI("DMX_SET_SOURCE _IOW(int) ok");
        return;
    }
#ifdef DMX_SET_SOURCE
    {
        enum dmx_source msrc = static_cast<enum dmx_source>(adapter);
        if (ioctl(g_state.dmxFd, DMX_SET_SOURCE, &msrc) == 0) {
            LOGI("DMX_SET_SOURCE (mainline) ok");
            return;
        }
    }
#endif
    LOGW("DMX_SET_SOURCE desteklenmiyor (varsayilan kaynak kullanilacak)");
}

int dvbTuneS2(long freqHz, bool polH, long loFreq, long hiFreq,
              int srKsym, int fecNum, int fecDen) {
    if (g_state.feFd < 0) {
        LOGE("tune: frontend acik degil");
        return -1;
    }

    logFrontendInfo();

    const long loHz = normalizeLnbHz(loFreq);
    const long hiHz = normalizeLnbHz(hiFreq);

    long ifFreq = freqHz;
    bool highBand = false;

    // Uydu frekansi (10.7-12.75 GHz) verildiyse LNB IF'e cevir.
    if (freqHz > 2000000000L) {
        highBand = freqHz >= 11700000000L;
        const long lo = highBand ? hiHz : loHz;
        ifFreq = freqHz - lo;
        if (ifFreq < 950000000L || ifFreq > 2150000000L) {
            LOGE("tune: LNB IF araligi disinda (freq=%ld, LO=%ld, IF=%ld)",
                 freqHz, lo, ifFreq);
        }
    }

    setLnbVoltage(polH);

    // Amlogic 3.14 bazi imajlarda ozellik dizisini tek seferde reddediyor
    // (EINVAL). Bu yuzden her ozellik ayri ayri yazilir ve zorunlu olmayanlar
    // (pilot/rolloff/fec) basarisiz olsa bile devam edilir.
    struct { const char* name; uint32_t cmd; uint32_t val; bool required; } props[] = {
        {"DELIVERY_SYSTEM", DTV_DELIVERY_SYSTEM, SYS_DVBS2, false},
        {"FREQUENCY",       DTV_FREQUENCY,       static_cast<uint32_t>(ifFreq), true},
        {"SYMBOL_RATE",     DTV_SYMBOL_RATE,     static_cast<uint32_t>(srKsym) * 1000U, true},
        {"INNER_FEC",       DTV_INNER_FEC,       static_cast<uint32_t>(fecFromRatio(fecNum, fecDen)), false},
        {"INVERSION",       DTV_INVERSION,       INVERSION_AUTO, false},
        {"PILOT",           DTV_PILOT,           PILOT_AUTO, false},
        {"ROLLOFF",         DTV_ROLLOFF,         ROLLOFF_AUTO, false},
        {"TONE",            DTV_TONE,            highBand ? SEC_TONE_ON : SEC_TONE_OFF, false},
    };

    for (auto& p : props) {
        int r = setProp(g_state.feFd, p.cmd, p.val);
        if (r == 0) {
            LOGI("  %s = %u ok", p.name, p.val);
            continue;
        }
        if (p.required) {
            LOGE("  %s = %u REDDEDILDI: %s", p.name, p.val, strerror(errno));
            return -1;
        }
        LOGW("  %s = %u desteklenmiyor (%s)", p.name, p.val, strerror(errno));
        if (p.cmd == DTV_DELIVERY_SYSTEM) {
            // S2 kabul edilmediyse S (QPSK) olarak dene.
            if (setProp(g_state.feFd, DTV_DELIVERY_SYSTEM, SYS_DVBS) == 0) {
                LOGI("  DELIVERY_SYSTEM = SYS_DVBS ok (S2 desteklenmiyor)");
            }
        }
    }

    LOGI("DVB tune: freq=%ld IF=%ld LNB=%s pol=%s SR=%d FEC=%d/%d",
         freqHz, ifFreq, highBand ? "HI" : "LO", polH ? "H" : "V", srKsym,
         fecNum, fecDen);

    if (waitLock(8000) != 0) return -1;
    g_state.tuned = true;
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

/** Kilit durumu: 0 = kilit var, -1 = yok. */
JNIEXPORT jint JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbStatus(JNIEnv*, jclass) {
    if (g_state.feFd < 0) return -1;
    fe_status_t st = static_cast<fe_status_t>(0);
    if (ioctl(g_state.feFd, FE_READ_STATUS, &st) < 0) return -1;
    return (st & FE_HAS_LOCK) ? 0 : -1;
}

JNIEXPORT void JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbClose(JNIEnv*, jclass) {
    closeAll();
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeCixOpen(
        JNIEnv*, jclass, jint ciSlot) {
    char path1[128] = {0};
    char path2[128] = {0};
    snprintf(path1, sizeof(path1), "/dev/ci%d", static_cast<int>(ciSlot));
    snprintf(path2, sizeof(path2), "/dev/dvb/adapter%d/ca0",
             static_cast<int>(ciSlot));

    int fd = open(path1, O_RDWR | O_CLOEXEC);
    if (fd < 0) fd = open(path2, O_RDWR | O_CLOEXEC);
    if (fd < 0) fd = open(path1, O_RDONLY | O_CLOEXEC);
    if (fd < 0) fd = open(path2, O_RDONLY | O_CLOEXEC);
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
