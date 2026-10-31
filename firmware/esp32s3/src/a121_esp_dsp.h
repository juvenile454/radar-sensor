/**
 * @file a121_esp_dsp.h
 * @brief On-Device 3-Subsweep Sparse-IQ & Vital-Signs DSP Engine für den ESP32-S3 (Core 1 / PSRAM).
 *
 * Implementiert die vollständige Signalverarbeitungskette direkt auf dem ESP32-S3:
 *   1. On-Chip Loopback Phasen-Derotation (Subsweep 0 -> kompensiert PLL-/Taktjitter pro Frame)
 *   2. Statisches Clutter-Filter & kohärentes 5-Bin Spatial Beamforming (Subsweep 1, 24 Bins @ 2.5 mm)
 *   3. Algebraischer Kåsa-Kreisfit in der komplexen IQ-Ebene + regularisiertes DACM Phase-Unwrapping
 *      (rechnet direkt in physikalische Thorax-Auslenkung in Mikrometern um: lambda/(4*pi) = 394.37 µm/rad)
 *   4. Gleitende 7-s Hann-Hilbert Atem-Oberwellen-Projektion (m = 1..6 per Orthogonal Least Squares)
 *   5. Tri-Modaler Herzfrequenz- & IBI-Schätzer (Spektral/HPS + Autokorrelation + Zeitbereichs-IBI/RMSSD)
 *   6. Simultaner Raum-Scan (Subsweep 2, 0.30..2.67 m @ 30 mm) mit Mehrpersonen-Erkennung & Auto-Rezoom.
 */

#ifndef A121_ESP_DSP_H
#define A121_ESP_DSP_H

#include <Arduino.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace a121_dsp {

static constexpr float kRadarFreqHz       = 60.5e9f;
static constexpr float kSpeedOfLight      = 299792458.0f;
static constexpr float kWavelengthM       = kSpeedOfLight / kRadarFreqHz;          /* ~4.9552 mm */
static constexpr float kRadToUm           = (kWavelengthM / (4.0f * (float)M_PI)) * 1.0e6f; /* ~394.37 µm/rad */
static constexpr float kPointStepM        = 0.0025f;                               /* 2.5 mm pro A121 Step */

static constexpr uint16_t kZoomPoints     = 24U;   /* Subsweep 1: 24 Punkte * 2.5 mm = 60 mm Fokusfenster */
static constexpr uint16_t kRoomStep       = 12U;   /* RSS: step_length muss Teiler oder Vielfaches von 24 sein (16 wird abgelehnt) */
static constexpr uint16_t kRoomPoints     = 80U;   /* Subsweep 2: 80 Punkte * 30 mm  = 2.40 m Raumfenster */
static constexpr uint16_t kTotal3SubPts   = 1U + kZoomPoints + kRoomPoints; /* 105 Punkte = 420 Bytes/Frame */

static constexpr uint16_t kHistoryLen     = 512U;  /* ~8.5 s @ 60 Hz für hochauflösende Vital-Schätzung */
static constexpr uint16_t kHilbertWinLen  = 420U;  /* 7.0 s @ 60 Hz für Hann-Hilbert Oberwellen-Projektion */
static constexpr uint8_t  kMaxHarmonics   = 6U;    /* m = 1..6 Atem-Oberwellen */

struct Complex16 {
    int16_t real;
    int16_t imag;
};

struct RoomTarget {
    float dist_m;
    float amplitude;
    float motion_energy;
};

struct VitalReport {
    bool     valid;
    uint32_t frame_count;
    float    fs_hz;
    int16_t  temperature_c;
    float    loopback_amp;
    float    loopback_phase_std_mrad;
    float    target_dist_m;
    uint8_t  best_zoom_bin;
    float    zoom_snr_db;
    float    resp_depth_um;
    float    heart_amp_um;
    float    br_bpm;
    float    hr_spec_bpm;
    float    hr_acf_bpm;
    float    hr_ibi_bpm;
    float    hr_fused_bpm;
    float    ibi_median_ms;
    float    rmssd_ms;
    float    confidence_pct;
    uint8_t  num_room_targets;
    RoomTarget room_targets[4];
    bool     needs_rezoom;
    int32_t  recommended_zoom_start_pt;
};

class UltimateEspDsp {
public:
    UltimateEspDsp() = default;

    bool begin(int32_t zoom_start_pt = 200, int32_t room_start_pt = 120, uint16_t room_step = kRoomStep) {
        zoom_start_pt_ = zoom_start_pt;
        room_start_pt_ = room_start_pt;
        room_step_     = room_step;

        if (!allocated_) {
            hist_i_       = allocFloat(kHistoryLen);
            hist_q_       = allocFloat(kHistoryLen);
            hist_um_      = allocFloat(kHistoryLen);
            hist_lb_phi_  = allocFloat(kHistoryLen);
            work_detrend_ = allocFloat(kHistoryLen);
            work_clean_   = allocFloat(kHistoryLen);
            work_hr_      = allocFloat(kHistoryLen);
            allocated_    = (hist_i_ && hist_q_ && hist_um_ && hist_lb_phi_ && work_detrend_ && work_clean_ && work_hr_);
        }
        resetState();
        return allocated_;
    }

