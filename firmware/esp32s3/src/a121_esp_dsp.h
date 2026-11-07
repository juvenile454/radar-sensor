/**
 * @file a121_esp_dsp.h
 * @brief On-device evaluation of the 3-subsweep sparse IQ session (`iq3`) on the ESP32-S3 (PSRAM).
 *
 * Chain (as of 2026-11-01, developed on `iq3raw_normal_120s_2026-11-01.npz` and `iq3raw_motion_120s_2026-11-01.npz`):
 *   1. Subsweep 0: on-chip loopback → phase derotation of all zoom points per frame (PLL/clock jitter).
 *   2. Subsweep 1: all 24 zoom points (60 mm, 2.5 mm grid) are stored individually (18 s history: 15 s window
 *      and 3 s during which the report computes from its snapshot while frames keep arriving; V2.1).
 *   3. Pulse: per point, unwrap the phase around the circle center of the IQ values (Kåsa fit, circleCenter) (→ µm), detrend linearly, zero-phase bandpass
 *      0.6–5.0 Hz (4th-order Butterworth, identical to scipy `sosfiltfilt`), motion damping per 1 s block
 *      (reference: quiet quarter of the window), decimate to 15 Hz, Hann, real FFT (1024 points, a121_fft.h, V2.2;
 *      previously Goertzel per frequency), harmonic sum S(f) = P(f) + 0.5 · P(2f) for 0.645–3.03 Hz; peaks only count
 *      between 0.674 and 3.003 Hz (40–180 /min, V1.2; previously 55–136), parabolically refined. Phase via fastAtan2.
 *      The spectra of all points are averaged, weighted by (peak/median)²;
 *      points with a clear pulse count more. Breathing harmonics are NOT projected out of the spectrum: if the pulse lies
 *      near m × breathing rate, the projection removes the heart component with it (on the dataset: 13–42 % instead of 61–89 % hits).
 *   4. Candidates (V1.3): top 3 peaks of S with a penalty on breathing harmonics (m = 4..8: × 0.5 at ±0.04 Hz;
 *      m = 2, 3: × 0.02 across the whole main lobe ±0.1 Hz). Octave check per candidate: peak at f/2 with
 *      S(f/2) ≥ 0.5 · S(f) → the candidate stands for f/2 (2nd heart harmonic), unless f/2 lies at 2 × / 3 × breathing.
 *      Tracking: a near candidate (±12 /min) with ≥ 50 % of the best value wins (smoothing 0.7/0.3); a far
 *      jump counts after 3 reports, onto a breathing harmonic only after 10. If the tracked value lies at 2 × / 3 ×
 *      breathing, it switches to a candidate without a harmonic after 3 reports. hr_harm = 1 only means "peak lies on
 *      a harmonic"; the pulse may lie there (live 2026-11-01: pulse 78 ≈ 4 × breathing 19.5).
 *   5. Confidence (V1.3, signalQuality): peak height, harmonic, agreement of spectrum, autocorrelation and
 *      individual beats, motion, jump. The earlier point agreement is at the end of @DSP as point_agree_pct.
 *      motion_pct = share of 1 s blocks with more than 3 × the quiet amplitude in the heart band (motion hint).
 *      Valid only with a sign of life: clean echo and breathing (≥ 100 µm) or held breath with a clear pulse peak,
 *      at most 30 % motion. Start (V2.4): window grows from 6 s to 15 s, confidence × N/900; below 8 s the
 *      value is preliminary (not tracked) and counts only if autocorrelation or individual beats confirm it and it
 *      does not lie on a breathing harmonic m ≤ 3 (V3.1). Variant A121_DSP_COMBINE (V3.1): instead of single spectra, the
 *      1st principal component of the 12 strongest points as one channel (combinePrincipal), effective point count at the end of @DSP.
 *      Variant A121_DSP_IBI_TEMPLATE (V3.6): individual beats in the 0.6–8 Hz band via template and matched filter
 *      (templateIbi) instead of peaks in the pulse band. A121_DSP_HEAVY (V5.2, phone only): A121_DSP_VITERBI and
 *      A121_DSP_HARM_REMOVE together.
 *   6. Breathing rate from the power sum of all zoom points, weighted by amplitude²: largest true local
 *      maximum in 0.115–0.585 Hz, otherwise 0 (no breathing peak). Previously unweighted and largest bin; that stuck in about
 *      a quarter of the reports at the edge at 6.9 /min (weak points, posture change). Breathing excursion at the breathing point (strongest
 *      breathing with a clean echo). Heart autocorrelation and time-domain beats (IBI, RMSSD) on the point with the
 *      clearest pulse, after removing the breathing harmonics except those at the pulse (removeBreathHarmonics).
 *   7. Subsweep 2: room scan 0.30–2.67 m (30 mm) with a rezoom recommendation. Noise level = lower quarter of all 80 points
 *      (amplitude and motion separately). Target = local maximum from 2 × noise level, "moving" from 3 × motion noise level;
 *      the 8 strongest are reported. Previously fixed raw values applied (value > 120, motion > 12), which were below the noise
 *      (≈ 55 / 55), and only the 4 nearest peaks. Consequence: noise peaks in the zoom window blocked the rezoom.
 *      Rezoom to the strongest moving target in the profile smoothed over ≈ 3 s (not in the snapshot, which flipped with two
 *      nearby echoes); no rezoom while this target lies within the zoom window ±60 mm (previously the zoom switched
 *      between body locations and cleared the history each time).
 *   V6 (A121_DSP_REACQ, off by default; draft docs/design/specs/2026-11-02-pulse-reacquisition-design.md):
 *      reacquire the pulse after motion. Motion map before the spectra (motionMap: 3 strongest points, 1 s block moving
 *      if 2 of 3 exceed 3 × the quiet quarter). Without a moving block as above. Moving and quiet tail ≥ 6 s: pulse
 *      (spectra, candidates, ACF, individual beats) only on the tail; breathing, breathing state and motion_pct on the
 *      whole window; the value counts only with confirmation by ACF or individual beats (±5 /min), as with preliminary values, and not
 *      on a breathing harmonic m ≤ 3; tracking is not advanced, hr_prev_ only from a confirmed
 *      value. Shorter tail: no pulse, tracking (hr_prev_, hr_miss_, alt_count_, Viterbi, hr_hist_) unchanged.
 *      Memory (ReacqMemory): a safe value (confidence ≥ 50 %, window ≥ 8 s) holds for 60 s while echo and target stay
 *      in the zoom; in the tail, the strongest candidate ±15 /min around it is chosen first (preferMemoryCandidate);
 *      once confirmed it is reacquired (reacq = 2) and replaces the breathing peak as the sign of life if it lies at most
 *      kMemSubstBpm (5 /min) from the remembered value (ruling R15). The 30 % motion rule does not apply. @DSP: win_s, reacq.
 *      Variant A121_DSP_REACQ_PERIOD (only with A121_DSP_REACQ; REJECTED, off by default, code kept as an experiment):
 *      a block that is too strong counts as moving only if the signal around it (3 s, 15 Hz, Hann, heart band) is not periodic
 *      (peak < 5 × mean of the bins outside the main lobe; segmentPeriodic). Bench (simulation): does not hold the limits
 *      (move hr_mae, short, fidget, move_away wrong after motion), tapping stays at 4 wrong values; the
 *      simulated motion is a sine, hence periodic. Numbers in docs/EXPERIMENTS.md (V6).
 *      Variant A121_DSP_PHASE_SLOPE (independent of A121_DSP_REACQ; REJECTED, off by default, code kept as an experiment):
 *      the chosen candidate (cf[0]) comes from the slope of the unwrapped phase of the 3-bin narrowband (phaseSlopeHz,
 *      1024 FFT, heart signal of the clearest point) instead of from the parabola. Bench (simulation): worse than the parabola
 *      (on the 1024 grid the three bins lie on the flat top of the main lobe, the slope snaps to the
 *      center bin: error up to 0.007 Hz instead of < 0.0001 Hz), pulse MAE higher in 12 of 19 scenarios (1 lower; in the
 *      10 s window 13 of 19), 7 new violations (alone). The verdict applies only to the 3-bin variant of the draft
 *      (k0 ± 1 snaps to the center bin); a band of about ± 2 · 1024 / m bins (m = window length in samples) was not tried against the parabola.
 *      Numbers in docs/EXPERIMENTS.md (V6). Cost with the switch: 14 KB RAM, two complex FFTs (1024) per report.
 *
 * Time base: fixed frame rate kFsHz (the session runs at 60 Hz, the sensor delivers without gaps). The earlier
 * estimate from ESP32 processing times was wrong because frames arrive bundled after every compute pause.
 * Results are rhythm estimates without any medical statement; judge accuracy only with a reference measurement.
 */

#ifndef A121_ESP_DSP_H
#define A121_ESP_DSP_H

#include <Arduino.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "a121_fft.h"

/* Phone build (V5.2): the more compute-intensive variants together, only in the app library radardsp_live. */
#ifdef A121_DSP_HEAVY
#ifndef A121_DSP_VITERBI
#define A121_DSP_VITERBI
#endif
#ifndef A121_DSP_HARM_REMOVE
#define A121_DSP_HARM_REMOVE
#endif
#endif

namespace a121_dsp {

static constexpr float kRadarFreqHz  = 60.5e9f;
static constexpr float kSpeedOfLight = 299792458.0f;
static constexpr float kWavelengthM  = kSpeedOfLight / kRadarFreqHz;                    /* ~4.9552 mm */
static constexpr float kRadToUm      = (kWavelengthM / (4.0f * (float)M_PI)) * 1.0e6f;  /* ~394.37 µm/rad */
static constexpr float kPointStepM   = 0.0025f;                                         /* 2.5 mm per A121 step */

#ifndef A121_ZOOM_POINTS
#define A121_ZOOM_POINTS 24  /* 48 = 120 mm (V2.3 step 3), prepared; choice after measurement V4.4 */
#endif
static constexpr uint16_t kZoomPoints   = A121_ZOOM_POINTS;  /* Subsweep 1: 24 points * 2.5 mm = 60 mm focus window */
static_assert(kZoomPoints >= 8 && kZoomPoints <= 64, "A121_ZOOM_POINTS: 8 .. 64");
/* Spectra only for the points with the strongest breathing when the window is wider than 24 points (compute time) */
static constexpr uint16_t kSpecPoints   = (kZoomPoints > 24U) ? 16U : kZoomPoints;
static constexpr uint16_t kRoomStep     = 12U;  /* RSS: step_length must be a divisor or multiple of 24 */
static constexpr uint16_t kRoomPoints   = 80U;  /* Subsweep 2: 80 points * 30 mm = 2.40 m room window */
static constexpr uint16_t kTotal3SubPts = 1U + kZoomPoints + kRoomPoints; /* 105 points = 420 bytes/frame */

static constexpr float    kFsHz        = 60.0f;  /* frame rate of the iq3 session; filter coefficients only apply to this */
#ifndef A121_HR_WIN
#define A121_HR_WIN 900  /* pulse window in frames; 600 / 720 for the comparison 10 / 12 / 15 s on day A (V2.5) */
#endif
static constexpr uint16_t kHrWin       = A121_HR_WIN;  /* 15 s pulse window */
static constexpr uint16_t kSnapMaxLag  = 180U;   /* pushFrame may keep writing for 3 s during a report (V2.1) */
static constexpr uint16_t kHistoryLen  = kHrWin + kSnapMaxLag;  /* 18 s @ 60 Hz (ReportSnapshot) */
static constexpr uint16_t kHrMinWin    = 360U;   /* a pulse is reported from 6 s of data, window grows to 15 s (V2.4; previously 8 s) */
static constexpr uint16_t kHrStableWin = 480U;   /* tracking only from an 8 s window; preliminary before that (V2.4) */
static_assert(kHrWin >= kHrStableWin && kHrWin % 60U == 0, "A121_HR_WIN: ganze Sekunden, mindestens 8 s");
static constexpr uint16_t kStartZoomFrames = 120U;  /* start: zoom to the target after 2 s (2 reports instead of 3, V2.4) */
static constexpr uint8_t  kDecim       = 4U;     /* 60 Hz → 15 Hz for the spectral search */
/* Spectra (V2.2, B13): real FFT of the sequence decimated to 15 Hz (≤ 225 values, Hann, zero-padded to 1024) instead of
 * Goertzel per frequency; grid 15 Hz / 1024 = 0.0146 Hz (0.88 /min), peaks parabolically refined. Previously pulse on a
 * 0.01 Hz grid without refinement, breathing on a 0.005 Hz grid. The limits in Hz stay as before. */
static constexpr uint16_t kFftN        = 1024U;
static constexpr float    kFftDf       = kFsHz / (float)kDecim / (float)kFftN;
static constexpr uint16_t kHrK0        = 44U;    /* first bin of the pulse spectrum (0.645 Hz) */
static constexpr float    kHrFMin      = (float)kHrK0 * kFftDf;
static constexpr float    kHrFStep     = kFftDf;
static constexpr uint16_t kHrNumF      = 164U;   /* bins 44 .. 207 (0.645 .. 3.032 Hz); searched 46 .. 205 = 40.4–180.2 /min */
static constexpr float    kHarm2W      = 0.5f;   /* harmonic sum S(f) = P(f) + 0.5 · P(2f) (2nd heart harmonic) */
static constexpr float    kHr2MaxHz    = 5.0f;   /* 2f above the bandpass limit does not count (V1.2) */
static constexpr uint16_t kHr2MaxBin   = (uint16_t)(kHr2MaxHz / kFftDf);  /* 341 = 4.995 Hz */
static constexpr uint16_t kBrK0        = 7U;     /* breathing spectrum over all zoom points: bins 7 .. 40 = 0.103 .. 0.586 Hz */
static constexpr float    kBrFMin      = (float)kBrK0 * kFftDf;
static constexpr float    kBrFStep     = kFftDf;
static constexpr uint16_t kBrNumF      = 34U;
static constexpr uint16_t kBrEdgeBins  = 1U;     /* breathing peak not at the band edge (often 6 /min before): 0.117 .. 0.571 Hz */
static constexpr float    kBrMinQ      = 2.0f;   /* breathing peak/median: below this no harmonic check */
static constexpr float    kHarmTolHz   = 0.04f;  /* pulse candidate counts as a breathing harmonic if |f − m·f_breath| ≤ 0.04 Hz */
static constexpr uint8_t  kHarmMMax    = 8U;     /* harmonics checked m = 2 .. 8 (V1.3) */
static constexpr float    kHarmPenalty = 0.5f;   /* candidate on a breathing harmonic m ≥ 4: S × 0.5 in the candidate selection */
static constexpr float    kHarmPenLow  = 0.02f;  /* m = 2, 3: S × 0.02 (practically blocked: test bench 4–12 × pulse, × 0.1 was not enough) */
static constexpr uint8_t  kHarmLowMax  = 3U;
static constexpr float    kLowLobeHz   = 0.1f;   /* m = 2, 3: tolerance ±0.1 Hz instead of ±0.04 (main lobe, Hann 15 s: ±0.13 Hz) */
static constexpr float    kSubRel      = 0.5f;   /* octave check: peak at f/2 with value ≥ 0.5 · value(f) counts as the fundamental */
static constexpr float    kNearMinRel  = 0.5f;   /* near candidate (±12 /min) wins only with S ≥ 0.5 · S_max */
static constexpr uint8_t  kAltReports  = 3U;     /* switch away from a harmonic after 3 reports with an alternative */
static constexpr float    kMaxMotionPct = 30.0f; /* below 70 % clean 1 s blocks the report is invalid */
static constexpr float    kPulseHeldDb = 8.0f;   /* breath held: pulse peak ≥ 8 dB above the median */
static constexpr float    kEchoSpreadMax = 0.4f; /* clean echo: distance to the circle center scatters < 40 % (noise ≈ 52 %) */
static constexpr float    kBreathMinUm = 100.0f; /* sign of life: breathing excursion ≥ 100 µm peak-to-peak at the breathing point */
static constexpr uint8_t  kHoldHarm    = 10U;    /* jump onto a harmonic only after 10 reports (otherwise 3) */
static constexpr float    kKeepTrackS  = 30.0f;  /* rezoom: tracking stays if the last valid value is < 30 s old */
/* Breathing state (V1.4, B9): amplitude per report = peak-to-peak of the 1 s mean over the last 10 s at the breathing point
 * (trend removed), reference = median of the last 60 values. A 1 s RMS (plan) depends on the phase at a 4–10 s breathing
 * period; 10 s contain a whole period down to 6 /min (5 s lost up to half of it at 6 /min). */
static constexpr float    kBrAmpWinS   = 10.0f;
static constexpr uint8_t  kBrRingLen   = 60U;
static constexpr uint8_t  kBrMinRing   = 10U;    /* median only from 10 values */
static constexpr float    kApneaRel    = 0.10f;  /* held: ≥ 90 % below the median */
static constexpr float    kShallowRel  = 0.70f;  /* shallow: ≥ 30 % below */
static constexpr uint8_t  kBrLowReports = 1U;    /* "for ≥ 10 s": one low value, the window is already 10 s long */
static constexpr float    kBrMotionPct = 50.0f;  /* state motion from motion_pct > 50 */
/* Without a clean echo in this many reports in a row the person is gone: forget the reference, otherwise the first
 * noise report with "echo" against the old breathing depth turns into apnea (final review C1, test bench gone) */
static constexpr uint8_t  kBrEchoLostReports = 3U;
static constexpr uint32_t kTickGapMs   = 25U;    /* tick distance > 25 ms counts as a gap (nominal 16.7 ms; XM125: 1 tick = 1 ms) */
static constexpr float    kSatMaxPct   = 1.0f;   /* from 1 % saturated frames in the window: confidence × 0.5 (V1.5) */
static constexpr uint8_t  kBrStateNormal = 0U, kBrStateNoPeak = 1U, kBrStateApnea = 2U, kBrStateMotion = 3U,
                          kBrStateShallow = 4U;
/* state of the measurement (V3.5, line @STATE) */
static constexpr uint8_t  kStNoPerson = 0U, kStSearch = 1U, kStAcquire = 2U, kStValid = 3U, kStMotion = 4U,
                          kStApnea = 5U;
static constexpr uint16_t kFiltPad     = 27U;    /* scipy sosfiltfilt: padlen = 3 * (2 * sections + 1) */
static constexpr uint16_t kGateBlk     = 60U;    /* motion damping: 1 s blocks in the heart band */
static constexpr float    kGateK       = 1.25f;  /* limit = 1.25 × RMS of the quiet quarter of the blocks */
static constexpr float    kGateP       = 3.0f;   /* gain above it (limit/RMS)^3: motion practically drops out */
static constexpr float    kMotionK     = 3.0f;   /* for motion_pct: block > 3 × quiet quarter counts as motion */
static constexpr uint8_t  kMaxBlocks   = kHrWin / kGateBlk;
static constexpr uint16_t kHrEdgeBins  = 2U;     /* peaks in the outer 2 bins (0.03 Hz) do not count (leakage from the band edge) */
static constexpr uint8_t  kMaxRoomTargets = 8U;  /* reported room targets (the strongest) */
static constexpr float    kRoomDetK    = 2.0f;   /* room target: amplitude or motion ≥ 2 × noise level */
static constexpr float    kRoomMoveK   = 3.0f;   /* moving: motion ≥ 3 × motion noise level */
static constexpr float    kRoomSlowA   = 0.00554f; /* smoothing for the zoom target: 1 − exp(−1 / (60 Hz · 3 s)), τ ≈ 3 s */
static constexpr float    kArcMinRad   = 2.0f;   /* circle fit counts as poorly determined if the phase sweeps less than this */

/* 4th-order Butterworth bandpass 0.6–5.0 Hz at 60 Hz (V1.2: pulse 40–180 /min incl. 2nd harmonic; previously 0.8–3.0 Hz):
 * scipy.signal.butter(4, [0.6, 5.0], 'band', fs=60, output='sos') */
static constexpr float kBpSos[4][6] = {
    {0.0016470443743298322f, 0.0032940887486596643f, 0.0016470443743298322f, 1.0f, -1.3464578520633204f, 0.47760903309535657f},
    {1.0f, 2.0f, 1.0f, 1.0f, -1.5148095433061484f, 0.7378001887686901f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.8687365868947912f, 0.8744518329372326f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.9571506605508442f, 0.9612033138279775f},
};

/** One pass of four biquads (DF2T) in forward or backward direction, initial state = step response. */
inline void sosPassSos(float *e, uint32_t L, bool reverse, const float (*sos)[6]) {
    const float x0 = reverse ? e[L - 1U] : e[0];
    float z1[4], z2[4];
    float level = x0;
    for (uint8_t s = 0; s < 4; s++) {
        const float *c = sos[s];
        const float g = (c[0] + c[1] + c[2]) / (1.0f + c[4] + c[5]);  /* DC gain of the section */
        z1[s] = (g - c[0]) * level;
        z2[s] = (c[2] - c[5] * g) * level;
        level *= g;
    }
    for (uint32_t i = 0; i < L; i++) {
        const uint32_t n = reverse ? (L - 1U - i) : i;
        float v = e[n];
        for (uint8_t s = 0; s < 4; s++) {
            const float *c = sos[s];
            const float y = c[0] * v + z1[s];
            z1[s] = c[1] * v - c[4] * y + z2[s];
            z2[s] = c[2] * v - c[5] * y;
            v = y;
        }
        e[n] = v;
    }
}

/**
 * Zero-phase bandpass like scipy `sosfiltfilt` (odd extension by kFiltPad, initial states from the
 * step response) with four biquads sos. ext: N + 2 · kFiltPad values; result: ext[kFiltPad .. kFiltPad + N).
 */
inline void sosFiltFiltSos(const float *x, uint16_t N, float *ext, const float (*sos)[6]) {
    const uint16_t P = (N > kFiltPad) ? kFiltPad : (uint16_t)(N - 1U);
    float *e = ext + (kFiltPad - P);
    const uint32_t L = (uint32_t)N + 2U * P;
    for (uint16_t i = 0; i < P; i++) e[i] = 2.0f * x[0] - x[P - i];
    memcpy(e + P, x, N * sizeof(float));
    for (uint16_t j = 0; j < P; j++) e[P + N + j] = 2.0f * x[N - 1U] - x[N - 2U - j];
    sosPassSos(e, L, false, sos);
    sosPassSos(e, L, true, sos);
}

#ifdef A121_DSP_IBI_TEMPLATE
/* V3.6: wider heart band for the individual beats; scipy.signal.butter(4, [0.6, 8.0], 'band', fs=60, output='sos') */
static constexpr float kBp8Sos[4][6] = {
    {0.009764843672775374f, 0.019529687345550748f, 0.009764843672775374f, 1.0f, -0.9137923786305936f, 0.24411561287811703f},
    {1.0f, 2.0f, 1.0f, 1.0f, -1.0869497349419404f, 0.6029896520783747f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.8749786163052726f, 0.8797806780795341f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.95389950449864f, 0.9578914690655628f},
};
#endif

/**
 * atan2 as a polynomial (V2.2, B13): atan(a) for a = min/max ∈ [0, 1] after Abramowitz/Stegun 4.4.49 (error ≤ 1 · 10⁻⁵ rad),
 * then mirrored into the quadrant. 21,600 calls per report during unwrapping; atan2f costs several
 * times as much on the ESP32-S3. 10⁻⁵ rad ≈ 0.004 µm of displacement.
 */
static inline float fastAtan2(float y, float x) {
    const float ax = fabsf(x), ay = fabsf(y);
    const float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (!(mx > 0.0f)) return 0.0f;
    const float a = mn / mx, z = a * a;
    float r = a * (0.99997726f + z * (-0.33262347f + z * (0.19354346f + z * (-0.11643287f + z * (0.05265332f + z * -0.01172120f)))));
    if (ay > ax) r = 1.57079633f - r;
    if (x < 0.0f) r = 3.14159265f - r;
    return (y < 0.0f) ? -r : r;
}

/**
 * Remove the linear trend (least squares) in float (V2.2; previously double, which the ESP32-S3 computes in software):
 * Σt² = N(N² − 1)/12 in closed form, sums shifted by x[0] so that float suffices.
 */
inline void removeLinearTrend(float *x, uint16_t N) {
    if (N < 2) return;
    const float half = 0.5f * (float)(N - 1U), x0 = x[0];
    float sum_y = 0.0f, sum_ty = 0.0f;
    for (uint16_t n = 0; n < N; n++) {
        const float y = x[n] - x0;
        sum_y += y;
        sum_ty += ((float)n - half) * y;
    }
    const float sum_tt = (float)N * ((float)N * (float)N - 1.0f) / 12.0f;
    const float mean = x0 + sum_y / (float)N, slope = sum_ty / sum_tt;
    for (uint16_t n = 0; n < N; n++) x[n] -= mean + slope * ((float)n - half);
}

/** sel[i] = true for the k largest values of v[0 .. n) (on a tie the earlier index). */
inline void selectStrongest(const float *v, uint16_t n, uint16_t k, bool *sel) {
    for (uint16_t i = 0; i < n; i++) sel[i] = false;
    for (uint16_t j = 0; j < k && j < n; j++) {
        int16_t best = -1;
        for (uint16_t i = 0; i < n; i++)
            if (!sel[i] && (best < 0 || v[i] > v[best])) best = (int16_t)i;
        sel[best] = true;
    }
}

#if defined(A121_DSP_REACQ_PERIOD) && !defined(A121_DSP_REACQ)
#error "A121_DSP_REACQ_PERIOD gibt es nur zusammen mit A121_DSP_REACQ"
#endif
#ifdef A121_DSP_REACQ
/* V6: motion map */
static constexpr uint8_t kReacqMinS    = 6U;     /* shortest window (s) after motion on which a pulse is still searched */
static constexpr uint8_t kMotionVotes  = 2U;     /* this many of the strongest points must report a block as moving */
static constexpr uint8_t kMotionPoints = 3U;     /* this many of the strongest points vote */
static_assert(kMaxBlocks <= 32U, "MotionMap::mask hat 32 Bit: höchstens 32 Blöcke");
/* V6: memory */
static constexpr float    kMemS        = 60.0f;  /* the last secured pulse holds for this long (s); forgotten afterwards */
static constexpr float    kMemBandBpm  = 15.0f;  /* candidate counts as "near the memory" at |bpm − remembered| ≤ 15 /min */
static constexpr float    kMemMinConf  = 50.0f;  /* pulse is remembered only from this confidence (store condition, task 6) */
/* Ruling R15: A value reacquired via the memory (reacq = 2) replaces the breathing peak as the sign of life only
 * if it lies at most this far from the remembered pulse. Same tolerance as the confirmation by ACF/individual beats
 * (±5 /min), tighter than the error limit of the test bench (10 /min). The trigger was the motion artifact in the test bench long
 * (84.7 instead of 72.5 /min in the band ±kMemBandBpm); those reports are valid via the breathing peak, though, so the rule changes
 * nothing there (docs/EXPERIMENTS.md, V6). */
static constexpr float    kMemSubstBpm = 5.0f;

/** Motion per 1 s block of the pulse window. Bit k of mask = block k moving (block 0 = oldest). */
struct MotionMap {
    uint32_t mask;
    uint8_t  n_blocks;
    uint8_t  clean_tail_blocks;  /* blocks after the last moving one (all if none) */
};

/**
 * Motion map from the block RMS (rms: n_pts rows × nb columns, row by row). Per point the reference is the
 * quiet quarter of its blocks as in gateMotion; a block counts as moving if at least votes points have
 * rms > k × reference (votes is limited to n_pts, so a single point decides alone). As in gateMotion,
 * there is no motion statement below 3 blocks; more than kMaxBlocks blocks are cut off.
 */
inline MotionMap motionMapFromRms(const float *rms, uint8_t n_pts, uint8_t nb, float k = kMotionK,
                                  uint8_t votes = kMotionVotes) {
    MotionMap m = {0U, (uint8_t)std::min<uint8_t>(nb, kMaxBlocks), 0U};
    m.clean_tail_blocks = m.n_blocks;
    if (m.n_blocks < 3U || n_pts == 0U) return m;
    nb = m.n_blocks;
    votes = std::min(votes, n_pts);
    uint8_t cnt[kMaxBlocks] = {};
    for (uint8_t p = 0; p < n_pts; p++) {
        const float *r = rms + (size_t)p * nb;
        float tmp[kMaxBlocks];
        for (uint8_t b = 0; b < nb; b++) tmp[b] = r[b];
        std::nth_element(tmp, tmp + nb / 4, tmp + nb);
        const float limit = k * (tmp[nb / 4] + 1e-6f);
        for (uint8_t b = 0; b < nb; b++)
            if (r[b] > limit) cnt[b]++;
    }
    for (uint8_t b = 0; b < nb; b++) {
        if (cnt[b] < votes) continue;
        m.mask |= (uint32_t)1U << b;
        m.clean_tail_blocks = (uint8_t)(nb - 1U - b);
    }
    return m;
}

#ifdef A121_DSP_REACQ_PERIOD
/* V6, periodicity variant (experiment, see the header comment and docs/EXPERIMENTS.md) */
static constexpr float    kPeriodK     = 5.0f;   /* periodic: strongest peak ≥ 5 × mean of the remaining bins in the heart band */
static constexpr uint16_t kPeriodK0    = 46U;    /* heart band bins 46 .. 205 = 0.674 .. 3.003 Hz (like the pulse search) */
static constexpr uint16_t kPeriodK1    = 205U;
static constexpr uint16_t kPeriodSegS  = 3U;     /* segment around the block: 3 s */
static constexpr uint16_t kPeriodSegN  = (uint16_t)(kPeriodSegS * (uint16_t)kFsHz / kDecim);  /* 45 values at 15 Hz */

/**
 * Segment for segmentPeriodic: up to 3 s of the bandpass-filtered displacement signal h[0 .. N) (60 Hz) around the position center,
 * decimated to 15 Hz (every kDecim-th value) and Hann-weighted, into out (≥ kPeriodSegN values). The segment is shifted
 * at the edge instead of shortened (center stays inside); N below 3 s: whole signal. Returns: number of values.
 */
inline uint16_t periodSegment(const float *h, uint16_t N, uint16_t center, float *out) {
    const uint16_t len = std::min<uint16_t>(N, (uint16_t)(kPeriodSegS * (uint16_t)kFsHz));
    int32_t a = (int32_t)center - (int32_t)len / 2;
    a = std::max<int32_t>(0, std::min<int32_t>(a, (int32_t)N - (int32_t)len));
    const uint16_t n = (uint16_t)((len + kDecim - 1U) / kDecim);
    for (uint16_t m = 0; m < n; m++) {
        const float w = (n > 1U) ? 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)m / (float)(n - 1U))) : 1.0f;
        out[m] = h[(uint32_t)a + (uint32_t)m * kDecim] * w;
    }
    return n;
}

