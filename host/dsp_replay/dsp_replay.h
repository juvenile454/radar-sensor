/**
 * @file dsp_replay.h
 * @brief Rechnet eine `iq3 … raw`-Aufnahme (stream.log) mit dem Firmware-DSP-Header neu durch und vergleicht jede
 *        Ausgabezeile mit der Zeile, die der ESP32 live geschrieben hat.
 *
 * Gemeinsame Quelle für den PC (`host/a121_esp_ultimate.py --mode replay-cpp`) und die Android-App (NDK, A5).
 * Ablauf wie `cmdIq3()` in `firmware/esp32s3/src/main.cpp`, aber vom Log gesteuert: Frames kommen aus `@RAW`, Reports
 * werden dort gerechnet, wo der ESP32 `@DSP` ausgegeben hat, das Zoom-Fenster wechselt bei `@REZOOM` bzw. nach
 * erfolgreichem `@RECONNECT`. Zeitstempel (`t_ms`) und die Rechenzeit in `@DSP` übernimmt der Replayer vom ESP32, alle
 * anderen Felder rechnet er selbst. Die neuen Zeilen haben dasselbe Format wie live (auswertbar mit `--mode plot`,
 * Formatfunktionen aus a121_esp_dsp.h). Ist ein eigener Report gültig, beim ESP32 aber kein `@VITAL` da, schreibt der
 * Replayer sein `@VITAL` trotzdem (mit der Zeit der `@DSP`-Zeile) und zählt die Zeile als fehlend; so lassen sich
 * Header-Varianten mit anderer Verfügbarkeit auswerten (host/vital_eval.py --replay).
 *
 * Braucht vor dem Einbinden ein `Arduino.h` (hier: Shim im selben Ordner) und den Include-Pfad zu
 * `firmware/esp32s3/src`. Für bitgleiche Ergebnisse ohne `-ffast-math` und mit `-ffp-contract=off` übersetzen.
 */
#ifndef DSP_REPLAY_H
#define DSP_REPLAY_H

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <string>

#include "a121_esp_dsp.h"

namespace dsp_replay {

using namespace a121_dsp;

struct TagStat {
    const char *tag;
    unsigned long compared = 0;
    unsigned long equal    = 0;
    unsigned long other    = 0;    /* abweichende Feldzahl oder nicht-numerisches Feld */
    double max_dev         = 0.0;  /* größte Abweichung eines Zahlenfelds (gleiche Feldzahl) */
    int max_dev_field      = 0;
    double max_units       = 0.0;  /* größte Abweichung in Einheiten der letzten ausgegebenen Stelle */
    unsigned shown         = 0;    /* ausgegebene Beispiele */
};

class Replayer {
public:
    /**
     * dsp: wird je Stream mit begin() neu gestartet. Der Header gibt seinen Speicher nie frei, deshalb über mehrere
     * Läufe dieselbe Instanz verwenden. out: neu gerechnete Zeilen, report: Bericht (beide dürfen nullptr sein).
     */
    Replayer(UltimateEspDsp &dsp, FILE *out, FILE *report) : out_(out), rep_out_(report), dsp_(dsp) {}

    void line(const char *s, size_t n) {
        line_no_++;
        while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) n--;
        cur_.assign(s, n);
        const char *c = cur_.c_str();
        if (starts(c, "@IQ3_START,")) onStart(c);
        else if (!running_) return;
        else if (starts(c, "@IQ3,")) onIq3(c);
        else if (starts(c, "@RAW,")) onRaw(c);
        else if (starts(c, "@ROOMPROF,")) onRoomProf(c);
        else if (starts(c, "@DSP,")) onDsp(c);
        else if (starts(c, "@ROOM,")) onRoom(c);
        else if (starts(c, "@VITAL,")) onVital(c);
        else if (starts(c, "@REZOOM,")) onRezoom(c);
        else if (starts(c, "@RECONNECT,")) onReconnect(c);
        else if (starts(c, "@WARN,")) emit(c);
        else if (strcmp(c, "@END") == 0) onEnd(c);
    }

