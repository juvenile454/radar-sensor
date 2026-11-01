// JNI-Anbindung für de.timo.radarsensor.dsp.LiveDsp (V5.2): Live-DSP am Handy aus @RAW, übersetzt mit A121_DSP_HEAVY.
#include <jni.h>

#include <mutex>
#include <string>

#include "dsp_live.h"

namespace {
std::mutex g_mutex;
a121_dsp::UltimateEspDsp g_dsp;    // eine Instanz für die ganze Laufzeit (der Header gibt Speicher nicht frei)
dsp_live::LiveDsp *g_live = nullptr;

dsp_live::LiveDsp &live() {
    if (!g_live) g_live = new dsp_live::LiveDsp(g_dsp);
    return *g_live;
}
}  // namespace

extern "C" JNIEXPORT void JNICALL Java_de_timo_radarsensor_dsp_LiveDsp_nativeReset(JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_mutex);
    live().reset();
}

/** Eine empfangene Zeile; Rückgabe die dadurch fertigen Handy-Zeilen (durch '\n' getrennt) oder null. */
extern "C" JNIEXPORT jstring JNICALL Java_de_timo_radarsensor_dsp_LiveDsp_nativeLine(JNIEnv *env, jobject,
                                                                                    jstring line) {
    if (!line) return nullptr;
    const char *c = env->GetStringUTFChars(line, nullptr);
    if (!c) return nullptr;
    const size_t n = (size_t)env->GetStringUTFLength(line);
    std::string out;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        out = live().line(c, n);
    }
    env->ReleaseStringUTFChars(line, c);
    return out.empty() ? nullptr : env->NewStringUTF(out.c_str());
}

/** CPU-Zeit aller nativeLine-Aufrufe seit dem Laden in ns (Thread-CPU-Zeit, CLOCK_THREAD_CPUTIME_ID). */
extern "C" JNIEXPORT jlong JNICALL Java_de_timo_radarsensor_dsp_LiveDsp_nativeCpuNanos(JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return (jlong)live().cpuNs();
}