/**
 * Periodic in the heart band? x: n weighted values from periodSegment (15 Hz). Spectrum |X|² via fft (spec: ≥ kPeriodK1 + 1
 * values, work array); true if the strongest peak in bins kPeriodK0 .. kPeriodK1 is at least k × the mean of the
 * remaining bins in the band (after Vital-Radio, Adib et al. 2015). A segment without power is not periodic.
 * Deviation from the draft: the "remaining" bins are those outside the Hann main lobe of the peak (± 2 resolution cells
 * kFftN / n bins). With all remaining bins, even a pure sine in the 3 s segment reaches only peak/mean ≈ 4.7
 * (the main lobe is 1.3 Hz wide, the band 2.3 Hz), noise up to 7: k = 5 then separates nothing.
 */
inline bool segmentPeriodic(const float *x, uint16_t n, a121_fft::RealFft<kFftN> &fft, float *spec, float k = kPeriodK) {
    if (n < 4U) return false;
    fft.power(x, n, spec, kPeriodK1 + 1U);
    float peak = 0.0f;
    uint16_t pk = kPeriodK0;
    for (uint16_t b = kPeriodK0; b <= kPeriodK1; b++)
        if (spec[b] > peak) {
            peak = spec[b];
            pk = b;
        }
    const int32_t excl = (int32_t)(2U * kFftN / n);
    float sum = 0.0f;
    uint16_t cnt = 0;
    for (uint16_t b = kPeriodK0; b <= kPeriodK1; b++) {
        if (abs((int32_t)b - (int32_t)pk) <= excl) continue;
        sum += spec[b];
        cnt++;
    }
    if (cnt == 0U || sum <= 0.0f) return false;
    return peak >= k * (sum / (float)cnt);
}
#endif
#endif

#ifdef A121_DSP_PHASE_SLOPE
static constexpr uint8_t kPhaseSlopeWalk = 4U;  /* at most this many bins uphill from the start frequency to the peak */

/**
 * V6, fine-estimation variant via the phase slope (experiment, REJECTED, see the header comment and docs/EXPERIMENTS.md; after
 * Vital-Radio, Adib et al. 2015): frequency of a spectrum peak from the slope of the unwrapped phase, instead of from the
 * parabolic refinement of the peak.
 * x: m values (decimated to fs, windowed), zero-padded to NF (m ≤ NF). Forward FFT, only the bin
 * k0 = round(f0 / fs · NF) and its neighbors k0 ± 1 are kept (all others, including the negative frequencies, become 0),
 * inverse FFT: complex narrowband signal of the peak. Unwrap the phase of the first m values (resolve jumps beyond ±π),
 * least-squares line, slope in rad/value · fs / 2π → Hz.
 * re, im: NF values of work memory each (contents invalid afterwards). cfft: twiddle factors, only read.
 * Returns f0_hz unchanged if k0 lies at the edge (k0 < 1 or k0 + 1 ≥ NF/2), m < 4 or m > NF, fs ≤ 0, the signal
 * has no power (mean narrowband amplitude < 1e-9) or the result is not finite.
 */
template <uint16_t NF>
inline float phaseSlopeHz(const float *x, uint16_t m, float fs, float f0_hz, const a121_fft::ComplexFft<NF> &cfft,
                          float *re, float *im) {
    if (m < 4U || m > NF || !(fs > 0.0f) || !(f0_hz >= 0.0f)) return f0_hz;
    const int32_t k0 = (int32_t)lroundf(f0_hz / fs * (float)NF);
    if (k0 < 1 || k0 + 1 >= (int32_t)(NF / 2U)) return f0_hz;
    for (uint16_t n = 0; n < NF; n++) {
        re[n] = (n < m) ? x[n] : 0.0f;
        im[n] = 0.0f;
    }
    cfft.transform(re, im, false);
    /* Find the peak: from k0 uphill in magnitude, at most kPhaseSlopeWalk steps (the start can be a few bins next to
     * the peak, the coarse choice of the pulse search is a different spectrum than that of this signal) */
    int32_t kp = k0;
    for (uint8_t i = 0; i < kPhaseSlopeWalk; i++) {
        const auto pw = [&](int32_t k) { return re[k] * re[k] + im[k] * im[k]; };
        const float here = pw(kp);
        if (kp + 2 < (int32_t)(NF / 2U) && pw(kp + 1) > here && pw(kp + 1) >= pw(kp - 1)) kp++;
        else if (kp - 1 >= 1 && pw(kp - 1) > here) kp--;
        else break;
    }
    for (uint16_t k = 0; k < NF; k++)
        if (k + 1 < kp || k > kp + 1) {  /* except kp − 1 .. kp + 1 */
            re[k] = 0.0f;
            im[k] = 0.0f;
        }
    cfft.transform(re, im, true);
    /* Unwrap the phase (into re, after reading re[n], im[n]); amplitude sum for the silence check */
    float amp_sum = 0.0f, prev = 0.0f, cum = 0.0f;
    for (uint16_t n = 0; n < m; n++) {
        amp_sum += sqrtf(re[n] * re[n] + im[n] * im[n]);
        const float ph = fastAtan2(im[n], re[n]);
        if (n == 0) {
            cum = ph;
        } else {
            float d = ph - prev;
            if (d > (float)M_PI) d -= 2.0f * (float)M_PI;
            else if (d < -(float)M_PI) d += 2.0f * (float)M_PI;
            cum += d;
        }
        prev  = ph;
        re[n] = cum;
    }
    if (amp_sum < 1e-9f * (float)m) return f0_hz;
    /* Line through (n, phase) by least squares: slope = Σ(n − n̄)(φ − φ̄) / Σ(n − n̄)² */
    const float half = 0.5f * (float)(m - 1U);
    float mean = 0.0f;
    for (uint16_t n = 0; n < m; n++) mean += re[n];
    mean /= (float)m;
    float sxy = 0.0f, sxx = 0.0f;
    for (uint16_t n = 0; n < m; n++) {
        const float dn = (float)n - half;
        sxy += dn * (re[n] - mean);
        sxx += dn * dn;
    }
    const float f = sxy / sxx * fs / (2.0f * (float)M_PI);
    return std::isfinite(f) ? f : f0_hz;
}
#endif

/**
 * Circle center of the IQ values of a zoom point in the window: Kåsa fit in Bullock form (algebraic, one 2×2 system).
 * The phase of a point must be measured around the center of the circle that the breathing motion describes (fixed
 * echo in the same point), not around the mean: with a breathing excursion with phase excursion β, the mean lies
 * J0(β) × radius off and creates breathing harmonics in the pulse band (docs/VITAL_AUDIT.md, B2).
 * Sums in float over (u, v) = (z − mean) / s with s = RMS of the distance to the mean, so that float suffices
 * (the ESP32-S3 computes double in software).
 * ring: [kHistoryLen][I, Q] of this point, window start .. start + N (modulo kHistoryLen).
 * Returns false and (ci, cq) = mean if the system is poorly determined (det ≤ 10⁻⁶ · Suu · Svv, e.g. all
 * points on a straight line) or the center would lie more than 10 × s from the mean.
 */
inline bool circleCenter(const float *ring, uint16_t start, uint16_t N, float mean_i, float mean_q, float &ci, float &cq) {
    ci = mean_i;
    cq = mean_q;
    if (N < 3) return false;
    float s2 = 0.0f;
    for (uint16_t n = 0; n < N; n++) {
        const float *z = ring + (size_t)((start + n) % kHistoryLen) * 2U;
        const float du = z[0] - mean_i, dv = z[1] - mean_q;
        s2 += du * du + dv * dv;
    }
    const float s = sqrtf(s2 / (float)N);
    if (!(s > 1e-6f)) return false;
    const float inv = 1.0f / s;
    float suu = 0.0f, svv = 0.0f, suv = 0.0f, suuu = 0.0f, svvv = 0.0f, suvv = 0.0f, svuu = 0.0f;
    for (uint16_t n = 0; n < N; n++) {
        const float *z = ring + (size_t)((start + n) % kHistoryLen) * 2U;
        const float u = (z[0] - mean_i) * inv, v = (z[1] - mean_q) * inv;
        const float uu = u * u, vv = v * v;
        suu += uu;
        svv += vv;
        suv += u * v;
        suuu += uu * u;
        svvv += vv * v;
        suvv += u * vv;
        svuu += v * uu;
    }
    const float det = suu * svv - suv * suv;
    if (!(det > 1e-6f * suu * svv)) return false;
    const float a = 0.5f * (suuu + suvv), c = 0.5f * (svvv + svuu);
    const float uc = (a * svv - c * suv) / det, vc = (c * suu - a * suv) / det;
    if (!(uc * uc + vc * vc <= 100.0f)) return false;
    ci = mean_i + uc * s;
    cq = mean_q + vc * s;
#ifdef A121_DSP_CIRCLE_GN
    /* Variant for comparison on recordings (V1.1): 3 Gauss-Newton steps on the geometric distance, radius
     * eliminated. No difference from Kåsa in the test bench (ideal circle arcs), +8 % compute time. */
    for (int it = 0; it < 3; it++) {
        float rs = 0.0f;
        for (uint16_t n = 0; n < N; n++) {
            const float *z = ring + (size_t)((start + n) % kHistoryLen) * 2U;
            rs += sqrtf((z[0] - ci) * (z[0] - ci) + (z[1] - cq) * (z[1] - cq));
        }
        const float R = rs / (float)N;
        float a00 = 0.0f, a01 = 0.0f, a11 = 0.0f, b0 = 0.0f, b1 = 0.0f, ju = 0.0f, jv = 0.0f;
        for (uint16_t n = 0; n < N; n++) {
            const float *z = ring + (size_t)((start + n) % kHistoryLen) * 2U;
            const float dx = z[0] - ci, dy = z[1] - cq, d = sqrtf(dx * dx + dy * dy) + 1e-6f;
            const float ux = dx / d, uy = dy / d, r = d - R;
            ju += ux; jv += uy; a00 += ux * ux; a01 += ux * uy; a11 += uy * uy; b0 += ux * r; b1 += uy * r;
        }
        ju /= (float)N; jv /= (float)N;
        a00 -= (float)N * ju * ju; a01 -= (float)N * ju * jv; a11 -= (float)N * jv * jv;
        const float dt = a00 * a11 - a01 * a01;
        if (!(dt > 0.0f)) break;
        ci += (a11 * b0 - a01 * b1) / dt;
        cq += (a00 * b1 - a01 * b0) / dt;
    }
#endif
    return true;
}

/** Autocorrelation of the heart signal (40–180 /min, V1.2): largest true local maximum with r > 0.05, otherwise
 *  out_peak = 0. At the smallest lag only a peak counts whose left neighbor is included in the computation (previously a
 *  curve falling off there counted as a peak: the test bench reported 189.5 /min). */
