//
// dvb_frontend.cpp
//
// DVB-S2 tuner /dev/dvb/frontend0 icin JNI kosesi (CI kutusu / DVB modulu).
//
// Amlogic S912 CI/DVB kutularinda genelde kernel "dvb-core" + "ttusb"
// / "stx107" / "cx23887" icin uyarli frontend node'lari vardir. Bu
// katman /dev/dvb/frontend0 (ve adapter0/dvr0) uzerinden:
//   - fe_set_frontend (LNB/frekans, pol, SR, FEC)
//   - dvr TS packet read (188-byte TS stream)
//   - DVB-S2 (QPSK/8PSK, 2, 8PSK, 23,24, 32,33,22) LNB ayarlamasi
//
// CI kartinin kendi audio/video demuxeri Amlogic kernel modulu
// uzerinden gelir (amlogic mpegts_ci / tsdec_ci). Bu JNI, sadece
// frontend tuning + TS ciktisi icin; demux/codec kosesi FFmpeg
// (codec_core.cpp) icindedir.
//
// Kullanilimi (Kotlin tarafi):
//   DvbFrontend.dvbOpen(0)                // frontend0 ac
//   DvbFrontend.dvbSetS2(freq=11330, pol=H, lnb_lo=10700, lnb_hi=11300,
//                          rate=29700, fec=5_6)
//   DvbFrontend.dvbPollPackets(buf, cap)  // 188-byte TS packet cek
//

#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>

#include <linux/dvb/version.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dvr.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <climits>

#define LOG_TAG "S912Dvb"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// DVB-S2 LNB ayari (birincil LNB; genelde 10.7-11.3 GHz)
struct DvbState {
    int  feFd = -1;         // /dev/dvb/frontend<N>
    int  dvrFd = -1;        // /dev/dvr0/<N> (adapter0/dvr0/dvr<N>)
    bool s2       = true;   // DVB-S2 varsayilan
    bool polH    = true;    // polarizasyon
    long loFreq  = 9750;   // LNB universal frekans kopsu (Hz)
    long hiFreq  = 10600;
    int  sr      = 27500;  // symbol rate (kSym/s)
    int  fecNum  = 5;      // FEC sayici (7, 11)
    int  fecDen  = 6;      // FEC payi
};

DvbState g_state;

int dvbOpen(int adapterId) {
    char path[128];
    snprintf(path, sizeof(path), "/dev/dvb/frontend%d", adapterId);
    g_state.feFd = open(path, O_RDWR);
    if (g_state.feFd < 0) {
        LOGE("open %s fail: %s", path, strerror(errno));
        return -1;
    }
    // DVR ciktisi (stream'de okunacak TS)
    char dvr[128];
    snprintf(dvr, sizeof(dvr), "/dev/dvr0/dvr%d", adapterId);
    g_state.dvrFd = open(dvr, O_RDONLY);
    if (g_state.dvrFd < 0) {
        LOGE("open %s fail: %s", dvr, strerror(errno));
        // DVB ayari yine degil; sadece TS ciktisi
        g_state.dvrFd = -1;
    } else {
        // DVB DVR'i demux modu 0 (pesiz TS stream)
        int buf = 0;
        // 64KB buffer, demux 0 = demuxed (TS pesleri)
        int dm[4] = {0, 0, 0, 0};
        if (ioctl(g_state.dvrFd, DVB_DVR_GET_BUFFER_SIZE, &buf) < 0) {
            LOGE("DVB_DVR_GET_BUFFER_SIZE fail: %s", strerror(errno));
        }
        LOGI("DVB open OK: fe=%d dvr=%d bufsize=%d",
             g_state.feFd, g_state.dvrFd, buf);
    }
    return 0;
}

/**
 * DVB-S2 tuning (LNB universal + SR/FEC).
 * @param freqHz  transponder merkezi frekans (Hz)
 * @param polH    true = H, false = V
 * @param loFreq  LNB lo bant frekansi (Hz)
 * @param hiFreq  LNB hi bant frekansi (Hz)
 * @param sr      symbol rate (kSym/s)
 * @param fecNum / fecDen  FEC oran
 */
