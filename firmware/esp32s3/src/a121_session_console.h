/**
 * @file a121_session_console.h
 * @brief Console command `sess`: set up an arbitrary sparse IQ session on the A121 and evaluate it on the ESP32.
 *
 * The configuration lives in RAM and is changed field by field; `sess run` sends it to the Exploration Server,
 * streams and evaluates per subsweep (amplitude averaged over all sweeps, strongest point). The output is
 * throttled so that the ESP32 never falls behind the data stream (otherwise the 32 KB UART buffer would overflow).
 * The server checks the limits; `sess` reports rejected configurations with the reason from the server log.
 *
 * Output lines (CSV, for scripts):
 *   @SESS_META,frame_pts,sweep_pts,max_sweep_rate_hz,high_speed,cal_temp_c,base_step_mm,kB_pro_Frame
 *   @SESS_SUB,i,offset,length,start_mm,step_mm
 *   @SESS_STAT,t_ms,fps,frames,delayed,saturated,temp_c,rx_backlog,{peak_mm,peak_amp}×Subsweeps   (1× per s)
 *   @PEAK,t_ms,tick,temp_c,flags,{peak_mm,peak_amp}×Subsweeps                                     (max. 100/s)
 *   @AMP,t_ms,i,start_mm,step_mm,a0,a1,...                                                       (max. 10/s)
 *   @SESS_END,frames,dauer_s,fps,delayed,saturated,cal_needed,timeouts,uart_ovf,uart_buf_full,uart_frame,max_backlog
 *   flags: Bit0 saturated, Bit1 frame_delayed, Bit2 calibration_needed
 */

#ifndef A121_SESSION_CONSOLE_H
#define A121_SESSION_CONSOLE_H

#include <Arduino.h>
#include <math.h>

#include "a121_server_client.h"

namespace a121_sess {

static const uint16_t kMaxFramePts = 4096;      // points per frame the ESP32 accepts (16 KB)
static const float    kLinkLimitKBs = 190.0f;   // net ≈ 195 kB/s at 2 Mbaud (on the device 2026-11-01)
static const float    kHeaderBytes  = 150.0f;   // JSON header line per frame; noticeable with small frames (137 Hz instead of 141 Hz)

struct Metadata {
    bool     valid = false;
    uint32_t frame_pts = 0, sweep_pts = 0;
    uint8_t  num_sub = 0;
    uint32_t sub_offset[4] = {0}, sub_length[4] = {0};
    float    max_sweep_rate = 0.0f, base_step_mm = 2.5f;
    int      cal_temp = 0;
    bool     high_speed = false;
};

// ---- small JSON helpers (the server replies are flat and known) ----
inline bool jsonNum(const String &js, const char *key, double &out) {
    String k = String("\"") + key + "\":";
    int i = js.indexOf(k);
    if (i < 0) return false;
    out = atof(js.c_str() + i + k.length());
    return true;
}
inline int jsonUintArray(const String &js, const char *key, uint32_t *out, int max) {
    String k = String("\"") + key + "\":[";
    int i = js.indexOf(k);
    if (i < 0) return 0;
    const char *p = js.c_str() + i + k.length();
    int n = 0;
    while (*p && *p != ']' && n < max) {
        char *e;
        unsigned long v = strtoul(p, &e, 10);
        if (e == p) break;
        out[n++] = (uint32_t)v;
        p = e;
        while (*p == ',' || *p == ' ') p++;
    }
    return n;
}
inline String jsonStr(const String &js, const char *key) {
    String k = String("\"") + key + "\":\"";
    int i = js.indexOf(k);
    if (i < 0) return String();
    int e = js.indexOf('"', i + k.length());
    return js.substring(i + k.length(), e < 0 ? js.length() : e);
}

inline Metadata parseMetadata(const String &ack) {
    Metadata md;
    int m = ack.indexOf("\"metadata\"");
    if (m < 0) return md;
    String js = ack.substring(m);
    double v;
    if (jsonNum(js, "frame_data_length", v)) md.frame_pts = (uint32_t)v; else return md;
    if (jsonNum(js, "sweep_data_length", v)) md.sweep_pts = (uint32_t)v;
    if (jsonNum(js, "max_sweep_rate", v)) md.max_sweep_rate = (float)v;
    if (jsonNum(js, "calibration_temperature", v)) md.cal_temp = (int)v;
    if (jsonNum(js, "base_step_length_m", v)) md.base_step_mm = (float)(v * 1000.0);
    md.high_speed = js.indexOf("\"high_speed_mode\":true") >= 0;
    int n1 = jsonUintArray(js, "subsweep_data_offset", md.sub_offset, 4);
    int n2 = jsonUintArray(js, "subsweep_data_length", md.sub_length, 4);
    md.num_sub = (uint8_t)min(n1, n2);
    md.valid = md.num_sub > 0 && md.sweep_pts > 0;
    return md;
}

// UART error counters during `sess run` (callback runs in the UART driver's event task)
static volatile uint32_t g_errOvf = 0, g_errBufFull = 0, g_errFrame = 0;

class SessionConsole {
public:
    SessionConsole() { setDefaults(); }