inline void cardiacAcf(const float *hr, uint16_t N, float fs, float &out_bpm, float &out_peak) {
    float e0 = 1e-6f;
    for (uint16_t n = 0; n < N; n++) e0 += hr[n] * hr[n];
    const uint16_t min_lag = (uint16_t)fmaxf(8.0f, floorf(fs * (60.0f / 185.0f)));
    const uint16_t max_lag = (uint16_t)fminf((float)(N / 2U), ceilf(fs * (60.0f / 38.0f)));
    float best_r = -1.0f, r_prev = 0.0f, r_curr = 0.0f;
    uint16_t best_lag = 0;
    for (uint16_t lag = min_lag - 1U; lag <= max_lag + 1U; lag++) {
        float s = 0.0f;
        for (uint16_t n = 0; n + lag < N; n++) s += hr[n] * hr[n + lag];
        const float r_next = (s / e0) * ((float)N / (float)(N - lag));
        if (lag >= min_lag + 1U && r_curr > r_prev && r_curr > r_next && r_curr > best_r) {
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

/** Solve G·coef = r for the harmonic fits (G symmetric positive definite, only the lower half filled; G and r are
 *  overwritten, r = coef). Cholesky G = L·Lᵀ with a small regularization against nearly equal frequencies. false: G
 *  not positive definite. */
inline bool harmSolve(float (*G)[2U * kHarmMMax], float *r, uint8_t P) {
    float tr = 0.0f;
    for (uint8_t a = 0; a < P; a++) tr += G[a][a];
    for (uint8_t a = 0; a < P; a++) G[a][a] += 1e-6f * tr / (float)P;
    for (uint8_t a = 0; a < P; a++) {
        for (uint8_t b = 0; b <= a; b++) {
            float v = G[a][b];
            for (uint8_t k = 0; k < b; k++) v -= G[a][k] * G[b][k];
            if (a == b) {
                if (v <= 0.0f) return false;
                G[a][a] = sqrtf(v);
            } else {
                G[a][b] = v / G[b][b];
            }
        }
    }
    for (uint8_t a = 0; a < P; a++) {  /* L·y = r */
        float v = r[a];
        for (uint8_t k = 0; k < a; k++) v -= G[a][k] * r[k];
        r[a] = v / G[a][a];
    }
    for (int a = (int)P - 1; a >= 0; a--) {  /* Lᵀ·coef = y */
        float v = r[a];
        for (uint8_t k = (uint8_t)(a + 1); k < P; k++) v -= G[k][a] * r[k];
        r[a] = v / G[a][a];
    }
    return true;
}

/** Subtract breathing harmonics m · f_breath (m = 1..kHarmMMax, below fs/2) from x: joint least-squares fit of
 *  cos and sin of all m, Hann-weighted (otherwise a strong harmonic leaks into the neighbors and into the pulse). m with
 *  |m · f_breath − f_keep| < 0.1 Hz or |m · f_breath − 2 f_keep| < 0.1 Hz stay, that is where the pulse lies (as in V3.3).
 *  f_breath ≤ 0: do nothing. For ACF and individual beats on the wide 0.6–5 Hz band, in which the 3rd breathing harmonic
 *  otherwise masks the heartbeat (test bench: ACF in 3 % of the reports at the pulse). */
inline void removeBreathHarmonics(float *x, uint16_t N, float fs, float f_breath, float f_keep) {
    if (f_breath <= 0.0f || N < 2U) return;
    constexpr uint8_t kP = 2U * kHarmMMax;
    float cw[kHarmMMax], sw[kHarmMMax], c[kHarmMMax], sn[kHarmMMax];
    uint8_t K = 0;
    for (uint8_t m = 1; m <= kHarmMMax; m++) {
        const float f = (float)m * f_breath;
        if (f >= 0.5f * fs) break;
        if (fabsf(f - f_keep) < 0.1f || fabsf(f - 2.0f * f_keep) < 0.1f) continue;
        const float w = 2.0f * (float)M_PI * f / fs;
        cw[K] = cosf(w);
        sw[K] = sinf(w);
        K++;
    }
    if (K == 0) return;
    const uint8_t P = 2U * K;
    float G[kP][kP] = {}, r[kP] = {}, phi[kP];
    auto start = [&]() { for (uint8_t k = 0; k < K; k++) { c[k] = 1.0f; sn[k] = 0.0f; } };
    auto step  = [&]() {
        for (uint8_t k = 0; k < K; k++) {
            const float c2 = c[k] * cw[k] - sn[k] * sw[k];
            sn[k] = sn[k] * cw[k] + c[k] * sw[k];
            c[k]  = c2;
        }
    };
    start();
    for (uint16_t n = 0; n < N; n++) {
        const float wn = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n / (float)(N - 1U)));
        for (uint8_t k = 0; k < K; k++) { phi[2U * k] = c[k]; phi[2U * k + 1U] = sn[k]; }
        for (uint8_t a = 0; a < P; a++) {
            const float wp = wn * phi[a];
            r[a] += wp * x[n];
            for (uint8_t b = 0; b <= a; b++) G[a][b] += wp * phi[b];
        }
        step();
    }
    if (!harmSolve(G, r, P)) return;
    start();
    for (uint16_t n = 0; n < N; n++) {
        float v = 0.0f;
        for (uint8_t k = 0; k < K; k++) v += r[2U * k] * c[k] + r[2U * k + 1U] * sn[k];
        x[n] -= v;
        step();
    }
}

#ifdef A121_DSP_HARM_REMOVE
static constexpr uint16_t kGuideFft = 256U;  /* ≥ window length after decimation (15 s · 15 Hz = 225) */
static constexpr float    kGuideMinConf = 50.0f;  /* confidence of the previous report from which removal applies (%) */

/**
 * V3.3 (variant): remove breathing harmonics guided by the breathing phase (like project_out_respiration_harmonics() in the
 * PC scripts). Phase φ(n) = angle of the analytic signal of the breathing fundamental from disp: FFT, positive
 * frequencies only, flat in the band 0.5–1.5 × f_breath, cos² flanks down to 0.2 and 1.8 × f_breath, back. So φ follows a
 * rate that fluctuates within the window or differs from the previous report (computation at ±23 % rate fluctuation and a breathing shape with
 * 2nd/3rd harmonic: 0.05 rad RMS; a narrow band ±0.3 gave 0.12 rad, fixed mixing with f_breath and averaging
 * over one period ±0.5 rad). Then a joint Hann-weighted least-squares fit of cos(mφ),
 * sin(mφ) for m = 1..kHarmMMax to x and subtract. m stays if f_keep or 2 f_keep lies in m × [smallest, largest
 * breathing rate in the window] ± 0.1 Hz: the m-th harmonic scatters by m × the rate fluctuation, its basis would otherwise take
 * part of the pulse away (computation: 40 % at ±23 % fluctuation and m = 5 near the pulse). Harmonics that
 * mask the pulse cannot be removed this way, only those away from it. f_breath ≤ 0: do nothing.
 * work: work memory with 2 · kGuideFft values.
 */
inline void removeBreathHarmonicsGuided(float *x, const float *disp, uint16_t N, float fs, float f_breath,
                                        float f_keep, const a121_fft::ComplexFft<kGuideFft> &cfft, float *work) {
    if (f_breath <= 0.0f || N < 8U || N > kGuideFft) return;
    float *ur = work, *ui = work + kGuideFft;
    float mean = 0.0f;
    for (uint16_t n = 0; n < N; n++) mean += disp[n];
    mean /= (float)N;
    for (uint16_t n = 0; n < kGuideFft; n++) {
        ur[n] = (n < N) ? disp[n] - mean : 0.0f;
        ui[n] = 0.0f;
    }
    cfft.transform(ur, ui, false);
    for (uint16_t k = 0; k < kGuideFft; k++) {  /* analytic and breathing fundamental: weight 2 in the band, 0 otherwise */
        const float rel = (k < kGuideFft / 2U) ? fabsf((float)k * fs / (float)kGuideFft / f_breath - 1.0f) : 9.0f;
        float g = 0.0f;
        if (rel <= 0.5f) g = 2.0f;
        else if (rel < 0.8f) {
            const float c = cosf(0.5f * (float)M_PI * (rel - 0.5f) / 0.3f);
            g = 2.0f * c * c;
        }
        ur[k] *= g;
        ui[k] *= g;
    }
    cfft.transform(ur, ui, true);
    for (uint16_t n = 0; n < N; n++) {
        const float mag = sqrtf(ur[n] * ur[n] + ui[n] * ui[n]);
        if (mag > 0.0f) {
            ur[n] /= mag;
            ui[n] /= mag;
        } else {
            ur[n] = 1.0f;
            ui[n] = 0.0f;
        }
    }
    /* Span of the breathing rate in the window: phase steps averaged over 1 s, without the edges (there the phase of the
     * analytic signal is uncertain). The m-th harmonic scatters over m × this span. */
    float f_lo = f_breath, f_hi = f_breath;
    {
        const uint16_t W = (uint16_t)lroundf(fs);
        if (N > 3U * W) {
            float acc = 0.0f;
            for (uint16_t n = W + 1U; n < N - W; n++) {
                acc += atan2f(ui[n] * ur[n - 1U] - ur[n] * ui[n - 1U], ur[n] * ur[n - 1U] + ui[n] * ui[n - 1U]);
                if (n >= 2U * W) {
                    const uint16_t o = (uint16_t)(n - W);
                    acc -= atan2f(ui[o] * ur[o - 1U] - ur[o] * ui[o - 1U], ur[o] * ur[o - 1U] + ui[o] * ui[o - 1U]);
                }
                if (n + 1U >= 2U * W) {
                    const float f = acc / (float)W * fs / (2.0f * (float)M_PI);
                    f_lo = fminf(f_lo, f);
                    f_hi = fmaxf(f_hi, f);
                }
            }
        }
    }
    uint8_t ms[kHarmMMax], K = 0;
    for (uint8_t m = 1; m <= kHarmMMax; m++) {
        if ((float)m * f_breath >= 0.5f * fs) break;
        const float lo = (float)m * f_lo - 0.1f, hi = (float)m * f_hi + 0.1f;
        if ((f_keep >= lo && f_keep <= hi) || (2.0f * f_keep >= lo && 2.0f * f_keep <= hi)) continue;
        ms[K++] = m;
    }
    if (K == 0) return;
    /* Fit of Re/Im(u^m), m ∈ ms */
    constexpr uint8_t kP = 2U * kHarmMMax;
    const uint8_t P = 2U * K;
    float G[kP][kP] = {}, r[kP] = {}, phi[kP];
    auto basis = [&](uint16_t n) {
        float pr = 1.0f, pi = 0.0f;
        uint8_t k = 0;
        for (uint8_t m = 1; m <= ms[K - 1U]; m++) {
            const float r2 = pr * ur[n] - pi * ui[n];
            pi = pr * ui[n] + pi * ur[n];
            pr = r2;
            if (m == ms[k]) {
                phi[2U * k]      = pr;
                phi[2U * k + 1U] = pi;
                k++;
            }
        }
    };
    for (uint16_t n = 0; n < N; n++) {
        const float wn = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n / (float)(N - 1U)));
        basis(n);
        for (uint8_t a = 0; a < P; a++) {
            const float wp = wn * phi[a];
            r[a] += wp * x[n];
            for (uint8_t b = 0; b <= a; b++) G[a][b] += wp * phi[b];
        }
    }
    if (!harmSolve(G, r, P)) return;
    for (uint16_t n = 0; n < N; n++) {
        basis(n);
        float v = 0.0f;
        for (uint8_t a = 0; a < P; a++) v += r[a] * phi[a];
        x[n] -= v;
    }
}
#endif

/** Breathing amplitude (µm): peak-to-peak of the moving 1 s mean over the last kBrAmpWinS of x, after subtracting
 *  a line (posture drift while holding the breath). < 0: too little data. The 1 s mean damps the heartbeat (76 /min to
 *  ≈ 20 %), the breathing (15 /min) hardly. */
inline float breathAmplitude(const float *x, uint16_t N, float fs) {
    const uint16_t W = (uint16_t)(fs + 0.5f);
    uint16_t L = (uint16_t)(kBrAmpWinS * fs + 0.5f);
    if (L > N) L = N;
    if (L < W + 2U) return -1.0f;
    const float *s = x + (N - L);
    const uint16_t M = (uint16_t)(L - W + 1U);
    float sy = 0.0f, sjy = 0.0f, run = 0.0f;
    for (uint16_t n = 0; n < W; n++) run += s[n];
    for (uint16_t j = 0; j < M; j++) {
        if (j > 0) run += s[j + W - 1U] - s[j - 1U];
        const float y = run / (float)W;
        sy += y;
        sjy += (float)j * y;
    }
    const float jm = 0.5f * (float)(M - 1U), sjj = (float)M * ((float)M * (float)M - 1.0f) / 12.0f;
    const float slope = (sjj > 0.0f) ? (sjy - jm * sy) / sjj : 0.0f, icpt = sy / (float)M - slope * jm;
    float lo = 0.0f, hi = 0.0f;
    run = 0.0f;
    for (uint16_t n = 0; n < W; n++) run += s[n];
    for (uint16_t j = 0; j < M; j++) {
        if (j > 0) run += s[j + W - 1U] - s[j - 1U];
        const float r = run / (float)W - (icpt + slope * (float)j);
        if (j == 0 || r < lo) lo = r;
        if (j == 0 || r > hi) hi = r;
    }
    return hi - lo;
}

/** Breathing state per report (V1.4): 0 normal, 1 no breathing peak (or no clean echo, amp < 0), 2 held /
 *  apnea (amplitude < 10 % of the median of the last 60 values, kBrLowReports in a row), 3 motion (motion_pct > 50),
 *  4 shallow (< 70 %, same duration). During 2 and 3 the reference does not grow along: a long hold or movement
 *  would otherwise shift the median. After kBrEchoLostReports reports without a clean echo the reference no longer applies (person
 *  gone); it rebuilds afterwards. */
class BreathStateTracker {
public:
    void reset() {
        n_ = idx_ = apnea_cnt_ = shallow_cnt_ = no_echo_ = 0;
        state_ = kBrStateNormal;
    }
    uint8_t update(float amp_um, bool breath_peak, float motion_pct) {
        uint8_t st;
        if (motion_pct > kBrMotionPct) {
            apnea_cnt_ = shallow_cnt_ = 0;
            st = kBrStateMotion;
        } else if (amp_um < 0.0f) {
            apnea_cnt_ = shallow_cnt_ = 0;
            no_echo_   = inc(no_echo_);
            if (no_echo_ >= kBrEchoLostReports) n_ = idx_ = 0;
            st = kBrStateNoPeak;
        } else {
            no_echo_ = 0;
            const float med = (n_ >= kBrMinRing) ? median() : -1.0f;
            apnea_cnt_   = (med > 0.0f && amp_um < kApneaRel * med) ? inc(apnea_cnt_) : 0;
            shallow_cnt_ = (med > 0.0f && amp_um < kShallowRel * med) ? inc(shallow_cnt_) : 0;
            if (apnea_cnt_ >= kBrLowReports) st = kBrStateApnea;
            else if (!breath_peak) st = kBrStateNoPeak;
            else if (shallow_cnt_ >= kBrLowReports) st = kBrStateShallow;
            else st = kBrStateNormal;
            if (st != kBrStateApnea) {
                ring_[idx_] = amp_um;
                idx_ = (uint8_t)((idx_ + 1U) % kBrRingLen);
                if (n_ < kBrRingLen) n_++;
            }
        }
        state_ = st;
        return st;
    }
    uint8_t state() const { return state_; }
    float median() const {
        if (n_ == 0) return -1.0f;
        float tmp[kBrRingLen];
        memcpy(tmp, ring_, n_ * sizeof(float));
        std::nth_element(tmp, tmp + n_ / 2, tmp + n_);
        return tmp[n_ / 2];
    }

private:
    static uint8_t inc(uint8_t c) { return c < 255U ? (uint8_t)(c + 1U) : c; }
    float   ring_[kBrRingLen] = {};
    uint8_t n_ = 0, idx_ = 0, apnea_cnt_ = 0, shallow_cnt_ = 0, no_echo_ = 0, state_ = kBrStateNormal;
};

#ifdef A121_DSP_REACQ
/** V6: short-term memory of the pulse. What is remembered: pulse (/min) and beat interval (ms) together with the frame counter of the last
 *  secured measurement. It holds for kMemS seconds (difference of the uint32 counters, an overflow does no harm) and
 *  expires as soon as the echo is missing or the target leaves the zoom zone (then it is probably another posture/person). */
class ReacqMemory {
public:
    void clear() { hr_ = ibi_ms_ = 0.0f; stored_frames_ = 0U; valid_ = false; }
    void store(float hr_bpm, float ibi_ms, uint32_t tot_frames) {
        hr_ = hr_bpm;
        ibi_ms_ = ibi_ms;
        stored_frames_ = tot_frames;
        valid_ = true;
    }
    /** true while a value is remembered and valid; an expired or invalidated memory is cleared in the process. */
    bool active(uint32_t tot_frames, bool echo_ok, bool target_in_zoom) {
        if (valid_ && (!echo_ok || !target_in_zoom || (uint32_t)(tot_frames - stored_frames_) >= kMemFrames)) clear();
        return valid_;
    }
    float hr() const { return hr_; }
    float ibi_ms() const { return ibi_ms_; }  /* kept (spec 4: mem_ibi_ms_), not yet read by the DSP; for
                                                 a later check of beat-interval continuity (tests read it) */
    bool stored() const { return valid_; }  /* remembered, without checking expiry and conditions (tests) */
    uint32_t storedAt() const { return stored_frames_; }  /* frame counter at the last store() (tests) */

private:
    static constexpr uint32_t kMemFrames = (uint32_t)(kMemS * kFsHz);
    float    hr_ = 0.0f, ibi_ms_ = 0.0f;
    uint32_t stored_frames_ = 0U;
    bool     valid_ = false;
};
#endif

/** Result of the individual beats (V1.6). */
struct IbiResult {
    float   bpm      = 0.0f;  /* 60000 / median of the valid IBI; 0 = no result (< 3 valid IBI) */
    float   ibi_ms   = 0.0f;
    float   rmssd_ms = 0.0f;  /* 0 = not computed (< 90 % of the expected IBI valid) */
    uint8_t n_beats  = 0;
    uint8_t n_valid  = 0;     /* valid IBI */
};

static constexpr uint8_t kIbiMaxPk = 48;  /* at most this many peaks per window (15 s × 180 /min = 45) */

/** Peaks above 0.35 × RMS with a refractory time of 0.6 × 60 / hr_bpm (hr_bpm already limited to 40–180). */
inline uint8_t findBeatPeaks(const float *hr, uint16_t N, float fs, float hr_bpm, uint16_t *pk) {
    float rms = 0.0f;
    for (uint16_t n = 0; n < N; n++) rms += hr[n] * hr[n];
    rms = sqrtf(rms / (float)(N ? N : 1U));
    const float thresh = 0.35f * rms;
    const uint16_t refractory = (uint16_t)roundf(0.6f * (60.0f / hr_bpm) * fs);
    uint8_t n_pk = 0;
    for (uint16_t n = 2; n + 2 < N; n++) {
        if (hr[n] > thresh && hr[n] > hr[n - 1] && hr[n] >= hr[n + 1] && hr[n] > hr[n - 2] && hr[n] >= hr[n + 2]) {
            if (n_pk == 0 || (uint16_t)(n - pk[n_pk - 1]) >= refractory) {
                pk[n_pk++] = n;
                if (n_pk >= kIbiMaxPk) break;
            } else if (hr[n] > hr[pk[n_pk - 1]]) {
                pk[n_pk - 1] = n;
            }
        }
    }
    return n_pk;
}

/** Position of the peaks parabolically between the samples (in samples). */
inline void refinePeaks(const float *hr, const uint16_t *pk, uint8_t n_pk, float *t) {
    for (uint8_t k = 0; k < n_pk; k++) {
        const uint16_t i = pk[k];
        const float a = hr[i - 1], b = hr[i], c = hr[i + 1], den = a - 2.0f * b + c;
        float d = (den < 0.0f) ? 0.5f * (a - c) / den : 0.0f;
        d = d < -0.5f ? -0.5f : (d > 0.5f ? 0.5f : d);
        t[k] = (float)i + d;
    }
}

/** IBI, artifact rules and RMSSD from beat times t (samples, V1.6). */
inline IbiResult ibiFromTimes(const float *t, uint8_t n_pk, uint16_t N, float fs, float hr_bpm) {
    IbiResult res;
    res.n_beats = n_pk;
    if (n_pk < 4) return res;
    const uint8_t n_ibi = (uint8_t)(n_pk - 1U);
    float ibi[kIbiMaxPk], tmp[kIbiMaxPk];
    bool  in_range[kIbiMaxPk], valid[kIbiMaxPk];
    uint8_t n_in = 0;
    for (uint8_t k = 0; k < n_ibi; k++) {
        ibi[k] = (t[k + 1] - t[k]) * 1000.0f / fs;
        in_range[k] = ibi[k] >= 60000.0f / 180.0f && ibi[k] <= 60000.0f / 40.0f;
        if (in_range[k]) tmp[n_in++] = ibi[k];
    }
    if (n_in < 3) return res;
    auto median = [](float *v, uint8_t n) {
        std::nth_element(v, v + n / 2, v + n);
        return v[n / 2];
    };
    const float win_med = median(tmp, n_in);
    float prev[5];
    uint8_t n_prev = 0, n_valid = 0;
    for (uint8_t k = 0; k < n_ibi; k++) {
        float ref = win_med;
        if (n_prev >= 3) {
            float p5[5];
            memcpy(p5, prev, n_prev * sizeof(float));
            ref = median(p5, n_prev);
        }
        valid[k] = in_range[k] && fabsf(ibi[k] - ref) <= 0.2f * ref;
        if (valid[k]) tmp[n_valid++] = ibi[k];
        if (in_range[k]) {  /* the 5 previous IBI in range (sliding) */
            if (n_prev < 5) prev[n_prev++] = ibi[k];
            else { memmove(prev, prev + 1, 4 * sizeof(float)); prev[4] = ibi[k]; }
        }
    }
    res.n_valid = n_valid;
    if (n_valid < 3) return res;
    res.ibi_ms = median(tmp, n_valid);
    res.bpm    = 60000.0f / res.ibi_ms;

    const float expected = (float)N / fs * hr_bpm / 60.0f - 1.0f;
    if ((float)n_valid >= 0.9f * expected) {
        float ss = 0.0f;
        uint8_t nd = 0;
        for (uint8_t k = 1; k < n_ibi; k++)
            if (valid[k] && valid[k - 1]) {
                const float dd = ibi[k] - ibi[k - 1];
                ss += dd * dd;
                nd++;
            }
        res.rmssd_ms = nd > 0 ? sqrtf(ss / (float)nd) : 0.0f;
    }
    return res;
}

/**
 * Individual beats in the heart signal (V1.6, B7): peaks above 0.35 × RMS, refractory time 0.6 × 60 / hr_bpm (previously fixed
 * 360 ms), position parabolically refined between the samples (previously quantized to 16.7 ms). An IBI counts if it lies
 * in the range 40–180 /min and deviates at most 20 % from the median of the 5 previous IBI (at the start of the window: median of
 * all IBI in range). RMSSD only from directly consecutive valid IBI and only if ≥ 90 % of the expected
 * IBI are valid (artifact rules of the HRV literature, VITAL_AUDIT.md 5.1); 15 s is short for HRV anyway.
 * t_out (optional, kIbiMaxPk values): beat times in samples, n_beats valid.
 */
inline IbiResult timeDomainIbi(const float *hr, uint16_t N, float fs, float hr_bpm, float *t_out = nullptr) {
    hr_bpm = hr_bpm < 40.0f ? 40.0f : (hr_bpm > 180.0f ? 180.0f : hr_bpm);
    uint16_t pk[kIbiMaxPk];
    const uint8_t n_pk = findBeatPeaks(hr, N, fs, hr_bpm, pk);
    float t[kIbiMaxPk];
    refinePeaks(hr, pk, n_pk, t);
    if (t_out) memcpy(t_out, t, n_pk * sizeof(float));
    return ibiFromTimes(t, n_pk, N, fs, hr_bpm);
}

#ifdef A121_DSP_IBI_TEMPLATE
static constexpr float   kTplHalf = 0.3f;  /* template ±0.3 × beat interval around the peak */
static constexpr uint8_t kTplMaxW = 28U;   /* 0.3 × 1.5 s × 60 Hz = 27 samples at 40 /min */

/**
 * Individual beats with a template (V3.6): first peaks as in V1.6, template = mean of the segments ±kTplHalf × beat interval
 * around them (without DC), matched filter c[n] = Σ_j v[j] · x[n + j], peaks of c searched as in V1.6 and
 * parabolically refined, then the same artifact rules. x: heart signal (V3.6: band 0.6–8 Hz), work: N values. With
 * fewer than 3 complete segments: like timeDomainIbi.
 */