int dvbTuneS2(long freqHz, bool polH, long loFreq, long hiFreq,
              int sr, int fecNum, int fecDen) {
    if (g_state.feFd < 0) { LOGE("feFd yok"); return -1; }

    struct dvb_frontend fe = {};
    fe.freq = static_cast<uint32_t>(freqHz);
    fe.delicity = 0; // DVB-S2 (DVB-S icin -1)
    fe.polarization = polH ? 0 : 1;  // 0 = H, 1 = V (genelde)
    fe.delivery_system = SYSTEM_DVB_S;
    fe.dvbcs2.pilot      = DVB Pilots_AUTO;
    fe.dvbcs2.fec_type  = DVB_FEC_7_8; // varsayilan; FEC num/den'e gore degistir
    fe.dvbcs2.roll_off  = DVB_ROLL_OFF_AUTO;
    fe.dvbcs2.transmission_mode = DVB_S2_T2_HIERARCHY_NONE;
    fe.dvbcs2.code_rate = DVB CodingRate_7_8;
    fe.dvbcs2.plm    = DVB Plms_ON;

    // LNB frekans hesabi: Hi/L0 Lo bandina gore
    // universal LNB'de Lo 9.75-10.6 GHz, Hi 10.7-12.75
    uint32_t lnbF;
    if (freqHz >= hiFreq) {
        lnbF = static_cast<uint32_t>(freqHz - hiFreq + loFreq); // Hi LNB out
    } else {
        lnbF = static_cast<uint32_t>(freqHz - loFreq + loFreq); // Lo LNB out
    }
    fe.tone = 0; // DVB-S icin genelde lo

    // SR / FEC (SR birimi kSym/s, FE icin 1000 carpani)
    fe.u.dvbs.spectral_inversion = DVB SpectralInversion_OFF;

    if (ioctl(g_state.feFd, FE_SET_FRONTEND, &fe) < 0) {
        LOGE("FE_SET_FRONTEND fail: %s", strerror(errno));
        return -1;
    }
    LOGI("DVB tune: freq=%ld LNB=%ld pol=%s SR=%d FEC=%d/%d",
         freqHz, lnbF, polH ? "H" : "V", sr, fecNum, fecDen);
    return 0;
}

// DVR'dan 188-byte TS paketleri oku (buffer'e ne kadar sicerse)
long dvbPollTSPackets(uint8_t* out, int capBytes) {
    if (g_state.dvrFd < 0) return -1;
    ssize_t n = read(g_state.dvrFd, out, capBytes);
    if (n < 0) { LOGE("DVB read fail: %s", strerror(errno)); return -1; }
    if (n == 0) return -1;   // EOF / kaynak tamsandi
    return n;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbOpen(
        JNIEnv*, jobject, jint adapter) {
    g_state = DvbState();
    return dvbOpen(static_cast<int>(adapter));
}

JNIEXPORT jint JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbTuneS2(
        JNIEnv*, jobject, jlong freq, jboolean polH, jlong lo, jlong hi,
        jint sr, jint num, jint den) {
    return dvbTuneS2(static_cast<long>(freq), polH == JNI_TRUE,
                     static_cast<long>(lo), static_cast<long>(hi),
                     static_cast<int>(sr), static_cast<int>(num),
                     static_cast<int>(den));
}

JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbPollTS(
        JNIEnv* env, jobject, jbyteArray out) {
    jsize len = env->GetArrayLength(out);
    auto* buf = new uint8_t[std::max(len, 188)];
    long n = dvbPollTSPackets(buf, len);
    if (n > 0) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                                 reinterpret_cast<jbyte*>(buf));
    }
    delete[] buf;
    return n;
}

JNIEXPORT void JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeDvbClose(JNIEnv*, jobject) {
    if (g_state.feFd > 0)  close(g_state.feFd);
    if (g_state.dvrFd > 0) close(g_state.dvrFd);
    g_state = DvbState();
}

} // extern C

// ======================================================================
// CIx / CI besleme (tuner kartinin TS ciktisini SRT/sunucu tarafina basmak
// veya FFmpeg'e devretmek icin). ttapi ioctl tabanli — tuner karti
// CI slotindan gelen TS'yi CIx protokolunde okur.
// ======================================================================

#include <linux/usb/usbvision.h>
#include <linux/types.h>

extern "C" {

/**
 * CIx beslemesi ac (modem/slot numarasina gore). Donus: fd veya -1.
 * @param ciSlot  CI slot index (genelde 0)
 */
JNIEXPORT jint JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeCixOpen(
        JNIEnv* env, jobject, jint ciSlot) {
    char path[128];
    snprintf(path, sizeof(path), "/dev/ci%d", (int)ciSlot);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        LOGE("open %s fail: %s", path, strerror(errno));
        return -1;
    }
    // CIx protokolunu baslat (CI_CAPA: host->CI)
    unsigned long caps = 1 << 4 | 1 << 5; // 4 = CAM slot, 5 = CI
    if (ioctl(fd, CI_CAPABILITIES, &caps) < 0) {
        LOGI("CI_CAPABILITIES ioctl fail (optional): %s", strerror(errno));
    }
    return fd;
}

/**
 * CIx'ten TS paket oku (188-byte). Donus: byte sayisi (>0), -1 yok/hata.
 */
JNIEXPORT jlong JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeCixPoll(
        JNIEnv* env, jobject, jint ciFd, jbyteArray out) {
    jsize len = env->GetArrayLength(out);
    auto* buf = new uint8_t[std::max(len, 188)];
    ssize_t n = read(ciFd, buf, len);
    if (n < 0) { delete[] buf; return -1; }
    if (n > 0) {
        env->SetByteArrayRegion(out, 0, (jsize)n,
                                 (jbyte*)buf);
    }
    delete[] buf;
    return n;
}

/** CIx beslemesini kaptirir. */
JNIEXPORT void JNICALL
Java_com_amlogic_codec_DvbFrontend_nativeCixClose(
        JNIEnv* env, jobject, jint ciFd) {
    if (ciFd > 0) close(ciFd);
}

} // extern C