    void setDefaults() {
        spec_ = {};
        spec_.sweeps_per_frame       = 1;
        spec_.sweep_rate_hz          = 0.0f;
        spec_.frame_rate_hz          = 10.0f;
        spec_.continuous_sweep_mode  = false;
        spec_.double_buffering       = false;
        spec_.inter_frame_idle_state = "deep_sleep";   // exptool default
        spec_.inter_sweep_idle_state = "ready";
        spec_.num_subsweeps          = 1;
        for (auto &s : spec_.subsweeps) s = defaultSub();
    }

    /** `args` = everything after "sess". `ensure` connects the server (break reset + baud negotiation). */
    void handle(const String &args, a121_srv::ExplorationServerClient &srv, bool (*ensure)()) {
        String tok[6];
        int n = split(args, tok, 6);
        if (n == 0) { show(); return; }
        const String &a = tok[0];
        if (a == "help") { help(); return; }
        if (a == "default") { setDefaults(); Serial.println("sess: Standardwerte"); show(); return; }
        if (a == "check") { run(srv, ensure, 0, Mode::CHECK); return; }
        if (a == "run") {
            int sek = n > 1 ? tok[1].toInt() : 10;
            Mode m = Mode::STAT;
            if (n > 2 && tok[2] == "peak") m = Mode::PEAK;
            else if (n > 2 && tok[2] == "amp") m = Mode::AMP;
            else if (n > 2 && tok[2] != "stat") { Serial.println("sess run [sek] [stat|peak|amp]"); return; }
            run(srv, ensure, sek < 0 ? 0 : (uint32_t)sek, m);
            return;
        }
        if (isdigit((unsigned char)a[0])) {
            int i = a.toInt();
            if (i < 0 || i > 3) { Serial.println("Subsweep 0..3"); return; }
            if (setSub(i, tok, n)) show(); else subHelp();
            return;
        }
        if (n >= 2 && setSession(tok, n)) { show(); return; }
        help();
    }

    const a121_srv::SessionSpec &spec() const { return spec_; }

private:
    enum class Mode { CHECK, STAT, PEAK, AMP };
    a121_srv::SessionSpec spec_;

    // large buffers static (the Arduino loop task has only 8 KB of stack)
    static a121_dsp::Complex16 *frameBuf() { static a121_dsp::Complex16 b[kMaxFramePts]; return b; }
    static float *ampBuf() { static float b[kMaxFramePts]; return b; }

    static a121_srv::SubsweepSpec defaultSub() {
        // exptool default, only a more useful range: 200–395 mm on a 5 mm grid
        return {80, 40, 2, 3, 8, 16, true, false, false, false, "15_6_MHz"};
    }

    static int split(const String &s, String *out, int max) {
        int n = 0, i = 0, len = s.length();
        while (i < len && n < max) {
            while (i < len && s[i] == ' ') i++;
            if (i >= len) break;
            int j = i;
            while (j < len && s[j] != ' ') j++;
            out[n++] = s.substring(i, j);
            i = j;
        }
        return n;
    }

    static bool parseBool(const String &v, bool &out) {
        if (v == "1" || v == "on" || v == "true") { out = true; return true; }
        if (v == "0" || v == "off" || v == "false") { out = false; return true; }
        return false;
    }

    static const char *parsePrf(const String &v) {
        static const char *names[] = {"19_5_MHz", "15_6_MHz", "13_0_MHz", "8_7_MHz", "6_5_MHz", "5_2_MHz"};
        static const char *shortNames[] = {"19.5", "15.6", "13", "8.7", "6.5", "5.2"};
        for (int i = 0; i < 6; i++)
            if (v == names[i] || v == shortNames[i] || (i == 2 && v == "13.0")) return names[i];
        return nullptr;
    }

