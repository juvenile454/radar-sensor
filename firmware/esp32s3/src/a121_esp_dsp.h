/**
 * @file a121_esp_dsp.h
 * @brief On-Device-Auswertung der 3-Subsweep-Sparse-IQ-Session (`iq3`) auf dem ESP32-S3 (PSRAM).
 *
 * Kette (Stand 2026-11-01, an `iq3raw_normal_120s_2026-11-01.npz` und `iq3raw_motion_120s_2026-11-01.npz` entwickelt):
 *   1. Subsweep 0: On-Chip-Loopback → Phasen-Derotation aller Zoom-Punkte pro Frame (PLL-/Taktjitter).
 *   2. Subsweep 1: alle 24 Zoom-Punkte (60 mm, 2,5-mm-Raster) werden einzeln gespeichert (15 s Historie).
 *   3. Puls: pro Punkt Phase um den Fenster-Mittelpunkt abwickeln (→ µm), linear entzerren, Nullphasen-Bandpass
 *      0,8–3,0 Hz (Butterworth 4. Ordnung, identisch zu scipy `sosfiltfilt`), Bewegungsdämpfung je 1-s-Block
 *      (Bezug: ruhiges Viertel des Fensters), auf 15 Hz dezimieren, Spektrum 0,88–2,30 Hz; Spitzen zählen nur
 *      zwischen 0,91 und 2,27 Hz (55–136 /min). Die Spektren aller Punkte werden mit (Spitze/Median)² gewichtet gemittelt;
 *      Punkte mit klarem Puls zählen mehr. Atem-Oberwellen werden NICHT herausprojiziert: Liegt der Puls nahe
 *      m × Atemfrequenz, entfernt die Projektion den Herzanteil mit (am Datensatz: 13–42 % statt 61–89 % Treffer).
 *   4. Verfolgung: Kandidaten (Top 3) mit ≥ 40 % der Spitzenleistung innerhalb ±12 /min des bisherigen Werts
 *      werden bevorzugt (Glättung 0,7/0,3); ein weiter Sprung gilt erst, wenn er 3 Reports hält, auf eine
 *      Atem-Oberwelle (m = 2..6) erst nach 10 Reports. hr_harm = 1 heißt nur "Spitze liegt auf einer
 *      Oberwelle"; der Puls kann dort liegen (live 2026-11-01: Puls 78 ≈ 4 × Atmung 19,5).
 *   5. Konfidenz = Anteil (gewichtet) der Punkte, deren eigener Spitzenwert ±5 /min am Ergebnis liegt.
 *      motion_pct = Anteil der 1-s-Blöcke mit mehr als 3-facher Ruhe-Amplitude im Herzband (Bewegungshinweis).
 *   6. Atemfrequenz aus der mit Amplitude² gewichteten Leistungssumme aller Zoom-Punkte: größtes echtes lokales
 *      Maximum in 0,115–0,585 Hz, sonst 0 (keine Atemspitze). Vorher ungewichtet und größtes Bin; das klebte in rund
 *      einem Viertel der Reports am Rand bei 6,9 /min (schwache Punkte, Lagewechsel). Atemhub,
 *      Herz-Autokorrelation und Zeitbereichs-Schläge (IBI, RMSSD) auf dem Punkt mit dem klarsten Puls.
 *   7. Subsweep 2: Raum-Scan 0,30–2,67 m (30 mm) mit Rezoom-Empfehlung. Rauschpegel = unteres Viertel aller 80 Punkte
 *      (Amplitude und Bewegung getrennt). Ziel = lokales Maximum ab 2 × Rauschpegel, "bewegt" ab 3 × Bewegungs-Rauschpegel;
 *      gemeldet werden die 8 stärksten. Vorher galten feste Rohwerte (Wert > 120, Bewegung > 12), die unter dem Rauschen
 *      lagen (≈ 55 / 55), und nur die 4 nächsten Spitzen. Folge: Rausch-Spitzen im Zoom-Fenster blockierten den Rezoom.
 *      Rezoom auf das stärkste bewegte Ziel im über ≈ 3 s geglätteten Profil (nicht in der Momentaufnahme, die bei zwei
 *      nahen Echos kippte); kein Rezoom, solange dieses Ziel im Zoom-Fenster ±60 mm liegt (vorher wechselte der Zoom
 *      zwischen Körperstellen und löschte jedes Mal die Historie).
 *
 * Zeitbasis: feste Frame-Rate kFsHz (die Session läuft mit 60 Hz, der Sensor liefert lückenlos). Die frühere
 * Schätzung aus ESP32-Verarbeitungszeiten war falsch, weil Frames nach jeder Rechenpause gebündelt ankommen.
 * Ergebnisse sind Rhythmus-Schätzungen ohne medizinische Aussage; Genauigkeit nur mit Referenzmessung bewerten.
 */

#ifndef A121_ESP_DSP_H
#define A121_ESP_DSP_H

#include <Arduino.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace a121_dsp {

static constexpr float kRadarFreqHz  = 60.5e9f;
static constexpr float kSpeedOfLight = 299792458.0f;
static constexpr float kWavelengthM  = kSpeedOfLight / kRadarFreqHz;                    /* ~4.9552 mm */
static constexpr float kRadToUm      = (kWavelengthM / (4.0f * (float)M_PI)) * 1.0e6f;  /* ~394.37 µm/rad */
static constexpr float kPointStepM   = 0.0025f;                                         /* 2.5 mm pro A121 Step */

static constexpr uint16_t kZoomPoints   = 24U;  /* Subsweep 1: 24 Punkte * 2.5 mm = 60 mm Fokusfenster */
static constexpr uint16_t kRoomStep     = 12U;  /* RSS: step_length muss Teiler oder Vielfaches von 24 sein */
static constexpr uint16_t kRoomPoints   = 80U;  /* Subsweep 2: 80 Punkte * 30 mm = 2.40 m Raumfenster */
static constexpr uint16_t kTotal3SubPts = 1U + kZoomPoints + kRoomPoints; /* 105 Punkte = 420 Bytes/Frame */