    void resetState() {
        frame_count_         = 0;
        hist_write_idx_      = 0;
        hist_count_          = 0;
        cum_phase_rad_       = 0.0f;
        prev_centered_i_     = 0.0f;
        prev_centered_q_     = 0.0f;
        have_prev_iq_        = false;
        dc_i_                = 0.0f;
        dc_q_                = 0.0f;
        best_zoom_bin_       = kZoomPoints / 2U;
        out_of_window_frames_ = 0;
        last_temp_c_         = 0;

        memset(zoom_clutter_i_, 0, sizeof(zoom_clutter_i_));
        memset(zoom_clutter_q_, 0, sizeof(zoom_clutter_q_));
        memset(zoom_var_ema_, 0, sizeof(zoom_var_ema_));
        memset(room_clutter_i_, 0, sizeof(room_clutter_i_));
        memset(room_clutter_q_, 0, sizeof(room_clutter_q_));
        memset(room_amp_ema_, 0, sizeof(room_amp_ema_));
        memset(room_var_ema_, 0, sizeof(room_var_ema_));
    }

    void setZoomStartPoint(int32_t zoom_start_pt) {
        zoom_start_pt_ = zoom_start_pt;
        resetState();
    }

    int32_t zoomStartPoint() const { return zoom_start_pt_; }