    static const char *parseIdle(const String &v) {
        if (v == "deep_sleep" || v == "deep") return "deep_sleep";
        if (v == "sleep") return "sleep";
        if (v == "ready") return "ready";
        return nullptr;
    }

    bool setSession(const String *t, int n) {
        const String &k = t[0], &v = t[1];
        bool b;
        if (k == "fr") spec_.frame_rate_hz = max(0.0f, v.toFloat());
        else if (k == "sr") spec_.sweep_rate_hz = max(0.0f, v.toFloat());
        else if (k == "spf") spec_.sweeps_per_frame = (uint16_t)constrain(v.toInt(), 1, 4095);
        else if (k == "csm" && parseBool(v, b)) spec_.continuous_sweep_mode = b;
        else if (k == "dbuf" && parseBool(v, b)) spec_.double_buffering = b;
        else if (k == "fidle" && parseIdle(v)) spec_.inter_frame_idle_state = parseIdle(v);
        else if (k == "sidle" && parseIdle(v)) spec_.inter_sweep_idle_state = parseIdle(v);
        else if (k == "subs") {
            int ns = constrain(v.toInt(), 1, 4);
            for (int i = spec_.num_subsweeps; i < ns; i++) spec_.subsweeps[i] = spec_.subsweeps[0];  // new ones = copy of 0
            spec_.num_subsweeps = (uint8_t)ns;
        } else return false;
        return true;
    }

    bool setSub(int i, const String *t, int n) {
        if (n < 3) return false;
        a121_srv::SubsweepSpec &s = spec_.subsweeps[i];
        const String &k = t[1], &v = t[2];
        bool b;
        if (k == "range" && n >= 4) {
            // a..b mm on the current grid: start at the next point, count such that b is still inside
            float a = v.toFloat(), e = t[3].toFloat();
            if (e < a) return false;
            s.start_point = (int32_t)lroundf(a / 2.5f);
            s.num_points  = (uint16_t)max(1L, (long)floorf((e - s.start_point * 2.5f) / (2.5f * s.step_length)) + 1);
        }
        else if (k == "start") s.start_point = v.toInt();
        else if (k == "n") s.num_points = (uint16_t)constrain(v.toInt(), 1, 4095);
        else if (k == "step") s.step_length = (uint16_t)constrain(v.toInt(), 1, 4095);
        else if (k == "profile") s.profile = (uint8_t)constrain(v.toInt(), 1, 5);
        else if (k == "hwaas") s.hwaas = (uint16_t)constrain(v.toInt(), 1, 511);
        else if (k == "gain") s.receiver_gain = (uint8_t)constrain(v.toInt(), 0, 23);
        else if (k == "prf" && parsePrf(v)) s.prf = parsePrf(v);
        else if (k == "tx" && parseBool(v, b)) s.enable_tx = b;
        else if (k == "loop" && parseBool(v, b)) s.enable_loopback = b;
        else if (k == "phase" && parseBool(v, b)) s.phase_enhancement = b;
        else if (k == "iqimb" && parseBool(v, b)) s.iq_imbalance_compensation = b;
        else return false;
        if (i >= spec_.num_subsweeps) Serial.printf("Hinweis: Subsweep %d ist gesetzt, aber aktiv sind nur %u ('sess subs %d').\n",
                                                    i, spec_.num_subsweeps, i + 1);
        return true;
    }

    uint32_t pointsPerSweep() const {
        uint32_t p = 0;
        for (int i = 0; i < spec_.num_subsweeps; i++) p += spec_.subsweeps[i].num_points;
        return p;
    }

    /** Expected frame rate: the set frame_rate, else with sweep_rate sweep_rate/spf, else unknown (0). */
    float expectedFps() const {
        if (spec_.frame_rate_hz > 0.0f) return spec_.frame_rate_hz;
        if (spec_.sweep_rate_hz > 0.0f) return spec_.sweep_rate_hz / spec_.sweeps_per_frame;
        return 0.0f;
    }