static constexpr float    kFsHz        = 60.0f;  /* Frame-Rate der iq3-Session; Filterkoeffizienten gelten nur hierfür */
static constexpr uint16_t kHistoryLen  = 960U;   /* 16 s @ 60 Hz */
static constexpr uint16_t kHrWin       = 900U;   /* 15 s Pulsfenster */
static constexpr uint16_t kHrMinWin    = 480U;   /* ab 8 s Daten wird ein Puls berichtet */
static constexpr uint16_t kRespMinWin  = 180U;   /* ab 3 s Atmung */
static constexpr uint8_t  kDecim       = 4U;     /* 60 Hz → 15 Hz für die Spektralsuche */
static constexpr float    kHrFMin      = 0.88f;  /* 53 /min */
static constexpr float    kHrFStep     = 0.01f;  /* 0,6 /min (feiner bringt bei 15-s-Fenster nichts, am Datensatz geprüft) */
static constexpr uint16_t kHrNumF      = 143U;   /* 0,88 .. 2,30 Hz */
static constexpr float    kBrFMin      = 0.10f;  /* Atem-Spektrum über alle Zoom-Punkte: 0,10 .. 0,60 Hz */
static constexpr float    kBrFStep     = 0.005f;
static constexpr uint16_t kBrNumF      = 101U;
static constexpr uint16_t kBrEdgeBins  = 3U;     /* Atem-Spitze nicht am Bandrand (früher oft 6 /min) */
static constexpr float    kBrMinQ      = 2.0f;   /* Atem-Spitze/Median: darunter keine Oberwellen-Prüfung */
static constexpr float    kHarmTolHz   = 0.04f;  /* Puls-Kandidat gilt als Atem-Oberwelle, wenn |f − m·f_Atem| ≤ 0,04 Hz */
static constexpr uint8_t  kHarmMMax    = 6U;     /* geprüfte Oberwellen m = 2 .. 6 */
static constexpr uint8_t  kHoldHarm    = 10U;    /* Sprung auf eine Oberwelle erst nach 10 Reports (sonst 3) */
static constexpr uint16_t kFiltPad     = 27U;    /* scipy sosfiltfilt: padlen = 3 * (2 * Sektionen + 1) */
static constexpr uint16_t kGateBlk     = 60U;    /* Bewegungsdämpfung: 1-s-Blöcke im Herzband */
static constexpr float    kGateK       = 1.25f;  /* Grenze = 1,25 × Effektivwert des ruhigen Viertels der Blöcke */
static constexpr float    kGateP       = 3.0f;   /* Verstärkung darüber (Grenze/Effektivwert)^3: Bewegung fällt praktisch heraus */
static constexpr float    kMotionK     = 3.0f;   /* für motion_pct: Block > 3 × ruhiges Viertel gilt als Bewegung */
static constexpr uint8_t  kMaxBlocks   = kHrWin / kGateBlk;
static constexpr uint16_t kHrEdgeBins  = 3U;     /* Spitzen in den äußeren 3 Frequenzschritten zählen nicht (Leckage vom Bandrand) */
static constexpr uint8_t  kMaxRoomTargets = 8U;  /* gemeldete Raum-Ziele (die stärksten) */
static constexpr float    kRoomDetK    = 2.0f;   /* Raum-Ziel: Amplitude oder Bewegung ≥ 2 × Rauschpegel */
static constexpr float    kRoomMoveK   = 3.0f;   /* bewegt: Bewegung ≥ 3 × Bewegungs-Rauschpegel */
static constexpr float    kRoomSlowA   = 0.00554f; /* Glättung für das Zoom-Ziel: 1 − exp(−1 / (60 Hz · 3 s)), τ ≈ 3 s */

/* Butterworth-Bandpass 4. Ordnung 0,8–3,0 Hz bei 60 Hz: scipy.signal.butter(4, [0.8, 3.0], 'band', fs=60, output='sos') */
static constexpr float kBpSos[4][6] = {
    {0.00013253330021777534f, 0.00026506660043555067f, 0.00013253330021777534f, 1.0f, -1.7068261607237221f, 0.75242884221456008f},
    {1.0f, 2.0f, 1.0f, 1.0f, -1.8535228619096662f, 0.86595611483803558f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.7862894348694949f, 0.87269707452243317f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.9543928216301054f, 0.96170037756512228f},
};

struct Complex16 {
    int16_t real;
    int16_t imag;
};