inline IbiResult templateIbi(const float *x, uint16_t N, float fs, float hr_bpm, float *work, float *t_out = nullptr) {
    hr_bpm = hr_bpm < 40.0f ? 40.0f : (hr_bpm > 180.0f ? 180.0f : hr_bpm);
    uint16_t pk[kIbiMaxPk];
    const uint8_t n_pk = findBeatPeaks(x, N, fs, hr_bpm, pk);
    uint16_t W = (uint16_t)roundf(kTplHalf * (60.0f / hr_bpm) * fs);
    if (W > kTplMaxW) W = kTplMaxW;
    float tpl[2U * kTplMaxW + 1U] = {};
    uint8_t n_seg = 0;
    for (uint8_t k = 0; k < n_pk; k++) {
        if (pk[k] < W || (uint32_t)pk[k] + W >= N) continue;
        for (int16_t j = -(int16_t)W; j <= (int16_t)W; j++) tpl[j + W] += x[pk[k] + j];
        n_seg++;
    }
    if (n_seg < 3 || W < 2U) return timeDomainIbi(x, N, fs, hr_bpm, t_out);
    const uint16_t L = (uint16_t)(2U * W + 1U);
    float m = 0.0f;
    for (uint16_t j = 0; j < L; j++) m += tpl[j];
    m /= (float)L;
    for (uint16_t j = 0; j < L; j++) tpl[j] = (tpl[j] - m) / (float)n_seg;
    for (uint16_t n = 0; n < N; n++) {
        float c = 0.0f;
        if (n >= W && (uint32_t)n + W < N)
            for (uint16_t j = 0; j < L; j++) c += tpl[j] * x[n - W + j];
        work[n] = c;
    }
    uint16_t pk2[kIbiMaxPk];
    const uint8_t n2 = findBeatPeaks(work, N, fs, hr_bpm, pk2);
    float t[kIbiMaxPk];
    refinePeaks(work, pk2, n2, t);
    if (t_out) memcpy(t_out, t, n2 * sizeof(float));
    return ibiFromTimes(t, n2, N, fs, hr_bpm);
}
#endif

#define A121_DSP_FRAME_FLAGS 1  /* pushFrame takes FrameFlags (for host/dsp_replay with older headers) */

/** Sensor flags of a frame from the server header (V1.5, B15, B16). */
struct FrameFlags {
    uint32_t tick      = 0;
    bool     have_tick = false;
    bool     saturated = false;  /* data_saturated */
    bool     delayed   = false;  /* frame_delayed: the server did not deliver frames in time */
};

struct Complex16 {
    int16_t real;
    int16_t imag;
};

struct RoomTarget {
    float dist_m;          /* parabolically refined between the 30 mm points */
    float amplitude;       /* smoothed echo amplitude (raw value) */
    float motion_energy;   /* smoothed deviation from the static image (raw value; pure noise ≈ noise amplitude) */
    bool  moving;          /* motion_energy ≥ kRoomMoveK × motion noise level */
};

/** Inputs of the confidence (V1.3). hr_fused_5s_ago ≤ 0 = unknown (no jump penalty). */
struct QualityInputs {
    float peak_db;          /* S at the tracked frequency above the median of the combined spectrum */
    bool  on_harmonic;      /* tracked frequency lies on a breathing harmonic */
    float hr_spec, hr_acf, hr_ibi, hr_fused, hr_fused_5s_ago;
    float motion_pct;       /* share of 1 s blocks with motion in the window */
    float sat_pct = 0.0f;   /* share of saturated frames in the window (V1.5) */
    float win_frac = 1.0f;  /* window length / 15 s while the window is still growing (V2.4) */
};

/**
 * Confidence 0–100 % from independent cues instead of the agreement of the 24 zoom points (they see almost the same
 * signal, B4): 100 × clamp((peak_db − 3) / 12) × 0.5 on a harmonic × 1 / 0.7 / 0.5 with 3 / 2 / ≤ 1 estimators
 * (spectrum, autocorrelation, individual beats) within ±5 /min of the result × (1 − motion share) × 0.7 for a
 * jump > 8 /min in 5 s × 0.5 from 1 % saturated frames (V1.5). Factors preliminary (test bench); tune on day A,
 * then freeze.
 */
inline float signalQuality(const QualityInputs &q) {
    float c = (q.peak_db - 3.0f) / 12.0f;
    c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
    if (q.on_harmonic) c *= 0.5f;
    const int agree = (fabsf(q.hr_spec - q.hr_fused) <= 5.0f) + (fabsf(q.hr_acf - q.hr_fused) <= 5.0f) +
                      (fabsf(q.hr_ibi - q.hr_fused) <= 5.0f);
    c *= (agree >= 3) ? 1.0f : (agree == 2) ? 0.7f : 0.5f;
    const float mot = q.motion_pct < 0.0f ? 0.0f : (q.motion_pct > 100.0f ? 100.0f : q.motion_pct);
    c *= 1.0f - mot / 100.0f;
    if (q.hr_fused_5s_ago > 0.0f && fabsf(q.hr_fused - q.hr_fused_5s_ago) > 8.0f) c *= 0.7f;
    if (q.sat_pct >= kSatMaxPct) c *= 0.5f;
    c *= std::max(0.0f, std::min(1.0f, q.win_frac));
    return 100.0f * c;
}

/**
 * State of the measurement (V3.5) from room profile (moving target = person in the room), zoom echo, breathing, validity,
 * motion share and breathing state. Priority: BEWEGUNG (motion; only with a clean echo in the zoom: without an echo the
 * motion share is noise) > APNOE (apnea) > GÜLTIG (valid) > ERFASSUNG (acquiring: clean echo in the zoom with breathing or
 * a person in the room, value not yet valid) > SUCHE (search: person in the room, no clean echo in the zoom) > KEINE_PERSON.
 */
inline uint8_t measurementState(bool person, bool echo_ok, bool breath, bool valid, float motion_pct, uint8_t br_state) {
    if (echo_ok && (breath || person) && (motion_pct > kMaxMotionPct || br_state == kBrStateMotion)) return kStMotion;
    if (br_state == kBrStateApnea) return kStApnea;
    if (valid) return kStValid;
    if (echo_ok && (breath || person)) return kStAcquire;
    return person ? kStSearch : kStNoPerson;
}

/** Gate for @BREATH (V3.4): only in ACQUIRING, VALID and APNEA. Without a person the path of the telemetry beam is
 *  noise (test bench empty/nobody: 19–22 "breaths" in 120 s), in MOTION body movement. */
inline bool breathGateOpen(uint8_t state) { return state == kStAcquire || state == kStValid || state == kStApnea; }

#define A121_DSP_BREATH 1  /* breath detection and @BREATH (V3.4; for host/dsp_replay with older headers) */

/** A detected breath (V3.4): peak of the filtered path, distance to the previous peak in the band 4–42 /min. */
struct BreathEvent {
    uint32_t frame       = 0;     /* frames since reset() at detection (peak + filter delay + hysteresis) */
    uint32_t interval_ms = 0;     /* distance to the previous peak */
    float    depth_um    = 0.0f;  /* peak − previous trough */
    float    rate_cpm    = 0.0f;  /* 60 / median of the last (up to) kBrRateN distances */
};

static constexpr uint32_t kBrMinIntervalFr = 86U;   /* 42 /min = 1.43 s */
static constexpr uint32_t kBrMaxIntervalFr = 900U;  /* 4 /min = 15 s */
static constexpr uint8_t  kBrRateN         = 4U;
static constexpr float    kBrHystFrac      = 0.3f;    /* hysteresis = 30 % of the peak-to-peak span of the last 8 s */
static constexpr float    kBrHystMinUm     = 200.0f;  /* below this pulse residual ripple (pulse 100 µm → ≈ 40 µm) and
                                                      * small waves while holding the breath (test bench 144–157 µm); real
                                                      * breathing excursion seated 2.3–3.4 mm (M2, M3) */
static constexpr uint8_t  kBrSpanBlocks    = 8U;      /* span from 8 blocks of 1 s */
static_assert(kFsHz == 60.0f, "Bandpass-Koeffizienten der Atemzug-Erkennung gelten für 60 Hz");

/**
 * Breathing breath by breath (V3.4) on the path of the telemetry beam (µm per frame): bandpass 0.05–0.8 Hz from a
 * 1st-order highpass (a 2nd-order highpass overshoots after a breath stop and produced a false breath;
 * price: depth at 6 /min ≈ 11 % too small) and a 2nd-order Butterworth lowpass, in double (the poles lie close to
 * z = 1), then peaks and troughs with hysteresis. The hysteresis
 * follows the span (max − min) of the last 8 s, so that shallow breaths get through again after deep ones; limited
 * from below to kBrHystMinUm against the residual ripple of the pulse. One event per peak with the distance to the previous
 * peak if it lies in the band 4–42 /min; at shorter distances the new peak replaces the old one as the reference (otherwise
 * every second peak of 45 /min would be counted as 22 /min), longer ones restart the rate, and only the peak after
 * next becomes the reference.
 */
class BreathDetector {
public:
    void reset() { *this = BreathDetector(); }

    /** Feed one value; true if a breath is detected with it (ev filled). */
    bool push(float disp_um, BreathEvent &ev) {
        static constexpr double kSos[2][5] = {  /* b0, b1, b2, a1, a2; scipy butter(1, 0.05, 'high') and butter(2, 0.8) */
            {0.9973888361673892, -0.9973888361673892, 0.0, -0.9947776723347783, 0.0},
            {0.0016556093437783469, 0.0033112186875566938, 0.0016556093437783469, -1.8816500461793153,
             0.8882724835544287}};
        double v = (double)disp_um;
        for (int i = 0; i < 2; i++) {  /* transposed direct form II */
            const double y = kSos[i][0] * v + s_[i][0];
            s_[i][0] = kSos[i][1] * v - kSos[i][3] * y + s_[i][1];
            s_[i][1] = kSos[i][2] * v - kSos[i][4] * y;
            v = y;
        }
        const float y = (float)v;
        const uint32_t n = n_++;

        /* span of the last kBrSpanBlocks seconds (current block included) */
        if (n % 60U == 0U) {
            blk_ = (uint8_t)((blk_ + 1U) % kBrSpanBlocks);
            bmax_[blk_] = bmin_[blk_] = y;
            if (nblk_ < kBrSpanBlocks) nblk_++;
        }
        if (y > bmax_[blk_]) bmax_[blk_] = y;
        if (y < bmin_[blk_]) bmin_[blk_] = y;
        float hi = bmax_[blk_], lo = bmin_[blk_];
        for (uint8_t b = 0; b < nblk_; b++) {
            if (bmax_[b] > hi) hi = bmax_[b];
            if (bmin_[b] < lo) lo = bmin_[b];
        }
        const float h = fmaxf(kBrHystMinUm, kBrHystFrac * (hi - lo));

        bool got = false;
        if (dir_ == 0) {  /* start: only fix the direction; the start value is neither a peak nor a trough */
            if (n == 0 || y > ext_hi_) ext_hi_ = y;
            if (n == 0 || y < ext_lo_) ext_lo_ = y;
            if (y < ext_hi_ - h) {
                dir_ = -1; ext_ = y; ext_n_ = n;
            } else if (y > ext_lo_ + h) {
                dir_ = 1; ext_ = y; ext_n_ = n;
            }
        } else if (dir_ > 0) {  /* search for a peak */
            if (y > ext_) { ext_ = y; ext_n_ = n; }
            else if (y < ext_ - h) {
                got = onPeak(ext_, ext_n_, n, ev);
                dir_ = -1; ext_ = y; ext_n_ = n;
            }
        } else {  /* search for a trough */
            if (y < ext_) { ext_ = y; ext_n_ = n; }
            else if (y > ext_ + h) {
                trough_ = ext_; have_trough_ = true;
                dir_ = 1; ext_ = y; ext_n_ = n;
            }
        }
        return got;
    }

private:
    bool onPeak(float v, uint32_t at, uint32_t now, BreathEvent &ev) {
        if (!have_peak_) {
            have_peak_ = true;
            peak_n_    = at;
            return false;
        }
        const uint32_t d = at - peak_n_;
        peak_n_ = at;
        if (d < kBrMinIntervalFr) return false;  /* too short (> 42 /min): no event, new peak is the reference */
        if (d > kBrMaxIntervalFr || !have_trough_) {  /* pause or gap (< 4 /min): restart rate */
            niv_ = 0;
            /* the first "peak" after a pause is often just the resting position before the path falls again: only the
             * next peak becomes the reference */
            have_peak_ = !(d > kBrMaxIntervalFr);
            return false;
        }
        const uint32_t iv = (uint32_t)lroundf((float)d * 1000.0f / kFsHz);
        for (uint8_t i = kBrRateN - 1U; i > 0; i--) iv_[i] = iv_[i - 1];
        iv_[0] = iv;
        if (niv_ < kBrRateN) niv_++;
        uint32_t srt[kBrRateN];
        memcpy(srt, iv_, sizeof(srt));
        for (uint8_t i = 1; i < niv_; i++)  /* insertion sort, at most 4 values */
            for (uint8_t j = i; j > 0 && srt[j - 1] > srt[j]; j--) {
                const uint32_t t = srt[j];
                srt[j] = srt[j - 1];
                srt[j - 1] = t;
            }
        const float med = (niv_ % 2U) ? (float)srt[niv_ / 2U] : 0.5f * (float)(srt[niv_ / 2U - 1U] + srt[niv_ / 2U]);
        ev.frame       = now;
        ev.interval_ms = iv;
        ev.depth_um    = v - trough_;
        ev.rate_cpm    = 60000.0f / med;
        return true;
    }

    double   s_[2][2]  = {{0.0, 0.0}, {0.0, 0.0}};
    uint32_t n_        = 0;
    float    bmax_[kBrSpanBlocks] = {}, bmin_[kBrSpanBlocks] = {};
    uint8_t  blk_ = kBrSpanBlocks - 1U, nblk_ = 0;
    int8_t   dir_      = 0;
    float    ext_ = 0.0f, ext_hi_ = 0.0f, ext_lo_ = 0.0f, trough_ = 0.0f;
    uint32_t ext_n_ = 0, peak_n_ = 0;
    bool     have_peak_ = false, have_trough_ = false;
    uint32_t iv_[kBrRateN] = {};
    uint8_t  niv_ = 0;
};

#ifdef A121_DSP_VITERBI
/* V3.2 (variant, A/B on day A): pulse tracking as Viterbi over the last kVitWin reports */
static constexpr uint16_t kVitMinBpm = 40U, kVitStates = 141U;  /* 40–180 /min in steps of 1 /min */
static constexpr uint8_t  kVitWin    = 20U;
static constexpr float    kVitSigma  = 3.0f;    /* transition: Gaussian σ = 3 /min per report (1 s) */
static constexpr int16_t  kVitReach  = 12;      /* ±4 σ */
static constexpr float    kVitJump   = 0.02f;   /* share "jump anywhere" (otherwise a wrong pick would last forever) */
static constexpr float    kVitFloor  = 0.1f;    /* base level of the emission (report without a candidate at the true pulse) */
static constexpr float    kVitEmSig  = 1.0f;    /* width per candidate in /min (FFT grid 0.88 /min) */
static constexpr float    kVitGamma  = 3.0f;    /* emission ∝ (candidate value / best)^γ; with γ = 1 the
                                                   * path in the test bench brady stuck for 9 reports on a weak but
                                                   * steady candidate (75 instead of 50); γ = 2: MAE 2.0, γ = 3: 1.5 */

/**
 * Viterbi over a fixed grid of 40–180 /min with the emissions of the last kVitWin reports. Emission per report from
 * the candidates (Gaussian per candidate, weight = candidate value with harmonic penalty and octave check), normalized and
 * with base level kVitFloor. update() recomputes the best path over the window (result = its last state)
 * and the forward probability; mass = share in the range ±5 /min around the result.
 */
class ViterbiTracker {
public:
    ViterbiTracker() {
        float sum = 0.0f;
        for (int16_t d = -kVitReach; d <= kVitReach; d++) sum += expf(-0.5f * (float)(d * d) / (kVitSigma * kVitSigma));
        for (int16_t d = -kVitReach; d <= kVitReach; d++) {
            const float g = (1.0f - kVitJump) * expf(-0.5f * (float)(d * d) / (kVitSigma * kVitSigma)) / sum +
                            kVitJump / (float)kVitStates;
            tr_[d + kVitReach]  = g;
            ltr_[d + kVitReach] = logf(g);
        }
    }

    void reset() { n_ = 0; head_ = 0; }

    /** New report with n candidates (bpm, value ≥ 0). Returns: bpm at the end of the best path; mass ∈ [0, 1]. */
    float update(const float *cand_bpm, const float *cand_w, uint8_t n, float &mass) {
        float *e = em_[head_];
        float sum = 0.0f, wmax = 0.0f;
        for (uint8_t j = 0; j < n; j++) wmax = fmaxf(wmax, cand_w[j]);
        for (uint16_t q = 0; q < kVitStates; q++) {
            const float bpm = (float)(kVitMinBpm + q);
            float v = 0.0f;
            for (uint8_t j = 0; j < n; j++) {
                const float d = (bpm - cand_bpm[j]) / kVitEmSig;
                if (cand_w[j] > 0.0f && fabsf(d) < 6.0f) v += powf(cand_w[j] / wmax, kVitGamma) * expf(-0.5f * d * d);
            }
            e[q] = v;
            sum += v;
        }
        for (uint16_t q = 0; q < kVitStates; q++)
            e[q] = (sum > 0.0f ? (1.0f - kVitFloor) * e[q] / sum : 0.0f) + kVitFloor / (float)kVitStates;
        head_ = (uint8_t)((head_ + 1U) % kVitWin);
        if (n_ < kVitWin) n_++;

        /* Viterbi (log) and forward (normalized) over the window, oldest report first */
        const float ljump = logf(kVitJump / (float)kVitStates);
        for (uint8_t t = 0; t < n_; t++) {
            const float *et = em_[(head_ + kVitWin - n_ + t) % kVitWin];
            if (t == 0) {
                for (uint16_t q = 0; q < kVitStates; q++) {
                    dl_[q] = logf(et[q]);
                    al_[q] = et[q];
                }
            } else {
                float dmax = dl_[0];
                for (uint16_t q = 1; q < kVitStates; q++) dmax = fmaxf(dmax, dl_[q]);
                for (uint16_t q = 0; q < kVitStates; q++) {
                    float best = dmax + ljump, acc = 0.0f;
                    for (int16_t d = -kVitReach; d <= kVitReach; d++) {
                        const int16_t r = (int16_t)q - d;
                        if (r < 0 || r >= (int16_t)kVitStates) continue;
                        best = fmaxf(best, dl_[r] + ltr_[d + kVitReach]);
                        acc += al_[r] * (tr_[d + kVitReach] - kVitJump / (float)kVitStates);
                    }
                    dn_[q] = best + logf(et[q]);
                    an_[q] = et[q] * (acc + kVitJump / (float)kVitStates);  /* Σ al_ = 1 */
                }
                memcpy(dl_, dn_, sizeof(dl_));
                memcpy(al_, an_, sizeof(al_));
            }
            float asum = 0.0f;
            for (uint16_t q = 0; q < kVitStates; q++) asum += al_[q];
            for (uint16_t q = 0; q < kVitStates; q++) al_[q] /= asum;
        }
        uint16_t qb = 0;
        for (uint16_t q = 1; q < kVitStates; q++)
            if (dl_[q] > dl_[qb]) qb = q;
        mass = 0.0f;
        for (int16_t q = (int16_t)qb - 5; q <= (int16_t)qb + 5; q++)
            if (q >= 0 && q < (int16_t)kVitStates) mass += al_[q];
        return (float)(kVitMinBpm + qb);
    }

private:
    float   em_[kVitWin][kVitStates];
    float   dl_[kVitStates], dn_[kVitStates], al_[kVitStates], an_[kVitStates];
    float   tr_[2 * kVitReach + 1], ltr_[2 * kVitReach + 1];
    uint8_t head_ = 0, n_ = 0;
};
#endif

#ifdef A121_DSP_COMBINE
/* V3.1: combine zoom points before demodulation instead of averaging spectra afterwards (Szmola et al. 2025;
 * Pi-ViMo 2023: sum coherently aligned and weighted, maximum ratio combining). */
static constexpr uint8_t kCombKMax = 16U;  /* upper limit for K (accumulators on the stack: 136 complex values) */
static constexpr uint8_t kCombK    = (kZoomPoints < 12U) ? (uint8_t)kZoomPoints : 12U;  /* best K points (plan 8–16) */
static constexpr uint8_t kCombIter = 12U;  /* power iterations on the K × K covariance */
static_assert(kCombK <= kCombKMax, "kCombK zu groß");

/**
 * 1st principal component of K zoom points. u_j[n] = z_j[n] − c_j (ring j as hist_zoom_, from start, N values, c_j =
 * circle center). C = Σ_n u uᴴ (Hermitian), v = eigenvector for the largest eigenvalue (power iteration starting from the column
 * with the largest power), y[n] = Σ_j conj(v_j) · u_j[n] (linear, [I,Q] × N). With equal noise per point this is
 * maximum ratio combining: v_j ∝ amplitude_j · e^{iθ_j}, the points add in phase. Returns: effective
 * point count N_eff = (Σλ)² / Σλ² = (tr C)² / ‖C‖²_F (≈ 1: all see the same motion; ≈ K: noise only); dominant = j
 * with the largest |v_j|. Loop order: n outside, so that each ring (PSRAM) is read only once.
 */
