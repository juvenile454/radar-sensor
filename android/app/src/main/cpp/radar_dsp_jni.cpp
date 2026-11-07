// JNI binding for io.github.juvenile454.radarsensor.dsp.DspReplay (A5).
#include <jni.h>

#include <mutex>

#include "dsp_replay.h"

namespace {
std::mutex g_mutex;
a121_dsp::UltimateEspDsp g_dsp;  // one instance for all runs (the header does not free memory)

struct Utf {
    JNIEnv *env;
    jstring s;
    const char *c;
    Utf(JNIEnv *e, jstring js) : env(e), s(js), c(js ? e->GetStringUTFChars(js, nullptr) : nullptr) {}
    ~Utf() {
        if (c) env->ReleaseStringUTFChars(s, c);
    }
};
}  // namespace

/** Returns: 0 = bit-identical, 1 = deviation, 2 = file not readable/writable. */
extern "C" JNIEXPORT jint JNICALL Java_io_github_juvenile454_radarsensor_dsp_DspReplay_nativeRun(JNIEnv *env, jobject, jstring in,
                                                                                    jstring out, jstring report) {
    Utf in_path(env, in), out_path(env, out), rep_path(env, report);
    std::lock_guard<std::mutex> lock(g_mutex);
    FILE *f_in = fopen(in_path.c, "r");
    FILE *f_out = fopen(out_path.c, "w");
    FILE *f_rep = fopen(rep_path.c, "w");
    jint rc = 2;
    if (f_in && f_out && f_rep) {
        dsp_replay::Replayer r(g_dsp, f_out, f_rep);
        r.runFile(f_in);
        rc = r.finish() ? 0 : 1;
    }
    if (f_in) fclose(f_in);
    if (f_out) fclose(f_out);
    if (f_rep) fclose(f_rep);
    return rc;
}