    /**
     * @brief Verarbeitet einen kompletten 3-Subsweep Sparse-IQ Frame (85 komplexe Punkte = 340 Bytes).
     * @param pts Zeiger auf `kTotal3SubPts` (85) komplexe `int16_t` Samples
     * @param n_pts Anzahl Punkte (muss >= 1 + kZoomPoints sein; Subsweep 2 optional falls 25 Punkte)
     * @param temp_c A121 Chiptemperatur in °C
     * @param dt_s Gemessenes Frame-Delta in Sekunden (z.B. ~0.01667 s @ 60 Hz)
     * @param out_um_sample Aktueller Phasen-Auslenkungswert in µm (für Echtzeit-Telemetrie)
     */
    void pushFrame(const Complex16 *pts, uint16_t n_pts, int16_t temp_c, float dt_s, float &out_um_sample) {
        if (!allocated_ || pts == nullptr || n_pts < (1U + kZoomPoints)) {
            out_um_sample = 0.0f;
            return;
        }

        last_temp_c_ = temp_c;
        if (dt_s > 0.002f && dt_s < 0.250f) {
            const float inst_fs = 1.0f / dt_s;
            fs_ema_hz_ = (frame_count_ == 0) ? inst_fs : (0.98f * fs_ema_hz_ + 0.02f * inst_fs);
        } else if (frame_count_ == 0) {
            fs_ema_hz_ = 60.0f;
        }

        /* ---------------------------------------------------------------------
         * 1. Subsweep 0: On-Chip Loopback Phasenreferenz & Derotation
         * --------------------------------------------------------------------- */
        const float lb_i   = (float)pts[0].real;
        const float lb_q   = (float)pts[0].imag;
        const float lb_amp = sqrtf(lb_i * lb_i + lb_q * lb_q);
        float       rot_re = 1.0f;
        float       rot_im = 0.0f;
        float       lb_phi = 0.0f;
        if (lb_amp > 1.0f) {
            rot_re =  lb_i / lb_amp;
            rot_im = -lb_q / lb_amp; /* e^{-j * phi_lb} */
            lb_phi = atan2f(lb_q, lb_i);
        }
        last_lb_amp_ = lb_amp;

        /* ---------------------------------------------------------------------
         * 2. Subsweep 1: 24 Zoom-Bins derotieren & Pulsations-Varianz schätzen
         * --------------------------------------------------------------------- */
        float derot_i[kZoomPoints];
        float derot_q[kZoomPoints];
        const Complex16 *zoom_pts = &pts[1];

        for (uint16_t b = 0; b < kZoomPoints; b++) {
            const float ri = (float)zoom_pts[b].real;
            const float rq = (float)zoom_pts[b].imag;
            const float zi = ri * rot_re - rq * rot_im;
            const float zq = ri * rot_im + rq * rot_re;
            derot_i[b] = zi;
            derot_q[b] = zq;

            if (frame_count_ == 0) {
                zoom_clutter_i_[b] = zi;
                zoom_clutter_q_[b] = zq;
                zoom_var_ema_[b]   = 0.0f;
            } else {
                const float di = zi - zoom_clutter_i_[b];
                const float dq = zq - zoom_clutter_q_[b];
                zoom_clutter_i_[b] += 0.008f * di;
                zoom_clutter_q_[b] += 0.008f * dq;
                const float dyn_pwr = di * di + dq * dq;
                zoom_var_ema_[b]    = 0.96f * zoom_var_ema_[b] + 0.04f * dyn_pwr;
            }
        }

        /* Alle 30 Frames (~0.5 s) den dominanten Thorax-Bin im 60-mm-Fenster aktualisieren */
        if (frame_count_ >= 15 && (frame_count_ % 30 == 0)) {
            float   best_score = -1.0f;
            uint8_t best_bin   = best_zoom_bin_;
            for (uint8_t b = 2; b < kZoomPoints - 2; b++) {
                const float amp_sq = zoom_clutter_i_[b] * zoom_clutter_i_[b] + zoom_clutter_q_[b] * zoom_clutter_q_[b];
                /* Kombiniere dynamische Pulsationsenergie mit hinreichender Trägeramplitude */
                const float score = zoom_var_ema_[b] * sqrtf(amp_sq + 1.0f);
                if (score > best_score) {
                    best_score = score;
                    best_bin   = b;
                }
            }
            best_zoom_bin_ = best_bin;
        }

        /* Kohärentes 5-Bin Spatial Beamforming um best_zoom_bin_ (Phasen-Alignment auf Zentral-Bin) */
        const uint8_t b0     = best_zoom_bin_;
        const float   ref_ci = zoom_clutter_i_[b0];
        const float   ref_cq = zoom_clutter_q_[b0];
        const float   ref_ca = sqrtf(ref_ci * ref_ci + ref_cq * ref_cq) + 1e-3f;

        float beam_i = 0.0f;
        float beam_q = 0.0f;
        float w_sum  = 0.0f;
        for (int8_t db = -2; db <= 2; db++) {
            const int16_t idx = (int16_t)b0 + db;
            if (idx < 0 || idx >= (int16_t)kZoomPoints) continue;
            const float ci  = zoom_clutter_i_[idx];
            const float cq  = zoom_clutter_q_[idx];
            const float ca  = sqrtf(ci * ci + cq * cq) + 1e-3f;
            /* Phasen-Rotation von Bin idx auf Referenz-Bin b0 anhand des mittleren Clutter-Vektors */
            const float align_re = (ci * ref_ci + cq * ref_cq) / (ca * ref_ca);
            const float align_im = (ci * ref_cq - cq * ref_ci) / (ca * ref_ca);
            const float w        = (db == 0) ? 1.0f : ((abs(db) == 1) ? 0.75f : 0.40f);
            const float si       = derot_i[idx] * align_re - derot_q[idx] * align_im;
            const float sq       = derot_i[idx] * align_im + derot_q[idx] * align_re;
            beam_i += w * si;
            beam_q += w * sq;
            w_sum  += w;
        }
        beam_i /= w_sum;
        beam_q /= w_sum;

        /* ---------------------------------------------------------------------
         * 3. Kåsa-Zentrierung & Regularisiertes DACM Phase-Unwrapping -> µm
         * --------------------------------------------------------------------- */
        if (frame_count_ == 0) {
            dc_i_ = beam_i;
            dc_q_ = beam_q;
        } else {
            dc_i_ += 0.002f * (beam_i - dc_i_);
            dc_q_ += 0.002f * (beam_q - dc_q_);
        }

        const float ci = beam_i - dc_i_;
        const float cq = beam_q - dc_q_;
        if (have_prev_iq_) {
            const float cross = prev_centered_i_ * cq - prev_centered_q_ * ci;
            const float dot   = prev_centered_i_ * ci + prev_centered_q_ * cq;
            const float mag2  = prev_centered_i_ * prev_centered_i_ + prev_centered_q_ * prev_centered_q_ + 25.0f;
            /* Kombination aus exakt unbeschränktem atan2(cross, dot) mit Regularisierung bei kleinem Radius */
            const float dphi  = (mag2 > 2500.0f) ? atan2f(cross, dot) : (cross / mag2);
            cum_phase_rad_   += dphi;
        }
        prev_centered_i_ = ci;
        prev_centered_q_ = cq;
        have_prev_iq_    = true;

        const float disp_um = cum_phase_rad_ * kRadToUm;
        out_um_sample       = disp_um;

        /* In Ringpuffer (PSRAM) ablegen */
        hist_i_[hist_write_idx_]      = beam_i;
        hist_q_[hist_write_idx_]      = beam_q;
        hist_um_[hist_write_idx_]     = disp_um;
        hist_lb_phi_[hist_write_idx_] = lb_phi;
        hist_write_idx_               = (hist_write_idx_ + 1U) % kHistoryLen;
        if (hist_count_ < kHistoryLen) {
            hist_count_++;
        }

        /* ---------------------------------------------------------------------
         * 4. Subsweep 2: Simultaner Full-Room Scan (80 Bins, 0.30..2.67 m)
         * --------------------------------------------------------------------- */
        if (n_pts >= kTotal3SubPts) {
            const Complex16 *room_pts = &pts[1U + kZoomPoints];
            for (uint16_t r = 0; r < kRoomPoints; r++) {
                const float ri  = (float)room_pts[r].real;
                const float rq  = (float)room_pts[r].imag;
                const float amp = sqrtf(ri * ri + rq * rq);
                if (frame_count_ == 0) {
                    room_clutter_i_[r] = ri;
                    room_clutter_q_[r] = rq;
                    room_amp_ema_[r]   = amp;
                    room_var_ema_[r]   = 0.0f;
                } else {
                    const float di = ri - room_clutter_i_[r];
                    const float dq = rq - room_clutter_q_[r];
                    room_clutter_i_[r] += 0.02f * di;
                    room_clutter_q_[r] += 0.02f * dq;
                    room_amp_ema_[r]    = 0.95f * room_amp_ema_[r] + 0.05f * amp;
                    room_var_ema_[r]    = 0.92f * room_var_ema_[r] + 0.08f * sqrtf(di * di + dq * dq);
                }
            }
        }

        frame_count_++;
    }