    /** Liest eine ganze Datei. Rückgabe false, wenn sie nicht lesbar ist. */
    bool runFile(FILE *in) {
        if (!in) return false;
        char *buf = nullptr;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&buf, &cap, in)) >= 0) line(buf, (size_t)n);
        free(buf);
        return true;
    }

    /** Schreibt den Bericht. Rückgabe: true = alle verglichenen Zeilen gleich und keine Lücke. */
    bool finish() {
        closePending();
        bool all_equal = frames_ > 0;
        unsigned long cmp = 0, eq = 0;
        for (TagStat &t : stats_) {
            cmp += t.compared;
            eq += t.equal;
            if (t.equal != t.compared) all_equal = false;
        }
        if (gaps_ > 0 || extra_ > 0) all_equal = false;
        bool last_digit = frames_ > 0 && gaps_ == 0 && extra_ == 0;
        for (TagStat &t : stats_)
            if (t.other > 0 || t.max_units > 1.0 + 1e-6) last_digit = false;
        report("DSP-Nachrechnung: %lu Frames in %u Stream(s), Frame-Lücken %lu, Rezooms %lu, Neuverbindungen %lu\n",
               frames_, streams_, gaps_, rezooms_, reconnects_);
        for (TagStat &t : stats_) {
            report("  %-10s verglichen %7lu, gleich %7lu", t.tag, t.compared, t.equal);
            if (t.equal != t.compared)
                report(", größte Abweichung %.4g (Feld %d, %.0f Einheit(en) der letzten Stelle), andere Unterschiede %lu",
                       t.max_dev, t.max_dev_field, t.max_units, t.other);
            report("\n");
        }
        report("  fehlende/zusätzliche Zeilen: %lu\n", extra_);
        report("ERGEBNIS: %s (%lu von %lu Zeilen gleich)\n",
               all_equal ? "BITGLEICH" : last_digit ? "GLEICH BIS AUF DIE LETZTE STELLE" : "ABWEICHUNG", eq, cmp);
        return all_equal;
    }