inline float combinePrincipal(const float *const *rings, const float *ci, const float *cq, uint8_t K, uint16_t start,
                              uint16_t N, float *y, uint8_t &dominant) {
    dominant = 0;
    if (K == 0 || K > kCombKMax || N == 0) return 0.0f;
    constexpr uint16_t kPairs = (uint16_t)kCombKMax * (kCombKMax + 1U) / 2U;
    float are[kPairs] = {}, aim[kPairs] = {};  /* C_jk for j ≤ k: index j·K − j(j−1)/2 + (k − j) */
    float ur[kCombKMax], ui[kCombKMax];
    for (uint16_t n = 0; n < N; n++) {
        const size_t idx = (size_t)((start + n) % kHistoryLen) * 2U;
        for (uint8_t j = 0; j < K; j++) {
            ur[j] = rings[j][idx] - ci[j];
            ui[j] = rings[j][idx + 1U] - cq[j];
        }
        uint16_t pidx = 0;
        for (uint8_t j = 0; j < K; j++)
            for (uint8_t k = j; k < K; k++, pidx++) {
                are[pidx] += ur[j] * ur[k] + ui[j] * ui[k];
                aim[pidx] += ui[j] * ur[k] - ur[j] * ui[k];
            }
    }
    auto at = [&](uint8_t j, uint8_t k, float &re, float &im) {  /* C_jk, lower half = conjugate */
        const bool sw = j > k;
        const uint8_t a = sw ? k : j, b = sw ? j : k;
        const uint16_t q = (uint16_t)(a * K - a * (a - 1) / 2 + (b - a));
        re = are[q];
        im = sw ? -aim[q] : aim[q];
    };
    float tr = 0.0f, fro = 0.0f;
    uint8_t jmax = 0;
    for (uint8_t j = 0; j < K; j++) {
        float re, im;
        at(j, j, re, im);
        tr += re;
        float rj, ij;
        at(jmax, jmax, rj, ij);
        if (re > rj) jmax = j;
        for (uint8_t k = 0; k < K; k++) {
            at(j, k, re, im);
            fro += re * re + im * im;
        }
    }
    float vr[kCombKMax], vi[kCombKMax], wr[kCombKMax], wi[kCombKMax];
    for (uint8_t j = 0; j < K; j++) at(j, jmax, vr[j], vi[j]);
    for (uint8_t it = 0; it <= kCombIter; it++) {
        float nrm = 0.0f;
        for (uint8_t j = 0; j < K; j++) nrm += vr[j] * vr[j] + vi[j] * vi[j];
        nrm = sqrtf(nrm);
        if (nrm <= 0.0f) {
            for (uint8_t j = 0; j < K; j++) { vr[j] = (j == jmax) ? 1.0f : 0.0f; vi[j] = 0.0f; }
            break;
        }
        for (uint8_t j = 0; j < K; j++) { vr[j] /= nrm; vi[j] /= nrm; }
        if (it == kCombIter) break;
        for (uint8_t j = 0; j < K; j++) {
            wr[j] = wi[j] = 0.0f;
            for (uint8_t k = 0; k < K; k++) {
                float re, im;
                at(j, k, re, im);
                wr[j] += re * vr[k] - im * vi[k];
                wi[j] += re * vi[k] + im * vr[k];
            }
        }
        memcpy(vr, wr, K * sizeof(float));
        memcpy(vi, wi, K * sizeof(float));
    }
    for (uint8_t j = 1; j < K; j++)
        if (vr[j] * vr[j] + vi[j] * vi[j] > vr[dominant] * vr[dominant] + vi[dominant] * vi[dominant]) dominant = j;
    for (uint16_t n = 0; n < N; n++) {
        const size_t idx = (size_t)((start + n) % kHistoryLen) * 2U;
        float yr = 0.0f, yi = 0.0f;
        for (uint8_t j = 0; j < K; j++) {
            const float a = rings[j][idx] - ci[j], b = rings[j][idx + 1U] - cq[j];
            yr += vr[j] * a + vi[j] * b;
            yi += vr[j] * b - vi[j] * a;
        }
        y[2U * n]      = yr;
        y[2U * n + 1U] = yi;
    }
    return (fro > 0.0f) ? tr * tr / fro : 0.0f;
}
#endif

/* V6: value of VitalReport::reacq */
static constexpr uint8_t kReacqNone = 0U;    /* normal */
static constexpr uint8_t kReacqShort = 1U;   /* short window after motion */
static constexpr uint8_t kReacqMemory = 2U;  /* reacquired via the memory */

struct VitalReport {
    bool     valid;
    uint32_t frame_count;
    float    fs_hz;
    int16_t  temperature_c;
    float    loopback_amp;
    float    loopback_phase_std_mrad;
    float    target_dist_m;
    uint8_t  best_zoom_bin;
    float    zoom_snr_db;      /* combined pulse spectrum: peak/median in dB */
    float    resp_depth_um;
    float    heart_amp_um;
    float    br_bpm;
    float    hr_spec_bpm;      /* strongest peak of the combined spectrum (unfiltered) */
    float    hr_acf_bpm;       /* autocorrelation, point with the clearest pulse */
    float    hr_ibi_bpm;       /* time domain (median IBI), point with the clearest pulse */
    float    hr_fused_bpm;     /* tracked, smoothed pulse (main output) */
    float    ibi_median_ms;
    float    rmssd_ms;
    float    confidence_pct;   /* signalQuality(): peak height, harmonic, agreement of the estimators, motion, jump */
    float    point_agree_pct;  /* earlier confidence: weighted share of the zoom points with their own peak ±5 /min */
    uint8_t  br_state;         /* breathing state (BreathStateTracker): 0 normal, 1 no peak, 2 held, 3 motion, 4 shallow */
    uint16_t n_delayed;        /* since the last report: frames with frame_delayed (V1.5) */
    uint16_t n_sat;            /* … with data_saturated */
    uint16_t n_tickgap;        /* … tick distances > kTickGapMs (without rezoom/restart) */
    uint8_t  hr_bin;           /* zoom point with the clearest pulse */
    uint32_t compute_us;       /* compute time of this report */
    bool     hr_harm;          /* strongest pulse peak lies on a breathing harmonic (a jump there needs 10 reports) */
    float    motion_pct;       /* share of damped 1 s blocks in the window (motion), over all zoom points */
    uint8_t  num_room_targets;
    RoomTarget room_targets[kMaxRoomTargets];  /* sorted by strength (amplitude + 8 × motion) */
    float    room_noise_amp;                   /* noise level amplitude (lower quarter of the room points) */
    float    room_noise_motion;                /* noise level motion */
    bool     needs_rezoom;
    int32_t  recommended_zoom_start_pt;
    bool     person_in_room;   /* moving target in the smoothed room profile (zoom target, V3.5) */
    bool     target_in_zoom;   /* … and it lies in the zoom window */
    uint8_t  state;            /* state of the measurement kSt* (V3.5) */
    uint32_t state_since_ms;   /* … since (frames × 1000 / 60) */
    float    n_eff;            /* effective point count of the combined channel (V3.1, only with A121_DSP_COMBINE) */
    uint8_t  win_s;            /* length of the pulse window of this report in s (V6, output only with A121_DSP_REACQ) */
    uint8_t  reacq;            /* reacquisition after motion (V6): kReacq* */
};

/**
 * State of the history for a report (V2.1). takeSnapshot() runs at the frame rate directly after pushFrame(); the report
 * then computes (on the ESP32 in the DSP task on core 0) only from this state and the ring. pushFrame() may meanwhile
 * keep writing up to kSnapMaxLag frames without touching the window of the snapshot. The sensor counters are
 * sums since begin(); the report gives the difference to the last computed snapshot.
 */
#define A121_DSP_SNAPSHOT 1
struct ReportSnapshot {
    uint16_t write_idx, count;
    uint32_t frame_count;
    uint8_t  best_zoom_bin;
    int16_t  temp_c;
    float    lb_amp;
    uint32_t tot_delayed, tot_sat, tot_tickgap;
    uint32_t tot_frames;  /* frames since begin(), across rezooms (V2.3) */
    float    room_amp[kRoomPoints], room_mot[kRoomPoints], room_amp_slow[kRoomPoints], room_mot_slow[kRoomPoints];
};

/** Report from a snapshot after which pushFrame() has already moved on by more than kSnapMaxLag frames: the window is
 *  partly overwritten, the report is not valid (ESP32 and replay check the same way on output). */
inline bool snapshotStale(uint32_t snap_frame, uint32_t frame_now) { return frame_now - snap_frame > kSnapMaxLag; }

#ifdef A121_DSP_REACQ
/** V6: index of the strongest FFT candidate that lies near the remembered pulse (|bpm − mem_hr| ≤ band) and has at least
 *  min_rel × the best value (cand_score sorted in descending order, [0] = best); otherwise −1 (also without candidates or
 *  memory, without a positive best value and for NaN: the conditions are phrased positively, NaN satisfies none). */
inline int8_t preferMemoryCandidate(const float *cand_bpm, const float *cand_score, uint8_t n, float mem_hr,
                                    float band = kMemBandBpm, float min_rel = 0.5f) {
    if (n == 0U || !(mem_hr > 0.0f) || !(cand_score[0] > 0.0f)) return -1;
    int8_t best = -1;
    for (uint8_t i = 0; i < n; i++) {
        if (!(fabsf(cand_bpm[i] - mem_hr) <= band) || !(cand_score[i] >= min_rel * cand_score[0])) continue;
        if (best < 0 || cand_score[i] > cand_score[best]) best = (int8_t)i;
    }
    return best;
}

/** V6 (ruling R15): true if a reported pulse hr_bpm may replace the breathing peak as the sign of life, i.e. a
 *  memory holds (mem_hr > 0) and |hr_bpm − mem_hr| ≤ tol (kMemSubstBpm). NaN does not satisfy the condition. */
inline bool memSubstitutesBreath(float hr_bpm, float mem_hr, float tol = kMemSubstBpm) {
    return mem_hr > 0.0f && fabsf(hr_bpm - mem_hr) <= tol;
}
#endif

class UltimateEspDsp {
public:
    UltimateEspDsp() = default;

    bool begin(int32_t zoom_start_pt = 200, int32_t room_start_pt = 120, uint16_t room_step = kRoomStep) {
        zoom_start_pt_ = zoom_start_pt;
        room_start_pt_ = room_start_pt;
        room_step_     = room_step;
        tot_delayed_ = tot_sat_ = tot_tickgap_ = tot_frames_ = 0;  /* new stream */
        state_ = 0xFFU;
        state_since_tot_ = 0;
        breath_gate_ = false;
        prev_delayed_ = prev_sat_ = prev_tickgap_ = 0;

        if (!allocated_) {
            hist_zoom_   = allocFloat((size_t)kHistoryLen * kZoomPoints * 2U);
            hist_lb_phi_ = allocFloat(kHistoryLen);
            work_x_      = allocFloat(kHrWin);
            work_ext_    = allocFloat(kHrWin + 2U * kFiltPad);
            best_x_      = allocFloat(kHrWin);
            best_h_      = allocFloat(kHrWin);
            br_x_        = allocFloat(kHrWin);
            dec_         = allocFloat(kHrWin / kDecim + 1U);
            spec_        = allocFloat(kHr2MaxBin + 1U);
            sum_         = allocFloat(kHrNumF);
            comb_        = allocFloat(kHrNumF);
            scratch_     = allocFloat(kHrNumF);
            pen_         = allocFloat(kHrNumF);
            br_comb_     = allocFloat(kBrNumF);
            allocated_   = hist_zoom_ && hist_lb_phi_ && work_x_ && work_ext_ && best_x_ && best_h_ && br_x_ &&
                         dec_ && spec_ && sum_ && comb_ && scratch_ && br_comb_ && pen_;
#ifdef A121_DSP_COMBINE
            comb_y_      = allocFloat(2U * kHrWin);  /* combined channel [I,Q] (V3.1) */
            allocated_   = allocated_ && comb_y_;
#endif
        }
        resetState();
        return allocated_;
    }

    /** Complete restart: history and tracking (begin()). */
    void resetState() {
        resetHistory();
        resetTracking();
    }

    /**
     * Restart of the history (rezoom, recalibration, reconnect; V2.3, B11): ring, filter and room state,
     * circle centers and breathing state anew. Pulse tracking stays if the last valid report is less than
     * kKeepTrackS ago; the first report after that then smooths against the previous value instead of starting over.
     * Call only while no report is computing.
     */
    void resetHistory() {
        if (!(have_valid_ && tot_frames_ - last_valid_tot_ < (uint32_t)(kKeepTrackS * kFsHz))) resetTracking();
        hr_miss_   = 0;
        alt_count_ = 0;
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
        person_run_           = 0;  /* room profile starts anew */
        br_track_.reset();
        breath_det_.reset();  /* path starts at 0: filter and peaks anew (V3.4) */
        breath_pending_       = false;
        have_last_tick_       = false;  /* rezoom/restart: the next tick distance does not count as a gap */
        memset(hist_sat_, 0, sizeof(hist_sat_));

        memset(zoom_clutter_i_, 0, sizeof(zoom_clutter_i_));
        memset(zoom_clutter_q_, 0, sizeof(zoom_clutter_q_));
        memset(zoom_var_ema_, 0, sizeof(zoom_var_ema_));
        memset(room_clutter_i_, 0, sizeof(room_clutter_i_));
        memset(room_clutter_q_, 0, sizeof(room_clutter_q_));
        memset(room_amp_ema_, 0, sizeof(room_amp_ema_));
        memset(room_var_ema_, 0, sizeof(room_var_ema_));
        memset(room_amp_slow_, 0, sizeof(room_amp_slow_));
        memset(room_mot_slow_, 0, sizeof(room_mot_slow_));
        memset(have_good_, 0, sizeof(have_good_));
    }

    /** Pulse tracking anew (no previous value). */
    void resetTracking() {
#ifdef A121_DSP_VITERBI
        vit_.reset();
#endif
#ifdef A121_DSP_HARM_REMOVE
        fb_prev_   = 0.0f;
        conf_prev_ = 0.0f;
#endif
        prov_       = true;
        hr_prev_    = 0.0f;
        hr_miss_    = 0;
        alt_count_  = 0;
        hr_hist_n_  = 0;
        have_valid_ = false;
#ifdef A121_DSP_REACQ
        mem_.clear();  /* V6: no tracking, no memory; resetHistory() with tracking kept keeps it */
#endif
    }

    void setZoomStartPoint(int32_t zoom_start_pt) {
        zoom_start_pt_ = zoom_start_pt;
        resetHistory();
    }

#ifdef A121_DSP_REACQ
    /** V6: Is a pulse remembered (without checking expiry and conditions; the next report does that)? For tests. */
    bool reacqMemoryActive() const { return mem_.stored(); }
    const ReacqMemory &reacqMemory() const { return mem_; }
    /** V6, for tests: tracking state (hr_prev_, hr_miss_, alt_count_, hr_hist_) that stays unchanged in case 4. */
    struct TrackState {
        float   hr_prev;
        uint8_t hr_miss, alt_count, hr_hist_n;
        float   hr_hist[5];
    };
    TrackState trackState() const {
        TrackState t = {hr_prev_, hr_miss_, alt_count_, hr_hist_n_, {}};
        memcpy(t.hr_hist, hr_hist_, sizeof(t.hr_hist));
        return t;
    }
#endif

    int32_t zoomStartPoint() const { return zoom_start_pt_; }
    uint32_t frameCount() const { return frame_count_; }  /* frames since begin()/rezoom/restart */

    /** Fetch the breath from the last pushFrame() (V3.4; at the frame rate, core 1); false = none or gate closed. */
    bool takeBreathEvent(BreathEvent &ev) {
        if (!breath_pending_) return false;
        breath_pending_ = false;
        if (!breath_gate_) return false;
        ev = breath_ev_;
        return true;
    }

    /**
     * Gate for @BREATH (V3.4) from the state of the last output report (breathGateOpen()). Set at the frame rate
     * when the report result is output (ESP32: after @STATE; replay: at @DSP), not in the report itself:
     * so ESP32 and replay see the gate from the same frame. Detection continues even with the gate closed.
     */
    void setBreathGate(bool open) { breath_gate_ = open; }

    /** Room profile (subsweep 2): smoothed amplitude and motion per point, kRoomPoints values. */
    const float *roomAmp() const { return room_amp_ema_; }
    const float *roomMotion() const { return room_var_ema_; }
    float roomStartMm() const { return (float)room_start_pt_ * kPointStepM * 1000.0f; }
    float roomStepMm() const { return (float)room_step_ * kPointStepM * 1000.0f; }