    void show() const {
        const uint32_t pts = pointsPerSweep() * spec_.sweeps_per_frame;
        const float kb = pts * 4.0f / 1000.0f, kbLink = (pts * 4.0f + kHeaderBytes) / 1000.0f, fps = expectedFps();
        Serial.printf("Session: spf %u, frame_rate %.1f Hz, sweep_rate %.1f Hz, csm %d, dbuf %d, idle frame %s / sweep %s\n",
                      spec_.sweeps_per_frame, spec_.frame_rate_hz, spec_.sweep_rate_hz, spec_.continuous_sweep_mode,
                      spec_.double_buffering, spec_.inter_frame_idle_state, spec_.inter_sweep_idle_state);
        for (int i = 0; i < spec_.num_subsweeps; i++) {
            const auto &s = spec_.subsweeps[i];
            Serial.printf("  [%d] %.1f–%.1f mm (start %ld, n %u, step %u = %.1f mm), profile %u, hwaas %u, gain %u, prf %s, "
                          "tx %d, loop %d, phase %d, iqimb %d\n",
                          i, s.start_point * 2.5f, (s.start_point + (s.num_points - 1) * s.step_length) * 2.5f,
                          (long)s.start_point, s.num_points, s.step_length, s.step_length * 2.5f, s.profile, s.hwaas,
                          s.receiver_gain, s.prf, s.enable_tx, s.enable_loopback, s.phase_enhancement,
                          s.iq_imbalance_compensation);
        }
        Serial.printf("  %lu Punkte/Frame = %.2f kB", (unsigned long)pts, kb);
        if (fps > 0.0f) {
            Serial.printf(" × %.1f Hz = %.1f kB/s (mit Kopfzeilen %.1f kB/s)", fps, kb * fps, kbLink * fps);
            if (kbLink * fps > kLinkLimitKBs) Serial.printf("  → über der Link-Grenze (~195 kB/s), Server meldet frame_delayed");
        } else {
            Serial.print(" (Rate frei: so schnell der Sensor misst)");
        }
        Serial.println();
        if (pts > kMaxFramePts) Serial.printf("  → mehr als %u Punkte/Frame: 'sess run' lehnt ab\n", kMaxFramePts);
    }

    void help() const {
        Serial.println(
            "sess                       Konfiguration anzeigen\n"
            "sess default               Standardwerte (1 Subsweep, 200–395 mm, Profil 3, 10 Hz)\n"
            "sess fr|sr <Hz>            frame_rate / sweep_rate (0 = frei)\n"
            "sess spf <n>               Sweeps pro Frame\n"
            "sess csm|dbuf 0|1          Continuous Sweep Mode / Double Buffering\n"
            "sess fidle|sidle deep|sleep|ready   Idle-Zustand zwischen Frames / Sweeps\n"
            "sess subs <1..4>           Anzahl Subsweeps (neue = Kopie von Subsweep 0)\n"
            "sess <i> ...               Subsweep i (0..3), 'sess 0' für die Liste\n"
            "sess check                 nur an den Server schicken: Metadaten oder Fehlergrund\n"
            "sess run [sek] [stat|peak|amp]   streamen + auswerten (Standard 10 s, stat; Taste bricht ab)");
    }

    void subHelp() const {
        Serial.println(
            "sess <i> range <a> <b>     Bereich a–b mm (mit aktuellem step)\n"
            "sess <i> start <pt>        Startpunkt (1 Punkt = 2,5 mm)\n"
            "sess <i> n <anz>           Anzahl Punkte\n"
            "sess <i> step <s>          Schrittweite in Punkten (Teiler/Vielfaches von 24, z. B. 1 2 3 4 6 8 12 24)\n"
            "sess <i> profile <1..5>    hwaas <1..511>   gain <0..23>\n"
            "sess <i> prf 19.5|15.6|13|8.7|6.5|5.2\n"
            "sess <i> tx|loop|phase|iqimb 0|1");
    }

    void printSetupError(const String &ack, const String &logs) const {
        String msg = jsonStr(ack, "message");
        Serial.printf("FEHLER: Server lehnt die Konfiguration ab: %s\n", msg.length() ? msg.c_str() : "(keine Antwort)");
        if (logs.length()) {
            Serial.print("  Server-Log:\n  ");
            String l = logs;
            l.trim();
            l.replace("\n", "\n  ");
            Serial.println(l);
        }
    }