private:
    enum { IQ3, ROOMPROF, DSP, ROOM, VITAL, REZOOM, NTAGS };

    static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

    /** Feld i (0 = Tag) als Zeiger auf den Anfang; nullptr, wenn es fehlt. */
    static const char *field(const char *s, int i) {
        for (; i > 0; i--) {
            s = strchr(s, ',');
            if (!s) return nullptr;
            s++;
        }
        return s;
    }
    static long lfield(const char *s, int i, long def = 0) {
        const char *f = field(s, i);
        return f ? strtol(f, nullptr, 10) : def;
    }
    static unsigned long ufield(const char *s, int i) {
        const char *f = field(s, i);
        return f ? strtoul(f, nullptr, 10) : 0UL;
    }

    void report(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!rep_out_) return;
        va_list ap;
        va_start(ap, fmt);
        vfprintf(rep_out_, fmt, ap);
        va_end(ap);
    }

    void emit(const char *s) {
        if (out_) {
            fputs(s, out_);
            fputc('\n', out_);
        }
    }

    /** Zahlenfelder vergleichen: größte |Differenz|, Feldnummer (1 = erstes Feld nach dem Tag); false = nicht vergleichbar. */
    static bool numericDiff(const char *a, const char *b, double &dev, int &field_no, double &units) {
        dev = 0.0;
        field_no = 0;
        units = 0.0;
        a = strchr(a, ',');
        b = strchr(b, ',');
        for (int i = 1; a && b; i++) {
            char *ea, *eb;
            const double va = strtod(a + 1, &ea), vb = strtod(b + 1, &eb);
            if (ea == a + 1 || eb == b + 1) return false;
            const char *dot = (const char *)memchr(a + 1, '.', (size_t)(ea - (a + 1)));
            const double unit = dot ? pow(10.0, -(double)(ea - dot - 1)) : 1.0;
            if (fabs(va - vb) > dev) {
                dev = fabs(va - vb);
                field_no = i;
            }
            if (fabs(va - vb) / unit > units) units = fabs(va - vb) / unit;
            if ((*ea == ',') != (*eb == ',') || (*ea != ',' && *ea != 0) || (*eb != ',' && *eb != 0)) return false;
            a = *ea == ',' ? ea : nullptr;
            b = *eb == ',' ? eb : nullptr;
        }
        return a == nullptr && b == nullptr;
    }

    void compare(int tag, const char *esp, const char *mine) {
        TagStat &t = stats_[tag];
        t.compared++;
        if (strcmp(esp, mine) == 0) {
            t.equal++;
            return;
        }
        double dev, units;
        int fno;
        if (!numericDiff(esp, mine, dev, fno, units)) {
            t.other++;
        } else {
            if (dev > t.max_dev) {
                t.max_dev = dev;
                t.max_dev_field = fno;
            }
            if (units > t.max_units) t.max_units = units;
        }
        if (t.shown < 3) {
            t.shown++;
            report("Abweichung %s, Zeile %lu\n  ESP32: %.300s\n  neu:   %.300s\n", t.tag, line_no_, esp, mine);
        }
    }

    void missing(const char *what) {
        extra_++;
        if (extra_ <= 5) report("Zeile %lu: %s\n", line_no_, what);
    }

    /** Eigenes @VITAL ausgeben, wenn der ESP32 zu diesem Report keines hatte (Zeit = @DSP-Zeile). */
    void flushVital() {
        if (!pend_vital_) return;
        missing("@VITAL neu gerechnet, beim ESP32 fehlt sie");
        formatVitalLine(buf_, sizeof(buf_), dsp_t_ms_, rep_);
        emit(buf_);
        pend_vital_ = false;
    }

    /** Erwartete, aber vom ESP32 nicht ausgegebene Zeilen des letzten Reports melden. */
    void closePending() {
        flushVital();
        if (pend_rezoom_) missing("@REZOOM neu gerechnet, beim ESP32 fehlt er");
        pend_rezoom_ = false;
    }

    static int32_t mmToPt(long mm) { return (int32_t)lroundf((float)mm / 2.5f); }

    void onStart(const char *c) {
        closePending();
        zoom_pt_ = mmToPt(lfield(c, 1));
        running_ = dsp_.begin(zoom_pt_, 120, kRoomStep);
        if (!running_) report("FEHLER: DSP-Speicher\n");
        streams_++;
        last_frame_ = -1;
        have_iq3_ = false;
        emit(c);
    }

    void onIq3(const char *) {
        closePending();
        iq3_ = cur_;
        have_iq3_ = true;
    }

    static int b64val(char ch) {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    }

    /** Base64 → Bytes; Rückgabe Anzahl Bytes oder -1. */
    static long b64decode(const char *s, uint8_t *dst, size_t cap) {
        size_t n = 0;
        uint32_t acc = 0;
        int bits = 0;
        for (; *s && *s != '='; s++) {
            const int v = b64val(*s);
            if (v < 0) return -1;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (n >= cap) return -1;
                dst[n++] = (uint8_t)(acc >> bits);
            }
        }
        return (long)n;
    }

    void onRaw(const char *c) {
        closePending();
        const long frame = lfield(c, 1);
        const unsigned long tick = ufield(c, 2);
        const long n_pts = lfield(c, 3);
        const char *b64 = field(c, 4);
        if (!b64 || n_pts <= 0 || n_pts > (long)kTotal3SubPts) {
            missing("@RAW unlesbar");
            return;
        }
        uint8_t bytes[kTotal3SubPts * sizeof(Complex16)];
        const long nb = b64decode(b64, bytes, sizeof(bytes));
        if (nb != n_pts * (long)sizeof(Complex16)) {
            missing("@RAW: Länge passt nicht");
            return;
        }
        Complex16 pts[kTotal3SubPts];
        for (long i = 0; i < n_pts; i++) {  /* Little Endian wie im ESP32-Speicher */
            pts[i].real = (int16_t)(bytes[4 * i] | (bytes[4 * i + 1] << 8));
            pts[i].imag = (int16_t)(bytes[4 * i + 2] | (bytes[4 * i + 3] << 8));
        }
        if (last_frame_ >= 0 && frame != last_frame_ + 1) gaps_++;
        last_frame_ = frame;
        last_tick_ = tick;
        frames_++;

        // dieselbe Reihenfolge wie cmdIq3(): pushFrame, dann Amplituden für @IQ3
        const char *iq3 = have_iq3_ && lfield(iq3_.c_str(), 7, -1) == frame ? iq3_.c_str() : nullptr;
        const int16_t temp = iq3 ? (int16_t)lfield(iq3, 2) : 0;
        float um = 0.0f;
        dsp_.pushFrame(pts, (uint16_t)n_pts, temp, 0.0f, um);
        const float lb_amp = sqrtf((float)pts[0].real * pts[0].real + (float)pts[0].imag * pts[0].imag);
        const uint16_t b0 = 1U + (kZoomPoints / 2U);
        const float z_amp = sqrtf((float)pts[b0].real * pts[b0].real + (float)pts[b0].imag * pts[b0].imag);
        if (!iq3) {
            missing("@IQ3 zum Frame fehlt");
            return;
        }
        snprintf(buf_, sizeof(buf_), "@IQ3,%lu,%d,%.1f,%.2f,%.1f,%lu,%lu,%lu", ufield(iq3, 1), (int)temp, lb_amp, um,
                 z_amp, ufield(iq3, 6), (unsigned long)frame, tick);
        compare(IQ3, iq3, buf_);
        emit(buf_);
        have_iq3_ = false;
    }

    void onRoomProf(const char *c) {
        formatRoomProfileLine(buf_, sizeof(buf_), (uint32_t)ufield(c, 1), (uint32_t)last_tick_, dsp_);
        compare(ROOMPROF, c, buf_);
        emit(buf_);
    }

    void onDsp(const char *c) {
        closePending();
        vital_ok_ = dsp_.computeVitalReport(rep_);
        have_rep_ = true;
        dsp_t_ms_ = (uint32_t)ufield(c, 1);
        formatDspLine(buf_, sizeof(buf_), dsp_t_ms_, vital_ok_, rep_, (uint32_t)ufield(c, 5) /* Rechenzeit vom ESP32 */);
        compare(DSP, c, buf_);
        emit(buf_);
        pend_vital_ = vital_ok_;
        pend_rezoom_ = rep_.needs_rezoom && rep_.recommended_zoom_start_pt != zoom_pt_;
    }

    void onRoom(const char *c) {
        if (!have_rep_) return;
        formatRoomLine(buf_, sizeof(buf_), (uint32_t)ufield(c, 1), (uint32_t)last_tick_, rep_);
        compare(ROOM, c, buf_);
        emit(buf_);
    }

    void onVital(const char *c) {
        if (!have_rep_) return;
        if (!pend_vital_) {
            missing("@VITAL beim ESP32, neu gerechnet ungültig");
            stats_[VITAL].compared++;
            return;
        }
        pend_vital_ = false;
        formatVitalLine(buf_, sizeof(buf_), (uint32_t)ufield(c, 1), rep_);
        compare(VITAL, c, buf_);
        emit(buf_);
    }

    void onRezoom(const char *c) {
        flushVital();
        const int32_t esp_pt = mmToPt(lfield(c, 1));
        if (pend_rezoom_) {
            formatRezoomLine(buf_, sizeof(buf_), rep_.recommended_zoom_start_pt);
            compare(REZOOM, c, buf_);
            pend_rezoom_ = false;
        } else {
            missing("@REZOOM beim ESP32, neu gerechnet keiner");
            stats_[REZOOM].compared++;
        }
        // dem ESP32 folgen: die folgenden Rohdaten gehören zu seinem Fenster
        zoom_pt_ = esp_pt;
        dsp_.setZoomStartPoint(zoom_pt_);
        rezooms_++;
        emit(c);
    }

    void onReconnect(const char *c) {
        if (lfield(c, 3) == 1) {
            dsp_.setZoomStartPoint(zoom_pt_);  /* wie cmdIq3(): Lücke im Signal, Verlauf neu */
            reconnects_++;
            last_frame_ = -1;  /* Frame-Zähler läuft weiter, aber über die Lücke nicht als Verlust zählen */
        }
        emit(c);
    }

    void onEnd(const char *c) {
        closePending();
        running_ = false;
        emit(c);
    }

    FILE *out_;
    FILE *rep_out_;
    UltimateEspDsp &dsp_;
    VitalReport rep_ = {};
    bool running_ = false, have_iq3_ = false, have_rep_ = false, vital_ok_ = false;
    bool pend_vital_ = false, pend_rezoom_ = false;
    int32_t zoom_pt_ = 0;
    uint32_t dsp_t_ms_ = 0;
    long last_frame_ = -1;
    unsigned long last_tick_ = 0, line_no_ = 0, frames_ = 0, gaps_ = 0, rezooms_ = 0, reconnects_ = 0, extra_ = 0;
    unsigned streams_ = 0;
    std::string cur_, iq3_;
    char buf_[kRoomLineMax];
    TagStat stats_[NTAGS] = {{"@IQ3"}, {"@ROOMPROF"}, {"@DSP"}, {"@ROOM"}, {"@VITAL"}, {"@REZOOM"}};
};

}  // namespace dsp_replay

#endif /* DSP_REPLAY_H */