    /**
     * @brief Processes a 3-subsweep frame (105 complex points).
     * @param dt_s no longer used (time base = kFsHz); stays for caller compatibility.
     * @param out_um_sample Displacement of the telemetry beam in µm (for `@IQ3`).
     */
    void pushFrame(const Complex16 *pts, uint16_t n_pts, int16_t temp_c, float dt_s, float &out_um_sample,
                   const FrameFlags &ff = FrameFlags()) {
        (void)dt_s;
        /* count sensor flags (reset per report); tick gap only within a history */
        if (ff.delayed) tot_delayed_++;
        if (ff.saturated) tot_sat_++;
        if (ff.have_tick) {
            if (have_last_tick_ && (uint32_t)(ff.tick - last_tick_) > kTickGapMs) tot_tickgap_++;
            last_tick_      = ff.tick;
            have_last_tick_ = true;
        }
        if (!allocated_ || pts == nullptr || n_pts < (1U + kZoomPoints)) {
            out_um_sample = 0.0f;
            return;
        }
        last_temp_c_ = temp_c;

        /* 1. Loopback phase reference */
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

        /* 2. Derotate zoom points, store them and estimate motion energy */
        float derot_i[kZoomPoints], derot_q[kZoomPoints];
        const Complex16 *zoom_pts = &pts[1];
        for (uint16_t b = 0; b < kZoomPoints; b++) {
            const float ri = (float)zoom_pts[b].real;
            const float rq = (float)zoom_pts[b].imag;
            const float zi = ri * rot_re - rq * rot_im;
            const float zq = ri * rot_im + rq * rot_re;
            derot_i[b] = zi;
            derot_q[b] = zq;
            float *slot = hist_zoom_ + ((size_t)b * kHistoryLen + hist_write_idx_) * 2U;  /* store point by point */
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

        /* Telemetry beam: point with the largest motion, switch only at a 1.5x better value (hysteresis).
         * A switch without hysteresis made the path signal jump by up to 1 mm every 0.5 s. */
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

        /* 3. Centering + phase unwrapping → µm (telemetry only; the pulse is computed from the individual points) */
        if (frame_count_ == 0 || beam_switched_) {
            dc_i_ = cl_i;  /* after a beam switch, start from the static part of the new beam */
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
        breath_pending_     = breath_det_.push(disp_um, breath_ev_);  /* V3.4 */
        hist_sat_[hist_write_idx_]    = ff.saturated ? 1U : 0U;
        hist_lb_phi_[hist_write_idx_] = lb_phi;
        hist_write_idx_ = (uint16_t)((hist_write_idx_ + 1U) % kHistoryLen);
        if (hist_count_ < kHistoryLen) hist_count_++;

        /* 4. Room scan (subsweep 2) */
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
        tot_frames_++;
    }

    /** Capture the state for a report (at the frame rate after pushFrame(), V2.1). */
    void takeSnapshot(ReportSnapshot &s) const {
        s.write_idx     = hist_write_idx_;
        s.count         = hist_count_;
        s.frame_count   = frame_count_;
        s.best_zoom_bin = best_zoom_bin_;
        s.temp_c        = last_temp_c_;
        s.lb_amp        = last_lb_amp_;
        s.tot_delayed   = tot_delayed_;
        s.tot_sat       = tot_sat_;
        s.tot_tickgap   = tot_tickgap_;
        s.tot_frames    = tot_frames_;
        memcpy(s.room_amp, room_amp_ema_, sizeof(s.room_amp));
        memcpy(s.room_mot, room_var_ema_, sizeof(s.room_mot));
        memcpy(s.room_amp_slow, room_amp_slow_, sizeof(s.room_amp_slow));
        memcpy(s.room_mot_slow, room_mot_slow_, sizeof(s.room_mot_slow));
    }

    /**
     * @brief Report for the current state (snapshot and computation in one, as before V2.1).
     *        Call about once per second (tracking and rezoom count in reports).
     */
    bool computeVitalReport(VitalReport &rep) {
        takeSnapshot(snap_);
        return computeVitalReport(snap_, rep);
    }

    /**
     * @brief Room targets + rezoom recommendation always; vital values (valid = true) from kHrMinWin frames. Besides the
     *        snapshot it reads only the ring in the window of the snapshot and report state (tracking, circle centers,
     *        work buffers); pushFrame() may run in parallel as long as it is at most kSnapMaxLag frames ahead and
     *        zoom_start_pt_ does not change (setZoomStartPoint/begin only after the report).
     */
    bool computeVitalReport(const ReportSnapshot &s, VitalReport &rep) {
        const uint32_t t0 = micros();
        memset(&rep, 0, sizeof(rep));
        rep.n_delayed = sat16(s.tot_delayed - prev_delayed_);
        rep.n_sat     = sat16(s.tot_sat - prev_sat_);
        rep.n_tickgap = sat16(s.tot_tickgap - prev_tickgap_);
        prev_delayed_ = s.tot_delayed;
        prev_sat_     = s.tot_sat;
        prev_tickgap_ = s.tot_tickgap;
        rep.frame_count   = s.frame_count;
        rep.fs_hz         = kFsHz;
        rep.temperature_c = s.temp_c;
        rep.loopback_amp  = s.lb_amp;
        rep.best_zoom_bin = s.best_zoom_bin;
        rep.target_dist_m = (float)(zoom_start_pt_ + (int32_t)s.best_zoom_bin) * kPointStepM;
        extractRoomTargets(s, rep);

        /* Person in the room: zoom target in the smoothed profile in 2 reports in a row (without a person it flickered in the
         * test bench from report to report) */
        person_run_ = rep.person_in_room ? (uint8_t)std::min(person_run_ + 1, 255) : 0U;
        const bool person = person_run_ >= 2U;
        if (s.count < kHrMinWin) {
            rep.valid = false;
            setState(s, rep, person ? (rep.target_in_zoom ? kStAcquire : kStSearch) : kStNoPerson);
            rep.compute_us = micros() - t0;
            return false;
        }
        const uint16_t N = (s.count > kHrWin) ? kHrWin : s.count;
        const uint16_t start = (uint16_t)((s.write_idx + kHistoryLen - N) % kHistoryLen);

        /* Loopback phase stability */
        float lb_mean = 0.0f, lb_var = 0.0f;
        for (uint16_t n = 0; n < N; n++) lb_mean += hist_lb_phi_[(start + n) % kHistoryLen];
        lb_mean /= (float)N;
        for (uint16_t n = 0; n < N; n++) {
            const float d = hist_lb_phi_[(start + n) % kHistoryLen] - lb_mean;
            lb_var += d * d;
        }
        rep.loopback_phase_std_mrad = sqrtf(lb_var / (float)N) * 1000.0f;

        /* Pulse: spectra of all zoom points, weighted average */
        const uint16_t M = (uint16_t)((N + kDecim - 1U) / kDecim);
        float peak_hz[kZoomPoints], weight[kZoomPoints];
        float br_best = -1.0f;
        int16_t br_bin = -1;
        float wsum = 0.0f, best_q = -1.0f;
        uint8_t hr_bin = 0;
        uint32_t moving_blocks = 0, total_blocks = 0;
        for (uint16_t k = 0; k < kHrNumF; k++) comb_[k] = 0.0f;
        for (uint16_t k = 0; k < kBrNumF; k++) br_comb_[k] = 0.0f;
        const float *hann = hannWindow(M);
        /* Wide zoom window (V2.3): breathing strength in advance as the scatter of the IQ values in the window (≈ amplitude² ×
         * phase excursion², the same quantity as the weight of the breathing spectrum), compute only the kSpecPoints strongest. */
        bool use[kZoomPoints];
#ifdef A121_DSP_COMBINE
        constexpr uint16_t kSelPoints = kCombK;  /* V3.1: only the K strongest points go into the combination */
#else
        constexpr uint16_t kSelPoints = kSpecPoints;
#endif
        if (kSelPoints < kZoomPoints) {
            float strength[kZoomPoints];
            zoomStrength(start, N, strength);
            selectStrongest(strength, kZoomPoints, kSelPoints, use);
        } else {
            for (uint8_t b = 0; b < kZoomPoints; b++) use[b] = true;
        }
        /* Pulse window: without A121_DSP_REACQ always the whole window (Np = N) */
        uint16_t Np = N;
#ifdef A121_DSP_REACQ
        /* V6: motion map before the spectra (before combineZoom: motionMap uses work_x_, in which combineZoom stores the
         * combined channel). No moving block: as before. Moving and quiet tail ≥ kReacqMinS: pulse
         * (spectra, candidates, ACF, individual beats) only on the tail, as with preliminary values (V2.4) with a confirmation requirement;
         * breathing, breathing state, motion_pct and room continue on the whole window. Moving and tail shorter: no pulse,
         * tracking stays unchanged (before, it ran on through the motion and jumped to artifacts). */
        const MotionMap mm = motionMap(start, N, use);
        const bool moved = mm.mask != 0U;
        const bool pulse_off = moved && mm.clean_tail_blocks < kReacqMinS;
        const bool short_win = moved && !pulse_off;
        if (short_win) Np = (uint16_t)(mm.clean_tail_blocks * kGateBlk);
        rep.win_s = pulse_off ? 0U : (uint8_t)(Np / 60U);
        rep.reacq = short_win ? kReacqShort : kReacqNone;
#endif
        const uint16_t start_p = (uint16_t)((start + (N - Np)) % kHistoryLen);  /* start of the pulse window */
        const uint16_t Mp = (uint16_t)((Np + kDecim - 1U) / kDecim);
#ifdef A121_DSP_REACQ
        const float *hann_p = (Np == N) ? hann : hannInto(hann_p_, hann_p_m_, Mp);
#else
        const float *hann_p = hann;
#endif
        uint32_t moving_p = 0, total_p = 0;  /* motion blocks in the pulse window (for the confidence) */
#ifdef A121_DSP_COMBINE
        /* V3.1: one combined channel instead of single spectra; the loop below runs once with its path signal and
         * the dominant point as hr_bin */
        bool comb_short = false;
        float comb_spread = 0.0f;
        float comb_amp    = 0.0f;
        const uint8_t comb_dom = combineZoom(start, N, use, comb_amp, comb_short, comb_spread, rep.n_eff);
        for (uint8_t b = 0; b < kZoomPoints; b++) use[b] = (b == comb_dom);
#endif

        for (uint8_t b = 0; b < kZoomPoints; b++) {
            if (!use[b]) {
                weight[b]  = 0.0f;
                peak_hz[b] = 0.0f;
                continue;
            }
            bool short_arc = false;
            float spread = 0.0f;
#ifdef A121_DSP_COMBINE
            short_arc = comb_short;  /* work_x_ already holds the path signal of the combined channel */
            spread    = comb_spread;
            const float amp_c = comb_amp;
#else
            const float amp_c = binDisplacement(b, start, N, work_x_, short_arc, spread);
#endif
            removeLinearTrend(work_x_, N);
            float *h = work_ext_ + kFiltPad;  /* filtered signal sits in the middle of work_ext_ after sosFiltFilt */
            sosFiltFilt(work_x_, N);
            const uint8_t moving_b = gateMotion(h, N);
            moving_blocks += moving_b;
            /* Breathing spectrum of this point (detrended path signal, decimated, Hann), summed with amplitude² weighting.
             * Weak points have phase noise ~ 1/amplitude, and unwrapping errors act there like steps of 2.5 mm:
             * unweighted, their low-frequency part masked the breathing (on the 2026-11-01 dataset points with |z| 120–300,
             * 140–240 phase jumps in 15 s, 8–11 mm "excursion"; the strong points showed a clean 17 /min). */
            const float wa = amp_c * amp_c;
            for (uint16_t m = 0; m < M; m++) dec_[m] = work_x_[(uint32_t)m * kDecim] * hann[m];
            fft_.power(dec_, M, spec_, kBrK0 + kBrNumF);
            float br_energy = 0.0f;
            for (uint16_t k = 0; k < kBrNumF; k++) {
                const float pk = spec_[kBrK0 + k];
                br_comb_[k] += wa * pk;
                br_energy += pk;
            }
            /* Breathing point: strongest breathing (amplitude² × breathing band) among the points with a clean echo. Pure noise
             * (no echo, scatter ≈ 0.52) is excluded; there the phase runs free and fakes millimeter excursion. */
            if (spread < kEchoSpreadMax && wa * br_energy > br_best) {
                br_best = wa * br_energy;
                br_bin  = (int16_t)b;
                memcpy(br_x_, work_x_, N * sizeof(float));
            }
            total_blocks += N / kGateBlk;
            /* Pulse part on the pulse window: xp = path signal, h = filtered and damped (without A121_DSP_REACQ the whole
             * window as before) */
            float *xp = work_x_;
#ifdef A121_DSP_REACQ
            if (pulse_off) {  /* V6: no pulse on this window; breathing and motion are already captured above */
                weight[b]  = 0.0f;
                peak_hz[b] = 0.0f;
                continue;
            }
            if (Np != N) {  /* V6: tail after motion, path signal detrended and filtered anew */
                xp = work_x_ + (N - Np);
                removeLinearTrend(xp, Np);
                sosFiltFilt(xp, Np);
                moving_p += gateMotion(h, Np);
                total_p += Np / kGateBlk;
            } else
#endif
            {
                moving_p += moving_b;
                total_p += N / kGateBlk;
            }
            /* decimate + Hann */
#ifdef A121_DSP_HARM_REMOVE
            /* V3.3: remove breathing harmonics guided by the breathing phase of this point, with the breathing and pulse frequency of the
             * previous report. Only if the previous report was safe: if it tracked a wrong pulse (test bench brady:
             * 2 × pulse, confidence ≈ 20 %), the rule spared the wrong one and erased the real one, which lay close to a
             * breathing harmonic (MAE 2.8 → 3.7 /min). */
            if (fb_prev_ > 0.0f && hr_prev_ > 0.0f && conf_prev_ >= kGuideMinConf) {
                for (uint16_t m = 0; m < Mp; m++) {
                    dec_[m]        = h[(uint32_t)m * kDecim];
                    guide_disp_[m] = xp[(uint32_t)m * kDecim];
                }
                removeBreathHarmonicsGuided(dec_, guide_disp_, Mp, kFsHz / kDecim, fb_prev_, hr_prev_ / 60.0f, cfft_,
                                            guide_work_);
                for (uint16_t m = 0; m < Mp; m++) dec_[m] *= hann_p[m];
            } else
#endif
            for (uint16_t m = 0; m < Mp; m++) dec_[m] = h[(uint32_t)m * kDecim] * hann_p[m];
            /* Harmonic sum S(f) = P(f) + 0.5 · P(2f): a heartbeat is a short impulse with a strong 2nd harmonic,
             * a breathing harmonic m · f_breath has hardly any power at 2f (V1.2). On the FFT grid 2f lies exactly on bin 2k. */
            float pmax = 0.0f;
            uint16_t kmax = 0;
            fft_.power(dec_, Mp, spec_, kHr2MaxBin + 1U);
            for (uint16_t k = 0; k < kHrNumF; k++) {
                const uint16_t k1 = (uint16_t)(kHrK0 + k), k2 = (uint16_t)(2U * k1);
                sum_[k] = spec_[k1] + kHarm2W * (k2 <= kHr2MaxBin ? spec_[k2] : 0.0f);
                if (sum_[k] > pmax) { pmax = sum_[k]; kmax = k; }
            }
            memcpy(scratch_, sum_, kHrNumF * sizeof(float));
            std::nth_element(scratch_, scratch_ + kHrNumF / 2, scratch_ + kHrNumF);
            const float med = scratch_[kHrNumF / 2] + 1e-12f;
            const float q   = pmax / med;
            const float w   = q * q * (short_arc ? 0.5f : 1.0f);  /* short arc: phase less certain */
            for (uint16_t k = 0; k < kHrNumF; k++) comb_[k] += w * (sum_[k] / med);
            wsum += w;
            weight[b]  = w;
            peak_hz[b] = kHrFMin + kHrFStep * kmax;
            if (q > best_q) {
                best_q = q;
                hr_bin = b;
                memcpy(best_x_, xp, Np * sizeof(float));
                memcpy(best_h_, h, Np * sizeof(float));
            }
        }
        for (uint16_t k = 0; k < kHrNumF; k++) comb_[k] /= (wsum + 1e-12f);
#ifdef A121_DSP_HARM_REMOVE
        conf_prev_ = 0.0f;  /* consumed; applies again only if this report ends valid */
#endif
        rep.hr_bin     = hr_bin;
        rep.motion_pct = 100.0f * (float)moving_blocks / (float)(total_blocks ? total_blocks : 1U);

        /* Breathing over all points: largest true local maximum (not at the band edge), parabolically refined.
         * Previously the largest bin in the search range counted; with posture changes (steps in the path signal) or residual drift the
         * power keeps rising toward low frequencies, and the result stuck at the edge at 6.9 /min. Without a true peak
         * fb stays 0 (br_bpm = 0 means: no breathing peak). Before the pulse search, because the harmonic check needs fb. */
        float fb = 0.0f, br_q = 0.0f;
        int16_t kb = -1;
        /* Threshold for the breathing peak grows with the window in a short window (V2.4): at 6 s the main lobe covers almost the
         * whole breathing band, peak/median in the test bench with a person was 1.5–1.7 (8 s: 2.0–4.3) */
        const float br_min_q = kBrMinQ * std::min(1.0f, (float)N / (float)kHrStableWin);
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
        /* Breathing state (V1.4) at the breathing point; before the pulse search, so that a report without a pulse candidate also carries it */
        const bool echo_ok = br_bin >= 0;
        rep.br_state = br_track_.update(echo_ok ? breathAmplitude(br_x_, N, kFsHz) : -1.0f, br_q >= br_min_q && fb > 0.0f,
                                        rep.motion_pct);
        const bool breath_held = rep.br_state == kBrStateApnea;
        /* Breathing excursion (peak-to-peak) at the breathing point at the breathing rate of all points (decimated to 15 Hz, Hann); before V1.3
         * at the point with the clearest pulse, which could also be a noise point (B10, B19). Without a clean echo at the point with the
         * clearest pulse over its pulse window (value then meaningless, the report is not valid without an echo). */
        {
            const uint16_t Mr = echo_ok ? M : Mp;
            const float *hr_w = echo_ok ? hann : hann_p;
            for (uint16_t m = 0; m < Mr; m++) dec_[m] = (echo_ok ? br_x_ : best_x_)[(uint32_t)m * kDecim] * hr_w[m];
            const float best_p = (fb > 0.0f) ? goertzelPower(dec_, Mr, kFsHz / kDecim, fb) : 0.0f;
            const float norm   = 0.25f * (float)Mr;  /* Hann gain → amplitude in µm */
            rep.br_bpm        = breath_held ? 0.0f : fb * 60.0f;  /* held: no breathing rate (previously 15.9 /min) */
            rep.resp_depth_um = 2.0f * sqrtf(best_p) / norm;
        }
#ifdef A121_DSP_REACQ
        /* V6: Without a pulse part (pulse_off) best_x_ is from the previous report; so without an echo no breathing excursion */
        if (pulse_off && !echo_ok) rep.resp_depth_um = 0.0f;
#endif
        const bool breath_ok = echo_ok && br_q >= br_min_q && rep.resp_depth_um >= kBreathMinUm;
#ifdef A121_DSP_REACQ
        /* V6: memory is checked every report (expired, no clean echo or target not in the zoom window: cleared) */
        const bool mem_on = mem_.active(s.tot_frames, echo_ok, rep.target_in_zoom);
        if (pulse_off) {  /* V6: moving, quiet tail < kReacqMinS: no pulse, tracking unchanged */
            rep.valid = false;
            setState(s, rep, measurementState(person, echo_ok, breath_ok, false, rep.motion_pct, rep.br_state));
            rep.compute_us = micros() - t0;
            return false;
        }
#endif
#ifdef A121_DSP_HARM_REMOVE
        fb_prev_ = (br_q >= br_min_q && fb > 0.0f && !breath_held) ? fb : 0.0f;  /* for the next report (V3.3) */
#endif
        /* Breathing harmonics (m = 2..8) in the pulse band: with fast breathing (≈ 20 /min) the 3rd harmonic lay at 55–60 /min
         * and masked the pulse completely. Candidates on m ≥ 4 get S × 0.5 in the selection (the pulse can lie on it,
         * live 2026-11-01: pulse 78 ≈ 4 × breathing 19.5), a jump there needs kHoldHarm instead of 3 reports, and the
         * confidence is halved (signalQuality). harmOrder: m if f lies on m · f_breath, otherwise 0. For m ≤ 3 the
         * whole main lobe applies (±kLowLobeHz) and S × 0.02: these harmonics are 10–100 × stronger than the pulse, a
         * pulse inside them cannot be separated spectrally, and in the first report (8 s window) f_breath is up to 8 % inaccurate
         * (test bench base/step/beweg: 3 × breathing reported for 4 s). */
        auto harmOrder = [&](float f) -> uint8_t {
            if (br_q < br_min_q || fb <= 0.0f) return 0;
            for (uint8_t m = 2; m <= kHarmMMax; m++)
                if (fabsf(f - (float)m * fb) <= (m <= kHarmLowMax ? kLowLobeHz : kHarmTolHz)) return m;
            return 0;
        };
        auto isHarmHz = [&](float f) { return harmOrder(f) != 0; };
        auto harmPenalty = [&](float f) {
            const uint8_t m = harmOrder(f);
            return m == 0 ? 1.0f : (m <= kHarmLowMax ? kHarmPenLow : kHarmPenalty);
        };
        auto binHz = [](int16_t k) { return kHrFMin + kHrFStep * (float)k; };
        /* Penalty per peak, not per frequency step: otherwise a notch appears in the harmonic peak, and its
         * flank becomes a spurious candidate (test bench: 57 instead of 54 /min). pen_ = S × penalty at local maxima of S. */
        for (uint16_t k = 0; k < kHrNumF; k++) {
            const bool peak = k > 0 && k + 1 < kHrNumF && comb_[k] >= comb_[k - 1] && comb_[k] >= comb_[k + 1];
            pen_[k] = peak ? comb_[k] * harmPenalty(binHz((int16_t)k)) : 0.0f;
        }

        /* Candidates: top 3 local maxima by pen_ with ≥ 0.08 Hz spacing, not at the band edge (on the dataset the
         * most common false peak lay at 54 /min = 0.90 Hz right above the search limit 0.88 Hz and was supported by
         * many points) */
        int16_t cand[3] = {-1, -1, -1};
        uint8_t n_cand  = 0;
        for (uint8_t c = 0; c < 3; c++) {
            int16_t bestk = -1;
            for (uint16_t k = kHrEdgeBins; k + kHrEdgeBins < kHrNumF; k++) {
                if (pen_[k] <= 0.0f) continue;
                bool far = true;
                for (uint8_t j = 0; j < n_cand; j++)
                    if (fabsf((float)(k - cand[j]) * kHrFStep) <= 0.08f) far = false;
                if (far && (bestk < 0 || pen_[k] > pen_[bestk])) bestk = (int16_t)k;
            }
            if (bestk < 0) break;
            cand[n_cand++] = bestk;
        }
        if (n_cand == 0) {
            rep.valid = false;
            setState(s, rep, measurementState(person, echo_ok, echo_ok && br_q >= br_min_q && fb > 0.0f, false,
                                              rep.motion_pct, rep.br_state));
            rep.compute_us = micros() - t0;
            return false;
        }
        /* Octave check (B5) per candidate: with a slow pulse the 2nd heart harmonic is often stronger than the fundamental
         * (test bench: pulse 50 → 100 reported), and tracking would otherwise also jump to 2 × pulse (145 instead of 72). If
         * at f/2 (±0.02 Hz, in the search band) there is a peak with S(f/2) ≥ kSubRel · S(f), the candidate stands for f/2; its
         * value is the larger of the two. S without penalty: the pulse often lies on a breathing harmonic m ≥ 4 (test bench
         * brady: 50 ≈ 4 × 12). Not if f/2 lies in the main lobe of a low breathing harmonic (m ≤ 3, ±kLowLobeHz):
         * that one is 10–100 × stronger than the pulse, and S(f/2) contains 0.5 · P(f) anyway, so that its flank there
         * forms a spurious peak (test bench tachy: 120 → 60.6 at 3 × 19 = 57). */
        float cs[3] = {0.0f, 0.0f, 0.0f};
        for (uint8_t j = 0; j < n_cand; j++) {
            cs[j] = pen_[cand[j]];
            const float half = 0.5f * binHz(cand[j]);
            int16_t sub = -1;
            for (int16_t k = (int16_t)kHrEdgeBins; k + (int16_t)kHrEdgeBins < (int16_t)kHrNumF; k++)
                if (pen_[k] > 0.0f && fabsf(binHz(k) - half) <= 0.02f && (sub < 0 || comb_[k] > comb_[sub])) sub = k;
            if (sub < 0) continue;
            const uint8_t m_sub = harmOrder(binHz(sub));
            if (m_sub != 0 && m_sub <= kHarmLowMax) continue;
            if (comb_[sub] >= kSubRel * comb_[cand[j]]) {
                cs[j]   = fmaxf(cs[j], pen_[sub]);
                cand[j] = sub;
            }
        }
        /* remove duplicates (≤ 0.08 Hz), sort by value */
        for (uint8_t j = 0; j < n_cand; j++)
            for (uint8_t i2 = j + 1U; i2 < n_cand;) {
                if (fabsf((float)(cand[i2] - cand[j]) * kHrFStep) <= 0.08f) {
                    cs[j] = fmaxf(cs[j], cs[i2]);
                    for (uint8_t r = i2; r + 1U < n_cand; r++) { cand[r] = cand[r + 1U]; cs[r] = cs[r + 1U]; }
                    n_cand--;
                } else {
                    i2++;
                }
            }
        for (uint8_t j = 1; j < n_cand; j++)
            for (uint8_t r = j; r > 0 && cs[r] > cs[r - 1U]; r--) {
                std::swap(cs[r], cs[r - 1U]);
                std::swap(cand[r], cand[r - 1U]);
            }
        /* Candidate frequency parabolically refined between the bins (V2.2; candidates are local maxima of S) */
        float cf[3] = {0.0f, 0.0f, 0.0f};
        for (uint8_t j = 0; j < n_cand; j++) {
            const int16_t k = cand[j];
            float d = 0.0f;
            if (k > 0 && k + 1 < (int16_t)kHrNumF) {
                const float a = comb_[k - 1], b = comb_[k], c = comb_[k + 1], den = a - 2.0f * b + c;
                if (b >= a && b >= c && den < 0.0f) d = std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / den));
            }
            cf[j] = binHz(k) + kHrFStep * d;
        }
#ifdef A121_DSP_PHASE_SLOPE
        /* V6 phase slope variant (experiment): the chosen candidate (cf[0]) comes from the phase instead of from the
         * parabola. Signal = heart signal of the clearest point (or of the combined channel) on the pulse window, decimated
         * and weighted as for the spectra; dec_ is free here (the point loop and the breathing excursion are done). The
         * candidate selection, the octave check and the other candidates stay as without the switch. */
        for (uint16_t m = 0; m < Mp; m++) dec_[m] = best_h_[(uint32_t)m * kDecim] * hann_p[m];
        cf[0] = phaseSlopeHz(dec_, Mp, kFsHz / kDecim, binHz(cand[0]), psl_fft_, psl_re_, psl_im_);
#endif
        rep.hr_harm = isHarmHz(cf[0]);
        const float raw_bpm = cf[0] * 60.0f;
        rep.hr_spec_bpm = raw_bpm;
        memcpy(scratch_, comb_, kHrNumF * sizeof(float));
        std::nth_element(scratch_, scratch_ + kHrNumF / 2, scratch_ + kHrNumF);
        const float comb_med = scratch_[kHrNumF / 2] + 1e-12f;
        rep.zoom_snr_db = 10.0f * log10f(comb_[cand[0]] / comb_med);
        auto sAtBpm = [&](float bpm) {  /* combined spectrum (without penalty) at the nearest frequency step */
            int k = (int)lroundf((bpm / 60.0f - kHrFMin) / kHrFStep);
            k = k < 0 ? 0 : (k >= (int)kHrNumF ? (int)kHrNumF - 1 : k);
            return comb_[k];
        };

        /* Tracking with hold time. A near candidate (±12 /min) wins only with ≥ 0.5 × the best candidate value
         * (previously 0.4 without penalty: tracking stuck on strong foreign peaks, B6). If the tracked value lies
         * on 2 × / 3 × breathing and for 3 reports there is a candidate without a harmonic with ≥ 0.5 × the value there,
         * it switches. Aborting the hold time via point agreement was rejected on 2026-11-01 (rhythmic
         * motion is supported by all points). */
        /* Preliminary (V2.4): if tracking starts in a window under 8 s, only the raw value of this report counts; it
         * is not committed (test bench: in the 6 s window the strongest peak often lay at the band edge or on a
         * breathing harmonic, and the hold time kept the wrong value for up to 10 reports). */
        const bool prov = prov_ && N < kHrStableWin;
        if (!prov) prov_ = false;
#ifdef A121_DSP_VITERBI
        float vit_mass = 0.0f;
#endif
#ifdef A121_DSP_REACQ
        /* V6: tail after motion as preliminary: tracking is not advanced (Viterbi neither).
         * The strongest candidate is reported, with an active memory first the strongest one near the remembered pulse
         * (preferMemoryCandidate); hr_prev_ stays only if the value is confirmed below, otherwise hr_keep applies again.
         * Open (final review): with A121_DSP_VITERBI (also via A121_DSP_HEAVY) together with A121_DSP_REACQ, vit_
         * stays frozen here; after a confirmed tail the next full report can choose the old Viterbi path and
         * pull hr_prev_ back to the value before the motion. No shipped build environment combines this today;
         * resolve before such a combination (e.g. reset vit_ after a confirmed tail) and check on the test bench. */
        const float hr_keep = hr_prev_;
        int8_t mem_j = -1;
        if (short_win) {
            if (mem_on) {
                float cb[3];
                for (uint8_t j = 0; j < n_cand; j++) cb[j] = cf[j] * 60.0f;
                mem_j = preferMemoryCandidate(cb, cs, n_cand, mem_.hr());
            }
            hr_prev_ = cf[mem_j >= 0 ? mem_j : 0] * 60.0f;
#ifdef A121_DSP_VITERBI
            vit_mass = 1.0f;  /* no Viterbi mass in this report: do not limit the confidence */
#endif
        } else {
#endif
#ifdef A121_DSP_VITERBI
        /* V3.2: Viterbi over the last 20 reports instead of hold time/switching rules; result pulled to the nearest candidate
         * (±3 /min, refined), near values smoothed as before */
        {
            float cb[3], cw[3];
            for (uint8_t j = 0; j < n_cand; j++) {
                cb[j] = cf[j] * 60.0f;
                cw[j] = cs[j];
            }
            float tgt = vit_.update(cb, cw, n_cand, vit_mass);
            int16_t nj = -1;
            for (uint8_t j = 0; j < n_cand; j++)
                if (fabsf(cb[j] - tgt) <= 3.0f && (nj < 0 || fabsf(cb[j] - tgt) < fabsf(cb[nj] - tgt))) nj = (int16_t)j;
            if (nj >= 0) tgt = cb[nj];
            if (hr_prev_ <= 0.0f || prov) hr_prev_ = raw_bpm;  /* preliminary (V2.4): raw value, as without the switch */
            else hr_prev_ = (fabsf(tgt - hr_prev_) < 12.0f) ? 0.7f * hr_prev_ + 0.3f * tgt : tgt;
        }
#else
        if (hr_prev_ <= 0.0f || prov) {
            hr_prev_   = raw_bpm;
            hr_miss_   = 0;
            alt_count_ = 0;
        } else {
            int16_t near = -1;
            for (uint8_t j = 0; j < n_cand; j++) {
                const float bpm = cf[j] * 60.0f;
                if (cs[j] >= kNearMinRel * cs[0] && fabsf(bpm - hr_prev_) < 12.0f && (near < 0 || cs[j] > cs[near]))
                    near = (int16_t)j;
            }
            if (near >= 0) {
                hr_prev_ = 0.7f * hr_prev_ + 0.3f * cf[near] * 60.0f;
                hr_miss_ = 0;
            } else if (++hr_miss_ >= (rep.hr_harm ? kHoldHarm : 3U)) {
                hr_prev_ = raw_bpm;
                hr_miss_ = 0;
            }
            /* Comparison with penalty: unclipped, tracking would stick to a strong breathing harmonic because
             * its value oscillates around half the peak height. Only for m ≤ 3: on m ≥ 4 the pulse itself often lies (test bench
             * harm4, brady), a switch there went to the 3rd heart harmonic (75 instead of 50). */
            int16_t alt = -1;
            if (harmOrder(hr_prev_ / 60.0f) != 0 && harmOrder(hr_prev_ / 60.0f) <= kHarmLowMax) {
                const float s_tr = sAtBpm(hr_prev_) * harmPenalty(hr_prev_ / 60.0f);
                for (uint8_t j = 0; j < n_cand; j++) {
                    const float bpm = cf[j] * 60.0f;
                    if (!isHarmHz(cf[j]) && fabsf(bpm - hr_prev_) >= 12.0f && cs[j] >= 0.5f * s_tr &&
                        (alt < 0 || cs[j] > cs[alt]))
                        alt = (int16_t)j;
                }
            }
            if (alt >= 0 && ++alt_count_ >= kAltReports) {
                hr_prev_   = cf[alt] * 60.0f;
                hr_miss_   = 0;
                alt_count_ = 0;
            } else if (alt < 0) {
                alt_count_ = 0;
            }
        }
#endif
#ifdef A121_DSP_REACQ
        }
#endif

        /* Heart signal of the clearest point (pulse window): amplitude, autocorrelation, individual beats */
        float hr_rms = 0.0f;
        for (uint16_t n = 0; n < Np; n++) hr_rms += best_h_[n] * best_h_[n];
        rep.heart_amp_um = sqrtf(2.0f * hr_rms / (float)Np);
        removeBreathHarmonics(best_h_, Np, kFsHz, (br_q >= br_min_q) ? fb : 0.0f, hr_prev_ / 60.0f);
        float acf_bpm = 0.0f, acf_peak = 0.0f;
        cardiacAcf(best_h_, Np, kFsHz, acf_bpm, acf_peak);
#ifdef A121_DSP_IBI_TEMPLATE
        /* V3.6: individual beats with a template on the 0.6–8 Hz band of the clearest point; motion damping and removal of the
         * breathing harmonics as for best_h_ */
        sosFiltFiltSos(best_x_, Np, work_ext_, kBp8Sos);
        memcpy(work_x_, work_ext_ + kFiltPad, Np * sizeof(float));
        gateMotion(work_x_, Np);
        removeBreathHarmonics(work_x_, Np, kFsHz, (br_q >= br_min_q) ? fb : 0.0f, hr_prev_ / 60.0f);
        const IbiResult ibi = templateIbi(work_x_, Np, kFsHz, hr_prev_, work_ext_);
#else
        const IbiResult ibi = timeDomainIbi(best_h_, Np, kFsHz, hr_prev_);
#endif
        const float ibi_bpm = ibi.bpm;
        const float acf_res = (acf_peak > 0.0f) ? acf_bpm : 0.0f;  /* 0 = no ACF result */
        /* Confirmation of a short window (V2.4 preliminary, V3.1; V6 tail): autocorrelation or individual beats
         * within ±5 /min of the candidate (f_hz) and not on a low breathing harmonic (m ≤ 3): ACF and
         * individual beats of the same signal see these just the same, so the confirmation does not count there (test bench base with
         * A121_DSP_COMBINE: 46 instead of 71 /min). */
        auto confirmedHz = [&](float f_hz) {
            const float bpm   = f_hz * 60.0f;
            const bool acf_ok = acf_res > 0.0f && fabsf(acf_res - bpm) <= 5.0f;
            const bool ibi_ok = ibi_bpm > 0.0f && fabsf(ibi_bpm - bpm) <= 5.0f;
            const uint8_t m   = harmOrder(f_hz);
            return (acf_ok || ibi_ok) && !(m != 0 && m <= kHarmLowMax);
        };
#ifdef A121_DSP_REACQ
        /* V6: a memory candidate without confirmation falls back to the normal search (strongest candidate); ACF and
         * individual beats stay the same for that (distance ≤ kMemBandBpm). */
        bool short_ok = false;
        if (short_win) {
            if (mem_j > 0 && !confirmedHz(cf[mem_j])) {
                mem_j    = -1;
                hr_prev_ = cf[0] * 60.0f;
            }
            short_ok = confirmedHz(cf[mem_j >= 0 ? mem_j : 0]);
            if (mem_j >= 0 && short_ok) rep.reacq = kReacqMemory;
        }
#endif
        rep.hr_fused_bpm = hr_prev_;

        /* earlier confidence as a diagnostic value: weighted share of the points with their own peak ±5 /min of the result */
        float agree = 0.0f;
        for (uint8_t b = 0; b < kZoomPoints; b++)
            if (fabsf(peak_hz[b] * 60.0f - hr_prev_) <= 5.0f) agree += weight[b];
        rep.point_agree_pct = 100.0f * agree / (wsum + 1e-12f);

        rep.hr_acf_bpm    = (acf_peak > 0.0f) ? acf_bpm : hr_prev_;
        rep.hr_ibi_bpm    = (ibi_bpm > 0.0f) ? ibi_bpm : hr_prev_;
        rep.ibi_median_ms = (ibi_bpm > 0.0f) ? ibi.ibi_ms : 60000.0f / hr_prev_;
        rep.rmssd_ms      = ibi.rmssd_ms;

        /* Confidence (V1.3) */
        QualityInputs qi;
        qi.peak_db         = 10.0f * log10f(sAtBpm(hr_prev_) / comb_med);
        qi.on_harmonic     = isHarmHz(hr_prev_ / 60.0f);
        qi.hr_spec         = rep.hr_spec_bpm;
        qi.hr_acf          = acf_res;                             /* without a result no agreement (@VITAL then shows */
        qi.hr_ibi          = ibi_bpm;                             /* the tracked value as before)                */
        qi.hr_fused        = hr_prev_;
        qi.hr_fused_5s_ago = (hr_hist_n_ >= 5) ? hr_hist_[(hr_hist_n_ - 5) % 5] : 0.0f;
        qi.motion_pct      = 100.0f * (float)moving_p / (float)(total_p ? total_p : 1U);  /* pulse window (= motion_pct) */
        {
            uint16_t n_sat_win = 0;
            for (uint16_t n = 0; n < Np; n++) n_sat_win += hist_sat_[(start_p + n) % kHistoryLen];
            qi.sat_pct = 100.0f * (float)n_sat_win / (float)Np;
        }
        qi.win_frac = (float)Np / (float)kHrWin;
        rep.confidence_pct = signalQuality(qi);
#ifdef A121_DSP_VITERBI
        rep.confidence_pct = fminf(rep.confidence_pct, 100.0f * vit_mass);  /* V3.2: mass ±5 /min */
#endif
#ifdef A121_DSP_REACQ
        if (!prov && !short_win) pushHrHist(hr_prev_);  /* tail: only after confirmation (below) */
#else
        if (!prov) pushHrHist(hr_prev_);
#endif

        /* Sign of life (B19): valid only with a clean echo in the zoom window and breathing (breathing peak/median ≥ kBrMinQ and
         * ≥ 100 µm excursion at the breathing point) or with held breath and a clear pulse peak; additionally at most 30 %
         * motion in the window. The breathing peak alone is not enough: noise points produce peaks even without a person. */
        const bool held_pulse = echo_ok && breath_held && qi.peak_db >= kPulseHeldDb;
#ifdef A121_DSP_REACQ
        /* V6: instead of the 30 % rule the window choice applies (moving with a too-short tail is already invalid above); the pulse
         * reacquired via the memory also counts as a sign of life (breathing peak often smeared after unrest).
         * Tail: only if confirmed; then the value is tracked, otherwise tracking stays as before the report.
         * Deliberate deviation from V2.4: a tail report at the start (short_win together with prov) commits the confirmed
         * value (hr_prev_ stays) instead of discarding it as preliminary (spec point 3: hr_prev_ from a confirmed
         * value); the prov branch below then does not apply.
         * Ruling R15: the memory value replaces the breathing peak only if it lies at most kMemSubstBpm (5 /min) from the
         * remembered pulse (memSubstitutesBreath); farther away (band ±kMemBandBpm) it needs breath_ok like any
         * other report. mem_ is unchanged here since mem_on (reacq = 2 only with an active memory). */
        const bool mem_ok = rep.reacq == kReacqMemory && memSubstitutesBreath(rep.hr_fused_bpm, mem_.hr());
        rep.valid = breath_ok || held_pulse || mem_ok;
        if (short_win) {
            rep.valid = rep.valid && short_ok;
            if (rep.valid) {
                hr_miss_   = 0;
                alt_count_ = 0;
                pushHrHist(hr_prev_);
            } else {
                hr_prev_ = hr_keep;
            }
        }
        const bool prov_check = prov && !short_win;
#else
        rep.valid      = (breath_ok || held_pulse) && rep.motion_pct <= kMaxMotionPct;
        const bool prov_check = prov;
#endif
        if (prov_check) {  /* preliminary only with confirmation of the spectral value (confirmedHz) */
            rep.valid = rep.valid && confirmedHz(cf[0]);
            hr_prev_  = 0.0f;
        }
        if (rep.valid) {
            have_valid_     = true;
            last_valid_tot_ = s.tot_frames;
        }
#ifdef A121_DSP_REACQ
        /* V6: safe value (confidence ≥ kMemMinConf, pulse window ≥ 8 s) into the memory */
        if (rep.valid && rep.confidence_pct >= kMemMinConf && Np >= kHrStableWin)
            mem_.store(rep.hr_fused_bpm, rep.ibi_median_ms, s.tot_frames);
#endif
#ifdef A121_DSP_HARM_REMOVE
        conf_prev_ = rep.valid ? rep.confidence_pct : 0.0f;
#endif
        setState(s, rep, measurementState(person, echo_ok, breath_ok, rep.valid, rep.motion_pct, rep.br_state));
        rep.compute_us = micros() - t0;
        return rep.valid;
    }

private:
    static uint16_t sat16(uint32_t v) { return (v > 0xFFFFU) ? 0xFFFFU : (uint16_t)v; }

