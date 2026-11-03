/**
 * @file dsp_live.h
 * @brief Live path of the phone DSP (V5.2): every line of an `iq3 … raw` stream received from the ESP32 goes into the same
 *        replayer as for the replay (dsp_replay.h); only the lines recomputed on the phone come back:
 *        `@DSP`, `@VITAL`, `@STATE`, `@BREATH`.
 *
 * The replayer decodes `@RAW`, computes at the same points and from the same snapshot as the ESP32 (at its
 * `@DSP` lines) and follows `@REZOOM`/`@RECAL`/`@RECONNECT`. This puts the ESP32 and phone values side by
 * side at the same time. In the app the library radardsp_live is compiled with `A121_DSP_HEAVY` (Viterbi tracking and
 * subtraction of the breathing harmonics), the replay (radardsp) without it. Compute time: CPU time of the calling thread
 * (CLOCK_THREAD_CPUTIME_ID, also on Android), summed over all line() calls.
 */
#ifndef DSP_LIVE_H
#define DSP_LIVE_H

#include <time.h>

#include <string>

#include "dsp_replay.h"

namespace dsp_live {

class LiveDsp {
public:
    /** dsp: one instance for the whole runtime (the header does not free its memory). */
    explicit LiveDsp(a121_dsp::UltimateEspDsp &dsp) : dsp_(dsp), rp_(new dsp_replay::Replayer(dsp, nullptr, nullptr)) {
        rp_->setSink(&LiveDsp::collect, this);
    }
    ~LiveDsp() { delete rp_; }
    LiveDsp(const LiveDsp &)            = delete;
    LiveDsp &operator=(const LiveDsp &) = delete;

    /** Restart (phone DSP switched on, new connection): waits for the next `@IQ3_START`. */
    void reset() {
        delete rp_;
        rp_ = new dsp_replay::Replayer(dsp_, nullptr, nullptr);
        rp_->setSink(&LiveDsp::collect, this);
    }

    /**
     * One received line (with or without line end). Returns: the phone lines completed by it, separated by
     * '\n' (empty = none). A line can come one line later than on the ESP32 (own `@VITAL`, if
     * the ESP32 had none, only with the following line).
     */
    const std::string &line(const char *s, size_t n) {
        out_.clear();
        const uint64_t t0 = threadCpuNs();
        rp_->line(s, n);
        cpu_ns_ += threadCpuNs() - t0;
        if (n >= 5 && strncmp(s, "@RAW,", 5) == 0) frames_++;
        return out_;
    }

    /** CPU time of all line() calls since program start in ns. */
    uint64_t cpuNs() const { return cpu_ns_; }
    /** `@RAW` frames seen since program start (60 per second of data). */
    unsigned long frames() const { return frames_; }

private:
    static bool phoneTag(const char *s) {
        return strncmp(s, "@DSP,", 5) == 0 || strncmp(s, "@VITAL,", 7) == 0 || strncmp(s, "@STATE,", 7) == 0 ||
               strncmp(s, "@BREATH,", 8) == 0;
    }
    static void collect(void *ctx, const char *s) {
        if (!phoneTag(s)) return;
        LiveDsp *self = static_cast<LiveDsp *>(ctx);
        if (!self->out_.empty()) self->out_.push_back('\n');
        self->out_.append(s);
    }
    static uint64_t threadCpuNs() {
        timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }

    a121_dsp::UltimateEspDsp &dsp_;
    dsp_replay::Replayer *rp_;
    std::string out_;
    uint64_t cpu_ns_      = 0;
    unsigned long frames_ = 0;
};

}  // namespace dsp_live

#endif /* DSP_LIVE_H */