    /**
     * @brief Führt die komplette Vital- und Raum-Auswertung auf dem aktuellen PSRAM-Gleitfenster aus.
     */
    bool computeVitalReport(VitalReport &rep) {
        memset(&rep, 0, sizeof(rep));
        rep.frame_count   = frame_count_;
        rep.fs_hz         = fs_ema_hz_;
        rep.temperature_c = last_temp_c_;
        rep.loopback_amp  = last_lb_amp_;
        rep.best_zoom_bin = best_zoom_bin_;
        rep.target_dist_m = (float)(zoom_start_pt_ + (int32_t)best_zoom_bin_) * kPointStepM;

        /* Raum-Ziele aus Subsweep 2 bestimmen (immer verfügbar ab wenigen Frames) */
        extractRoomTargets(rep);

        if (hist_count_ < 180U) { /* Mindestens 3.0 s Daten @ 60 Hz für Vital-Analyse */
            rep.valid = false;
            return false;
        }

        const uint16_t N  = (hist_count_ > kHilbertWinLen) ? kHilbertWinLen : hist_count_;
        const float    fs = (fs_ema_hz_ > 10.0f) ? fs_ema_hz_ : 60.0f;

        /* 1. Letzte N Samples chronologisch in Arbeits-Arrays kopieren & Loopback-Jitter berechnen */
        const uint16_t start_idx = (hist_write_idx_ + kHistoryLen - N) % kHistoryLen;
        float lb_mean = 0.0f;
        for (uint16_t n = 0; n < N; n++) {
            const uint16_t idx = (start_idx + n) % kHistoryLen;
            work_detrend_[n]   = hist_um_[idx];
            lb_mean           += hist_lb_phi_[idx];
        }
        lb_mean /= (float)N;
        float lb_var = 0.0f;
        for (uint16_t n = 0; n < N; n++) {
            const uint16_t idx = (start_idx + n) % kHistoryLen;
            const float    d   = hist_lb_phi_[idx] - lb_mean;
            lb_var += d * d;
        }
        rep.loopback_phase_std_mrad = sqrtf(lb_var / (float)N) * 1000.0f;

        /* 2. Kåsa Circle Fit Verfeinerung auf den letzten N IQ-Samples */
        refineKasaPhaseTrace(start_idx, N, work_detrend_);

        /* 3. Linearen Drift (Mittelwert + Steigung) aus dem µm-Phasensignal entfernen */
        removeLinearTrend(work_detrend_, N);

        /* 4. Atemfrequenz f_resp in [0.10 .. 0.55 Hz] (6.0 .. 33.0 BPM) mit hoher Frequenzauflösung finden */
        float best_resp_f   = 0.22f;
        float best_resp_pwr = -1.0f;
        for (float f = 0.10f; f <= 0.55f; f += 0.0025f) {
            const float pwr = goertzelPowerHann(work_detrend_, N, fs, f);
            if (pwr > best_resp_pwr) {
                best_resp_pwr = pwr;
                best_resp_f   = f;
            }
        }
        rep.br_bpm        = best_resp_f * 60.0f;
        rep.resp_depth_um = 2.0f * sqrtf(best_resp_pwr);

        /* 5. Hann-gewichtete Orthogonal-Projektion der Atem-Oberwellen m = 1..6 (Sub-Bin Unterdrückung) */
        memcpy(work_clean_, work_detrend_, N * sizeof(float));
        projectOutRespirationHarmonics(work_clean_, N, fs, best_resp_f, kMaxHarmonics);

        /* 6. Vorwärts-Rückwärts 2. Ordnung Butterworth-Bandpass [0.88 .. 3.00 Hz] (53 .. 180 BPM) */
        bandpassCardiacZeroPhase(work_clean_, work_hr_, N, fs);

        float hr_rms = 0.0f;
        for (uint16_t n = 0; n < N; n++) {
            hr_rms += work_hr_[n] * work_hr_[n];
        }
        rep.heart_amp_um = sqrtf(2.0f * hr_rms / (float)N);

        /* 7. Schätzer A: Spektral + Harmonic Product Spectrum (HPS) auf [0.88 .. 2.30 Hz] (53 .. 138 BPM) */
        float best_hr_f   = 1.15f;
        float best_hr_scr = -1.0f;
        float total_hr_p  = 1e-6f;
        for (float f = 0.88f; f <= 2.30f; f += 0.005f) {
            const float p1 = goertzelPowerHann(work_hr_, N, fs, f);
            const float p2 = goertzelPowerHann(work_dot_diff(work_detrend_, N, fs), N, fs, 2.0f * f);
            total_hr_p += p1;
            const float score = p1 * (1.0f + 0.45f * sqrtf(p2 / (p1 + 1.0f)));
            if (score > best_hr_scr) {
                best_hr_scr = score;
                best_hr_f   = f;
            }
        }
        rep.hr_spec_bpm = best_hr_f * 60.0f;
        const float spec_snr = best_hr_scr / (total_hr_p / 285.0f + 1e-6f);
        rep.zoom_snr_db = 10.0f * log10f(spec_snr + 1.0f);

        /* 8. Schätzer B: Normierte Autokorrelation (ACF) im Herz-Lags-Bereich [0.33 .. 1.15 s] */
        float acf_bpm  = rep.hr_spec_bpm;
        float acf_peak = 0.0f;
        computeCardiacAcf(work_hr_, N, fs, acf_bpm, acf_peak);
        rep.hr_acf_bpm = acf_bpm;

        /* 9. Schätzer C: Zeitbereichs-Herzschlag-Detektor (IBI Median & RMSSD) */
        float ibi_bpm   = rep.hr_spec_bpm;
        float ibi_ms    = 60000.0f / rep.hr_spec_bpm;
        float rmssd_ms  = 0.0f;
        uint8_t n_beats = 0;
        computeTimeDomainIbi(work_hr_, N, fs, ibi_bpm, ibi_ms, rmssd_ms, n_beats);
        rep.hr_ibi_bpm    = ibi_bpm;
        rep.ibi_median_ms = ibi_ms;
        rep.rmssd_ms      = rmssd_ms;

        /* 10. Konfidenz-gewichtete Mehrheits-Fusion der 3 Herzfrequenz-Schätzer (Spec/HPS, ACF, IBI) */
        const float diff_spec_acf = fabsf(rep.hr_spec_bpm - rep.hr_acf_bpm);
        const float diff_spec_ibi = fabsf(rep.hr_spec_bpm - rep.hr_ibi_bpm);
        const float diff_acf_ibi  = fabsf(rep.hr_acf_bpm - rep.hr_ibi_bpm);
        if (diff_spec_acf < 6.0f && diff_spec_ibi < 8.0f) {
            rep.hr_fused_bpm = 0.45f * rep.hr_spec_bpm + 0.35f * rep.hr_acf_bpm + 0.20f * rep.hr_ibi_bpm;
        } else if (diff_spec_acf < 8.0f) {
            rep.hr_fused_bpm = 0.55f * rep.hr_spec_bpm + 0.45f * rep.hr_acf_bpm;
        } else if (diff_spec_ibi < 8.0f) {
            rep.hr_fused_bpm = 0.55f * rep.hr_spec_bpm + 0.45f * rep.hr_ibi_bpm;
        } else if (diff_acf_ibi < 8.0f) {
            rep.hr_fused_bpm = 0.55f * rep.hr_acf_bpm + 0.45f * rep.hr_ibi_bpm;
        } else {
            rep.hr_fused_bpm = rep.hr_spec_bpm;
        }

        float conf = 25.0f + 35.0f * fminf(1.0f, acf_peak / 0.45f)
                   + 25.0f * fminf(1.0f, spec_snr / 8.0f)
                   + 15.0f * fmaxf(0.0f, 1.0f - diff_spec_acf / 15.0f);
        if (conf > 99.0f) conf = 99.0f;
        if (conf < 5.0f)  conf = 5.0f;
        rep.confidence_pct = conf;
        rep.valid          = true;
        return true;
    }

private:
    static float *allocFloat(size_t count) {
        void *p = nullptr;
        if (psramFound()) {
            p = ps_malloc(count * sizeof(float));
        }
        if (!p) {
            p = malloc(count * sizeof(float));
        }
        if (p) {
            memset(p, 0, count * sizeof(float));
        }
        return (float *)p;
    }

