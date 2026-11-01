/**
 * @file dsp_live.h
 * @brief Live-Pfad der Handy-DSP (V5.2): jede vom ESP32 empfangene Zeile eines `iq3 … raw`-Streams geht in denselben
 *        Replayer wie beim Nachrechnen (dsp_replay.h); zurück kommen nur die am Handy neu gerechneten Zeilen
 *        `@DSP`, `@VITAL`, `@STATE`, `@BREATH`.
 *
 * Der Replayer dekodiert `@RAW`, rechnet an denselben Stellen und aus demselben Schnappschuss wie der ESP32 (an dessen
 * `@DSP`-Zeilen) und folgt `@REZOOM`/`@RECAL`/`@RECONNECT`. Damit liegen ESP32- und Handy-Wert zeitgleich
 * nebeneinander. In der App wird die Bibliothek radardsp_live mit `A121_DSP_HEAVY` übersetzt (Viterbi-Verfolgung und
 * Abzug der Atem-Oberwellen), die Nachrechnung (radardsp) ohne. Rechenzeit: CPU-Zeit des aufrufenden Threads
 * (CLOCK_THREAD_CPUTIME_ID, auch unter Android), summiert über alle line()-Aufrufe.
 */
#ifndef DSP_LIVE_H
#define DSP_LIVE_H

#include <time.h>

#include <string>

#include "dsp_replay.h"

namespace dsp_live {

class LiveDsp {
public:
    /** dsp: eine Instanz für die ganze Laufzeit (der Header gibt seinen Speicher nicht frei). */
    explicit LiveDsp(a121_dsp::UltimateEspDsp &dsp) : dsp_(dsp), rp_(new dsp_replay::Replayer(dsp, nullptr, nullptr)) {
        rp_->setSink(&LiveDsp::collect, this);
    }
    ~LiveDsp() { delete rp_; }
    LiveDsp(const LiveDsp &)            = delete;
    LiveDsp &operator=(const LiveDsp &) = delete;

    /** Neubeginn (Handy-DSP eingeschaltet, neue Verbindung): wartet auf das nächste `@IQ3_START`. */
    void reset() {
        delete rp_;
        rp_ = new dsp_replay::Replayer(dsp_, nullptr, nullptr);
        rp_->setSink(&LiveDsp::collect, this);
    }

    /**
     * Eine empfangene Zeile (ohne oder mit Zeilenende). Rückgabe: die dadurch fertig gewordenen Handy-Zeilen, durch
     * '\n' getrennt (leer = keine). Eine Zeile kann eine Zeile später kommen als beim ESP32 (eigenes `@VITAL`, wenn
     * der ESP32 keines hatte, erst mit der folgenden Zeile).
     */
    const std::string &line(const char *s, size_t n) {
        out_.clear();
        const uint64_t t0 = threadCpuNs();
        rp_->line(s, n);
        cpu_ns_ += threadCpuNs() - t0;
        if (n >= 5 && strncmp(s, "@RAW,", 5) == 0) frames_++;
        return out_;
    }

    /** CPU-Zeit aller line()-Aufrufe seit Programmstart in ns. */
    uint64_t cpuNs() const { return cpu_ns_; }
    /** Gesehene `@RAW`-Frames seit Programmstart (60 je Sekunde Daten). */
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