struct RoomTarget {
    float dist_m;          /* parabolisch zwischen den 30-mm-Punkten verfeinert */
    float amplitude;       /* geglättete Echo-Amplitude (Rohwert) */
    float motion_energy;   /* geglättete Abweichung vom Standbild (Rohwert; reines Rauschen ≈ Rausch-Amplitude) */
    bool  moving;          /* motion_energy ≥ kRoomMoveK × Bewegungs-Rauschpegel */
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
    float    zoom_snr_db;      /* kombiniertes Pulsspektrum: Spitze/Median in dB */
    float    resp_depth_um;
    float    heart_amp_um;
    float    br_bpm;
    float    hr_spec_bpm;      /* stärkster Peak des kombinierten Spektrums (ungefiltert) */
    float    hr_acf_bpm;       /* Autokorrelation, Punkt mit klarstem Puls */
    float    hr_ibi_bpm;       /* Zeitbereich (Median-IBI), Punkt mit klarstem Puls */
    float    hr_fused_bpm;     /* verfolgter, geglätteter Puls (Hauptausgabe) */
    float    ibi_median_ms;
    float    rmssd_ms;
    float    confidence_pct;   /* Übereinstimmung der Zoom-Punkte (gewichtet) */
    uint8_t  hr_bin;           /* Zoom-Punkt mit klarstem Puls */
    uint32_t compute_us;       /* Rechenzeit dieses Reports */
    bool     hr_harm;          /* stärkste Pulsspitze liegt auf einer Atem-Oberwelle (ein Sprung dorthin braucht 10 Reports) */
    float    motion_pct;       /* Anteil gedämpfter 1-s-Blöcke im Fenster (Bewegung), über alle Zoom-Punkte */
    uint8_t  num_room_targets;
    RoomTarget room_targets[kMaxRoomTargets];  /* nach Stärke sortiert (Amplitude + 8 × Bewegung) */
    float    room_noise_amp;                   /* Rauschpegel Amplitude (unteres Viertel der Raum-Punkte) */
    float    room_noise_motion;                /* Rauschpegel Bewegung */
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
            hist_zoom_   = allocFloat((size_t)kHistoryLen * kZoomPoints * 2U);
            hist_um_     = allocFloat(kHistoryLen);
            hist_lb_phi_ = allocFloat(kHistoryLen);
            work_x_      = allocFloat(kHrWin);
            work_ext_    = allocFloat(kHrWin + 2U * kFiltPad);
            best_x_      = allocFloat(kHrWin);
            best_h_      = allocFloat(kHrWin);
            dec_         = allocFloat(kHrWin / kDecim + 1U);
            spec_        = allocFloat(kHrNumF);
            comb_        = allocFloat(kHrNumF);
            scratch_     = allocFloat(kHrNumF);
            br_comb_     = allocFloat(kBrNumF);
            allocated_   = hist_zoom_ && hist_um_ && hist_lb_phi_ && work_x_ && work_ext_ && best_x_ && best_h_ &&
                         dec_ && spec_ && comb_ && scratch_ && br_comb_;
        }
        resetState();
        return allocated_;
    }

    void resetState() {
        frame_count_          = 0;
        hist_write_idx_       = 0;
        hist_count_           = 0;
        cum_phase_rad_        = 0.0f;
        prev_centered_i_      = 0.0f;
        prev_centered_q_      = 0.0f;
        have_prev_iq_         = false;
        dc_i_                 = 0.0f;
        dc_q_                 = 0.0f;
        best_zoom_bin_        = kZoomPoints / 2U;
        out_of_window_frames_ = 0;
        last_temp_c_          = 0;
        hr_prev_              = 0.0f;
        hr_miss_              = 0;

        memset(zoom_clutter_i_, 0, sizeof(zoom_clutter_i_));
        memset(zoom_clutter_q_, 0, sizeof(zoom_clutter_q_));
        memset(zoom_var_ema_, 0, sizeof(zoom_var_ema_));
        memset(room_clutter_i_, 0, sizeof(room_clutter_i_));
        memset(room_clutter_q_, 0, sizeof(room_clutter_q_));
        memset(room_amp_ema_, 0, sizeof(room_amp_ema_));
        memset(room_var_ema_, 0, sizeof(room_var_ema_));
        memset(room_amp_slow_, 0, sizeof(room_amp_slow_));
        memset(room_mot_slow_, 0, sizeof(room_mot_slow_));
    }

    void setZoomStartPoint(int32_t zoom_start_pt) {
        zoom_start_pt_ = zoom_start_pt;
        resetState();
    }

    int32_t zoomStartPoint() const { return zoom_start_pt_; }

    /** Raumprofil (Subsweep 2): geglättete Amplitude und Bewegung je Punkt, kRoomPoints Werte. */
    const float *roomAmp() const { return room_amp_ema_; }
    const float *roomMotion() const { return room_var_ema_; }
    float roomStartMm() const { return (float)room_start_pt_ * kPointStepM * 1000.0f; }
    float roomStepMm() const { return (float)room_step_ * kPointStepM * 1000.0f; }

    /**
     * @brief Verarbeitet einen 3-Subsweep-Frame (105 komplexe Punkte).
     * @param dt_s wird nicht mehr verwendet (Zeitbasis = kFsHz); bleibt für die Aufrufer-Kompatibilität.
     * @param out_um_sample Auslenkung des Telemetrie-Strahls in µm (für `@IQ3`).
     */
    void pushFrame(const Complex16 *pts, uint16_t n_pts, int16_t temp_c, float dt_s, float &out_um_sample) {
        (void)dt_s;
        if (!allocated_ || pts == nullptr || n_pts < (1U + kZoomPoints)) {
            out_um_sample = 0.0f;
            return;
        }
        last_temp_c_ = temp_c;

        /* 1. Loopback-Phasenreferenz */
        const float lb_i   = (float)pts[0].real;
        const float lb_q   = (float)pts[0].imag;
        const float lb_amp = sqrtf(lb_i * lb_i + lb_q * lb_q);
        float rot_re = 1.0f, rot_im = 0.0f, lb_phi = 0.0f;
        if (lb_amp > 1.0f) {
            rot_re =  lb_i / lb_amp;
            rot_im = -lb_q / lb_amp; /* e^{-j * phi_lb} */
            lb_phi = atan2f(lb_q, lb_i);
        }
        last_lb_amp_ = lb_amp;

        /* 2. Zoom-Punkte derotieren, speichern und Bewegungsenergie schätzen */
        float derot_i[kZoomPoints], derot_q[kZoomPoints];
        const Complex16 *zoom_pts = &pts[1];
        for (uint16_t b = 0; b < kZoomPoints; b++) {
            const float ri = (float)zoom_pts[b].real;
            const float rq = (float)zoom_pts[b].imag;
            const float zi = ri * rot_re - rq * rot_im;
            const float zq = ri * rot_im + rq * rot_re;
            derot_i[b] = zi;
            derot_q[b] = zq;
            float *slot = hist_zoom_ + ((size_t)b * kHistoryLen + hist_write_idx_) * 2U;  /* punktweise ablegen */
            slot[0] = zi;
            slot[1] = zq;
            if (frame_count_ == 0) {
                zoom_clutter_i_[b] = zi;
                zoom_clutter_q_[b] = zq;
                zoom_var_ema_[b]   = 0.0f;
            } else {
                const float di = zi - zoom_clutter_i_[b];
                const float dq = zq - zoom_clutter_q_[b];
                zoom_clutter_i_[b] += 0.008f * di;
                zoom_clutter_q_[b] += 0.008f * dq;
                zoom_var_ema_[b]    = 0.96f * zoom_var_ema_[b] + 0.04f * (di * di + dq * dq);
            }
        }

        /* Telemetrie-Strahl: Punkt mit größter Bewegung, Wechsel nur bei 1,5-fach besserem Wert (Hysterese).
         * Ein Wechsel ohne Hysterese ließ das Wegsignal alle 0,5 s um bis zu 1 mm springen. */
        if (frame_count_ >= 15 && (frame_count_ % 30 == 0)) {
            float best_score = -1.0f, cur_score = 0.0f;
            uint8_t best_bin = best_zoom_bin_;
            for (uint8_t b = 2; b < kZoomPoints - 2; b++) {
                const float amp_sq = zoom_clutter_i_[b] * zoom_clutter_i_[b] + zoom_clutter_q_[b] * zoom_clutter_q_[b];
                const float score  = zoom_var_ema_[b] * sqrtf(amp_sq + 1.0f);
                if (b == best_zoom_bin_) cur_score = score;
                if (score > best_score) {
                    best_score = score;
                    best_bin   = b;
                }
            }
            if (best_bin != best_zoom_bin_ && best_score > 1.5f * cur_score) {
                best_zoom_bin_ = best_bin;
                beam_switched_ = true;
            }
        }

        float beam_i = 0.0f, beam_q = 0.0f, cl_i = 0.0f, cl_q = 0.0f;
        beamform(derot_i, derot_q, beam_i, beam_q, cl_i, cl_q);

        /* 3. Zentrierung + Phasenabwicklung → µm (nur Telemetrie; der Puls wird aus den Einzelpunkten berechnet) */
        if (frame_count_ == 0 || beam_switched_) {
            dc_i_ = cl_i;  /* nach einem Strahlwechsel vom statischen Anteil des neuen Strahls starten */
            dc_q_ = cl_q;
            have_prev_iq_  = false;
            beam_switched_ = false;
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
            cum_phase_rad_   += (mag2 > 2500.0f) ? atan2f(cross, dot) : (cross / mag2);
        }
        prev_centered_i_ = ci;
        prev_centered_q_ = cq;
        have_prev_iq_    = true;

        const float disp_um = cum_phase_rad_ * kRadToUm;
        out_um_sample       = disp_um;
        hist_um_[hist_write_idx_]     = disp_um;
        hist_lb_phi_[hist_write_idx_] = lb_phi;
        hist_write_idx_ = (uint16_t)((hist_write_idx_ + 1U) % kHistoryLen);
        if (hist_count_ < kHistoryLen) hist_count_++;

        /* 4. Raum-Scan (Subsweep 2) */
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
                    room_amp_slow_[r]  = amp;
                    room_mot_slow_[r]  = 0.0f;
                } else {
                    const float di = ri - room_clutter_i_[r];
                    const float dq = rq - room_clutter_q_[r];
                    room_clutter_i_[r] += 0.02f * di;
                    room_clutter_q_[r] += 0.02f * dq;
                    room_amp_ema_[r]    = 0.95f * room_amp_ema_[r] + 0.05f * amp;
                    room_var_ema_[r]    = 0.92f * room_var_ema_[r] + 0.08f * sqrtf(di * di + dq * dq);
                    room_amp_slow_[r]  += kRoomSlowA * (room_amp_ema_[r] - room_amp_slow_[r]);
                    room_mot_slow_[r]  += kRoomSlowA * (room_var_ema_[r] - room_mot_slow_[r]);
                }
            }
        }
        frame_count_++;
    }

    /**
     * @brief Raum-Ziele + Rezoom-Empfehlung immer; Vitalwerte (valid = true) ab kHrMinWin Frames.
     *        Etwa einmal pro Sekunde aufrufen (Verfolgung und Rezoom zählen in Reports).
     */
    bool computeVitalReport(VitalReport &rep) {
        const uint32_t t0 = micros();
        memset(&rep, 0, sizeof(rep));
        rep.frame_count   = frame_count_;
        rep.fs_hz         = kFsHz;
        rep.temperature_c = last_temp_c_;
        rep.loopback_amp  = last_lb_amp_;
        rep.best_zoom_bin = best_zoom_bin_;
        rep.target_dist_m = (float)(zoom_start_pt_ + (int32_t)best_zoom_bin_) * kPointStepM;
        extractRoomTargets(rep);

        if (hist_count_ < kHrMinWin) {
            rep.valid = false;
            rep.compute_us = micros() - t0;
            return false;
        }
        const uint16_t N = (hist_count_ > kHrWin) ? kHrWin : hist_count_;
        const uint16_t start = (uint16_t)((hist_write_idx_ + kHistoryLen - N) % kHistoryLen);

        /* Loopback-Phasenstabilität */
        float lb_mean = 0.0f, lb_var = 0.0f;
        for (uint16_t n = 0; n < N; n++) lb_mean += hist_lb_phi_[(start + n) % kHistoryLen];
        lb_mean /= (float)N;
        for (uint16_t n = 0; n < N; n++) {
            const float d = hist_lb_phi_[(start + n) % kHistoryLen] - lb_mean;
            lb_var += d * d;
        }
        rep.loopback_phase_std_mrad = sqrtf(lb_var / (float)N) * 1000.0f;

        /* Puls: Spektren aller Zoom-Punkte, gewichtet gemittelt */
        const uint16_t M = (uint16_t)((N + kDecim - 1U) / kDecim);
        float peak_hz[kZoomPoints], weight[kZoomPoints];
        float wsum = 0.0f, best_q = -1.0f;
        uint8_t hr_bin = 0;
        uint32_t moving_blocks = 0, total_blocks = 0;
        for (uint16_t k = 0; k < kHrNumF; k++) comb_[k] = 0.0f;
        for (uint16_t k = 0; k < kBrNumF; k++) br_comb_[k] = 0.0f;

        for (uint8_t b = 0; b < kZoomPoints; b++) {
            const float amp_c = binDisplacement(b, start, N, work_x_);
            removeLinearTrend(work_x_, N);
            float *h = work_ext_ + kFiltPad;  /* gefiltertes Signal steht nach sosFiltFilt mittig in work_ext_ */
            sosFiltFilt(work_x_, N);
            moving_blocks += gateMotion(h, N);
            /* Atem-Spektrum dieses Punkts (entzerrtes Wegsignal, dezimiert, Hann), gewichtet mit Amplitude² summiert.
             * Schwache Punkte haben Phasenrauschen ~ 1/Amplitude, und Abwickel-Fehler wirken dort wie Stufen von 2,5 mm:
             * Ungewichtet überdeckte ihr Tieftonanteil die Atmung (am Datensatz 2026-11-01 Punkte mit |z| 120–300,
             * 140–240 Phasensprünge in 15 s, 8–11 mm "Hub"; die starken Punkte zeigten sauber 17 /min). */
            const float wa = amp_c * amp_c;
            for (uint16_t m = 0; m < M; m++) {
                const float w = (M > 1) ? 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)m / (float)(M - 1U))) : 1.0f;
                dec_[m] = work_x_[(uint32_t)m * kDecim] * w;
            }
            for (uint16_t k = 0; k < kBrNumF; k++)
                br_comb_[k] += wa * goertzelPower(dec_, M, kFsHz / kDecim, kBrFMin + kBrFStep * k);
            total_blocks += N / kGateBlk;
            /* dezimieren + Hann */
            for (uint16_t m = 0; m < M; m++) {
                const float w = (M > 1) ? 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)m / (float)(M - 1U))) : 1.0f;
                dec_[m] = h[(uint32_t)m * kDecim] * w;
            }
            float pmax = 0.0f;
            uint16_t kmax = 0;
            for (uint16_t k = 0; k < kHrNumF; k++) {
                spec_[k] = goertzelPower(dec_, M, kFsHz / kDecim, kHrFMin + kHrFStep * k);
                if (spec_[k] > pmax) { pmax = spec_[k]; kmax = k; }
            }
            memcpy(scratch_, spec_, kHrNumF * sizeof(float));
            std::nth_element(scratch_, scratch_ + kHrNumF / 2, scratch_ + kHrNumF);
            const float med = scratch_[kHrNumF / 2] + 1e-12f;
            const float q   = pmax / med;
            const float w   = q * q;
            for (uint16_t k = 0; k < kHrNumF; k++) comb_[k] += w * (spec_[k] / med);
            wsum += w;
            weight[b]  = w;
            peak_hz[b] = kHrFMin + kHrFStep * kmax;
            if (q > best_q) {
                best_q = q;
                hr_bin = b;
                memcpy(best_x_, work_x_, N * sizeof(float));
                memcpy(best_h_, h, N * sizeof(float));
            }
        }
        for (uint16_t k = 0; k < kHrNumF; k++) comb_[k] /= (wsum + 1e-12f);
        rep.hr_bin     = hr_bin;
        rep.motion_pct = 100.0f * (float)moving_blocks / (float)(total_blocks ? total_blocks : 1U);

        /* Kandidaten: Top 3 lokale Maxima mit ≥ 0,08 Hz Abstand, nicht am Bandrand (am Datensatz lag die häufigste
         * Fehlspitze bei 54 /min = 0,90 Hz direkt über der Suchgrenze 0,88 Hz und wurde von vielen Punkten gestützt) */
        int16_t cand[3] = {-1, -1, -1};
        uint8_t n_cand  = 0;
        for (uint8_t c = 0; c < 3; c++) {
            int16_t bestk = -1;
            for (uint16_t k = kHrEdgeBins; k + kHrEdgeBins < kHrNumF; k++) {
                if (comb_[k] < comb_[k - 1] || comb_[k] < comb_[k + 1]) continue;
                bool far = true;
                for (uint8_t j = 0; j < n_cand; j++)
                    if (fabsf((float)(k - cand[j]) * kHrFStep) <= 0.08f) far = false;
                if (far && (bestk < 0 || comb_[k] > comb_[bestk])) bestk = (int16_t)k;
            }
            if (bestk < 0) break;
            cand[n_cand++] = bestk;
        }
        if (n_cand == 0) {
            rep.valid = false;
            rep.compute_us = micros() - t0;
            return false;
        }
        /* Atmung über alle Punkte: größtes echtes lokales Maximum (nicht am Bandrand), parabolisch verfeinert.
         * Vorher zählte das größte Bin im Suchbereich; bei Lagewechseln (Stufen im Wegsignal) oder Rest-Drift steigt die
         * Leistung zu tiefen Frequenzen weiter an, und das Ergebnis klebte am Rand bei 6,9 /min. Ohne echte Spitze
         * bleibt fb = 0 (br_bpm = 0 heißt: keine Atemspitze). */
        float fb = 0.0f, br_q = 0.0f;
        int16_t kb = -1;
        for (uint16_t k = kBrEdgeBins; k + kBrEdgeBins < kBrNumF; k++)
            if (br_comb_[k] > br_comb_[k - 1] && br_comb_[k] >= br_comb_[k + 1] && (kb < 0 || br_comb_[k] > br_comb_[kb]))
                kb = (int16_t)k;
        if (kb >= 0) {
            const float a = br_comb_[kb - 1], b = br_comb_[kb], c = br_comb_[kb + 1];
            float d = 0.0f;
            if (b >= a && b >= c && a - 2.0f * b + c < 0.0f)
                d = std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / (a - 2.0f * b + c)));
            fb = kBrFMin + kBrFStep * ((float)kb + d);
            memcpy(scratch_, br_comb_, kBrNumF * sizeof(float));
            std::nth_element(scratch_, scratch_ + kBrNumF / 2, scratch_ + kBrNumF);
            br_q = b / (scratch_[kBrNumF / 2] + 1e-12f);
        }
        /* Atem-Oberwellen (m = 2..6) im Pulsband: Bei schneller Atmung (≈ 20 /min) lag die 3. Oberwelle bei 55–60 /min
         * und überdeckte den Puls ganz. Eine Oberwelle wird nicht entfernt (der Puls kann darauf liegen), aber ein
         * Sprung dorthin braucht kHoldHarm statt 3 Reports. Am Datensatz mit schneller Atmung: ±8 /min von 47 auf 89 %. */
        auto isHarm = [&](int16_t k) {
            if (br_q < kBrMinQ) return false;
            const float f = kHrFMin + kHrFStep * k;
            for (uint8_t m = 2; m <= kHarmMMax; m++)
                if (fabsf(f - (float)m * fb) <= kHarmTolHz) return true;
            return false;
        };
        rep.hr_harm = isHarm(cand[0]);
        const float raw_bpm = (kHrFMin + kHrFStep * cand[0]) * 60.0f;
        rep.hr_spec_bpm = raw_bpm;
        memcpy(scratch_, comb_, kHrNumF * sizeof(float));
        std::nth_element(scratch_, scratch_ + kHrNumF / 2, scratch_ + kHrNumF);
        rep.zoom_snr_db = 10.0f * log10f(comb_[cand[0]] / (scratch_[kHrNumF / 2] + 1e-12f));

        /* Verfolgung mit Haltezeit. Ein Abbruch der Haltezeit über die Punkt-Zustimmung wurde getestet und
         * verworfen: Rhythmische Bewegung im Herzband wird von allen Punkten gestützt, der Puls sprang häufiger. */
        auto agreeAt = [&](float bpm) {
            float a = 0.0f;
            for (uint8_t b = 0; b < kZoomPoints; b++)
                if (fabsf(peak_hz[b] * 60.0f - bpm) <= 5.0f) a += weight[b];
            return a / (wsum + 1e-12f);
        };
        if (hr_prev_ <= 0.0f) {
            hr_prev_ = raw_bpm;
            hr_miss_ = 0;
        } else {
            int16_t near = -1;
            for (uint8_t j = 0; j < n_cand; j++) {
                const float bpm = (kHrFMin + kHrFStep * cand[j]) * 60.0f;
                if (comb_[cand[j]] >= 0.4f * comb_[cand[0]] && fabsf(bpm - hr_prev_) < 12.0f &&
                    (near < 0 || comb_[cand[j]] > comb_[near]))
                    near = cand[j];
            }
            if (near >= 0) {
                hr_prev_ = 0.7f * hr_prev_ + 0.3f * (kHrFMin + kHrFStep * near) * 60.0f;
                hr_miss_ = 0;
            } else if (++hr_miss_ >= (rep.hr_harm ? kHoldHarm : 3U)) {
                hr_prev_ = raw_bpm;
                hr_miss_ = 0;
            }
        }
        rep.hr_fused_bpm   = hr_prev_;
        rep.confidence_pct = 100.0f * agreeAt(hr_prev_);

        /* Herzsignal des klarsten Punkts: Amplitude, Autokorrelation, Einzelschläge */
        float hr_rms = 0.0f;
        for (uint16_t n = 0; n < N; n++) hr_rms += best_h_[n] * best_h_[n];
        rep.heart_amp_um = sqrtf(2.0f * hr_rms / (float)N);
        float acf_bpm = hr_prev_, acf_peak = 0.0f;
        computeCardiacAcf(best_h_, N, kFsHz, acf_bpm, acf_peak);
        rep.hr_acf_bpm = acf_bpm;
        float ibi_bpm = hr_prev_, ibi_ms = 60000.0f / hr_prev_, rmssd_ms = 0.0f;
        uint8_t n_beats = 0;
        computeTimeDomainIbi(best_h_, N, kFsHz, ibi_bpm, ibi_ms, rmssd_ms, n_beats);
        rep.hr_ibi_bpm    = ibi_bpm;
        rep.ibi_median_ms = ibi_ms;
        rep.rmssd_ms      = rmssd_ms;

        /* Atemhub am Punkt mit klarstem Puls, bei der Atemfrequenz aller Punkte (auf 15 Hz dezimiert, Hann) */
        for (uint16_t m = 0; m < M; m++) {
            const float w = (M > 1) ? 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)m / (float)(M - 1U))) : 1.0f;
            dec_[m] = best_x_[(uint32_t)m * kDecim] * w;
        }
        const float best_p = (fb > 0.0f) ? goertzelPower(dec_, M, kFsHz / kDecim, fb) : 0.0f;
        const float norm   = 0.25f * (float)M;  /* Hann-Verstärkung → Amplitude in µm */
        rep.br_bpm        = fb * 60.0f;
        rep.resp_depth_um = 2.0f * sqrtf(best_p) / norm;

        rep.valid      = true;
        rep.compute_us = micros() - t0;
        return true;
    }