    /**
     * @brief Algebraischer Kåsa-Kreisfit über die letzten N komplexen IQ-Punkte.
     *        Wenn der Bogen groß genug ist, korrigiert er das statische Clutter-Zentrum (I0, Q0).
     */
    void refineKasaPhaseTrace(uint16_t start_idx, uint16_t N, float *out_um) {
        double mean_i = 0.0, mean_q = 0.0;
        for (uint16_t n = 0; n < N; n++) {
            const uint16_t idx = (start_idx + n) % kHistoryLen;
            mean_i += hist_i_[idx];
            mean_q += hist_q_[idx];
        }
        mean_i /= (double)N;
        mean_q /= (double)N;

        double suu = 0.0, suv = 0.0, svv = 0.0, suuu = 0.0, svvv = 0.0, suvv = 0.0, svuu = 0.0;
        for (uint16_t n = 0; n < N; n++) {
            const uint16_t idx = (start_idx + n) % kHistoryLen;
            const double u = hist_i_[idx] - mean_i;
            const double v = hist_q_[idx] - mean_q;
            suu  += u * u;
            suv  += u * v;
            svv  += v * v;
            suuu += u * u * u;
            svvv += v * v * v;
            suvv += u * v * v;
            svuu += v * u * u;
        }

        const double det = suu * svv - suv * suv;
        const double rms_r = sqrt((suu + svv) / (double)N);
        double center_i = mean_i;
        double center_q = mean_q;

        if (det > 1e-3 && rms_r > 15.0) {
            const double rhs_u = 0.5 * (suuu + suvv);
            const double rhs_v = 0.5 * (svvv + svuu);
            const double uc    = (rhs_u * svv - rhs_v * suv) / det;
            const double vc    = (suu * rhs_v - suv * rhs_u) / det;
            const double rad   = sqrt(uc * uc + vc * vc + (suu + svv) / (double)N);
            /* Nur übernehmen, wenn der geschätzte Kreisradius physikalisch plausibel ist */
            if (rad > 0.3 * rms_r && rad < 6.0 * rms_r) {
                center_i = mean_i + uc;
                center_q = mean_q + vc;
            }
        }

        float phi = 0.0f;
        out_um[0] = 0.0f;
        for (uint16_t n = 1; n < N; n++) {
            const uint16_t idx0 = (start_idx + n - 1U) % kHistoryLen;
            const uint16_t idx1 = (start_idx + n) % kHistoryLen;
            const float i0 = hist_i_[idx0] - (float)center_i;
            const float q0 = hist_q_[idx0] - (float)center_q;
            const float i1 = hist_i_[idx1] - (float)center_i;
            const float q1 = hist_q_[idx1] - (float)center_q;
            const float cross = i0 * q1 - q0 * i1;
            const float dot   = i0 * i1 + q0 * q1;
            const float mag2  = i0 * i0 + q0 * q0 + 25.0f;
            const float dphi  = (mag2 > 1600.0f) ? atan2f(cross, dot) : (cross / mag2);
            phi       += dphi;
            out_um[n]  = phi * kRadToUm;
        }
    }