    /** hr_fused of the last 5 reports (jump penalty of the confidence) */
    void pushHrHist(float hr) {
        hr_hist_[hr_hist_n_ % 5] = hr;
        if (hr_hist_n_ < 250) hr_hist_n_++;
        else hr_hist_n_ = 5 + (hr_hist_n_ % 5);
    }

    /** Enter the state of the measurement; since_ms counts from the last change, across rezooms (V3.5). */
    void setState(const ReportSnapshot &s, VitalReport &rep, uint8_t st) {
        if (st != state_) {
            state_           = st;
            state_since_tot_ = s.tot_frames;
        }
        rep.state          = st;
        rep.state_since_ms = (uint32_t)lroundf((float)(s.tot_frames - state_since_tot_) * 1000.0f / kFsHz);
    }

    /** Hann window of length M (decimated sequence); recomputed only when M changes (window growth), V2.2. */
    const float *hannWindow(uint16_t M) { return hannInto(hann_, hann_m_, M); }

    static const float *hannInto(float *w, uint16_t &w_m, uint16_t M) {
        if (M != w_m) {
            for (uint16_t m = 0; m < M; m++)
                w[m] = (M > 1) ? 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)m / (float)(M - 1U))) : 1.0f;
            w_m = M;
        }
        return w;
    }

    static float *allocFloat(size_t count) {
        void *p = nullptr;
        if (psramFound()) p = ps_malloc(count * sizeof(float));
        if (!p) p = malloc(count * sizeof(float));
        if (p) memset(p, 0, count * sizeof(float));
        return (float *)p;
    }

    /** Coherent 5-point beam around best_zoom_bin_ (for telemetry); cl_* = the same beam over the clutter estimate. */
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

    /** Unwrap the phase around (ci, cq) → out[] in µm. Returns: mean distance to the center (radius);
     *  span = swept phase (max − min, rad), spread = scatter of the distance relative to the radius. */
    static float unwrapAround(const float *base, uint16_t start, uint16_t N, float ci, float cq, float *out, float &span,
                              float &spread) {
        float prev = 0.0f, acc = 0.0f, lo = 0.0f, hi = 0.0f, r_sum = 0.0f, r2_sum = 0.0f;
        for (uint16_t n = 0; n < N; n++) {
            const float *z = base + (size_t)((start + n) % kHistoryLen) * 2U;
            const float di = z[0] - ci, dq = z[1] - cq;
            const float phi = fastAtan2(dq, di);
            const float r2 = di * di + dq * dq;
            r_sum += sqrtf(r2);
            r2_sum += r2;
            if (n > 0) {
                float d = phi - prev;
                if (d > (float)M_PI) d -= 2.0f * (float)M_PI;
                else if (d < -(float)M_PI) d += 2.0f * (float)M_PI;
                acc += d;
            } else {
                acc = lo = hi = phi;
            }
            if (acc < lo) lo = acc;
            if (acc > hi) hi = acc;
            prev   = phi;
            out[n] = acc * kRadToUm;
        }
        const float r = r_sum / (float)N;
        const float var = r2_sum / (float)N - r * r;
        span   = hi - lo;
        spread = (r > 1e-6f && var > 0.0f) ? sqrtf(var) / r : 0.0f;
        return r;
    }

    /** Unwrap the phase of zoom point b around the circle center (circleCenter) → µm. If the phase sweeps less than
     *  kArcMinRad, the fit is poorly determined (short arc): then the last good center of this point applies,
     *  as long as the IQ values lie on its circle (scatter at most twice as large as around the fit), and short_arc
     *  tells the caller to halve the weight of the point. Returns: mean distance to the center (radius),
     *  spread: scatter of the distance relative to the radius (small = clean echo, ≈ 0.5 = noise only). */
    float binDisplacement(uint8_t b, uint16_t start, uint16_t N, float *out, bool &short_arc, float &spread,
                          float *c_out = nullptr) {
        const float *base = hist_zoom_ + (size_t)b * kHistoryLen * 2U;  /* ring of this point, contiguous in PSRAM */
        float mi = 0.0f, mq = 0.0f;
        for (uint16_t n = 0; n < N; n++) {
            const float *z = base + (size_t)((start + n) % kHistoryLen) * 2U;
            mi += z[0];
            mq += z[1];
        }
        mi /= (float)N;
        mq /= (float)N;
        float ci, cq, span;
        const bool fit_ok = circleCenter(base, start, N, mi, mq, ci, cq);
        float radius = unwrapAround(base, start, N, ci, cq, out, span, spread);
        short_arc = span < kArcMinRad;
        if (!short_arc) {
            if (fit_ok) {
                good_ci_[b]   = ci;
                good_cq_[b]   = cq;
                have_good_[b] = true;
            }
        } else if (have_good_[b]) {
            float span_g, spread_g;
            const float r_g = unwrapAround(base, start, N, good_ci_[b], good_cq_[b], work_ext_, span_g, spread_g);
            if (spread_g <= 2.0f * spread + 0.02f) {
                memcpy(out, work_ext_, N * sizeof(float));
                radius = r_g;
                spread = spread_g;
                ci     = good_ci_[b];
                cq     = good_cq_[b];
            } else {
                have_good_[b] = false;  /* scene has changed: old center no longer fits */
            }
        }
        if (c_out) {  /* center used (V3.1) */
            c_out[0] = ci;
            c_out[1] = cq;
        }
        return radius;
    }

#ifdef A121_DSP_COMBINE
    /** V3.1: shift the chosen points (use) around their circle centers (binDisplacement, falling back to the
     *  last good center) and combine into the 1st principal component (combinePrincipal). Points with noise instead of
     *  echo (scatter ≥ kEchoSpreadMax) stay out unless there is no other. The path signal of the channel is
     *  then in work_x_ (µm). Returns: dominant zoom point; amp/short_arc/spread as binDisplacement. */
    uint8_t combineZoom(uint16_t start, uint16_t N, const bool *use, float &amp, bool &short_arc, float &spread,
                        float &n_eff) {
        const float *rings[kCombKMax];
        float ci[kCombKMax], cq[kCombKMax];
        uint8_t idx[kCombKMax], K = 0, n_clean = 0;
        bool clean[kCombKMax];
        for (uint8_t b = 0; b < kZoomPoints && K < kCombK; b++) {
            if (!use[b]) continue;
            bool sa = false;
            float sp = 0.0f, c[2];
            binDisplacement(b, start, N, work_x_, sa, sp, c);
            rings[K] = hist_zoom_ + (size_t)b * kHistoryLen * 2U;
            ci[K]    = c[0];
            cq[K]    = c[1];
            idx[K]   = b;
            clean[K] = sp < kEchoSpreadMax;
            n_clean += clean[K];
            K++;
        }
        if (n_clean > 0 && n_clean < K) {  /* keep only clean points */
            uint8_t w = 0;
            for (uint8_t j = 0; j < K; j++)
                if (clean[j]) {
                    rings[w] = rings[j];
                    ci[w]    = ci[j];
                    cq[w]    = cq[j];
                    idx[w]   = idx[j];
                    w++;
                }
            K = w;
        }
        uint8_t dom = 0;
        n_eff = combinePrincipal(rings, ci, cq, K, start, N, comb_y_, dom);
        float span = 0.0f;
        amp       = unwrapAround(comb_y_, 0, N, 0.0f, 0.0f, work_x_, span, spread);
        short_arc = span < kArcMinRad;
        return K ? idx[dom] : 0U;
    }
#endif

    /** Zero-phase bandpass 0.6–5 Hz (kBpSos) like scipy `sosfiltfilt`. Result: work_ext_[kFiltPad .. kFiltPad + N). */
    void sosFiltFilt(const float *x, uint16_t N) { sosFiltFiltSos(x, N, work_ext_, kBpSos); }

    /** Scatter of the IQ values of each zoom point in the window (≈ amplitude² × phase excursion², shifted by the first value). */
    void zoomStrength(uint16_t start, uint16_t N, float *strength) const {
        for (uint8_t b = 0; b < kZoomPoints; b++) {
            const float *base = hist_zoom_ + (size_t)b * kHistoryLen * 2U;
            const float *z0 = base + (size_t)start * 2U;  /* shifted by the first value (cancellation) */
            float mi = 0.0f, mq = 0.0f, p2 = 0.0f;
            for (uint16_t n = 0; n < N; n++) {
                const float *z = base + (size_t)((start + n) % kHistoryLen) * 2U;
                const float di = z[0] - z0[0], dq = z[1] - z0[1];
                mi += di;
                mq += dq;
                p2 += di * di + dq * dq;
            }
            mi /= (float)N;
            mq /= (float)N;
            strength[b] = p2 / (float)N - (mi * mi + mq * mq);
        }
    }