private:
    static float *allocFloat(size_t count) {
        void *p = nullptr;
        if (psramFound()) p = ps_malloc(count * sizeof(float));
        if (!p) p = malloc(count * sizeof(float));
        if (p) memset(p, 0, count * sizeof(float));
        return (float *)p;
    }

    /** Kohärenter 5-Punkt-Strahl um best_zoom_bin_ (für Telemetrie); cl_* = derselbe Strahl über die Clutter-Schätzung. */
    void beamform(const float *di, const float *dq, float &bi, float &bq, float &cli, float &clq) const {
        const uint8_t b0 = best_zoom_bin_;
        const float ref_ci = zoom_clutter_i_[b0], ref_cq = zoom_clutter_q_[b0];
        const float ref_ca = sqrtf(ref_ci * ref_ci + ref_cq * ref_cq) + 1e-3f;
        float w_sum = 0.0f;
        for (int8_t db = -2; db <= 2; db++) {
            const int16_t idx = (int16_t)b0 + db;
            if (idx < 0 || idx >= (int16_t)kZoomPoints) continue;
            const float ci = zoom_clutter_i_[idx], cq = zoom_clutter_q_[idx];
            const float ca = sqrtf(ci * ci + cq * cq) + 1e-3f;
            const float are = (ci * ref_ci + cq * ref_cq) / (ca * ref_ca);
            const float aim = (ci * ref_cq - cq * ref_ci) / (ca * ref_ca);
            const float w   = (db == 0) ? 1.0f : ((abs(db) == 1) ? 0.75f : 0.40f);
            bi  += w * (di[idx] * are - dq[idx] * aim);
            bq  += w * (di[idx] * aim + dq[idx] * are);
            cli += w * (ci * are - cq * aim);
            clq += w * (ci * aim + cq * are);
            w_sum += w;
        }
        bi /= w_sum; bq /= w_sum; cli /= w_sum; clq /= w_sum;
    }

    /** Phase des Zoom-Punkts b um den Fenster-Mittelpunkt abwickeln → µm. Rückgabe: mittlerer Abstand zum Mittelpunkt
     *  (|z − Mittelwert|, Maß für die Phasenqualität dieses Punkts). */
    float binDisplacement(uint8_t b, uint16_t start, uint16_t N, float *out) const {
        const float *base = hist_zoom_ + (size_t)b * kHistoryLen * 2U;  /* Ring dieses Punkts, fortlaufend im PSRAM */
        double mi = 0.0, mq = 0.0;
        for (uint16_t n = 0; n < N; n++) {
            const float *z = base + (size_t)((start + n) % kHistoryLen) * 2U;
            mi += z[0];
            mq += z[1];
        }
        mi /= (double)N;
        mq /= (double)N;
        float prev = 0.0f, acc = 0.0f;
        double amp_sum = 0.0;
        for (uint16_t n = 0; n < N; n++) {
            const float *z = base + (size_t)((start + n) % kHistoryLen) * 2U;
            const float ci = z[0] - (float)mi, cq = z[1] - (float)mq;
            const float phi = atan2f(cq, ci);
            amp_sum += sqrtf(ci * ci + cq * cq);
            if (n > 0) {
                float d = phi - prev;
                if (d > (float)M_PI) d -= 2.0f * (float)M_PI;
                else if (d < -(float)M_PI) d += 2.0f * (float)M_PI;
                acc += d;
            } else {
                acc = phi;
            }
            prev   = phi;
            out[n] = acc * kRadToUm;
        }
        return (float)(amp_sum / (double)N);
    }

    /**
     * Nullphasen-Bandpass wie scipy `sosfiltfilt` (ungerade Erweiterung um kFiltPad, Anfangszustände aus der
     * Sprungantwort). Ergebnis: work_ext_[kFiltPad .. kFiltPad + N).
     */
    void sosFiltFilt(const float *x, uint16_t N) {
        const uint16_t P = (N > kFiltPad) ? kFiltPad : (uint16_t)(N - 1U);
        float *e = work_ext_ + (kFiltPad - P);
        const uint32_t L = (uint32_t)N + 2U * P;
        for (uint16_t i = 0; i < P; i++) e[i] = 2.0f * x[0] - x[P - i];
        memcpy(e + P, x, N * sizeof(float));
        for (uint16_t j = 0; j < P; j++) e[P + N + j] = 2.0f * x[N - 1U] - x[N - 2U - j];
        sosPass(e, L, false);
        sosPass(e, L, true);
    }

    /** Ein Durchlauf der vier Biquads (DF2T) in Vorwärts- oder Rückwärtsrichtung, Anfangszustand = Sprungantwort. */
    static void sosPass(float *e, uint32_t L, bool reverse) {
        const float x0 = reverse ? e[L - 1U] : e[0];
        float z1[4], z2[4];
        float level = x0;
        for (uint8_t s = 0; s < 4; s++) {
            const float *c = kBpSos[s];
            const float g = (c[0] + c[1] + c[2]) / (1.0f + c[4] + c[5]);  /* DC-Verstärkung der Sektion */
            z1[s] = (g - c[0]) * level;
            z2[s] = (c[2] - c[5] * g) * level;
            level *= g;
        }
        for (uint32_t i = 0; i < L; i++) {
            const uint32_t n = reverse ? (L - 1U - i) : i;
            float v = e[n];
            for (uint8_t s = 0; s < 4; s++) {
                const float *c = kBpSos[s];
                const float y = c[0] * v + z1[s];
                z1[s] = c[1] * v - c[4] * y + z2[s];
                z2[s] = c[2] * v - c[5] * y;
                v = y;
            }
            e[n] = v;
        }
    }

    /* Bewegungsdämpfung im Herzband: Effektivwert je 1-s-Block. Bezug ist das ruhige Viertel der Blöcke (nicht der
     * Median: bei 8 s Bewegung in 15 s ist der Median selbst Bewegung). Blöcke über kGateK × Bezug bekommen die
     * Verstärkung (Grenze/Effektivwert)^kGateP, linear zwischen den Blockmitten übergeblendet. Ein paar Sekunden
     * Bewegung überdecken sonst 10 s sauberen Puls (am Datensatz mit eingefügter Bewegung geprüft).
     * Rückgabe: Zahl der Blöcke über kMotionK × Bezug (Bewegung). */
    static uint8_t gateMotion(float *h, uint16_t N) {
        const uint8_t nb = (uint8_t)std::min<uint16_t>(N / kGateBlk, kMaxBlocks);
        if (nb < 3) return 0;
        float rms[kMaxBlocks], tmp[kMaxBlocks], gain[kMaxBlocks];
        for (uint8_t k = 0; k < nb; k++) {
            const uint16_t a = k * kGateBlk, e = (k + 1U == nb) ? N : (uint16_t)(a + kGateBlk);
            float s = 0.0f;
            for (uint16_t n = a; n < e; n++) s += h[n] * h[n];
            rms[k] = tmp[k] = sqrtf(s / (float)(e - a));
        }
        std::nth_element(tmp, tmp + nb / 4, tmp + nb);
        const float ref   = tmp[nb / 4] + 1e-6f;
        const float limit = kGateK * ref;
        uint8_t gated = 0, moving = 0;
        for (uint8_t k = 0; k < nb; k++) {
            gain[k] = (rms[k] > limit) ? powf(limit / rms[k], kGateP) : 1.0f;
            if (gain[k] < 1.0f) gated++;
            if (rms[k] > kMotionK * ref) moving++;
        }
        if (!gated) return 0;
        const float half = 0.5f * (float)kGateBlk;
        for (uint16_t n = 0; n < N; n++) {
            const float pos = ((float)n - half) / (float)kGateBlk;  /* Blockmitten bei 0, 1, 2, ... */
            float g;
            if (pos <= 0.0f) g = gain[0];
            else if (pos >= (float)(nb - 1)) g = gain[nb - 1];
            else {
                const uint8_t k0 = (uint8_t)pos;
                const float fr = pos - (float)k0;
                g = gain[k0] + fr * (gain[k0 + 1] - gain[k0]);
            }
            h[n] *= g;
        }
        return moving;
    }

    static void removeLinearTrend(float *x, uint16_t N) {
        if (N < 2) return;
        double sum_y = 0.0, sum_tt = 0.0, sum_ty = 0.0;
        const double half = 0.5 * (double)(N - 1U);
        for (uint16_t n = 0; n < N; n++) {
            const double t = (double)n - half;
            sum_y  += x[n];
            sum_tt += t * t;
            sum_ty += t * x[n];
        }
        const double mean_y = sum_y / (double)N;
        const double slope  = (sum_tt > 1e-6) ? (sum_ty / sum_tt) : 0.0;
        for (uint16_t n = 0; n < N; n++) x[n] = (float)((double)x[n] - (mean_y + slope * ((double)n - half)));
    }

    /** Goertzel-Leistung ohne Fenster (Fenster ist schon im Signal). */
    static float goertzelPower(const float *x, uint16_t N, float fs, float freq_hz) {
        const float coeff = 2.0f * cosf(2.0f * (float)M_PI * freq_hz / fs);
        float s1 = 0.0f, s2 = 0.0f;
        for (uint16_t n = 0; n < N; n++) {
            const float s0 = x[n] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        return power > 0.0f ? power : 0.0f;
    }

    static void computeCardiacAcf(const float *hr, uint16_t N, float fs, float &out_bpm, float &out_peak) {
        float e0 = 1e-6f;
        for (uint16_t n = 0; n < N; n++) e0 += hr[n] * hr[n];
        const uint16_t min_lag = (uint16_t)fmaxf(8.0f, floorf(fs * (60.0f / 170.0f)));
        const uint16_t max_lag = (uint16_t)fminf((float)(N / 2U), ceilf(fs * (60.0f / 50.0f)));
        float best_r = -1.0f, r_prev = 0.0f, r_curr = 0.0f;
        uint16_t best_lag = min_lag;
        for (uint16_t lag = min_lag; lag <= max_lag; lag++) {
            float s = 0.0f;
            for (uint16_t n = 0; n + lag < N; n++) s += hr[n] * hr[n + lag];
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
        const uint16_t refractory = (uint16_t)roundf(0.36f * fs); /* ≥ 360 ms (≤ 166 /min) */

        uint16_t peak_idx[48];
        uint8_t  n_peaks = 0;
        uint16_t last_pk = 0;
        for (uint16_t n = 2; n + 2 < N; n++) {
            if (hr[n] > thresh && hr[n] > hr[n - 1] && hr[n] >= hr[n + 1] && hr[n] > hr[n - 2] && hr[n] >= hr[n + 2]) {
                if (n_peaks == 0 || (n - last_pk) >= refractory) {
                    peak_idx[n_peaks++] = n;
                    last_pk = n;
                    if (n_peaks >= 48) break;
                } else if (hr[n] > hr[last_pk]) {
                    peak_idx[n_peaks - 1] = n;
                    last_pk = n;
                }
            }
        }
        out_beats = n_peaks;
        if (n_peaks < 4) return;

        float ibis_ms[47];
        uint8_t n_ibi = 0;
        for (uint8_t k = 1; k < n_peaks; k++) {
            const float dt_ms = ((float)(peak_idx[k] - peak_idx[k - 1]) / fs) * 1000.0f;
            if (dt_ms >= 350.0f && dt_ms <= 1250.0f) ibis_ms[n_ibi++] = dt_ms;
        }
        if (n_ibi < 3) return;

        float sorted[47];
        memcpy(sorted, ibis_ms, n_ibi * sizeof(float));
        std::sort(sorted, sorted + n_ibi);
        const float median_ms = sorted[n_ibi / 2];
        out_ibi_ms = median_ms;
        out_bpm    = 60000.0f / median_ms;

        float sum_sq_diff = 0.0f;
        uint8_t n_diff = 0;
        for (uint8_t k = 1; k < n_ibi; k++) {
            if (fabsf(ibis_ms[k] - median_ms) < 0.25f * median_ms && fabsf(ibis_ms[k - 1] - median_ms) < 0.25f * median_ms) {
                const float d = ibis_ms[k] - ibis_ms[k - 1];
                sum_sq_diff += d * d;
                n_diff++;
            }
        }
        out_rmssd_ms = (n_diff > 0) ? sqrtf(sum_sq_diff / (float)n_diff) : 0.0f;
    }

    /** Unteres Viertel von 80 Werten (Rauschpegel; robust, solange weniger als drei Viertel des Raums Echos enthalten). */
    static float lowerQuartile(const float *v) {
        float tmp[kRoomPoints];
        memcpy(tmp, v, sizeof(tmp));
        std::nth_element(tmp, tmp + kRoomPoints / 4, tmp + kRoomPoints);
        return tmp[kRoomPoints / 4];
    }

    /** Lokale Maxima von max(Amplitude, Bewegung) relativ zum Rauschen ab kRoomDetK (bei gleich hohen Nachbarn zählt
     *  der vordere). f(r, d, bewegt) je Spitze; d = parabolische Verfeinerung in Punkten (−0,5 … 0,5). */
    template <typename F>
    static void forEachRoomPeak(const float *amp, const float *mot, float noise_amp, float noise_mot, F f) {
        auto level = [&](uint16_t r) { return std::max(amp[r] / noise_amp, mot[r] / noise_mot); };
        for (uint16_t r = 1; r + 1 < kRoomPoints; r++) {
            const float a = level(r - 1), b = level(r), c = level(r + 1);
            if (b < kRoomDetK || b <= a || b < c) continue;
            const float den = a - 2.0f * b + c;
            const float d   = (den < 0.0f) ? std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / den)) : 0.0f;
            f(r, d, mot[r] >= kRoomMoveK * noise_mot);
        }
    }

    float roomDistM(uint16_t r, float d) const {
        return ((float)room_start_pt_ + ((float)r + d) * (float)room_step_) * kPointStepM;
    }

    void extractRoomTargets(VitalReport &rep) {
        rep.num_room_targets = 0;
        /* Rauschpegel getrennt für Amplitude und Bewegung. Reines Rauschen hat Bewegung ≈ Amplitude (das Standbild
         * mittelt es weg); am Datensatz 2026-11-01 beide ≈ 55, die Person bis 3300 / 2600. +1 gegen Division durch 0
         * in den ersten Frames. */
        const float noise_amp = lowerQuartile(room_amp_ema_) + 1.0f;
        const float noise_mot = lowerQuartile(room_var_ema_) + 1.0f;
        rep.room_noise_amp    = noise_amp;
        rep.room_noise_motion = noise_mot;

        /* Ziel = lokales Maximum von max(Amplitude, Bewegung) relativ zum Rauschen, ab kRoomDetK. So zählen feste Echos
         * (Wand, Möbel: Amplitude hoch, Bewegung ≈ Rauschen) und schwache bewegte Echos. Behalten werden die
         * kMaxRoomTargets stärksten nach Amplitude + 8 × Bewegung (Bewegung zählt stärker), egal in welcher Entfernung. */
        auto key = [](const RoomTarget &t) { return t.amplitude + 8.0f * t.motion_energy; };
        forEachRoomPeak(room_amp_ema_, room_var_ema_, noise_amp, noise_mot, [&](uint16_t r, float d, bool moving) {
            const RoomTarget t = {roomDistM(r, d), room_amp_ema_[r], room_var_ema_[r], moving};
            uint8_t n = rep.num_room_targets;
            if (n == kMaxRoomTargets && key(t) <= key(rep.room_targets[n - 1])) return;
            uint8_t pos = (n < kMaxRoomTargets) ? n : (uint8_t)(n - 1U);  /* volle Liste: schwächstes Ziel fällt raus */
            while (pos > 0 && key(rep.room_targets[pos - 1]) < key(t)) {
                rep.room_targets[pos] = rep.room_targets[pos - 1];
                pos--;
            }
            rep.room_targets[pos] = t;
            if (n < kMaxRoomTargets) rep.num_room_targets++;
        });

        /* Zoom-Ziel: stärkstes bewegtes Ziel (Amplitude + 8 × Bewegung) im geglätteten Profil (τ ≈ 3 s), nicht in der
         * Momentaufnahme. Live 2026-11-01 lagen zwei bewegte Echos nah beieinander (Brust 0,66 m mit schwankender
         * Amplitude 440–1220, zweites Echo 0,78 m gleichmäßig ≈ 950, aber halb so viel Bewegung); die Momentaufnahme bei
         * t = 3 s wählte das hintere um 1 %, und dort war der Puls schlechter (Konfidenz 78 statt 99 %). Geglättet trifft
         * die Wahl in allen vier Logs die Brust (τ = 1–8 s geprüft). Das stärkste Echo allein wäre falsch gewesen.
         * Das Fenster hält nur dieses Ziel (±60 mm), nicht jedes bewegte Echo: Sonst blieb es 19 s am hinteren Echo. */
        const float ns_amp = lowerQuartile(room_amp_slow_) + 1.0f;
        const float ns_mot = lowerQuartile(room_mot_slow_) + 1.0f;
        float best_m = -1.0f, best_key = 0.0f;
        forEachRoomPeak(room_amp_slow_, room_mot_slow_, ns_amp, ns_mot, [&](uint16_t r, float d, bool moving) {
            const float k = room_amp_slow_[r] + 8.0f * room_mot_slow_[r];
            if (moving && (best_m < 0.0f || k > best_key)) {
                best_m   = roomDistM(r, d);
                best_key = k;
            }
        });

        /* Rezoom nur, wenn das Zoom-Ziel nicht im Zoom-Fenster ±60 mm (zwei Raum-Schritte) liegt, und erst nach
         * 3 Reports in den ersten 10 s (Ziel suchen) bzw. 5 Reports danach. Jeder Rezoom löscht die 15-s-Historie.
         * Abwägung am 2026-11-01: Ohne Toleranz wechselte der Zoom zwischen Körperstellen (Raum-Ziel 660 mm, bester
         * Puls bei 620 mm); mit ±30 mm wollte er 20-mal von 570–630 auf 630–690 mm; mit ±150 mm bliebe ein Ziel bei
         * 0,68 m neben einem Fenster bei 0,50–0,56 m hängen. Am Körperrand (Fenster 500–560 mm, Brust 580–620 mm)
         * lieferte der Puls live trotzdem 100 % im Bereich ±8 /min. */
        const float zoom_min_m = (float)(zoom_start_pt_ + 3) * kPointStepM - 0.06f;
        const float zoom_max_m = (float)(zoom_start_pt_ + (int32_t)kZoomPoints - 4) * kPointStepM + 0.06f;
        rep.needs_rezoom              = false;
        rep.recommended_zoom_start_pt = zoom_start_pt_;
        if (best_m < 0.0f || (best_m >= zoom_min_m && best_m <= zoom_max_m)) {
            out_of_window_frames_ = 0;
            return;
        }
        const uint16_t needed = (frame_count_ < 600U) ? 3U : 5U;
        if (++out_of_window_frames_ >= needed) {
            int32_t new_start = (int32_t)roundf(best_m / kPointStepM) - (int32_t)(kZoomPoints / 2U);
            if (new_start < 80) new_start = 80; /* ≥ 0,20 m */
            rep.needs_rezoom              = true;
            rep.recommended_zoom_start_pt = new_start;
            out_of_window_frames_         = 0;
        }
    }

    bool     allocated_            = false;
    int32_t  zoom_start_pt_        = 200;
    int32_t  room_start_pt_        = 120;
    uint16_t room_step_            = kRoomStep;
    uint32_t frame_count_          = 0;
    uint16_t hist_write_idx_       = 0;
    uint16_t hist_count_           = 0;
    int16_t  last_temp_c_          = 0;
    float    last_lb_amp_          = 0.0f;
    float    cum_phase_rad_        = 0.0f;
    float    prev_centered_i_      = 0.0f;
    float    prev_centered_q_      = 0.0f;
    bool     have_prev_iq_         = false;
    bool     beam_switched_        = false;
    float    dc_i_                 = 0.0f;
    float    dc_q_                 = 0.0f;
    uint8_t  best_zoom_bin_        = 12;
    uint16_t out_of_window_frames_ = 0;
    float    hr_prev_              = 0.0f;
    uint8_t  hr_miss_              = 0;

    float zoom_clutter_i_[kZoomPoints] = {};
    float zoom_clutter_q_[kZoomPoints] = {};
    float zoom_var_ema_[kZoomPoints]   = {};
    float room_clutter_i_[kRoomPoints] = {};
    float room_clutter_q_[kRoomPoints] = {};
    float room_amp_ema_[kRoomPoints]   = {};
    float room_var_ema_[kRoomPoints]   = {};
    float room_amp_slow_[kRoomPoints]  = {};  /* room_amp_ema_ zusätzlich geglättet (τ ≈ 3 s), für das Zoom-Ziel */
    float room_mot_slow_[kRoomPoints]  = {};  /* room_var_ema_ zusätzlich geglättet (τ ≈ 3 s) */

    float *hist_zoom_   = nullptr;  /* [kZoomPoints][kHistoryLen][I,Q], derotiert, je Punkt ein Ring */
    float *hist_um_     = nullptr;
    float *hist_lb_phi_ = nullptr;
    float *work_x_      = nullptr;
    float *work_ext_    = nullptr;  /* sosfiltfilt mit Rand: kHrWin + 2 * kFiltPad */
    float *best_x_      = nullptr;
    float *best_h_      = nullptr;
    float *dec_         = nullptr;
    float *spec_        = nullptr;
    float *comb_        = nullptr;
    float *scratch_     = nullptr;
    float *br_comb_     = nullptr;
};