    static void removeLinearTrend(float *x, uint16_t N) {
        if (N < 2) return;
        double sum_t = 0.0, sum_y = 0.0, sum_tt = 0.0, sum_ty = 0.0;
        const double half = 0.5 * (double)(N - 1U);
        for (uint16_t n = 0; n < N; n++) {
            const double t = (double)n - half;
            const double y = x[n];
            sum_t  += t;
            sum_y  += y;
            sum_tt += t * t;
            sum_ty += t * y;
        }
        const double mean_y = sum_y / (double)N;
        const double slope  = (sum_tt > 1e-6) ? (sum_ty / sum_tt) : 0.0;
        for (uint16_t n = 0; n < N; n++) {
            const double t = (double)n - half;
            x[n] = (float)((double)x[n] - (mean_y + slope * t));
        }
    }

    static float goertzelPowerHann(const float *x, uint16_t N, float fs, float freq_hz) {
        const float omega = 2.0f * (float)M_PI * freq_hz / fs;
        const float coeff = 2.0f * cosf(omega);
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
        const float inv_nm1 = 1.0f / (float)(N - 1U);
        for (uint16_t n = 0; n < N; n++) {
            const float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n * inv_nm1));
            s0 = x[n] * w + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        const float norm  = 0.25f * (float)N; /* Hann-Fenster-Verstärkung */
        return (power > 0.0f) ? (power / (norm * norm)) : 0.0f;
    }

    /**
     * @brief Projiziert die Grundschwingung und die Oberwellen m = 1..M der Atmung
     *        mittels Hann-gewichteter Orthogonal-Projektion aus dem Signal heraus.
     */
    static void projectOutRespirationHarmonics(float *x, uint16_t N, float fs, float f_resp, uint8_t max_m) {
        const float inv_nm1 = 1.0f / (float)(N - 1U);
        for (uint8_t m = 1; m <= max_m; m++) {
            const float fm = (float)m * f_resp;
            if (fm >= 2.60f) break;
            const float omega = 2.0f * (float)M_PI * fm / fs;
            double cc = 0.0, ss = 0.0, cs = 0.0, cx = 0.0, sx = 0.0;
            for (uint16_t n = 0; n < N; n++) {
                const float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n * inv_nm1));
                const float c = cosf(omega * (float)n);
                const float s = sinf(omega * (float)n);
                const float wc = w * c;
                const float ws = w * s;
                cc += wc * c;
                ss += ws * s;
                cs += wc * s;
                cx += wc * x[n];
                sx += ws * x[n];
            }
            const double det = cc * ss - cs * cs;
            if (det > 1e-6) {
                const float a = (float)((cx * ss - sx * cs) / det);
                const float b = (float)((cc * sx - cs * cx) / det);
                for (uint16_t n = 0; n < N; n++) {
                    const float phi = omega * (float)n;
                    x[n] -= (a * cosf(phi) + b * sinf(phi));
                }
            }
        }
    }

    /**
     * @brief Nullphasen-Bandpassfilterung für das Herzsignal [0.88 .. 3.00 Hz]:
     *        Kombiniert 1. Ableitung (ballistokardiographische Betonung) mit gleitendem
     *        Vorwärts-/Rückwärts-Hoch- und Tiefpass.
     */
    static void bandpassCardiacZeroPhase(const float *in, float *out, uint16_t N, float fs) {
        /* Schritt 1: Hochpass durch Subtraktion eines gleitenden Mittelwerts der Länge ~0.55 s */
        const int16_t hp_win = (int16_t)fmaxf(5.0f, roundf(0.55f * fs));
        for (uint16_t n = 0; n < N; n++) {
            int16_t a = (int16_t)n - hp_win / 2;
            int16_t b = (int16_t)n + hp_win / 2;
            if (a < 0) a = 0;
            if (b >= (int16_t)N) b = (int16_t)N - 1;
            float sum = 0.0f;
            for (int16_t k = a; k <= b; k++) sum += in[k];
            out[n] = in[n] - sum / (float)(b - a + 1);
        }

        /* Schritt 2: Tiefpass durch Vorwärts-/Rückwärts-Glättung der Länge ~0.16 s (fc ~ 3.0 Hz) */
        const int16_t lp_win = (int16_t)fmaxf(3.0f, roundf(0.16f * fs));
        for (uint8_t pass = 0; pass < 2; pass++) {
            float prev = out[0];
            for (uint16_t n = 0; n < N; n++) {
                int16_t a = (int16_t)n - lp_win / 2;
                int16_t b = (int16_t)n + lp_win / 2;
                if (a < 0) a = 0;
                if (b >= (int16_t)N) b = (int16_t)N - 1;
                float sum = 0.0f;
                for (int16_t k = a; k <= b; k++) sum += out[k];
                const float cur = sum / (float)(b - a + 1);
                if (n > 0) out[n - 1] = prev;
                prev = cur;
            }
            out[N - 1] = prev;
        }
    }

    const float *work_dot_diff(const float *x, uint16_t N, float fs) {
        for (uint16_t n = 1; n < N; n++) {
            work_clean_[n] = (x[n] - x[n - 1U]) * fs;
        }
        work_clean_[0] = work_clean_[1];
        return work_clean_;
    }

    static void computeCardiacAcf(const float *hr, uint16_t N, float fs, float &out_bpm, float &out_peak) {
        float e0 = 1e-6f;
        for (uint16_t n = 0; n < N; n++) e0 += hr[n] * hr[n];

        const uint16_t min_lag = (uint16_t)fmaxf(8.0f, floorf(fs * (60.0f / 170.0f)));  /* 170 BPM */
        const uint16_t max_lag = (uint16_t)fminf((float)(N / 2U), ceilf(fs * (60.0f / 50.0f))); /* 50 BPM */

        float    best_r   = -1.0f;
        uint16_t best_lag = min_lag;
        float    r_prev   = 0.0f;
        float    r_curr   = 0.0f;

        for (uint16_t lag = min_lag; lag <= max_lag; lag++) {
            float s = 0.0f;
            for (uint16_t n = 0; n + lag < N; n++) {
                s += hr[n] * hr[n + lag];
            }
            const float r_next = (s / e0) * ((float)N / (float)(N - lag));
            if (lag > min_lag && r_curr > r_prev && r_curr > r_next && r_curr > best_r) {
                best_r   = r_curr;
                best_lag = lag - 1U;
            }
            r_prev = r_curr;
            r_curr = r_next;
        }

        if (best_r > 0.05f && best_lag > 0) {
            out_peak = best_r;
            out_bpm  = (60.0f * fs) / (float)best_lag;
        } else {
            out_peak = 0.0f;
        }
    }

    static void computeTimeDomainIbi(const float *hr, uint16_t N, float fs,
                                     float &out_bpm, float &out_ibi_ms, float &out_rmssd_ms, uint8_t &out_beats) {
        float rms = 0.0f;
        for (uint16_t n = 0; n < N; n++) rms += hr[n] * hr[n];
        rms = sqrtf(rms / (float)N);
        const float thresh = 0.35f * rms;
        const uint16_t refractory = (uint16_t)roundf(0.36f * fs); /* Mindestens 360 ms Abstand (<= 166 BPM) */

        uint16_t peak_idx[32];
        uint8_t  n_peaks = 0;
        uint16_t last_pk = 0;

        for (uint16_t n = 2; n + 2 < N; n++) {
            if (hr[n] > thresh && hr[n] > hr[n - 1] && hr[n] >= hr[n + 1] &&
                hr[n] > hr[n - 2] && hr[n] >= hr[n + 2]) {
                if (n_peaks == 0 || (n - last_pk) >= refractory) {
                    peak_idx[n_peaks++] = n;
                    last_pk = n;
                    if (n_peaks >= 32) break;
                } else if (hr[n] > hr[last_pk]) {
                    peak_idx[n_peaks - 1] = n;
                    last_pk = n;
                }
            }
        }

        out_beats = n_peaks;
        if (n_peaks < 4) return;

        float ibis_ms[31];
        uint8_t n_ibi = 0;
        for (uint8_t k = 1; k < n_peaks; k++) {
            const float dt_ms = ((float)(peak_idx[k] - peak_idx[k - 1]) / fs) * 1000.0f;
            if (dt_ms >= 350.0f && dt_ms <= 1250.0f) {
                ibis_ms[n_ibi++] = dt_ms;
            }
        }
        if (n_ibi < 3) return;

        /* Kopie für Median-Sortierung */
        float sorted[31];
        memcpy(sorted, ibis_ms, n_ibi * sizeof(float));
        for (uint8_t i = 0; i < n_ibi; i++) {
            for (uint8_t j = i + 1; j < n_ibi; j++) {
                if (sorted[j] < sorted[i]) {
                    const float tmp = sorted[i];
                    sorted[i] = sorted[j];
                    sorted[j] = tmp;
                }
            }
        }
        const float median_ms = sorted[n_ibi / 2];
        out_ibi_ms = median_ms;
        out_bpm    = 60000.0f / median_ms;

        /* RMSSD über benachbarte valide IBIs (Ausreißer > 25 % vom Median ausblenden) */
        float sum_sq_diff = 0.0f;
        uint8_t n_diff = 0;
        for (uint8_t k = 1; k < n_ibi; k++) {
            if (fabsf(ibis_ms[k] - median_ms) < 0.25f * median_ms &&
                fabsf(ibis_ms[k - 1] - median_ms) < 0.25f * median_ms) {
                const float d = ibis_ms[k] - ibis_ms[k - 1];
                sum_sq_diff += d * d;
                n_diff++;
            }
        }
        out_rmssd_ms = (n_diff > 0) ? sqrtf(sum_sq_diff / (float)n_diff) : 0.0f;
    }

    void extractRoomTargets(VitalReport &rep) {
        rep.num_room_targets = 0;
        for (uint16_t r = 1; r + 1 < kRoomPoints; r++) {
            const float score = room_amp_ema_[r] + 6.0f * room_var_ema_[r];
            const float prev  = room_amp_ema_[r - 1] + 6.0f * room_var_ema_[r - 1];
            const float next  = room_amp_ema_[r + 1] + 6.0f * room_var_ema_[r + 1];
            if (score > 120.0f && score >= prev && score >= next) {
                /* In Top-4 Liste nach kombinierter Energie einfügen */
                const float dist_m = (float)(room_start_pt_ + (int32_t)(r * room_step_)) * kPointStepM;
                if (rep.num_room_targets < 4) {
                    uint8_t idx = rep.num_room_targets++;
                    rep.room_targets[idx] = {dist_m, room_amp_ema_[r], room_var_ema_[r]};
                }
            }
        }

        /* Nach Bewegungsenergie + Amplitude absteigend sortieren */
        for (uint8_t i = 0; i < rep.num_room_targets; i++) {
            for (uint8_t j = i + 1; j < rep.num_room_targets; j++) {
                const float si = rep.room_targets[i].amplitude + 8.0f * rep.room_targets[i].motion_energy;
                const float sj = rep.room_targets[j].amplitude + 8.0f * rep.room_targets[j].motion_energy;
                if (sj > si) {
                    const RoomTarget tmp = rep.room_targets[i];
                    rep.room_targets[i]  = rep.room_targets[j];
                    rep.room_targets[j]  = tmp;
                }
            }
        }

        /* Prüfe, ob das primäre Ziel das aktuelle 60-mm Vital-Zoom-Fenster verlassen hat */
        const float zoom_min_m = (float)(zoom_start_pt_ + 3) * kPointStepM;
        const float zoom_max_m = (float)(zoom_start_pt_ + (int32_t)kZoomPoints - 4) * kPointStepM;
        rep.needs_rezoom = false;
        rep.recommended_zoom_start_pt = zoom_start_pt_;

        if (rep.num_room_targets > 0 && rep.room_targets[0].motion_energy > 12.0f) {
            const float primary_m = rep.room_targets[0].dist_m;
            if (primary_m < zoom_min_m || primary_m > zoom_max_m) {
                out_of_window_frames_++;
                if (out_of_window_frames_ >= 3) { /* ~3 s außerhalb -> Auto-Rezoom empfehlen */
                    int32_t new_start = (int32_t)roundf(primary_m / kPointStepM) - (int32_t)(kZoomPoints / 2U);
                    if (new_start < 80) new_start = 80; /* >= 0.20 m */
                    rep.needs_rezoom              = true;
                    rep.recommended_zoom_start_pt = new_start;
                    out_of_window_frames_         = 0;
                }
            } else {
                out_of_window_frames_ = 0;
            }
        }
    }

    bool     allocated_            = false;
    int32_t  zoom_start_pt_        = 200;
    int32_t  room_start_pt_        = 120;
    uint16_t room_step_            = kRoomStep;
    uint32_t frame_count_          = 0;
    uint16_t hist_write_idx_       = 0;
    uint16_t hist_count_           = 0;
    float    fs_ema_hz_            = 60.0f;
    int16_t  last_temp_c_          = 0;
    float    last_lb_amp_          = 0.0f;
    float    cum_phase_rad_        = 0.0f;
    float    prev_centered_i_      = 0.0f;
    float    prev_centered_q_      = 0.0f;
    bool     have_prev_iq_         = false;
    float    dc_i_                 = 0.0f;
    float    dc_q_                 = 0.0f;
    uint8_t  best_zoom_bin_        = 12;
    uint16_t out_of_window_frames_ = 0;

    float zoom_clutter_i_[kZoomPoints] = {};
    float zoom_clutter_q_[kZoomPoints] = {};
    float zoom_var_ema_[kZoomPoints]   = {};

    float room_clutter_i_[kRoomPoints] = {};
    float room_clutter_q_[kRoomPoints] = {};
    float room_amp_ema_[kRoomPoints]   = {};
    float room_var_ema_[kRoomPoints]   = {};

    float *hist_i_       = nullptr;
    float *hist_q_       = nullptr;
    float *hist_um_      = nullptr;
    float *hist_lb_phi_  = nullptr;
    float *work_detrend_ = nullptr;
    float *work_clean_   = nullptr;
    float *work_hr_      = nullptr;
};

} /* namespace a121_dsp */

#endif /* A121_ESP_DSP_H */