    void run(a121_srv::ExplorationServerClient &srv, bool (*ensure)(), uint32_t durationSec, Mode mode) {
        if (pointsPerSweep() * spec_.sweeps_per_frame > kMaxFramePts) {
            Serial.printf("FEHLER: %lu Punkte/Frame, der ESP32 nimmt höchstens %u an.\n",
                          (unsigned long)(pointsPerSweep() * spec_.sweeps_per_frame), kMaxFramePts);
            return;
        }
        if (!ensure()) return;

        String ack, logs;
        if (!srv.setupSession(spec_, &ack, &logs)) { printSetupError(ack, logs); return; }
        const Metadata md = parseMetadata(ack);
        if (!md.valid || md.num_sub != spec_.num_subsweeps || md.frame_pts > kMaxFramePts ||
            md.frame_pts != md.sweep_pts * spec_.sweeps_per_frame) {
            Serial.printf("FEHLER: unerwartete Metadaten (frame %lu, sweep %lu, subs %u)\n",
                          (unsigned long)md.frame_pts, (unsigned long)md.sweep_pts, md.num_sub);
            return;
        }
        Serial.printf("@SESS_META,%lu,%lu,%.1f,%d,%d,%.4f,%.2f\n", (unsigned long)md.frame_pts, (unsigned long)md.sweep_pts,
                      md.max_sweep_rate, md.high_speed, md.cal_temp, md.base_step_mm, md.frame_pts * 4.0f / 1000.0f);
        float subStartMm[4], subStepMm[4];
        for (int i = 0; i < md.num_sub; i++) {
            const auto &s = spec_.subsweeps[i];
            subStartMm[i] = s.start_point * md.base_step_mm;
            subStepMm[i]  = s.step_length * md.base_step_mm;
            Serial.printf("@SESS_SUB,%d,%lu,%lu,%.1f,%.2f\n", i, (unsigned long)md.sub_offset[i],
                          (unsigned long)md.sub_length[i], subStartMm[i], subStepMm[i]);
        }
        if (spec_.sweep_rate_hz > md.max_sweep_rate && md.max_sweep_rate > 0.0f)
            Serial.printf("Hinweis: sweep_rate %.1f Hz > max_sweep_rate %.1f Hz dieser Konfiguration.\n",
                          spec_.sweep_rate_hz, md.max_sweep_rate);
        if (logs.length()) { Serial.print("Server-Log: "); Serial.print(logs); }
        if (mode == Mode::CHECK) { Serial.println("sess check: ok (nicht gestartet)"); return; }

        HardwareSerial &uart = srv.uart();
        g_errOvf = g_errBufFull = g_errFrame = 0;
        uart.onReceiveError([](hardwareSerial_error_t e) {
            if (e == UART_FIFO_OVF_ERROR) g_errOvf++;
            else if (e == UART_BUFFER_FULL_ERROR) g_errBufFull++;
            else if (e == UART_FRAME_ERROR) g_errFrame++;
        });

        if (!srv.startStreaming()) {
            Serial.println("FEHLER: start_streaming abgelehnt");
            uart.onReceiveError(nullptr);
            return;
        }

        // Timeout per frame: three times the expected period, at least 0.5 s, at most 10 s
        const float fps = expectedFps();
        const uint32_t frameTimeout = fps > 0.0f ? (uint32_t)constrain(3000.0f / fps + 300.0f, 500.0f, 10000.0f) : 1000U;

        a121_dsp::Complex16 *buf = frameBuf();
        float *amp = ampBuf();
        a121_srv::FrameMeta meta = {};
        float peakMm[4] = {0}, peakAmp[4] = {0};
        uint32_t frames = 0, delayed = 0, saturated = 0, calNeeded = 0, timeouts = 0, consecTimeouts = 0;
        uint32_t secFrames = 0, maxBacklog = 0;
        bool calWarned = false;
        const uint32_t tStart = millis();
        uint32_t tSec = tStart, tLastPeak = 0, tLastAmp = 0, tFirstFrame = 0, tLastFrame = 0;

        while (true) {
            if (Serial.available()) { while (Serial.available()) Serial.read(); break; }
            if (durationSec > 0 && (millis() - tStart) >= durationSec * 1000UL) break;

            if (!srv.readStreamFrame(buf, kMaxFramePts, meta, frameTimeout)) {
                timeouts++;
                if (++consecTimeouts >= 5) { Serial.println("FEHLER: 5 Frames in Folge ausgeblieben, Abbruch."); break; }
                Serial.println("@WARN,frame_timeout");
                continue;
            }
            consecTimeouts = 0;
            const uint32_t now = millis();
            if (frames == 0) tFirstFrame = now;
            tLastFrame = now;
            frames++;
            secFrames++;
            delayed   += meta.frame_delayed;
            saturated += meta.data_saturated;
            calNeeded += meta.calibration_needed;
            if (meta.calibration_needed && !calWarned) {
                Serial.println("@WARN,calibration_needed (Temperatur seit Setup verändert; 'sess run' neu starten)");
                calWarned = true;
            }
            const size_t backlog = uart.available();
            if (backlog > maxBacklog) maxBacklog = backlog;
            if (meta.payload_bytes != md.frame_pts * 4U) {
                Serial.printf("@WARN,payload %u statt %lu Bytes\n", meta.payload_bytes, (unsigned long)(md.frame_pts * 4U));
                continue;
            }

            // average amplitude per point over all sweeps, find the strongest point per subsweep
            const uint32_t sp = md.sweep_pts;
            for (uint32_t k = 0; k < sp; k++) amp[k] = 0.0f;
            for (uint16_t s = 0; s < spec_.sweeps_per_frame; s++) {
                const a121_dsp::Complex16 *sw = buf + (size_t)s * sp;
                for (uint32_t k = 0; k < sp; k++) amp[k] += sqrtf((float)sw[k].real * sw[k].real + (float)sw[k].imag * sw[k].imag);
            }
            const float inv = 1.0f / spec_.sweeps_per_frame;
            for (uint32_t k = 0; k < sp; k++) amp[k] *= inv;
            for (int i = 0; i < md.num_sub; i++) {
                uint32_t best = 0;
                for (uint32_t k = 1; k < md.sub_length[i]; k++)
                    if (amp[md.sub_offset[i] + k] > amp[md.sub_offset[i] + best]) best = k;
                peakMm[i]  = subStartMm[i] + best * subStepMm[i];
                peakAmp[i] = amp[md.sub_offset[i] + best];
            }

            const unsigned flags = (meta.data_saturated ? 1 : 0) | (meta.frame_delayed ? 2 : 0) | (meta.calibration_needed ? 4 : 0);
            if (mode == Mode::PEAK && now - tLastPeak >= 10) {
                tLastPeak = now;
                Serial.printf("@PEAK,%lu,%lu,%d,%u", (unsigned long)(now - tStart), (unsigned long)meta.tick, meta.temperature_c, flags);
                for (int i = 0; i < md.num_sub; i++) Serial.printf(",%.1f,%.0f", peakMm[i], peakAmp[i]);
                Serial.println();
            }
            if (mode == Mode::AMP && now - tLastAmp >= 100) {
                tLastAmp = now;
                for (int i = 0; i < md.num_sub; i++) {
                    Serial.printf("@AMP,%lu,%d,%.1f,%.2f", (unsigned long)(now - tStart), i, subStartMm[i], subStepMm[i]);
                    for (uint32_t k = 0; k < md.sub_length[i]; k++) Serial.printf(",%.0f", amp[md.sub_offset[i] + k]);
                    Serial.println();
                }
            }
            if (now - tSec >= 1000) {
                const float secFps = secFrames * 1000.0f / (now - tSec);
                Serial.printf("@SESS_STAT,%lu,%.2f,%lu,%lu,%lu,%d,%u", (unsigned long)(now - tStart), secFps,
                              (unsigned long)frames, (unsigned long)delayed, (unsigned long)saturated, meta.temperature_c,
                              (unsigned)backlog);
                for (int i = 0; i < md.num_sub; i++) Serial.printf(",%.1f,%.0f", peakMm[i], peakAmp[i]);
                Serial.println();
                tSec = now;
                secFrames = 0;
            }
        }

        srv.stopStreaming();
        uart.onReceiveError(nullptr);
        const float durS = (tLastFrame - tFirstFrame) / 1000.0f;
        const float fpsAll = (frames > 1 && durS > 0.0f) ? (frames - 1) / durS : 0.0f;
        Serial.printf("@SESS_END,%lu,%.2f,%.2f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n", (unsigned long)frames, durS, fpsAll,
                      (unsigned long)delayed, (unsigned long)saturated, (unsigned long)calNeeded, (unsigned long)timeouts,
                      (unsigned long)g_errOvf, (unsigned long)g_errBufFull, (unsigned long)g_errFrame,
                      (unsigned long)maxBacklog);
    }
};

}  // namespace a121_sess

#endif