/*
 * Ausgabezeilen für Raum-Ziele und Raumprofil (`iq3`). Firmware und PC-Prüfung (host/a121_esp_ultimate.py, verify-cpp)
 * nutzen dieselben Funktionen, damit das Format übereinstimmt. tick = Sensor-Zeitstempel in ms; er läuft lückenlos
 * weiter, auch über Rechenpausen und Rezooms. Rückgabe: Länge ohne Zeilenende (bei zu kleinem Puffer gekürzt).
 *   @ROOM,t_ms,tick,rausch_amp,rausch_bew,n,{dist_mm,amp,bew,bewegt}×n      (Ziele nach Stärke sortiert)
 *   @ROOMPROF,t_ms,tick,start_mm,step_mm,n,amp_0..amp_n-1,bew_0..bew_n-1    (Rohwerte, geglättet)
 */
static constexpr size_t kRoomLineMax = 1400U;  /* reicht für @ROOMPROF mit 2 × 80 Werten (≤ ~1100 Zeichen) */

inline size_t formatRoomLine(char *buf, size_t cap, uint32_t t_ms, uint32_t tick, const VitalReport &rep) {
    int len = snprintf(buf, cap, "@ROOM,%lu,%lu,%.0f,%.0f,%u", (unsigned long)t_ms, (unsigned long)tick,
                       rep.room_noise_amp, rep.room_noise_motion, (unsigned)rep.num_room_targets);
    for (uint8_t i = 0; i < rep.num_room_targets && len > 0 && (size_t)len < cap; i++) {
        const RoomTarget &t = rep.room_targets[i];
        len += snprintf(buf + len, cap - (size_t)len, ",%.0f,%.0f,%.0f,%u", t.dist_m * 1000.0f, t.amplitude,
                        t.motion_energy, (unsigned)t.moving);
    }
    return (len < 0) ? 0U : std::min((size_t)len, cap - 1U);
}

inline size_t formatRoomProfileLine(char *buf, size_t cap, uint32_t t_ms, uint32_t tick, const UltimateEspDsp &dsp) {
    const float *amp = dsp.roomAmp(), *mot = dsp.roomMotion();
    int len = snprintf(buf, cap, "@ROOMPROF,%lu,%lu,%.0f,%.0f,%u", (unsigned long)t_ms, (unsigned long)tick,
                       dsp.roomStartMm(), dsp.roomStepMm(), (unsigned)kRoomPoints);
    for (uint16_t r = 0; r < 2U * kRoomPoints && len > 0 && (size_t)len < cap; r++)
        len += snprintf(buf + len, cap - (size_t)len, ",%.0f", r < kRoomPoints ? amp[r] : mot[r - kRoomPoints]);
    return (len < 0) ? 0U : std::min((size_t)len, cap - 1U);
}

} /* namespace a121_dsp */

#endif /* A121_ESP_DSP_H */