#ifdef A121_DSP_REACQ
    /** V6: motion map of the window start .. start + N from the kMotionPoints strongest points under use: path signal as
     *  in the main loop (binDisplacement, trend, bandpass), RMS per 1 s block (without gateMotion damping),
     *  then motionMapFromRms. Uses work_x_ and work_ext_ as a work array (overwrites the path signal of the main loop);
     *  binDisplacement maintains good_ci_/good_cq_/have_good_ as there. No heap: 3 × kMaxBlocks values on the stack. */
    MotionMap motionMap(uint16_t start, uint16_t N, const bool *use) {
        const uint8_t nb = (uint8_t)std::min<uint16_t>(N / kGateBlk, kMaxBlocks);
        float strength[kZoomPoints];
        zoomStrength(start, N, strength);
        for (uint8_t b = 0; b < kZoomPoints; b++)
            if (!use[b]) strength[b] = -INFINITY;
        bool sel[kZoomPoints];
        selectStrongest(strength, kZoomPoints, kMotionPoints, sel);
        float rms[kMotionPoints * kMaxBlocks];
        uint8_t pts[kMotionPoints];
        uint8_t n_pts = 0;
        for (uint8_t b = 0; b < kZoomPoints && nb; b++)
            if (sel[b] && use[b]) pts[n_pts++] = b;  /* selectStrongest pads with unused points when use has few points */
#ifdef A121_DSP_REACQ_PERIOD
        /* the strongest point comes last: its filtered path signal stays in work_ext_ for the periodicity check */
        if (n_pts > 1U) {
            uint8_t top = 0;
            for (uint8_t i = 1; i < n_pts; i++)
                if (strength[pts[i]] > strength[pts[top]]) top = i;
            const uint8_t t = pts[top];
            for (uint8_t j = top; j + 1U < n_pts; j++) pts[j] = pts[j + 1U];
            pts[n_pts - 1U] = t;
        }
#endif
        for (uint8_t i = 0; i < n_pts; i++) {
            bool short_arc = false;
            float spread = 0.0f;
            binDisplacement(pts[i], start, N, work_x_, short_arc, spread);
            removeLinearTrend(work_x_, N);
            sosFiltFilt(work_x_, N);
            const float *h = work_ext_ + kFiltPad;
            for (uint8_t k = 0; k < nb; k++) {
                const uint16_t a = k * kGateBlk, e = (k + 1U == nb) ? N : (uint16_t)(a + kGateBlk);  /* as gateMotion */
                float acc = 0.0f;
                for (uint16_t n = a; n < e; n++) acc += h[n] * h[n];
                rms[i * nb + k] = sqrtf(acc / (float)(e - a));
            }
        }
        MotionMap mm = motionMapFromRms(rms, n_pts, nb);
#ifdef A121_DSP_REACQ_PERIOD
        /* Variant: a block that is too strong counts as moving only if the signal around it (3 s at the strongest point) is not
         * periodic. The segment is in dec_ (refilled in the main loop before use). */
        if (mm.mask) {
            const float *h = work_ext_ + kFiltPad;  /* path of the strongest point (computed last) */
            for (uint8_t k = 0; k < mm.n_blocks; k++) {
                if (!(mm.mask & ((uint32_t)1U << k))) continue;
                const uint16_t a = k * kGateBlk, e = (k + 1U == nb) ? N : (uint16_t)(a + kGateBlk);
                const uint16_t n = periodSegment(h, N, (uint16_t)((a + e) / 2U), dec_);
                if (segmentPeriodic(dec_, n, fft_, spec_)) mm.mask &= ~((uint32_t)1U << k);
            }
            mm.clean_tail_blocks = mm.n_blocks;
            for (uint8_t k = 0; k < mm.n_blocks; k++)
                if (mm.mask & ((uint32_t)1U << k)) mm.clean_tail_blocks = (uint8_t)(mm.n_blocks - 1U - k);
        }
#endif
        return mm;
    }
#endif

    /* Motion damping in the heart band: RMS per 1 s block. The reference is the quiet quarter of the blocks (not the
     * median: with 8 s of motion in 15 s the median itself is motion). Blocks above kGateK × reference get the
     * gain (limit/RMS)^kGateP, blended linearly between the block centers. A few seconds of
     * motion would otherwise mask 10 s of clean pulse (checked on the dataset with inserted motion).
     * Returns: number of blocks above kMotionK × reference (motion). */
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
            const float pos = ((float)n - half) / (float)kGateBlk;  /* block centers at 0, 1, 2, ... */
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

    /** Goertzel power without a window (the window is already in the signal). */
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

    /** Lower quartile of 80 values (noise level; robust as long as fewer than three quarters of the room contain echoes). */
    static float lowerQuartile(const float *v) {
        float tmp[kRoomPoints];
        memcpy(tmp, v, sizeof(tmp));
        std::nth_element(tmp, tmp + kRoomPoints / 4, tmp + kRoomPoints);
        return tmp[kRoomPoints / 4];
    }

    /** Local maxima of max(amplitude, motion) relative to the noise from kRoomDetK (with equally high neighbors the earlier
     *  one counts). f(r, d, moving) per peak; d = parabolic refinement in points (−0.5 … 0.5). */
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

    void extractRoomTargets(const ReportSnapshot &s, VitalReport &rep) {
        rep.num_room_targets = 0;
        /* Noise level separately for amplitude and motion. Pure noise has motion ≈ amplitude (the static image
         * averages it away); on the 2026-11-01 dataset both ≈ 55, the person up to 3300 / 2600. +1 against division by 0
         * in the first frames. */
        const float noise_amp = lowerQuartile(s.room_amp) + 1.0f;
        const float noise_mot = lowerQuartile(s.room_mot) + 1.0f;
        rep.room_noise_amp    = noise_amp;
        rep.room_noise_motion = noise_mot;

        /* Target = local maximum of max(amplitude, motion) relative to the noise, from kRoomDetK. So fixed echoes count
         * (wall, furniture: amplitude high, motion ≈ noise) and weak moving echoes. The
         * kMaxRoomTargets strongest by amplitude + 8 × motion are kept (motion counts more), regardless of distance. */
        auto key = [](const RoomTarget &t) { return t.amplitude + 8.0f * t.motion_energy; };
        forEachRoomPeak(s.room_amp, s.room_mot, noise_amp, noise_mot, [&](uint16_t r, float d, bool moving) {
            const RoomTarget t = {roomDistM(r, d), s.room_amp[r], s.room_mot[r], moving};
            uint8_t n = rep.num_room_targets;
            if (n == kMaxRoomTargets && key(t) <= key(rep.room_targets[n - 1])) return;
            uint8_t pos = (n < kMaxRoomTargets) ? n : (uint8_t)(n - 1U);  /* full list: weakest target drops out */
            while (pos > 0 && key(rep.room_targets[pos - 1]) < key(t)) {
                rep.room_targets[pos] = rep.room_targets[pos - 1];
                pos--;
            }
            rep.room_targets[pos] = t;
            if (n < kMaxRoomTargets) rep.num_room_targets++;
        });

        /* Zoom target: strongest moving target (amplitude + 8 × motion) in the smoothed profile (τ ≈ 3 s), not in the
         * snapshot. Live 2026-11-01 two moving echoes lay close together (chest 0.66 m with fluctuating
         * amplitude 440–1220, second echo 0.78 m steady ≈ 950, but half as much motion); the snapshot at
         * t = 3 s chose the rear one by 1 %, and the pulse was worse there (confidence 78 instead of 99 %). Smoothed, the
         * choice hits the chest in all four logs (τ = 1–8 s checked). The strongest echo alone would have been wrong.
         * The window holds only this target (±60 mm), not every moving echo: otherwise it stayed on the rear echo for 19 s. */
        const float ns_amp = lowerQuartile(s.room_amp_slow) + 1.0f;
        const float ns_mot = lowerQuartile(s.room_mot_slow) + 1.0f;
        float best_m = -1.0f, best_key = 0.0f;
        forEachRoomPeak(s.room_amp_slow, s.room_mot_slow, ns_amp, ns_mot, [&](uint16_t r, float d, bool moving) {
            const float k = s.room_amp_slow[r] + 8.0f * s.room_mot_slow[r];
            if (moving && (best_m < 0.0f || k > best_key)) {
                best_m   = roomDistM(r, d);
                best_key = k;
            }
        });

        /* Rezoom only if the zoom target does not lie in the zoom window ±60 mm (two room steps), and only after
         * 3 reports in the first 10 s (find the target) or 5 reports after that. Every rezoom clears the 15 s history.
         * Trade-off on 2026-11-01: without tolerance the zoom switched between body locations (room target 660 mm, best
         * pulse at 620 mm); with ±30 mm it wanted to go 20 times from 570–630 to 630–690 mm; with ±150 mm a target at
         * 0.68 m would stay stuck next to a window at 0.50–0.56 m. At the body edge (window 500–560 mm, chest 580–620 mm)
         * the pulse live still delivered 100 % within ±8 /min. */
        const float zoom_min_m = (float)(zoom_start_pt_ + 3) * kPointStepM - 0.06f;
        const float zoom_max_m = (float)(zoom_start_pt_ + (int32_t)kZoomPoints - 4) * kPointStepM + 0.06f;
        rep.needs_rezoom              = false;
        rep.recommended_zoom_start_pt = zoom_start_pt_;
        rep.person_in_room = best_m >= 0.0f;
        rep.target_in_zoom = best_m >= 0.0f && best_m >= zoom_min_m && best_m <= zoom_max_m;
        if (best_m < 0.0f || (best_m >= zoom_min_m && best_m <= zoom_max_m)) {
            out_of_window_frames_ = 0;
            return;
        }
        /* Start (no restart since begin() yet): zoom after 2 s, i.e. at the 2nd report (V2.4); otherwise 3 or 5 */
        const bool start = s.tot_frames == s.frame_count && s.frame_count <= kStartZoomFrames;
        const uint16_t needed = start ? 2U : ((s.frame_count < 600U) ? 3U : 5U);
        if (++out_of_window_frames_ >= needed) {
            int32_t new_start = (int32_t)roundf(best_m / kPointStepM) - (int32_t)(kZoomPoints / 2U);
            if (new_start < 80) new_start = 80; /* ≥ 0.20 m */
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
#ifdef A121_DSP_VITERBI
    ViterbiTracker vit_;                                           /* V3.2, report side */
#endif
#ifdef A121_DSP_HARM_REMOVE
    static_assert(kHrWin / kDecim + 1U <= kGuideFft, "Fenster nach Dezimierung passt nicht in kGuideFft");
    a121_fft::ComplexFft<kGuideFft> cfft_;                        /* V3.3, report side */
    float    guide_disp_[kGuideFft];
    float    guide_work_[2U * kGuideFft];
    float    fb_prev_              = 0.0f;  /* breathing rate of the previous report (0 = no breathing peak) */
    float    conf_prev_            = 0.0f;  /* confidence of the previous report (0 = invalid) */
#endif
    BreathDetector breath_det_;                                    /* V3.4, push side only */
    BreathEvent    breath_ev_;
    bool     breath_pending_       = false;
    bool     breath_gate_          = false;  /* V3.4, push side */
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
    uint8_t  alt_count_            = 0;     /* reports in a row with an alternative to the harmonic */
    BreathStateTracker br_track_;           /* breathing state (V1.4) */
    uint32_t tot_delayed_ = 0, tot_sat_ = 0, tot_tickgap_ = 0;    /* sensor counters since begin() (V1.5, sums since V2.1) */
    uint32_t prev_delayed_ = 0, prev_sat_ = 0, prev_tickgap_ = 0; /* … at the last computed report */
    uint32_t tot_frames_ = 0;                                      /* frames since begin() (V2.3) */
    uint32_t last_valid_tot_ = 0;                                  /* tot_frames of the last valid report */
    bool     have_valid_ = false;
    bool     prov_ = true;                                         /* tracking still preliminary (window < 8 s, V2.4) */
    uint8_t  state_ = 0xFFU;                                       /* last state of the measurement (V3.5) */
    uint8_t  person_run_ = 0;                                      /* reports in a row with a zoom target in the room profile */
    uint32_t state_since_tot_ = 0;
    ReportSnapshot snap_ = {};                                     /* for computeVitalReport(rep) without its own state */
    a121_fft::RealFft<kFftN> fft_;                                  /* spectra per zoom point (V2.2) */
#ifdef A121_DSP_PHASE_SLOPE
    a121_fft::ComplexFft<kFftN> psl_fft_;                           /* V6 phase slope, report side: 6 KB twiddle factors */
    float    psl_re_[kFftN];                                        /* … work memory 2 × 4 KB, only in the report */
    float    psl_im_[kFftN];
#endif
    float    hann_[kHrWin / kDecim + 1U] = {};
    uint16_t hann_m_ = 0;
#ifdef A121_DSP_REACQ
    float    hann_p_[kHrWin / kDecim + 1U] = {};  /* V6: Hann for the shorter pulse window (tail after motion) */
    uint16_t hann_p_m_ = 0;
    ReacqMemory mem_;                              /* V6: last secured pulse */
#endif
    uint32_t last_tick_ = 0;
    bool     have_last_tick_ = false;
    uint8_t  hist_sat_[kHistoryLen] = {};   /* data_saturated per frame of the history */
    float    hr_hist_[5]           = {};    /* hr_fused of the last 5 reports (jump penalty) */
    uint8_t  hr_hist_n_            = 0;

    float good_ci_[kZoomPoints]        = {};  /* last well-determined circle center per zoom point (arc ≥ 2 rad) */
    float good_cq_[kZoomPoints]        = {};
    bool  have_good_[kZoomPoints]      = {};
    float zoom_clutter_i_[kZoomPoints] = {};
    float zoom_clutter_q_[kZoomPoints] = {};
    float zoom_var_ema_[kZoomPoints]   = {};
    float room_clutter_i_[kRoomPoints] = {};
    float room_clutter_q_[kRoomPoints] = {};
    float room_amp_ema_[kRoomPoints]   = {};
    float room_var_ema_[kRoomPoints]   = {};
    float room_amp_slow_[kRoomPoints]  = {};  /* room_amp_ema_ additionally smoothed (τ ≈ 3 s), for the zoom target */
    float room_mot_slow_[kRoomPoints]  = {};  /* room_var_ema_ additionally smoothed (τ ≈ 3 s) */

    float *hist_zoom_   = nullptr;  /* [kZoomPoints][kHistoryLen][I,Q], derotated, one ring per point */
    float *hist_lb_phi_ = nullptr;
    float *work_x_      = nullptr;
    float *work_ext_    = nullptr;  /* sosfiltfilt with padding: kHrWin + 2 * kFiltPad */
#ifdef A121_DSP_COMBINE
    float *comb_y_      = nullptr;  /* combined channel [I,Q] × kHrWin (V3.1) */
#endif
    float *best_x_      = nullptr;
    float *best_h_      = nullptr;
    float *br_x_        = nullptr;  /* path signal of the breathing point (strongest breathing among the points with a clean echo) */
    float *dec_         = nullptr;
    float *spec_        = nullptr;  /* P(f) per point */
    float *sum_         = nullptr;  /* S(f) = P(f) + 0.5 · P(2f) per point */
    float *comb_        = nullptr;
    float *scratch_     = nullptr;
    float *br_comb_     = nullptr;
    float *pen_         = nullptr;  /* comb_ with penalty on breathing harmonics (candidate selection) */
};

/*
 * Output lines for room targets and room profile (`iq3`). Firmware and PC check (host/a121_esp_ultimate.py, verify-cpp)
 * use the same functions so that the format matches. tick = sensor timestamp in ms; it keeps running without
 * gaps, also across compute pauses and rezooms. Returns: length without line ending (truncated if the buffer is too small).
 *   @ROOM,t_ms,tick,rausch_amp,rausch_bew,n,{dist_mm,amp,bew,bewegt}×n      (targets sorted by strength)
 *   @ROOMPROF,t_ms,tick,start_mm,step_mm,n,amp_0..amp_n-1,bew_0..bew_n-1    (raw values, smoothed)
 */
static constexpr size_t kRoomLineMax = 1400U;  /* enough for @ROOMPROF with 2 × 80 values (≤ ~1100 characters) */

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

/*
 * Report lines (`iq3`), shared by firmware, PC/phone replay (host/dsp_replay) and test bench
 * (host/vital_sim.py). Only append new fields at the end; @VITAL keeps its 15 fields (docs/ANDROID_APP.md).
 *   @DSP,t_ms,ok,hr_bin,konfidenz_pct,compute_us,motion_pct,hr_harm,point_agree_pct,br_state,n_delayed,n_sat,n_tickgap
 *        (+ with A121_DSP_COMBINE n_eff as the 15th field; + with A121_DSP_REACQ: n_eff (possibly empty), win_s, reacq)
 *   @VITAL,t_ms,br,hr_fused,hr_spec,hr_acf,hr_ibi,ibi_ms,rmssd_ms,konfidenz,resp_um,heart_um,snr_db,lb_std_mrad,
 *          target_mm,n_room
 *   @REZOOM,zoom_start_mm,zoom_end_mm
 * compute_us separate, because the replay takes over the compute time of the ESP32.
 */
#define A121_DSP_FORMATS 1  /* host/dsp_replay uses this to recognize headers from V0.4 (older ones get the old formats there) */
static constexpr size_t kReportLineMax = 256U;

inline size_t clampLen(int len, size_t cap) { return (len < 0) ? 0U : std::min((size_t)len, cap - 1U); }

inline size_t formatDspLine(char *buf, size_t cap, uint32_t t_ms, bool ok, const VitalReport &rep, uint32_t compute_us) {
#ifdef A121_DSP_REACQ
    /* V6: field 14 n_eff (number with A121_DSP_COMBINE, otherwise empty), field 15 win_s, field 16 reacq; only with the switch */
    char n_eff[16] = "";
#ifdef A121_DSP_COMBINE
    snprintf(n_eff, sizeof n_eff, "%.1f", rep.n_eff);
#endif
    return clampLen(snprintf(buf, cap, "@DSP,%lu,%u,%u,%.1f,%lu,%.0f,%u,%.0f,%u,%u,%u,%u,%lu,%s,%u,%u",
                             (unsigned long)t_ms, (unsigned)ok, (unsigned)rep.hr_bin, rep.confidence_pct,
                             (unsigned long)compute_us, rep.motion_pct, (unsigned)rep.hr_harm, rep.point_agree_pct,
                             (unsigned)rep.br_state, (unsigned)rep.n_delayed, (unsigned)rep.n_sat,
                             (unsigned)rep.n_tickgap, (unsigned long)rep.frame_count, n_eff, (unsigned)rep.win_s,
                             (unsigned)rep.reacq),
                    cap);
#elif defined(A121_DSP_COMBINE)
    /* V3.1: effective point count as the 15th field, only with the switch (standard line unchanged) */
    return clampLen(snprintf(buf, cap, "@DSP,%lu,%u,%u,%.1f,%lu,%.0f,%u,%.0f,%u,%u,%u,%u,%lu,%.1f", (unsigned long)t_ms,
                             (unsigned)ok, (unsigned)rep.hr_bin, rep.confidence_pct, (unsigned long)compute_us,
                             rep.motion_pct, (unsigned)rep.hr_harm, rep.point_agree_pct, (unsigned)rep.br_state,
                             (unsigned)rep.n_delayed, (unsigned)rep.n_sat, (unsigned)rep.n_tickgap,
                             (unsigned long)rep.frame_count, rep.n_eff),
                    cap);
#else
    return clampLen(snprintf(buf, cap, "@DSP,%lu,%u,%u,%.1f,%lu,%.0f,%u,%.0f,%u,%u,%u,%u,%lu", (unsigned long)t_ms,
                             (unsigned)ok, (unsigned)rep.hr_bin, rep.confidence_pct, (unsigned long)compute_us,
                             rep.motion_pct, (unsigned)rep.hr_harm, rep.point_agree_pct, (unsigned)rep.br_state,
                             (unsigned)rep.n_delayed, (unsigned)rep.n_sat, (unsigned)rep.n_tickgap,
                             (unsigned long)rep.frame_count),
                    cap);
#endif
}

inline size_t formatVitalLine(char *buf, size_t cap, uint32_t t_ms, const VitalReport &r) {
    return clampLen(snprintf(buf, cap, "@VITAL,%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.1f,%.1f,%.1f,%.2f,%.0f,%u",
                             (unsigned long)t_ms, r.br_bpm, r.hr_fused_bpm, r.hr_spec_bpm, r.hr_acf_bpm, r.hr_ibi_bpm,
                             r.ibi_median_ms, r.rmssd_ms, r.confidence_pct, r.resp_depth_um, r.heart_amp_um,
                             r.zoom_snr_db, r.loopback_phase_std_mrad, r.target_dist_m * 1000.0f,
                             (unsigned)r.num_room_targets),
                    cap);
}

#define A121_DSP_STATE 1
/** @STATE,t_ms,zustand,seit_ms (V3.5): per report after @VITAL or @ROOM. */
inline size_t formatStateLine(char *buf, size_t cap, uint32_t t_ms, const VitalReport &r) {
    return clampLen(snprintf(buf, cap, "@STATE,%lu,%u,%lu", (unsigned long)t_ms, (unsigned)r.state,
                             (unsigned long)r.state_since_ms),
                    cap);
}

/** @BREATH,t_ms,tick,intervall_ms,tiefe_um,rate (V3.4): per detected breath, directly after @IQ3/@RAW of the frame.
 *  rate = 60 / median of the last (up to) 4 distances, /min. */
inline size_t formatBreathLine(char *buf, size_t cap, uint32_t t_ms, uint32_t tick, const BreathEvent &ev) {
    return clampLen(snprintf(buf, cap, "@BREATH,%lu,%lu,%lu,%.0f,%.1f", (unsigned long)t_ms, (unsigned long)tick,
                             (unsigned long)ev.interval_ms, ev.depth_um, ev.rate_cpm),
                    cap);
}

inline size_t formatRezoomLine(char *buf, size_t cap, int32_t zoom_start_pt) {
    return clampLen(snprintf(buf, cap, "@REZOOM,%ld,%ld", (long)(zoom_start_pt * 25 / 10),
                             (long)((zoom_start_pt + (int32_t)kZoomPoints) * 25 / 10)),
                    cap);
}

} /* namespace a121_dsp */

#endif /* A121_ESP_DSP_H */
