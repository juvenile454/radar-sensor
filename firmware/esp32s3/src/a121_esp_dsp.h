/**
 * @file a121_esp_dsp.h
 * @brief On-Device-Auswertung der 3-Subsweep-Sparse-IQ-Session (`iq3`) auf dem ESP32-S3 (PSRAM).
 *
 * Kette (Stand 2026-11-01, an `iq3raw_normal_120s_2026-11-01.npz` und `iq3raw_motion_120s_2026-11-01.npz` entwickelt):
 *   1. Subsweep 0: On-Chip-Loopback → Phasen-Derotation aller Zoom-Punkte pro Frame (PLL-/Taktjitter).
 *   2. Subsweep 1: alle 24 Zoom-Punkte (60 mm, 2,5-mm-Raster) werden einzeln gespeichert (18 s Historie: 15-s-Fenster
 *      und 3 s, in denen der Report aus seinem Schnappschuss rechnet, während weiter Frames ankommen; V2.1).
 *   3. Puls: pro Punkt Phase um den Kreismittelpunkt der IQ-Werte (Kåsa-Fit, circleCenter) abwickeln (→ µm), linear entzerren, Nullphasen-Bandpass
 *      0,6–5,0 Hz (Butterworth 4. Ordnung, identisch zu scipy `sosfiltfilt`), Bewegungsdämpfung je 1-s-Block
 *      (Bezug: ruhiges Viertel des Fensters), auf 15 Hz dezimieren, Hann, reelle FFT (1024 Punkte, a121_fft.h, V2.2;
 *      vorher Goertzel je Frequenz), Oberwellen-Summe S(f) = P(f) + 0,5 · P(2f) für 0,645–3,03 Hz; Spitzen zählen nur
 *      zwischen 0,674 und 3,003 Hz (40–180 /min, V1.2; vorher 55–136), parabolisch verfeinert. Phase per fastAtan2.
 *      Die Spektren aller Punkte werden mit (Spitze/Median)² gewichtet gemittelt;
 *      Punkte mit klarem Puls zählen mehr. Atem-Oberwellen werden im Spektrum NICHT herausprojiziert: Liegt der Puls
 *      nahe m × Atemfrequenz, entfernt die Projektion den Herzanteil mit (am Datensatz: 13–42 % statt 61–89 % Treffer).
 *   4. Kandidaten (V1.3): Top 3 der Spitzen von S mit Abschlag auf Atem-Oberwellen (m = 4..8: × 0,5 bei ±0,04 Hz;
 *      m = 2, 3: × 0,02 in der ganzen Hauptkeule ±0,1 Hz). Oktav-Prüfung je Kandidat: Spitze bei f/2 mit
 *      S(f/2) ≥ 0,5 · S(f) → Kandidat steht für f/2 (2. Herz-Oberwelle), außer f/2 liegt bei 2 × / 3 × Atmung.
 *      Verfolgung: naher Kandidat (±12 /min) mit ≥ 50 % des besten Werts gewinnt (Glättung 0,7/0,3); ein weiter
 *      Sprung gilt nach 3 Reports, auf eine Atem-Oberwelle erst nach 10. Liegt der verfolgte Wert auf 2 × / 3 ×
 *      Atmung, wechselt er nach 3 Reports zu einem Kandidaten ohne Oberwelle. hr_harm = 1 heißt nur "Spitze liegt auf
 *      einer Oberwelle"; der Puls kann dort liegen (live 2026-11-01: Puls 78 ≈ 4 × Atmung 19,5).
 *   5. Konfidenz (V1.3, signalQuality): Spitzenhöhe, Oberwelle, Einigkeit von Spektrum, Autokorrelation und
 *      Einzelschlägen, Bewegung, Sprung. Die frühere Punkt-Einigkeit steht als point_agree_pct hinten in @DSP.
 *      motion_pct = Anteil der 1-s-Blöcke mit mehr als 3-facher Ruhe-Amplitude im Herzband (Bewegungshinweis).
 *      Gültig nur mit Lebenszeichen: sauberes Echo und Atmung (≥ 100 µm) oder angehaltener Atem mit klarer Pulsspitze,
 *      höchstens 30 % Bewegung. Start (V2.4): Fenster wächst ab 6 s bis 15 s, Konfidenz × N/900; unter 8 s ist der
 *      Wert vorläufig (nicht verfolgt) und gilt nur, wenn Autokorrelation oder Einzelschläge ihn bestätigen und er
 *      nicht auf einer Atem-Oberwelle m ≤ 3 liegt (V3.1). Variante A121_DSP_COMBINE (V3.1): statt Einzelspektren die
 *      1. Hauptkomponente der 12 stärksten Punkte als ein Kanal (combinePrincipal), effektive Punktzahl hinten in @DSP.
 *      Variante A121_DSP_IBI_TEMPLATE (V3.6): Einzelschläge im Band 0,6–8 Hz über Vorlage und Korrelationsfilter
 *      (templateIbi) statt Spitzen im Pulsband. A121_DSP_HEAVY (V5.2, nur Handy): A121_DSP_VITERBI und
 *      A121_DSP_HARM_REMOVE zusammen.
 *   6. Atemfrequenz aus der mit Amplitude² gewichteten Leistungssumme aller Zoom-Punkte: größtes echtes lokales
 *      Maximum in 0,115–0,585 Hz, sonst 0 (keine Atemspitze). Vorher ungewichtet und größtes Bin; das klebte in rund
 *      einem Viertel der Reports am Rand bei 6,9 /min (schwache Punkte, Lagewechsel). Atemhub am Atem-Punkt (stärkste
 *      Atmung mit sauberem Echo). Herz-Autokorrelation und Zeitbereichs-Schläge (IBI, RMSSD) auf dem Punkt mit dem
 *      klarsten Puls, nach Abzug der Atem-Oberwellen außer denen am Puls (removeBreathHarmonics).
 *   7. Subsweep 2: Raum-Scan 0,30–2,67 m (30 mm) mit Rezoom-Empfehlung. Rauschpegel = unteres Viertel aller 80 Punkte
 *      (Amplitude und Bewegung getrennt). Ziel = lokales Maximum ab 2 × Rauschpegel, "bewegt" ab 3 × Bewegungs-Rauschpegel;
 *      gemeldet werden die 8 stärksten. Vorher galten feste Rohwerte (Wert > 120, Bewegung > 12), die unter dem Rauschen
 *      lagen (≈ 55 / 55), und nur die 4 nächsten Spitzen. Folge: Rausch-Spitzen im Zoom-Fenster blockierten den Rezoom.
 *      Rezoom auf das stärkste bewegte Ziel im über ≈ 3 s geglätteten Profil (nicht in der Momentaufnahme, die bei zwei
 *      nahen Echos kippte); kein Rezoom, solange dieses Ziel im Zoom-Fenster ±60 mm liegt (vorher wechselte der Zoom
 *      zwischen Körperstellen und löschte jedes Mal die Historie).
 *   V6 (A121_DSP_REACQ, Standard aus; Entwurf docs/superpowers/specs/2026-11-02-puls-wiederfinden-design.md):
 *      Puls nach Bewegung wiederfinden. Bewegungskarte vor den Spektren (motionMap: 3 stärkste Punkte, 1-s-Block bewegt,
 *      wenn bei 2 von 3 über 3 × ruhiges Viertel). Ohne bewegten Block wie oben. Bewegt und ruhiges Endstück ≥ 6 s: Puls
 *      (Spektren, Kandidaten, AKF, Einzelschläge) nur auf dem Endstück, Atmung, Atemzustand und motion_pct auf dem
 *      ganzen Fenster; der Wert gilt wie vorläufig nur mit Bestätigung durch AKF oder Einzelschläge (±5 /min) und nicht
 *      auf einer Atem-Oberwelle m ≤ 3, die Verfolgung wird nicht fortgeschrieben, hr_prev_ nur aus einem bestätigten
 *      Wert. Endstück kürzer: kein Puls, Verfolgung (hr_prev_, hr_miss_, alt_count_, Viterbi, hr_hist_) unverändert.
 *      Gedächtnis (ReacqMemory): sicherer Wert (Konfidenz ≥ 50 %, Fenster ≥ 8 s) gilt 60 s, solange Echo und Ziel im
 *      Zoom bleiben; im Endstück wird zuerst der stärkste Kandidat ±15 /min um ihn gewählt (preferMemoryCandidate),
 *      bestätigt zählt er als Lebenszeichen (reacq = 2). Die 30-%-Bewegungsregel entfällt. @DSP: win_s, reacq.
 *      Variante A121_DSP_REACQ_PERIOD (nur mit A121_DSP_REACQ; VERWORFEN, Standard aus, Code als Experiment behalten):
 *      ein zu starker Block zählt nur als bewegt, wenn das Signal um ihn (3 s, 15 Hz, Hann, Herzband) nicht periodisch ist
 *      (Spitze < 5 × Mittel der Bins außerhalb der Hauptkeule; segmentPeriodic). Bench (Simulation): hält die Grenzen
 *      nicht (beweg hr_mae, kurz, zappel, beweg_weg falsch nach Bewegung), tippen bleibt bei 4 falschen Werten; die
 *      simulierte Bewegung ist ein Sinus, also periodisch. Zahlen in docs/EXPERIMENTE.md (V6).
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

#include "a121_fft.h"

/* Handy-Fassung (V5.2): die rechenaufwendigeren Varianten zusammen, nur in der App-Bibliothek radardsp_live. */
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
static constexpr float kPointStepM   = 0.0025f;                                         /* 2.5 mm pro A121 Step */

#ifndef A121_ZOOM_POINTS
#define A121_ZOOM_POINTS 24  /* 48 = 120 mm (V2.3 Schritt 3), vorbereitet; Wahl nach Messung V4.4 */
#endif
static constexpr uint16_t kZoomPoints   = A121_ZOOM_POINTS;  /* Subsweep 1: 24 Punkte * 2.5 mm = 60 mm Fokusfenster */
static_assert(kZoomPoints >= 8 && kZoomPoints <= 64, "A121_ZOOM_POINTS: 8 .. 64");
/* Spektren nur für die Punkte mit der stärksten Atmung, wenn das Fenster breiter als 24 Punkte ist (Rechenzeit) */
static constexpr uint16_t kSpecPoints   = (kZoomPoints > 24U) ? 16U : kZoomPoints;
static constexpr uint16_t kRoomStep     = 12U;  /* RSS: step_length muss Teiler oder Vielfaches von 24 sein */
static constexpr uint16_t kRoomPoints   = 80U;  /* Subsweep 2: 80 Punkte * 30 mm = 2.40 m Raumfenster */
static constexpr uint16_t kTotal3SubPts = 1U + kZoomPoints + kRoomPoints; /* 105 Punkte = 420 Bytes/Frame */

static constexpr float    kFsHz        = 60.0f;  /* Frame-Rate der iq3-Session; Filterkoeffizienten gelten nur hierfür */
#ifndef A121_HR_WIN
#define A121_HR_WIN 900  /* Pulsfenster in Frames; 600 / 720 für den Vergleich 10 / 12 / 15 s an Tag A (V2.5) */
#endif
static constexpr uint16_t kHrWin       = A121_HR_WIN;  /* 15 s Pulsfenster */
static constexpr uint16_t kSnapMaxLag  = 180U;   /* pushFrame darf während eines Reports 3 s weiterschreiben (V2.1) */
static constexpr uint16_t kHistoryLen  = kHrWin + kSnapMaxLag;  /* 18 s @ 60 Hz (ReportSnapshot) */
static constexpr uint16_t kHrMinWin    = 360U;   /* ab 6 s Daten wird ein Puls berichtet, Fenster wächst bis 15 s (V2.4; vorher 8 s) */
static constexpr uint16_t kHrStableWin = 480U;   /* Verfolgung erst ab 8 s Fenster; davor vorläufig (V2.4) */
static_assert(kHrWin >= kHrStableWin && kHrWin % 60U == 0, "A121_HR_WIN: ganze Sekunden, mindestens 8 s");
static constexpr uint16_t kStartZoomFrames = 120U;  /* Start: nach 2 s auf das Ziel zoomen (2 Reports statt 3, V2.4) */
static constexpr uint8_t  kDecim       = 4U;     /* 60 Hz → 15 Hz für die Spektralsuche */
/* Spektren (V2.2, B13): reelle FFT der auf 15 Hz dezimierten Folge (≤ 225 Werte, Hann, auf 1024 nullaufgefüllt) statt
 * Goertzel je Frequenz; Raster 15 Hz / 1024 = 0,0146 Hz (0,88 /min), Spitzen parabolisch verfeinert. Vorher Puls im
 * 0,01-Hz-Raster ohne Verfeinerung, Atmung im 0,005-Hz-Raster. Die Grenzen in Hz bleiben wie vorher. */
static constexpr uint16_t kFftN        = 1024U;
static constexpr float    kFftDf       = kFsHz / (float)kDecim / (float)kFftN;
static constexpr uint16_t kHrK0        = 44U;    /* erster Bin des Pulsspektrums (0,645 Hz) */
static constexpr float    kHrFMin      = (float)kHrK0 * kFftDf;
static constexpr float    kHrFStep     = kFftDf;
static constexpr uint16_t kHrNumF      = 164U;   /* Bins 44 .. 207 (0,645 .. 3,032 Hz); gesucht 46 .. 205 = 40,4–180,2 /min */
static constexpr float    kHarm2W      = 0.5f;   /* Oberwellen-Summe S(f) = P(f) + 0,5 · P(2f) (2. Herz-Oberwelle) */
static constexpr float    kHr2MaxHz    = 5.0f;   /* 2f über der Bandpass-Grenze zählt nicht (V1.2) */
static constexpr uint16_t kHr2MaxBin   = (uint16_t)(kHr2MaxHz / kFftDf);  /* 341 = 4,995 Hz */
static constexpr uint16_t kBrK0        = 7U;     /* Atem-Spektrum über alle Zoom-Punkte: Bins 7 .. 40 = 0,103 .. 0,586 Hz */
static constexpr float    kBrFMin      = (float)kBrK0 * kFftDf;
static constexpr float    kBrFStep     = kFftDf;
static constexpr uint16_t kBrNumF      = 34U;
static constexpr uint16_t kBrEdgeBins  = 1U;     /* Atem-Spitze nicht am Bandrand (früher oft 6 /min): 0,117 .. 0,571 Hz */
static constexpr float    kBrMinQ      = 2.0f;   /* Atem-Spitze/Median: darunter keine Oberwellen-Prüfung */
static constexpr float    kHarmTolHz   = 0.04f;  /* Puls-Kandidat gilt als Atem-Oberwelle, wenn |f − m·f_Atem| ≤ 0,04 Hz */
static constexpr uint8_t  kHarmMMax    = 8U;     /* geprüfte Oberwellen m = 2 .. 8 (V1.3) */
static constexpr float    kHarmPenalty = 0.5f;   /* Kandidat auf einer Atem-Oberwelle m ≥ 4: S × 0,5 bei der Kandidatenwahl */
static constexpr float    kHarmPenLow  = 0.02f;  /* m = 2, 3: S × 0,02 (praktisch gesperrt: Prüfstand 4–12 × Puls, × 0,1 reichte nicht) */
static constexpr uint8_t  kHarmLowMax  = 3U;
static constexpr float    kLowLobeHz   = 0.1f;   /* m = 2, 3: Toleranz ±0,1 Hz statt ±0,04 (Hauptkeule, Hann 15 s: ±0,13 Hz) */
static constexpr float    kSubRel      = 0.5f;   /* Oktav-Prüfung: Spitze bei f/2 mit Wert ≥ 0,5 · Wert(f) gilt als Grundwelle */
static constexpr float    kNearMinRel  = 0.5f;   /* naher Kandidat (±12 /min) gewinnt nur mit S ≥ 0,5 · S_max */
static constexpr uint8_t  kAltReports  = 3U;     /* Wechsel weg von einer Oberwelle nach 3 Reports mit Alternative */
static constexpr float    kMaxMotionPct = 30.0f; /* unter 70 % sauberer 1-s-Blöcke ist der Report ungültig */
static constexpr float    kPulseHeldDb = 8.0f;   /* Atem angehalten: Pulsspitze ≥ 8 dB über dem Median */
static constexpr float    kEchoSpreadMax = 0.4f; /* sauberes Echo: Abstand zum Kreismittelpunkt streut < 40 % (Rauschen ≈ 52 %) */
static constexpr float    kBreathMinUm = 100.0f; /* Lebenszeichen: Atemhub ≥ 100 µm Spitze-Spitze am Atem-Punkt */
static constexpr uint8_t  kHoldHarm    = 10U;    /* Sprung auf eine Oberwelle erst nach 10 Reports (sonst 3) */
static constexpr float    kKeepTrackS  = 30.0f;  /* Rezoom: Verfolgung bleibt, wenn der letzte gültige Wert < 30 s alt ist */
/* Atemzustand (V1.4, B9): Amplitude je Report = Spitze-Spitze des 1-s-Mittels über die letzten 10 s am Atem-Punkt
 * (Trend entfernt), Bezug = Median der letzten 60 Werte. Ein 1-s-Effektivwert (Plan) hängt bei 4–10 s Atemperiode von
 * der Phase ab; 10 s enthalten eine ganze Periode bis 6 /min (5 s verloren bei 6 /min bis zur Hälfte). */
static constexpr float    kBrAmpWinS   = 10.0f;
static constexpr uint8_t  kBrRingLen   = 60U;
static constexpr uint8_t  kBrMinRing   = 10U;    /* Median erst ab 10 Werten */
static constexpr float    kApneaRel    = 0.10f;  /* angehalten: ≥ 90 % unter dem Median */
static constexpr float    kShallowRel  = 0.70f;  /* flach: ≥ 30 % darunter */
static constexpr uint8_t  kBrLowReports = 1U;    /* „seit ≥ 10 s": ein niedriger Wert, das Fenster ist schon 10 s lang */
static constexpr float    kBrMotionPct = 50.0f;  /* Zustand Bewegung ab motion_pct > 50 */
/* Ohne sauberes Echo in so vielen Reports in Folge ist die Person weg: Referenz vergessen, sonst wird der erste
 * Rausch-Report mit „Echo" gegen die alte Atemtiefe zur Apnoe (Abschluss-Review C1, Prüfstand weg) */
static constexpr uint8_t  kBrEchoLostReports = 3U;
static constexpr uint32_t kTickGapMs   = 25U;    /* tick-Abstand > 25 ms gilt als Lücke (Soll 16,7 ms; XM125: 1 tick = 1 ms) */
static constexpr float    kSatMaxPct   = 1.0f;   /* ab 1 % gesättigter Frames im Fenster: Konfidenz × 0,5 (V1.5) */
static constexpr uint8_t  kBrStateNormal = 0U, kBrStateNoPeak = 1U, kBrStateApnea = 2U, kBrStateMotion = 3U,
                          kBrStateShallow = 4U;
/* Zustand der Messung (V3.5, Zeile @STATE) */
static constexpr uint8_t  kStNoPerson = 0U, kStSearch = 1U, kStAcquire = 2U, kStValid = 3U, kStMotion = 4U,
                          kStApnea = 5U;
static constexpr uint16_t kFiltPad     = 27U;    /* scipy sosfiltfilt: padlen = 3 * (2 * Sektionen + 1) */
static constexpr uint16_t kGateBlk     = 60U;    /* Bewegungsdämpfung: 1-s-Blöcke im Herzband */
static constexpr float    kGateK       = 1.25f;  /* Grenze = 1,25 × Effektivwert des ruhigen Viertels der Blöcke */
static constexpr float    kGateP       = 3.0f;   /* Verstärkung darüber (Grenze/Effektivwert)^3: Bewegung fällt praktisch heraus */
static constexpr float    kMotionK     = 3.0f;   /* für motion_pct: Block > 3 × ruhiges Viertel gilt als Bewegung */
static constexpr uint8_t  kMaxBlocks   = kHrWin / kGateBlk;
static constexpr uint16_t kHrEdgeBins  = 2U;     /* Spitzen in den äußeren 2 Bins (0,03 Hz) zählen nicht (Leckage vom Bandrand) */
static constexpr uint8_t  kMaxRoomTargets = 8U;  /* gemeldete Raum-Ziele (die stärksten) */
static constexpr float    kRoomDetK    = 2.0f;   /* Raum-Ziel: Amplitude oder Bewegung ≥ 2 × Rauschpegel */
static constexpr float    kRoomMoveK   = 3.0f;   /* bewegt: Bewegung ≥ 3 × Bewegungs-Rauschpegel */
static constexpr float    kRoomSlowA   = 0.00554f; /* Glättung für das Zoom-Ziel: 1 − exp(−1 / (60 Hz · 3 s)), τ ≈ 3 s */
static constexpr float    kArcMinRad   = 2.0f;   /* Kreisfit gilt als schlecht bestimmt, wenn die Phase weniger überstreicht */

/* Butterworth-Bandpass 4. Ordnung 0,6–5,0 Hz bei 60 Hz (V1.2: Puls 40–180 /min samt 2. Oberwelle; vorher 0,8–3,0 Hz):
 * scipy.signal.butter(4, [0.6, 5.0], 'band', fs=60, output='sos') */
static constexpr float kBpSos[4][6] = {
    {0.0016470443743298322f, 0.0032940887486596643f, 0.0016470443743298322f, 1.0f, -1.3464578520633204f, 0.47760903309535657f},
    {1.0f, 2.0f, 1.0f, 1.0f, -1.5148095433061484f, 0.7378001887686901f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.8687365868947912f, 0.8744518329372326f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.9571506605508442f, 0.9612033138279775f},
};

/** Ein Durchlauf von vier Biquads (DF2T) in Vorwärts- oder Rückwärtsrichtung, Anfangszustand = Sprungantwort. */
inline void sosPassSos(float *e, uint32_t L, bool reverse, const float (*sos)[6]) {
    const float x0 = reverse ? e[L - 1U] : e[0];
    float z1[4], z2[4];
    float level = x0;
    for (uint8_t s = 0; s < 4; s++) {
        const float *c = sos[s];
        const float g = (c[0] + c[1] + c[2]) / (1.0f + c[4] + c[5]);  /* DC-Verstärkung der Sektion */
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
 * Nullphasen-Bandpass wie scipy `sosfiltfilt` (ungerade Erweiterung um kFiltPad, Anfangszustände aus der
 * Sprungantwort) mit vier Biquads sos. ext: N + 2 · kFiltPad Werte; Ergebnis: ext[kFiltPad .. kFiltPad + N).
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
/* V3.6: breiteres Herzband für die Einzelschläge; scipy.signal.butter(4, [0.6, 8.0], 'band', fs=60, output='sos') */
static constexpr float kBp8Sos[4][6] = {
    {0.009764843672775374f, 0.019529687345550748f, 0.009764843672775374f, 1.0f, -0.9137923786305936f, 0.24411561287811703f},
    {1.0f, 2.0f, 1.0f, 1.0f, -1.0869497349419404f, 0.6029896520783747f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.8749786163052726f, 0.8797806780795341f},
    {1.0f, -2.0f, 1.0f, 1.0f, -1.95389950449864f, 0.9578914690655628f},
};
#endif

/**
 * atan2 als Polynom (V2.2, B13): atan(a) für a = min/max ∈ [0, 1] nach Abramowitz/Stegun 4.4.49 (Fehler ≤ 1 · 10⁻⁵ rad),
 * dann auf den Quadranten gespiegelt. Beim Abwickeln 21 600 Aufrufe je Report; atan2f kostet auf dem ESP32-S3 ein
 * Vielfaches. 10⁻⁵ rad ≈ 0,004 µm Weg.
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
 * Linearen Trend entfernen (kleinste Quadrate) in float (V2.2; vorher double, das der ESP32-S3 in Software rechnet):
 * Σt² = N(N² − 1)/12 geschlossen, Summen um x[0] verschoben, damit float reicht.
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

/** sel[i] = true für die k größten Werte von v[0 .. n) (bei Gleichstand der frühere Index). */
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
/* V6: Bewegungskarte */
static constexpr uint8_t kReacqMinS    = 6U;     /* kürzestes Fenster (s) nach Bewegung, auf dem noch ein Puls gesucht wird */
static constexpr uint8_t kMotionVotes  = 2U;     /* so viele der stärksten Punkte müssen einen Block bewegt melden */
static constexpr uint8_t kMotionPoints = 3U;     /* so viele der stärksten Punkte stimmen ab */
static_assert(kMaxBlocks <= 32U, "MotionMap::mask hat 32 Bit: höchstens 32 Blöcke");
/* V6: Gedächtnis */
static constexpr float    kMemS        = 60.0f;  /* der zuletzt gesicherte Puls gilt so lange (s); danach vergessen */
static constexpr float    kMemBandBpm  = 15.0f;  /* Kandidat gilt als "nah am Gedächtnis" bei |bpm − gemerkt| ≤ 15 /min */
static constexpr float    kMemMinConf  = 50.0f;  /* Puls wird nur ab dieser Konfidenz gemerkt (Speicherbedingung, Task 6) */

/** Bewegung je 1-s-Block des Pulsfensters. Bit k von mask = Block k bewegt (Block 0 = ältester). */
struct MotionMap {
    uint32_t mask;
    uint8_t  n_blocks;
    uint8_t  clean_tail_blocks;  /* Blöcke nach dem letzten bewegten (alle, wenn keiner) */
};

/**
 * Bewegungskarte aus dem Block-Effektivwert (rms: n_pts Zeilen × nb Spalten, zeilenweise). Je Punkt ist der Bezug das
 * ruhige Viertel seiner Blöcke wie in gateMotion; ein Block gilt als bewegt, wenn bei mindestens votes Punkten
 * rms > k × Bezug ist (votes wird auf n_pts begrenzt, ein einzelner Punkt entscheidet also allein). Wie gateMotion
 * gibt es unter 3 Blöcken keine Bewegungsaussage; mehr als kMaxBlocks Blöcke werden abgeschnitten.
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
/* V6, Variante Periodizität (Experiment, siehe Kopfkommentar und docs/EXPERIMENTE.md) */
static constexpr float    kPeriodK     = 5.0f;   /* periodisch: stärkste Spitze ≥ 5 × Mittel der übrigen Bins im Herzband */
static constexpr uint16_t kPeriodK0    = 46U;    /* Herzband-Bins 46 .. 205 = 0,674 .. 3,003 Hz (wie die Pulssuche) */
static constexpr uint16_t kPeriodK1    = 205U;
static constexpr uint16_t kPeriodSegS  = 3U;     /* Ausschnitt um den Block: 3 s */
static constexpr uint16_t kPeriodSegN  = (uint16_t)(kPeriodSegS * (uint16_t)kFsHz / kDecim);  /* 45 Werte bei 15 Hz */

/**
 * Ausschnitt für segmentPeriodic: bis zu 3 s des bandgefilterten Wegsignals h[0 .. N) (60 Hz) um die Stelle center,
 * auf 15 Hz dezimiert (jeder kDecim-te Wert) und mit Hann gewichtet, nach out (≥ kPeriodSegN Werte). Der Ausschnitt wird
 * am Rand verschoben statt gekürzt (center bleibt darin); N unter 3 s: ganzes Signal. Rückgabe: Zahl der Werte.
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
 * Periodisch im Herzband? x: n gewichtete Werte aus periodSegment (15 Hz). Spektrum |X|² über fft (spec: ≥ kPeriodK1 + 1
 * Werte, Arbeitsfeld); wahr, wenn die stärkste Spitze in den Bins kPeriodK0 .. kPeriodK1 mindestens k × Mittel der
 * übrigen Bins im Band ist (nach Vital-Radio, Adib et al. 2015). Ein Ausschnitt ohne Leistung ist nicht periodisch.
 * Abweichung vom Entwurf: "übrige" Bins sind die außerhalb der Hann-Hauptkeule der Spitze (± 2 Auflösungszellen
 * kFftN / n Bins). Mit allen übrigen Bins liegt schon ein reiner Sinus im 3-s-Ausschnitt nur bei Spitze/Mittel ≈ 4,7
 * (die Hauptkeule ist 1,3 Hz breit, das Band 2,3 Hz), Rauschen bis 7: k = 5 trennt dann nichts.
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

/**
 * Kreismittelpunkt der IQ-Werte eines Zoom-Punkts im Fenster: Kåsa-Fit in Bullock-Form (algebraisch, ein 2×2-System).
 * Die Phase eines Punkts muss um den Mittelpunkt des Kreises gemessen werden, den die Atembewegung beschreibt (festes
 * Echo im selben Punkt), nicht um den Mittelwert: Der liegt bei einem Atemhub mit Phasenhub β um J0(β) × Radius
 * daneben und erzeugt Atem-Oberwellen im Pulsband (docs/AUDIT_VITAL.md, B2).
 * Summen in float über (u, v) = (z − Mittelwert) / s mit s = Effektivwert des Abstands zum Mittelwert, damit float reicht
 * (der ESP32-S3 rechnet double in Software).
 * ring: [kHistoryLen][I, Q] dieses Punkts, Fenster start .. start + N (modulo kHistoryLen).
 * Rückgabe false und (ci, cq) = Mittelwert, wenn das System schlecht bestimmt ist (det ≤ 10⁻⁶ · Suu · Svv, z. B. alle
 * Punkte auf einer Geraden) oder der Mittelpunkt mehr als 10 × s vom Mittelwert entfernt läge.
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
    /* Variante für den Vergleich an Aufnahmen (V1.1): 3 Gauß-Newton-Schritte auf den geometrischen Abstand, Radius
     * eliminiert. Im Prüfstand ohne Unterschied zu Kåsa (ideale Kreisbögen), +8 % Rechenzeit. */
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

/** Autokorrelation des Herzsignals (40–180 /min, V1.2): größtes echtes lokales Maximum mit r > 0,05, sonst
 *  out_peak = 0. Am kleinsten Abstand zählt nur eine Spitze, deren linker Nachbar mitgerechnet ist (vorher galt ein
 *  dort abfallender Verlauf als Spitze: Prüfstand meldete 189,5 /min). */
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

/** G·coef = r für die Oberwellen-Fits lösen (G symmetrisch positiv definit, nur untere Hälfte gefüllt; G und r werden
 *  überschrieben, r = coef). Cholesky G = L·Lᵀ mit kleiner Regularisierung gegen fast gleiche Frequenzen. false: G
 *  nicht positiv definit. */
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

/** Atem-Oberwellen m · f_breath (m = 1..kHarmMMax, unter fs/2) aus x abziehen: gemeinsamer Kleinste-Quadrate-Fit von
 *  cos und sin aller m, Hann-gewichtet (sonst sickert eine starke Oberwelle in die Nachbarn und in den Puls). m mit
 *  |m · f_breath − f_keep| < 0,1 Hz oder |m · f_breath − 2 f_keep| < 0,1 Hz bleiben, dort liegt der Puls (wie V3.3).
 *  f_breath ≤ 0: nichts tun. Für AKF und Einzelschläge auf dem breiten Band 0,6–5 Hz, in dem die 3. Atem-Oberwelle
 *  sonst den Herzschlag überdeckt (Prüfstand: AKF in 3 % der Reports beim Puls). */
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
static constexpr uint16_t kGuideFft = 256U;  /* ≥ Fensterlänge nach Dezimierung (15 s · 15 Hz = 225) */
static constexpr float    kGuideMinConf = 50.0f;  /* Konfidenz des vorigen Reports, ab der entfernt wird (%) */

/**
 * V3.3 (Variante): Atem-Oberwellen geführt von der Atemphase entfernen (wie project_out_respiration_harmonics() in den
 * PC-Skripten). Phase φ(n) = Winkel des analytischen Signals der Atem-Grundwelle aus disp: FFT, nur positive
 * Frequenzen, flach im Band 0,5–1,5 × f_breath, cos²-Flanken bis 0,2 bzw. 1,8 × f_breath, zurück. So folgt φ einer
 * Rate, die im Fenster schwankt oder vom vorigen Report abweicht (Rechnung bei ±23 % Ratenschwankung und Atemform mit
 * 2./3. Oberwelle: 0,05 rad Effektivwert; ein enges Band ±0,3 gab 0,12 rad, festes Mischen mit f_breath und Mittel
 * über eine Periode ±0,5 rad). Dann gemeinsamer Hann-gewichteter Kleinste-Quadrate-Fit von cos(mφ),
 * sin(mφ) für m = 1..kHarmMMax an x und abziehen. m bleibt, wenn f_keep oder 2 f_keep in m × [kleinste, größte
 * Atemfrequenz im Fenster] ± 0,1 Hz liegt: Die m-te Oberwelle streut um m × die Ratenschwankung, ihre Basis nähme dem
 * Puls sonst einen Teil weg (Rechnung: 40 % bei ±23 % Schwankung und m = 5 nahe am Puls). Oberwellen, die den Puls
 * überdecken, lassen sich so nicht entfernen, nur die abseits. f_breath ≤ 0: nichts tun.
 * work: Arbeitsspeicher mit 2 · kGuideFft Werten.
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
    for (uint16_t k = 0; k < kGuideFft; k++) {  /* analytisch und Atem-Grundwelle: Gewicht 2 im Band, 0 sonst */
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
    /* Spanne der Atemfrequenz im Fenster: Phasenschritte über 1 s gemittelt, ohne die Ränder (dort ist die Phase des
     * analytischen Signals unsicher). Die m-te Oberwelle streut über m × diese Spanne. */
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
    /* Fit von Re/Im(u^m), m ∈ ms */
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

/** Atem-Amplitude (µm): Spitze-Spitze des gleitenden 1-s-Mittels über die letzten kBrAmpWinS von x, nach Abzug
 *  einer Geraden (Lage-Drift beim Anhalten). < 0: zu wenig Daten. Das 1-s-Mittel dämpft den Herzschlag (76 /min auf
 *  ≈ 20 %), die Atmung (15 /min) kaum. */
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

/** Atemzustand je Report (V1.4): 0 normal, 1 keine Atemspitze (oder kein sauberes Echo, amp < 0), 2 angehalten /
 *  Apnoe (Amplitude < 10 % des Medians der letzten 60 Werte, kBrLowReports in Folge), 3 Bewegung (motion_pct > 50),
 *  4 flach (< 70 %, ebenso lange). Während 2 und 3 wächst der Bezug nicht mit: Ein langes Anhalten oder Bewegen
 *  verschöbe sonst den Median. Nach kBrEchoLostReports Reports ohne sauberes Echo gilt der Bezug nicht mehr (Person
 *  gegangen); er baut sich danach neu auf. */
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
/** V6: Kurzzeit-Gedächtnis des Pulses. Gemerkt werden Puls (/min) und Schlagabstand (ms) samt Frame-Zähler der letzten
 *  gesicherten Messung. Es gilt kMemS Sekunden (Unterschied der uint32-Zähler, ein Überlauf schadet nicht) und
 *  erlischt, sobald das Echo fehlt oder das Ziel die Zoomzone verlässt (dann ist es wohl eine andere Lage/Person). */
class ReacqMemory {
public:
    void clear() { hr_ = ibi_ms_ = 0.0f; stored_frames_ = 0U; valid_ = false; }
    void store(float hr_bpm, float ibi_ms, uint32_t tot_frames) {
        hr_ = hr_bpm;
        ibi_ms_ = ibi_ms;
        stored_frames_ = tot_frames;
        valid_ = true;
    }
    /** true, solange ein Wert gemerkt ist und gilt; ein abgelaufenes oder entwertetes Gedächtnis wird dabei gelöscht. */
    bool active(uint32_t tot_frames, bool echo_ok, bool target_in_zoom) {
        if (valid_ && (!echo_ok || !target_in_zoom || (uint32_t)(tot_frames - stored_frames_) >= kMemFrames)) clear();
        return valid_;
    }
    float hr() const { return hr_; }
    float ibi_ms() const { return ibi_ms_; }
    bool stored() const { return valid_; }  /* gemerkt, ohne Ablauf und Bedingungen zu prüfen (Tests) */
    uint32_t storedAt() const { return stored_frames_; }  /* Frame-Zähler beim letzten store() (Tests) */

private:
    static constexpr uint32_t kMemFrames = (uint32_t)(kMemS * kFsHz);
    float    hr_ = 0.0f, ibi_ms_ = 0.0f;
    uint32_t stored_frames_ = 0U;
    bool     valid_ = false;
};
#endif

/** Ergebnis der Einzelschläge (V1.6). */
struct IbiResult {
    float   bpm      = 0.0f;  /* 60000 / Median der gültigen IBI; 0 = kein Ergebnis (< 3 gültige IBI) */
    float   ibi_ms   = 0.0f;
    float   rmssd_ms = 0.0f;  /* 0 = nicht berechnet (< 90 % der erwarteten IBI gültig) */
    uint8_t n_beats  = 0;
    uint8_t n_valid  = 0;     /* gültige IBI */
};

static constexpr uint8_t kIbiMaxPk = 48;  /* höchstens so viele Spitzen je Fenster (15 s × 180 /min = 45) */

/** Spitzen über 0,35 × Effektivwert mit Sperrzeit 0,6 × 60 / hr_bpm (hr_bpm schon auf 40–180 begrenzt). */
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

/** Lage der Spitzen parabolisch zwischen den Abtastwerten (in Abtastwerten). */
inline void refinePeaks(const float *hr, const uint16_t *pk, uint8_t n_pk, float *t) {
    for (uint8_t k = 0; k < n_pk; k++) {
        const uint16_t i = pk[k];
        const float a = hr[i - 1], b = hr[i], c = hr[i + 1], den = a - 2.0f * b + c;
        float d = (den < 0.0f) ? 0.5f * (a - c) / den : 0.0f;
        d = d < -0.5f ? -0.5f : (d > 0.5f ? 0.5f : d);
        t[k] = (float)i + d;
    }
}

/** IBI, Artefakt-Regeln und RMSSD aus Schlagzeiten t (Abtastwerte, V1.6). */
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
        if (in_range[k]) {  /* die 5 vorigen IBI im Bereich (gleitend) */
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
 * Einzelschläge im Herzsignal (V1.6, B7): Spitzen über 0,35 × Effektivwert, Sperrzeit 0,6 × 60 / hr_bpm (vorher fest
 * 360 ms), Lage parabolisch zwischen den Abtastwerten verfeinert (vorher auf 16,7 ms gerastert). Ein IBI gilt, wenn es
 * im Bereich 40–180 /min liegt und höchstens 20 % vom Median der 5 vorigen IBI abweicht (am Fensteranfang: Median
 * aller IBI im Bereich). RMSSD nur aus direkt aufeinanderfolgenden gültigen IBI und nur, wenn ≥ 90 % der erwarteten
 * IBI gültig sind (Artefakt-Regeln der HRV-Literatur, AUDIT_VITAL.md 5.1); 15 s sind für HRV ohnehin kurz.
 * t_out (optional, kIbiMaxPk Werte): Schlagzeiten in Abtastwerten, n_beats gültig.
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
static constexpr float   kTplHalf = 0.3f;  /* Vorlage ±0,3 × Schlagabstand um die Spitze */
static constexpr uint8_t kTplMaxW = 28U;   /* 0,3 × 1,5 s × 60 Hz = 27 Abtastwerte bei 40 /min */

/**
 * Einzelschläge mit Vorlage (V3.6): erste Spitzen wie V1.6, Vorlage = Mittel der Abschnitte ±kTplHalf × Schlagabstand
 * um sie (ohne Gleichanteil), Korrelationsfilter c[n] = Σ_j v[j] · x[n + j], Spitzen von c wie V1.6 gesucht und
 * parabolisch verfeinert, dann dieselben Artefakt-Regeln. x: Herzsignal (V3.6: Band 0,6–8 Hz), work: N Werte. Mit
 * weniger als 3 vollständigen Abschnitten: wie timeDomainIbi.
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

#define A121_DSP_FRAME_FLAGS 1  /* pushFrame nimmt FrameFlags (für host/dsp_replay mit älteren Headern) */

/** Sensor-Flags eines Frames aus dem Server-Kopf (V1.5, B15, B16). */
struct FrameFlags {
    uint32_t tick      = 0;
    bool     have_tick = false;
    bool     saturated = false;  /* data_saturated */
    bool     delayed   = false;  /* frame_delayed: der Server hat Frames nicht rechtzeitig geliefert */
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

/** Eingänge der Konfidenz (V1.3). hr_fused_5s_ago ≤ 0 = unbekannt (kein Sprung-Abschlag). */
struct QualityInputs {
    float peak_db;          /* S an der verfolgten Frequenz über dem Median des kombinierten Spektrums */
    bool  on_harmonic;      /* verfolgte Frequenz liegt auf einer Atem-Oberwelle */
    float hr_spec, hr_acf, hr_ibi, hr_fused, hr_fused_5s_ago;
    float motion_pct;       /* Anteil der 1-s-Blöcke mit Bewegung im Fenster */
    float sat_pct = 0.0f;   /* Anteil gesättigter Frames im Fenster (V1.5) */
    float win_frac = 1.0f;  /* Fensterlänge / 15 s, solange das Fenster noch wächst (V2.4) */
};

/**
 * Konfidenz 0–100 % aus unabhängigen Hinweisen statt aus der Einigkeit der 24 Zoom-Punkte (die sehen fast dasselbe
 * Signal, B4): 100 × clamp((peak_db − 3) / 12) × 0,5 auf einer Oberwelle × 1 / 0,7 / 0,5 bei 3 / 2 / ≤ 1 Schätzern
 * (Spektrum, Autokorrelation, Einzelschläge) innerhalb ±5 /min des Ergebnisses × (1 − Bewegungsanteil) × 0,7 bei einem
 * Sprung > 8 /min in 5 s × 0,5 ab 1 % gesättigter Frames (V1.5). Faktoren vorläufig (Prüfstand); an Tag A
 * abstimmen, danach festhalten.
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
 * Zustand der Messung (V3.5) aus Raumprofil (bewegtes Ziel = Person im Raum), Zoom-Echo, Atmung, Gültigkeit,
 * Bewegungsanteil und Atemzustand. Vorrang: BEWEGUNG (nur mit sauberem Echo im Zoom: ohne Echo ist der Bewegungsanteil
 * Rauschen) > APNOE > GÜLTIG > ERFASSUNG (sauberes Echo im Zoom mit Atmung oder Person im Raum, Wert noch nicht
 * gültig) > SUCHE (Person im Raum, im Zoom kein sauberes Echo) > KEINE_PERSON.
 */
inline uint8_t measurementState(bool person, bool echo_ok, bool breath, bool valid, float motion_pct, uint8_t br_state) {
    if (echo_ok && (breath || person) && (motion_pct > kMaxMotionPct || br_state == kBrStateMotion)) return kStMotion;
    if (br_state == kBrStateApnea) return kStApnea;
    if (valid) return kStValid;
    if (echo_ok && (breath || person)) return kStAcquire;
    return person ? kStSearch : kStNoPerson;
}

/** Tor für @BREATH (V3.4): nur in ERFASSUNG, GÜLTIG und APNOE. Ohne Person ist der Weg des Telemetrie-Strahls
 *  Rauschen (Prüfstand leer/niemand: 19–22 „Atemzüge" in 120 s), in BEWEGUNG Körperbewegung. */
inline bool breathGateOpen(uint8_t state) { return state == kStAcquire || state == kStValid || state == kStApnea; }

#define A121_DSP_BREATH 1  /* Atemzug-Erkennung und @BREATH (V3.4; für host/dsp_replay mit älteren Headern) */

/** Ein erkannter Atemzug (V3.4): Spitze des gefilterten Wegs, Abstand zur vorigen Spitze im Band 4–42 /min. */
struct BreathEvent {
    uint32_t frame       = 0;     /* Frames seit reset() beim Erkennen (Spitze + Filterlaufzeit + Hysterese) */
    uint32_t interval_ms = 0;     /* Abstand zur vorigen Spitze */
    float    depth_um    = 0.0f;  /* Spitze − voriges Tal */
    float    rate_cpm    = 0.0f;  /* 60 / Median der letzten (bis zu) kBrRateN Abstände */
};

static constexpr uint32_t kBrMinIntervalFr = 86U;   /* 42 /min = 1,43 s */
static constexpr uint32_t kBrMaxIntervalFr = 900U;  /* 4 /min = 15 s */
static constexpr uint8_t  kBrRateN         = 4U;
static constexpr float    kBrHystFrac      = 0.3f;    /* Hysterese = 30 % der Spitze-Spitze-Spanne der letzten 8 s */
static constexpr float    kBrHystMinUm     = 200.0f;  /* darunter Puls-Restwelligkeit (Puls 100 µm → ≈ 40 µm) und
                                                      * kleine Wellen beim Anhalten (Prüfstand 144–157 µm); echter
                                                      * Atemhub sitzend 2,3–3,4 mm (M2, M3) */
static constexpr uint8_t  kBrSpanBlocks    = 8U;      /* Spanne aus 8 Blöcken zu 1 s */
static_assert(kFsHz == 60.0f, "Bandpass-Koeffizienten der Atemzug-Erkennung gelten für 60 Hz");

/**
 * Atmung Atemzug für Atemzug (V3.4) auf dem Weg des Telemetrie-Strahls (µm je Frame): Bandpass 0,05–0,8 Hz aus
 * Hochpass 1. Ordnung (ein Hochpass 2. Ordnung schwingt nach einem Atemstopp über und erzeugte einen falschen Atemzug;
 * Preis: Tiefe bei 6 /min ≈ 11 % zu klein) und Butterworth-Tiefpass 2. Ordnung, in double (die Pole liegen dicht an
 * z = 1), dann Spitzen und Täler mit Hysterese. Die Hysterese
 * folgt der Spanne (max − min) der letzten 8 s, damit nach tiefen Atemzügen auch flache wieder durchgehen; nach unten
 * begrenzt auf kBrHystMinUm gegen die Restwelligkeit des Pulses. Je Spitze ein Ereignis mit dem Abstand zur vorigen
 * Spitze, wenn er im Band 4–42 /min liegt; bei kürzeren Abständen ersetzt die neue Spitze die alte als Bezug (sonst
 * würde aus 45 /min jede zweite Spitze als 22 /min gezählt), längere beginnen die Rate neu, und erst die übernächste
 * Spitze wird Bezug.
 */
class BreathDetector {
public:
    void reset() { *this = BreathDetector(); }

    /** Einen Wert einspeisen; true, wenn damit ein Atemzug erkannt ist (ev gefüllt). */
    bool push(float disp_um, BreathEvent &ev) {
        static constexpr double kSos[2][5] = {  /* b0, b1, b2, a1, a2; scipy butter(1, 0.05, 'high') und butter(2, 0.8) */
            {0.9973888361673892, -0.9973888361673892, 0.0, -0.9947776723347783, 0.0},
            {0.0016556093437783469, 0.0033112186875566938, 0.0016556093437783469, -1.8816500461793153,
             0.8882724835544287}};
        double v = (double)disp_um;
        for (int i = 0; i < 2; i++) {  /* Direktform II transponiert */
            const double y = kSos[i][0] * v + s_[i][0];
            s_[i][0] = kSos[i][1] * v - kSos[i][3] * y + s_[i][1];
            s_[i][1] = kSos[i][2] * v - kSos[i][4] * y;
            v = y;
        }
        const float y = (float)v;
        const uint32_t n = n_++;

        /* Spanne der letzten kBrSpanBlocks Sekunden (laufender Block eingeschlossen) */
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
        if (dir_ == 0) {  /* Start: nur die Richtung festlegen; der Startwert ist keine Spitze und kein Tal */
            if (n == 0 || y > ext_hi_) ext_hi_ = y;
            if (n == 0 || y < ext_lo_) ext_lo_ = y;
            if (y < ext_hi_ - h) {
                dir_ = -1; ext_ = y; ext_n_ = n;
            } else if (y > ext_lo_ + h) {
                dir_ = 1; ext_ = y; ext_n_ = n;
            }
        } else if (dir_ > 0) {  /* Spitze suchen */
            if (y > ext_) { ext_ = y; ext_n_ = n; }
            else if (y < ext_ - h) {
                got = onPeak(ext_, ext_n_, n, ev);
                dir_ = -1; ext_ = y; ext_n_ = n;
            }
        } else {  /* Tal suchen */
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
        if (d < kBrMinIntervalFr) return false;  /* zu kurz (> 42 /min): kein Ereignis, neue Spitze ist Bezug */
        if (d > kBrMaxIntervalFr || !have_trough_) {  /* Pause oder Lücke (< 4 /min): Rate neu */
            niv_ = 0;
            /* die erste „Spitze" nach einer Pause ist oft nur die Ruhelage, bevor der Weg wieder fällt: erst die
             * nächste Spitze wird Bezug */
            have_peak_ = !(d > kBrMaxIntervalFr);
            return false;
        }
        const uint32_t iv = (uint32_t)lroundf((float)d * 1000.0f / kFsHz);
        for (uint8_t i = kBrRateN - 1U; i > 0; i--) iv_[i] = iv_[i - 1];
        iv_[0] = iv;
        if (niv_ < kBrRateN) niv_++;
        uint32_t srt[kBrRateN];
        memcpy(srt, iv_, sizeof(srt));
        for (uint8_t i = 1; i < niv_; i++)  /* Einfügesortierung, höchstens 4 Werte */
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
/* V3.2 (Variante, A/B an Tag A): Puls-Verfolgung als Viterbi über die letzten kVitWin Reports */
static constexpr uint16_t kVitMinBpm = 40U, kVitStates = 141U;  /* 40–180 /min in 1-/min-Schritten */
static constexpr uint8_t  kVitWin    = 20U;
static constexpr float    kVitSigma  = 3.0f;    /* Übergang: Gauß σ = 3 /min je Report (1 s) */
static constexpr int16_t  kVitReach  = 12;      /* ±4 σ */
static constexpr float    kVitJump   = 0.02f;   /* Anteil „Sprung irgendwohin" (sonst hielte ein Fehlgriff ewig) */
static constexpr float    kVitFloor  = 0.1f;    /* Grundpegel der Emission (Report ohne Kandidat am wahren Puls) */
static constexpr float    kVitEmSig  = 1.0f;    /* Breite je Kandidat in /min (FFT-Raster 0,88 /min) */
static constexpr float    kVitGamma  = 3.0f;    /* Emission ∝ (Kandidatenwert / bester)^γ; mit γ = 1 hing der
                                                   * Pfad im Prüfstand brady 9 Reports auf einem schwachen, aber
                                                   * steten Kandidaten (75 statt 50); γ = 2: MAE 2,0, γ = 3: 1,5 */

/**
 * Viterbi über ein festes Gitter 40–180 /min mit den Emissionen der letzten kVitWin Reports. Emission je Report aus
 * den Kandidaten (Gauß je Kandidat, Gewicht = Kandidatenwert mit Oberwellen-Abschlag und Oktav-Prüfung), normiert und
 * mit Grundpegel kVitFloor. update() rechnet den besten Pfad über das Fenster neu (Ergebnis = sein letzter Zustand)
 * und die Vorwärts-Wahrscheinlichkeit; mass = Anteil im Bereich ±5 /min um das Ergebnis.
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

    /** Neuer Report mit n Kandidaten (bpm, Wert ≥ 0). Rückgabe: bpm am Ende des besten Pfads; mass ∈ [0, 1]. */
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

        /* Viterbi (log) und Vorwärts (normiert) über das Fenster, ältester Report zuerst */
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
/* V3.1: Zoom-Punkte vor der Demodulation kombinieren statt Spektren nachträglich zu mitteln (Szmola et al. 2025;
 * Pi-ViMo 2023: kohärent ausgerichtet und gewichtet summieren, Maximum Ratio Combining). */
static constexpr uint8_t kCombKMax = 16U;  /* Obergrenze für K (Akkumulatoren auf dem Stapel: 136 komplexe Werte) */
static constexpr uint8_t kCombK    = (kZoomPoints < 12U) ? (uint8_t)kZoomPoints : 12U;  /* beste K Punkte (Plan 8–16) */
static constexpr uint8_t kCombIter = 12U;  /* Potenz-Iterationen auf der K × K-Kovarianz */
static_assert(kCombK <= kCombKMax, "kCombK zu groß");

/**
 * 1. Hauptkomponente von K Zoom-Punkten. u_j[n] = z_j[n] − c_j (Ring j wie hist_zoom_, ab start, N Werte, c_j =
 * Kreismittelpunkt). C = Σ_n u uᴴ (hermitesch), v = Eigenvektor zum größten Eigenwert (Potenz-Iteration ab der Spalte
 * mit der größten Leistung), y[n] = Σ_j conj(v_j) · u_j[n] (linear, [I,Q] × N). Bei gleichem Rauschen je Punkt ist das
 * Maximum Ratio Combining: v_j ∝ Amplitude_j · e^{iθ_j}, die Punkte addieren phasengleich. Rückgabe: effektive
 * Punktzahl N_eff = (Σλ)² / Σλ² = (tr C)² / ‖C‖²_F (≈ 1: alle sehen dieselbe Bewegung; ≈ K: nur Rauschen); dominant = j
 * mit dem größten |v_j|. Reihenfolge der Schleifen: n außen, damit jeder Ring (PSRAM) nur einmal gelesen wird.
 */
inline float combinePrincipal(const float *const *rings, const float *ci, const float *cq, uint8_t K, uint16_t start,
                              uint16_t N, float *y, uint8_t &dominant) {
    dominant = 0;
    if (K == 0 || K > kCombKMax || N == 0) return 0.0f;
    constexpr uint16_t kPairs = (uint16_t)kCombKMax * (kCombKMax + 1U) / 2U;
    float are[kPairs] = {}, aim[kPairs] = {};  /* C_jk für j ≤ k: Index j·K − j(j−1)/2 + (k − j) */
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
    auto at = [&](uint8_t j, uint8_t k, float &re, float &im) {  /* C_jk, untere Hälfte = konjugiert */
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

/* V6: Wert von VitalReport::reacq */
static constexpr uint8_t kReacqNone = 0U;    /* normal */
static constexpr uint8_t kReacqShort = 1U;   /* kurzes Fenster nach Bewegung */
static constexpr uint8_t kReacqMemory = 2U;  /* über das Gedächtnis wiedergefunden */

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
    float    confidence_pct;   /* signalQuality(): Spitzenhöhe, Oberwelle, Einigkeit der Schätzer, Bewegung, Sprung */
    float    point_agree_pct;  /* frühere Konfidenz: gewichteter Anteil der Zoom-Punkte mit eigener Spitze ±5 /min */
    uint8_t  br_state;         /* Atemzustand (BreathStateTracker): 0 normal, 1 keine Spitze, 2 angehalten, 3 Bewegung, 4 flach */
    uint16_t n_delayed;        /* seit dem letzten Report: Frames mit frame_delayed (V1.5) */
    uint16_t n_sat;            /* … mit data_saturated */
    uint16_t n_tickgap;        /* … tick-Abstände > kTickGapMs (ohne Rezoom/Neubeginn) */
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
    bool     person_in_room;   /* bewegtes Ziel im geglätteten Raumprofil (Zoom-Ziel, V3.5) */
    bool     target_in_zoom;   /* … und es liegt im Zoom-Fenster */
    uint8_t  state;            /* Zustand der Messung kSt* (V3.5) */
    uint32_t state_since_ms;   /* … seit (Frames × 1000 / 60) */
    float    n_eff;            /* effektive Punktzahl des kombinierten Kanals (V3.1, nur mit A121_DSP_COMBINE) */
    uint8_t  win_s;            /* Länge des Pulsfensters dieses Reports in s (V6, Ausgabe nur mit A121_DSP_REACQ) */
    uint8_t  reacq;            /* Wiederfinden nach Bewegung (V6): kReacq* */
};

/**
 * Stand der Historie für einen Report (V2.1). takeSnapshot() läuft im Frame-Takt direkt nach pushFrame(); der Report
 * rechnet danach (auf dem ESP32 im DSP-Task auf Kern 0) nur aus diesem Stand und dem Ring. pushFrame() darf dabei
 * bis kSnapMaxLag Frames weiterschreiben, ohne das Fenster des Schnappschusses zu berühren. Die Sensor-Zähler sind
 * Summen seit begin(); der Report meldet die Differenz zum zuletzt gerechneten Schnappschuss.
 */
#define A121_DSP_SNAPSHOT 1
struct ReportSnapshot {
    uint16_t write_idx, count;
    uint32_t frame_count;
    uint8_t  best_zoom_bin;
    int16_t  temp_c;
    float    lb_amp;
    uint32_t tot_delayed, tot_sat, tot_tickgap;
    uint32_t tot_frames;  /* Frames seit begin(), über Rezooms hinweg (V2.3) */
    float    room_amp[kRoomPoints], room_mot[kRoomPoints], room_amp_slow[kRoomPoints], room_mot_slow[kRoomPoints];
};

/** Report aus einem Schnappschuss, nach dem pushFrame() schon mehr als kSnapMaxLag Frames weiter war: Das Fenster ist
 *  teils überschrieben, der Report gilt nicht (ESP32 und Nachrechnung prüfen beim Ausgeben gleich). */
inline bool snapshotStale(uint32_t snap_frame, uint32_t frame_now) { return frame_now - snap_frame > kSnapMaxLag; }

#ifdef A121_DSP_REACQ
/** V6: Index des stärksten FFT-Kandidaten, der nahe am gemerkten Puls liegt (|bpm − mem_hr| ≤ band) und mindestens
 *  min_rel × den besten Wert hat (cand_score absteigend sortiert, [0] = bester); sonst −1 (auch ohne Kandidaten oder
 *  Gedächtnis, ohne positiven besten Wert und für NaN: die Bedingungen sind positiv formuliert, NaN erfüllt keine). */
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
#endif

class UltimateEspDsp {
public:
    UltimateEspDsp() = default;

    bool begin(int32_t zoom_start_pt = 200, int32_t room_start_pt = 120, uint16_t room_step = kRoomStep) {
        zoom_start_pt_ = zoom_start_pt;
        room_start_pt_ = room_start_pt;
        room_step_     = room_step;
        tot_delayed_ = tot_sat_ = tot_tickgap_ = tot_frames_ = 0;  /* neuer Stream */
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
            comb_y_      = allocFloat(2U * kHrWin);  /* kombinierter Kanal [I,Q] (V3.1) */
            allocated_   = allocated_ && comb_y_;
#endif
        }
        resetState();
        return allocated_;
    }

    /** Neubeginn ganz: Historie und Verfolgung (begin()). */
    void resetState() {
        resetHistory();
        resetTracking();
    }

    /**
     * Neubeginn der Historie (Rezoom, Neu-Kalibrierung, Neuverbinden; V2.3, B11): Ring, Filter- und Raumzustand,
     * Kreismittelpunkte und Atemzustand neu. Die Puls-Verfolgung bleibt, wenn der letzte gültige Report weniger als
     * kKeepTrackS zurückliegt; der erste Report danach glättet dann gegen den bisherigen Wert statt neu zu beginnen.
     * Nur aufrufen, während kein Report rechnet.
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
        person_run_           = 0;  /* Raumprofil beginnt neu */
        br_track_.reset();
        breath_det_.reset();  /* Weg beginnt bei 0: Filter und Spitzen neu (V3.4) */
        breath_pending_       = false;
        have_last_tick_       = false;  /* Rezoom/Neubeginn: der nächste tick-Abstand zählt nicht als Lücke */
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

    /** Puls-Verfolgung neu (kein bisheriger Wert). */
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
        mem_.clear();  /* V6: ohne Verfolgung kein Gedächtnis; resetHistory() mit beibehaltener Verfolgung behält es */
#endif
    }

    void setZoomStartPoint(int32_t zoom_start_pt) {
        zoom_start_pt_ = zoom_start_pt;
        resetHistory();
    }

#ifdef A121_DSP_REACQ
    /** V6: Ist ein Puls gemerkt (ohne Ablauf und Bedingungen zu prüfen; die prüft der nächste Report)? Für Tests. */
    bool reacqMemoryActive() const { return mem_.stored(); }
    const ReacqMemory &reacqMemory() const { return mem_; }
    /** V6, für Tests: Verfolgungszustand (hr_prev_, hr_miss_, alt_count_, hr_hist_), der in Fall 4 unverändert bleibt. */
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
    uint32_t frameCount() const { return frame_count_; }  /* Frames seit begin()/Rezoom/Neubeginn */

    /** Atemzug aus dem letzten pushFrame() abholen (V3.4; im Frame-Takt, Kern 1); false = keiner oder Tor zu. */
    bool takeBreathEvent(BreathEvent &ev) {
        if (!breath_pending_) return false;
        breath_pending_ = false;
        if (!breath_gate_) return false;
        ev = breath_ev_;
        return true;
    }

    /**
     * Tor für @BREATH (V3.4) aus dem Zustand des letzten ausgegebenen Reports (breathGateOpen()). Im Frame-Takt setzen,
     * wenn das Report-Ergebnis ausgegeben wird (ESP32: nach @STATE; Nachrechnung: bei @DSP), nicht im Report selbst:
     * so sehen ESP32 und Nachrechnung das Tor ab demselben Frame. Erkannt wird auch bei geschlossenem Tor weiter.
     */
    void setBreathGate(bool open) { breath_gate_ = open; }

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
    void pushFrame(const Complex16 *pts, uint16_t n_pts, int16_t temp_c, float dt_s, float &out_um_sample,
                   const FrameFlags &ff = FrameFlags()) {
        (void)dt_s;
        /* Sensor-Flags zählen (je Report zurückgesetzt); tick-Lücke nur innerhalb einer Historie */
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
        breath_pending_     = breath_det_.push(disp_um, breath_ev_);  /* V3.4 */
        hist_sat_[hist_write_idx_]    = ff.saturated ? 1U : 0U;
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
        tot_frames_++;
    }

    /** Stand für einen Report festhalten (im Frame-Takt nach pushFrame(), V2.1). */
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
     * @brief Report zum aktuellen Stand (Schnappschuss und Rechnung in einem, wie vor V2.1).
     *        Etwa einmal pro Sekunde aufrufen (Verfolgung und Rezoom zählen in Reports).
     */
    bool computeVitalReport(VitalReport &rep) {
        takeSnapshot(snap_);
        return computeVitalReport(snap_, rep);
    }

    /**
     * @brief Raum-Ziele + Rezoom-Empfehlung immer; Vitalwerte (valid = true) ab kHrMinWin Frames. Liest außer dem
     *        Schnappschuss nur den Ring im Fenster des Schnappschusses und Report-Zustand (Verfolgung, Kreismittelpunkte,
     *        Arbeitspuffer); pushFrame() darf parallel laufen, solange es höchstens kSnapMaxLag Frames weiter ist und
     *        zoom_start_pt_ nicht wechselt (setZoomStartPoint/begin erst nach dem Report).
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

        /* Person im Raum: Zoom-Ziel im geglätteten Profil in 2 Reports hintereinander (ohne Person flackerte es im
         * Prüfstand von Report zu Report) */
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
        float br_best = -1.0f;
        int16_t br_bin = -1;
        float wsum = 0.0f, best_q = -1.0f;
        uint8_t hr_bin = 0;
        uint32_t moving_blocks = 0, total_blocks = 0;
        for (uint16_t k = 0; k < kHrNumF; k++) comb_[k] = 0.0f;
        for (uint16_t k = 0; k < kBrNumF; k++) br_comb_[k] = 0.0f;
        const float *hann = hannWindow(M);
        /* Breites Zoom-Fenster (V2.3): Atem-Stärke vorab als Streuung der IQ-Werte im Fenster (≈ Amplitude² ×
         * Phasenhub², dieselbe Größe wie das Gewicht des Atem-Spektrums), nur die kSpecPoints stärksten rechnen. */
        bool use[kZoomPoints];
#ifdef A121_DSP_COMBINE
        constexpr uint16_t kSelPoints = kCombK;  /* V3.1: nur die K stärksten Punkte gehen in die Kombination */
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
        /* Pulsfenster: ohne A121_DSP_REACQ immer das ganze Fenster (Np = N) */
        uint16_t Np = N;
#ifdef A121_DSP_REACQ
        /* V6: Bewegungskarte vor den Spektren (vor combineZoom: motionMap nutzt work_x_, in dem combineZoom den
         * kombinierten Kanal ablegt). Kein bewegter Block: wie bisher. Bewegt und ruhiges Endstück ≥ kReacqMinS: Puls
         * (Spektren, Kandidaten, AKF, Einzelschläge) nur auf dem Endstück, wie vorläufig (V2.4) mit Bestätigungspflicht;
         * Atmung, Atemzustand, motion_pct und Raum weiter auf dem ganzen Fenster. Bewegt und Endstück kürzer: kein Puls,
         * die Verfolgung bleibt unverändert (vorher lief sie durch die Bewegung weiter und sprang auf Artefakte). */
        const MotionMap mm = motionMap(start, N, use);
        const bool moved = mm.mask != 0U;
        const bool pulse_off = moved && mm.clean_tail_blocks < kReacqMinS;
        const bool short_win = moved && !pulse_off;
        if (short_win) Np = (uint16_t)(mm.clean_tail_blocks * kGateBlk);
        rep.win_s = pulse_off ? 0U : (uint8_t)(Np / 60U);
        rep.reacq = short_win ? kReacqShort : kReacqNone;
#endif
        const uint16_t start_p = (uint16_t)((start + (N - Np)) % kHistoryLen);  /* Beginn des Pulsfensters */
        const uint16_t Mp = (uint16_t)((Np + kDecim - 1U) / kDecim);
#ifdef A121_DSP_REACQ
        const float *hann_p = (Np == N) ? hann : hannInto(hann_p_, hann_p_m_, Mp);
#else
        const float *hann_p = hann;
#endif
        uint32_t moving_p = 0, total_p = 0;  /* Bewegungsblöcke im Pulsfenster (für die Konfidenz) */
#ifdef A121_DSP_COMBINE
        /* V3.1: ein kombinierter Kanal statt Einzelspektren; die Schleife unten läuft einmal mit seinem Wegsignal und
         * dem dominanten Punkt als hr_bin */
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
            short_arc = comb_short;  /* work_x_ hält schon das Wegsignal des kombinierten Kanals */
            spread    = comb_spread;
            const float amp_c = comb_amp;
#else
            const float amp_c = binDisplacement(b, start, N, work_x_, short_arc, spread);
#endif
            removeLinearTrend(work_x_, N);
            float *h = work_ext_ + kFiltPad;  /* gefiltertes Signal steht nach sosFiltFilt mittig in work_ext_ */
            sosFiltFilt(work_x_, N);
            const uint8_t moving_b = gateMotion(h, N);
            moving_blocks += moving_b;
            /* Atem-Spektrum dieses Punkts (entzerrtes Wegsignal, dezimiert, Hann), gewichtet mit Amplitude² summiert.
             * Schwache Punkte haben Phasenrauschen ~ 1/Amplitude, und Abwickel-Fehler wirken dort wie Stufen von 2,5 mm:
             * Ungewichtet überdeckte ihr Tieftonanteil die Atmung (am Datensatz 2026-11-01 Punkte mit |z| 120–300,
             * 140–240 Phasensprünge in 15 s, 8–11 mm "Hub"; die starken Punkte zeigten sauber 17 /min). */
            const float wa = amp_c * amp_c;
            for (uint16_t m = 0; m < M; m++) dec_[m] = work_x_[(uint32_t)m * kDecim] * hann[m];
            fft_.power(dec_, M, spec_, kBrK0 + kBrNumF);
            float br_energy = 0.0f;
            for (uint16_t k = 0; k < kBrNumF; k++) {
                const float pk = spec_[kBrK0 + k];
                br_comb_[k] += wa * pk;
                br_energy += pk;
            }
            /* Atem-Punkt: stärkste Atmung (Amplitude² × Atemband) unter den Punkten mit sauberem Echo. Reines Rauschen
             * (kein Echo, Streuung ≈ 0,52) scheidet aus; dort läuft die Phase frei und täuscht Millimeter-Hub vor. */
            if (spread < kEchoSpreadMax && wa * br_energy > br_best) {
                br_best = wa * br_energy;
                br_bin  = (int16_t)b;
                memcpy(br_x_, work_x_, N * sizeof(float));
            }
            total_blocks += N / kGateBlk;
            /* Pulsteil auf dem Pulsfenster: xp = Wegsignal, h = gefiltert und gedämpft (ohne A121_DSP_REACQ das ganze
             * Fenster wie bisher) */
            float *xp = work_x_;
#ifdef A121_DSP_REACQ
            if (pulse_off) {  /* V6: kein Puls auf diesem Fenster; Atmung und Bewegung sind oben schon erfasst */
                weight[b]  = 0.0f;
                peak_hz[b] = 0.0f;
                continue;
            }
            if (Np != N) {  /* V6: Endstück nach Bewegung, Wegsignal neu entzerrt und gefiltert */
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
            /* dezimieren + Hann */
#ifdef A121_DSP_HARM_REMOVE
            /* V3.3: Atem-Oberwellen geführt von der Atemphase dieses Punkts entfernen, mit Atem- und Pulsfrequenz des
             * vorigen Reports. Nur, wenn der vorige Report sicher war: Verfolgte er einen falschen Puls (Prüfstand brady:
             * 2 × Puls, Konfidenz ≈ 20 %), schonte die Regel den falschen und löschte den echten, der dicht an einer
             * Atem-Oberwelle lag (MAE 2,8 → 3,7 /min). */
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
            /* Oberwellen-Summe S(f) = P(f) + 0,5 · P(2f): Ein Herzschlag ist ein kurzer Impuls mit kräftiger 2. Oberwelle,
             * eine Atem-Oberwelle m · f_Atem hat bei 2f kaum Leistung (V1.2). Im FFT-Raster liegt 2f genau auf Bin 2k. */
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
            const float w   = q * q * (short_arc ? 0.5f : 1.0f);  /* kurzer Bogen: Phase weniger sicher */
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
        conf_prev_ = 0.0f;  /* verbraucht; gilt erst wieder, wenn dieser Report gültig endet */
#endif
        rep.hr_bin     = hr_bin;
        rep.motion_pct = 100.0f * (float)moving_blocks / (float)(total_blocks ? total_blocks : 1U);

        /* Atmung über alle Punkte: größtes echtes lokales Maximum (nicht am Bandrand), parabolisch verfeinert.
         * Vorher zählte das größte Bin im Suchbereich; bei Lagewechseln (Stufen im Wegsignal) oder Rest-Drift steigt die
         * Leistung zu tiefen Frequenzen weiter an, und das Ergebnis klebte am Rand bei 6,9 /min. Ohne echte Spitze
         * bleibt fb = 0 (br_bpm = 0 heißt: keine Atemspitze). Vor der Pulssuche, weil die Oberwellen-Prüfung fb braucht. */
        float fb = 0.0f, br_q = 0.0f;
        int16_t kb = -1;
        /* Schwelle für die Atemspitze wächst im kurzen Fenster mit (V2.4): Bei 6 s überdeckt die Hauptkeule fast das
         * ganze Atemband, Spitze/Median lag im Prüfstand mit Person bei 1,5–1,7 (8 s: 2,0–4,3) */
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
        /* Atemzustand (V1.4) am Atem-Punkt; vor der Pulssuche, damit auch ein Report ohne Pulskandidat ihn trägt */
        const bool echo_ok = br_bin >= 0;
        rep.br_state = br_track_.update(echo_ok ? breathAmplitude(br_x_, N, kFsHz) : -1.0f, br_q >= br_min_q && fb > 0.0f,
                                        rep.motion_pct);
        const bool breath_held = rep.br_state == kBrStateApnea;
        /* Atemhub (Spitze-Spitze) am Atem-Punkt bei der Atemfrequenz aller Punkte (auf 15 Hz dezimiert, Hann); vor V1.3
         * am Punkt mit klarstem Puls, der auch ein Rausch-Punkt sein konnte (B10, B19). Ohne sauberes Echo am Punkt mit
         * klarstem Puls über dessen Pulsfenster (Wert dann bedeutungslos, der Report gilt ohne Echo nicht). */
        {
            const uint16_t Mr = echo_ok ? M : Mp;
            const float *hr_w = echo_ok ? hann : hann_p;
            for (uint16_t m = 0; m < Mr; m++) dec_[m] = (echo_ok ? br_x_ : best_x_)[(uint32_t)m * kDecim] * hr_w[m];
            const float best_p = (fb > 0.0f) ? goertzelPower(dec_, Mr, kFsHz / kDecim, fb) : 0.0f;
            const float norm   = 0.25f * (float)Mr;  /* Hann-Verstärkung → Amplitude in µm */
            rep.br_bpm        = breath_held ? 0.0f : fb * 60.0f;  /* angehalten: keine Atemfrequenz (vorher 15,9 /min) */
            rep.resp_depth_um = 2.0f * sqrtf(best_p) / norm;
        }
#ifdef A121_DSP_REACQ
        /* V6: Ohne Pulsteil (pulse_off) ist best_x_ vom Vorreport; ohne Echo also kein Atemhub */
        if (pulse_off && !echo_ok) rep.resp_depth_um = 0.0f;
#endif
        const bool breath_ok = echo_ok && br_q >= br_min_q && rep.resp_depth_um >= kBreathMinUm;
#ifdef A121_DSP_REACQ
        /* V6: Gedächtnis prüft jeden Report (abgelaufen, kein sauberes Echo oder Ziel nicht im Zoom-Fenster: gelöscht) */
        const bool mem_on = mem_.active(s.tot_frames, echo_ok, rep.target_in_zoom);
        if (pulse_off) {  /* V6: bewegt, ruhiges Endstück < kReacqMinS: kein Puls, Verfolgung unverändert */
            rep.valid = false;
            setState(s, rep, measurementState(person, echo_ok, breath_ok, false, rep.motion_pct, rep.br_state));
            rep.compute_us = micros() - t0;
            return false;
        }
#endif
#ifdef A121_DSP_HARM_REMOVE
        fb_prev_ = (br_q >= br_min_q && fb > 0.0f && !breath_held) ? fb : 0.0f;  /* für den nächsten Report (V3.3) */
#endif
        /* Atem-Oberwellen (m = 2..8) im Pulsband: Bei schneller Atmung (≈ 20 /min) lag die 3. Oberwelle bei 55–60 /min
         * und überdeckte den Puls ganz. Kandidaten auf m ≥ 4 bekommen bei der Wahl S × 0,5 (der Puls kann darauf liegen,
         * live 2026-11-01: Puls 78 ≈ 4 × Atmung 19,5), ein Sprung dorthin braucht kHoldHarm statt 3 Reports, und die
         * Konfidenz halbiert sich (signalQuality). harmOrder: m, wenn f auf m · f_Atem liegt, sonst 0. Für m ≤ 3 gilt
         * die ganze Hauptkeule (±kLowLobeHz) und S × 0,02: Diese Oberwellen sind 10–100 × stärker als der Puls, ein
         * Puls darin ist spektral nicht zu trennen, und im ersten Report (8-s-Fenster) ist f_Atem bis 8 % ungenau
         * (Prüfstand base/step/beweg: 3 × Atmung 4 s lang gemeldet). */
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
        /* Abschlag je Spitze, nicht je Frequenzschritt: sonst entsteht in der Oberwellen-Spitze eine Kerbe, und ihre
         * Flanke wird zum Scheinkandidaten (Prüfstand: 57 statt 54 /min). pen_ = S × Abschlag an lokalen Maxima von S. */
        for (uint16_t k = 0; k < kHrNumF; k++) {
            const bool peak = k > 0 && k + 1 < kHrNumF && comb_[k] >= comb_[k - 1] && comb_[k] >= comb_[k + 1];
            pen_[k] = peak ? comb_[k] * harmPenalty(binHz((int16_t)k)) : 0.0f;
        }

        /* Kandidaten: Top 3 lokale Maxima nach pen_ mit ≥ 0,08 Hz Abstand, nicht am Bandrand (am Datensatz lag die
         * häufigste Fehlspitze bei 54 /min = 0,90 Hz direkt über der Suchgrenze 0,88 Hz und wurde von vielen Punkten
         * gestützt) */
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
        /* Oktav-Prüfung (B5) je Kandidat: Bei langsamem Puls ist die 2. Herz-Oberwelle oft stärker als die Grundwelle
         * (Prüfstand: Puls 50 → 100 gemeldet), und die Verfolgung sprang sonst auch auf 2 × Puls (145 statt 72). Liegt
         * bei f/2 (±0,02 Hz, im Suchband) eine Spitze mit S(f/2) ≥ kSubRel · S(f), steht der Kandidat für f/2; sein
         * Wert ist der größere von beiden. S ohne Abschlag: Der Puls liegt oft auf einer Atem-Oberwelle m ≥ 4 (Prüfstand
         * brady: 50 ≈ 4 × 12). Nicht, wenn f/2 in der Hauptkeule einer niedrigen Atem-Oberwelle (m ≤ 3, ±kLowLobeHz)
         * liegt: Die ist 10–100 × stärker als der Puls, und S(f/2) enthält ohnehin 0,5 · P(f), sodass ihre Flanke dort
         * eine Scheinspitze bildet (Prüfstand tachy: 120 → 60,6 bei 3 × 19 = 57). */
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
        /* doppelte entfernen (≤ 0,08 Hz), nach Wert sortieren */
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
        /* Kandidaten-Frequenz parabolisch zwischen den Bins verfeinert (V2.2; Kandidaten sind lokale Maxima von S) */
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
        rep.hr_harm = isHarmHz(cf[0]);
        const float raw_bpm = cf[0] * 60.0f;
        rep.hr_spec_bpm = raw_bpm;
        memcpy(scratch_, comb_, kHrNumF * sizeof(float));
        std::nth_element(scratch_, scratch_ + kHrNumF / 2, scratch_ + kHrNumF);
        const float comb_med = scratch_[kHrNumF / 2] + 1e-12f;
        rep.zoom_snr_db = 10.0f * log10f(comb_[cand[0]] / comb_med);
        auto sAtBpm = [&](float bpm) {  /* kombiniertes Spektrum (ohne Abschlag) am nächsten Frequenzschritt */
            int k = (int)lroundf((bpm / 60.0f - kHrFMin) / kHrFStep);
            k = k < 0 ? 0 : (k >= (int)kHrNumF ? (int)kHrNumF - 1 : k);
            return comb_[k];
        };

        /* Verfolgung mit Haltezeit. Ein naher Kandidat (±12 /min) gewinnt nur mit ≥ 0,5 × dem besten Kandidatenwert
         * (vorher 0,4 ohne Abschlag: die Verfolgung blieb an starken Fremdspitzen hängen, B6). Liegt der verfolgte Wert
         * auf 2 × / 3 × Atmung und gibt es 3 Reports lang einen Kandidaten ohne Oberwelle mit ≥ 0,5 × dem Wert dort,
         * wird gewechselt. Ein Abbruch der Haltezeit über die Punkt-Zustimmung wurde 2026-11-01 verworfen (rhythmische
         * Bewegung stützen alle Punkte). */
        /* Vorläufig (V2.4): Beginnt die Verfolgung in einem Fenster unter 8 s, gilt nur der rohe Wert dieses Reports; er
         * wird nicht festgeschrieben (Prüfstand: im 6-s-Fenster lag die stärkste Spitze oft am Bandrand oder auf einer
         * Atem-Oberwelle, und die Haltezeit hielt den Fehlwert bis zu 10 Reports). */
        const bool prov = prov_ && N < kHrStableWin;
        if (!prov) prov_ = false;
#ifdef A121_DSP_VITERBI
        float vit_mass = 0.0f;
#endif
#ifdef A121_DSP_REACQ
        /* V6: Endstück nach Bewegung wie vorläufig: Die Verfolgung wird nicht fortgeschrieben (auch Viterbi nicht).
         * Gemeldet wird der stärkste Kandidat, bei aktivem Gedächtnis zuerst der stärkste nahe am gemerkten Puls
         * (preferMemoryCandidate); hr_prev_ bleibt nur, wenn der Wert unten bestätigt wird, sonst gilt wieder hr_keep. */
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
            vit_mass = 1.0f;  /* keine Viterbi-Masse in diesem Report: Konfidenz nicht begrenzen */
#endif
        } else {
#endif
#ifdef A121_DSP_VITERBI
        /* V3.2: Viterbi über die letzten 20 Reports statt Haltezeit/Wechselregeln; Ergebnis auf den nächsten Kandidaten
         * (±3 /min, verfeinert) gezogen, nahe Werte wie bisher geglättet */
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
            if (hr_prev_ <= 0.0f || prov) hr_prev_ = raw_bpm;  /* vorläufig (V2.4): roher Wert, wie ohne Schalter */
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
            /* Vergleich mit Abschlag: Unbeschnitten bliebe die Verfolgung an einer starken Atem-Oberwelle hängen, weil
             * deren Wert um die halbe Spitzenhöhe pendelt. Nur für m ≤ 3: Auf m ≥ 4 liegt oft der Puls selbst (Prüfstand
             * harm4, brady), ein Wechsel ging dort auf die 3. Herz-Oberwelle (75 statt 50). */
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

        /* Herzsignal des klarsten Punkts (Pulsfenster): Amplitude, Autokorrelation, Einzelschläge */
        float hr_rms = 0.0f;
        for (uint16_t n = 0; n < Np; n++) hr_rms += best_h_[n] * best_h_[n];
        rep.heart_amp_um = sqrtf(2.0f * hr_rms / (float)Np);
        removeBreathHarmonics(best_h_, Np, kFsHz, (br_q >= br_min_q) ? fb : 0.0f, hr_prev_ / 60.0f);
        float acf_bpm = 0.0f, acf_peak = 0.0f;
        cardiacAcf(best_h_, Np, kFsHz, acf_bpm, acf_peak);
#ifdef A121_DSP_IBI_TEMPLATE
        /* V3.6: Einzelschläge mit Vorlage auf dem Band 0,6–8 Hz des klarsten Punkts; Bewegungsdämpfung und Abzug der
         * Atem-Oberwellen wie für best_h_ */
        sosFiltFiltSos(best_x_, Np, work_ext_, kBp8Sos);
        memcpy(work_x_, work_ext_ + kFiltPad, Np * sizeof(float));
        gateMotion(work_x_, Np);
        removeBreathHarmonics(work_x_, Np, kFsHz, (br_q >= br_min_q) ? fb : 0.0f, hr_prev_ / 60.0f);
        const IbiResult ibi = templateIbi(work_x_, Np, kFsHz, hr_prev_, work_ext_);
#else
        const IbiResult ibi = timeDomainIbi(best_h_, Np, kFsHz, hr_prev_);
#endif
        const float ibi_bpm = ibi.bpm;
        const float acf_res = (acf_peak > 0.0f) ? acf_bpm : 0.0f;  /* 0 = kein AKF-Ergebnis */
        /* Bestätigung eines kurzen Fensters (V2.4 vorläufig, V3.1; V6 Endstück): Autokorrelation oder Einzelschläge
         * innerhalb ±5 /min des Kandidaten (f_hz) und nicht auf einer niedrigen Atem-Oberwelle (m ≤ 3): Die sehen AKF und
         * Einzelschläge desselben Signals genauso, die Bestätigung zählt dort nicht (Prüfstand base mit
         * A121_DSP_COMBINE: 46 statt 71 /min). */
        auto confirmedHz = [&](float f_hz) {
            const float bpm   = f_hz * 60.0f;
            const bool acf_ok = acf_res > 0.0f && fabsf(acf_res - bpm) <= 5.0f;
            const bool ibi_ok = ibi_bpm > 0.0f && fabsf(ibi_bpm - bpm) <= 5.0f;
            const uint8_t m   = harmOrder(f_hz);
            return (acf_ok || ibi_ok) && !(m != 0 && m <= kHarmLowMax);
        };
#ifdef A121_DSP_REACQ
        /* V6: Ein Gedächtnis-Kandidat ohne Bestätigung fällt auf die normale Suche (stärkster Kandidat) zurück; AKF und
         * Einzelschläge bleiben dafür dieselben (Abstand ≤ kMemBandBpm). */
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

        /* frühere Konfidenz als Diagnosewert: gewichteter Anteil der Punkte mit eigener Spitze ±5 /min am Ergebnis */
        float agree = 0.0f;
        for (uint8_t b = 0; b < kZoomPoints; b++)
            if (fabsf(peak_hz[b] * 60.0f - hr_prev_) <= 5.0f) agree += weight[b];
        rep.point_agree_pct = 100.0f * agree / (wsum + 1e-12f);

        rep.hr_acf_bpm    = (acf_peak > 0.0f) ? acf_bpm : hr_prev_;
        rep.hr_ibi_bpm    = (ibi_bpm > 0.0f) ? ibi_bpm : hr_prev_;
        rep.ibi_median_ms = (ibi_bpm > 0.0f) ? ibi.ibi_ms : 60000.0f / hr_prev_;
        rep.rmssd_ms      = ibi.rmssd_ms;

        /* Konfidenz (V1.3) */
        QualityInputs qi;
        qi.peak_db         = 10.0f * log10f(sAtBpm(hr_prev_) / comb_med);
        qi.on_harmonic     = isHarmHz(hr_prev_ / 60.0f);
        qi.hr_spec         = rep.hr_spec_bpm;
        qi.hr_acf          = acf_res;                             /* ohne Ergebnis keine Zustimmung (in @VITAL steht */
        qi.hr_ibi          = ibi_bpm;                             /* dann wie bisher der verfolgte Wert)            */
        qi.hr_fused        = hr_prev_;
        qi.hr_fused_5s_ago = (hr_hist_n_ >= 5) ? hr_hist_[(hr_hist_n_ - 5) % 5] : 0.0f;
        qi.motion_pct      = 100.0f * (float)moving_p / (float)(total_p ? total_p : 1U);  /* Pulsfenster (= motion_pct) */
        {
            uint16_t n_sat_win = 0;
            for (uint16_t n = 0; n < Np; n++) n_sat_win += hist_sat_[(start_p + n) % kHistoryLen];
            qi.sat_pct = 100.0f * (float)n_sat_win / (float)Np;
        }
        qi.win_frac = (float)Np / (float)kHrWin;
        rep.confidence_pct = signalQuality(qi);
#ifdef A121_DSP_VITERBI
        rep.confidence_pct = fminf(rep.confidence_pct, 100.0f * vit_mass);  /* V3.2: Masse ±5 /min */
#endif
#ifdef A121_DSP_REACQ
        if (!prov && !short_win) pushHrHist(hr_prev_);  /* Endstück: erst nach Bestätigung (unten) */
#else
        if (!prov) pushHrHist(hr_prev_);
#endif

        /* Lebenszeichen (B19): gültig nur mit sauberem Echo im Zoom-Fenster und Atmung (Atemspitze/Median ≥ kBrMinQ und
         * ≥ 100 µm Hub am Atem-Punkt) oder bei angehaltenem Atem mit deutlicher Pulsspitze; dazu höchstens 30 %
         * Bewegung im Fenster. Die Atemspitze allein reicht nicht: Rausch-Punkte erzeugen auch ohne Person Spitzen. */
        const bool held_pulse = echo_ok && breath_held && qi.peak_db >= kPulseHeldDb;
#ifdef A121_DSP_REACQ
        /* V6: Statt der 30-%-Regel gilt die Fensterwahl (bewegt mit zu kurzem Endstück ist oben schon ungültig); als
         * Lebenszeichen zählt auch der über das Gedächtnis wiedergefundene Puls (Atemspitze nach Unruhe oft verschmiert).
         * Endstück: nur bestätigt; dann wird der Wert verfolgt, sonst bleibt die Verfolgung wie vor dem Report.
         * Bewusste Abweichung von V2.4: Ein Endstück-Report im Start (short_win zusammen mit prov) schreibt den bestätigten
         * Wert fest (hr_prev_ bleibt), statt ihn wie vorläufig zu verwerfen (Spec Punkt 3: hr_prev_ aus einem bestätigten
         * Wert); der prov-Zweig unten gilt dann nicht. */
        rep.valid = breath_ok || held_pulse || rep.reacq == kReacqMemory;
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
        if (prov_check) {  /* vorläufig nur mit Bestätigung des Spektralwerts (confirmedHz) */
            rep.valid = rep.valid && confirmedHz(cf[0]);
            hr_prev_  = 0.0f;
        }
        if (rep.valid) {
            have_valid_     = true;
            last_valid_tot_ = s.tot_frames;
        }
#ifdef A121_DSP_REACQ
        /* V6: sicherer Wert (Konfidenz ≥ kMemMinConf, Pulsfenster ≥ 8 s) ins Gedächtnis */
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

    /** hr_fused der letzten 5 Reports (Sprung-Abschlag der Konfidenz) */
    void pushHrHist(float hr) {
        hr_hist_[hr_hist_n_ % 5] = hr;
        if (hr_hist_n_ < 250) hr_hist_n_++;
        else hr_hist_n_ = 5 + (hr_hist_n_ % 5);
    }

    /** Zustand der Messung eintragen; seit_ms zählt ab dem letzten Wechsel, über Rezooms hinweg (V3.5). */
    void setState(const ReportSnapshot &s, VitalReport &rep, uint8_t st) {
        if (st != state_) {
            state_           = st;
            state_since_tot_ = s.tot_frames;
        }
        rep.state          = st;
        rep.state_since_ms = (uint32_t)lroundf((float)(s.tot_frames - state_since_tot_) * 1000.0f / kFsHz);
    }

    /** Hann-Fenster der Länge M (dezimierte Folge); neu gerechnet nur, wenn sich M ändert (Fensteraufbau), V2.2. */
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

    /** Phase um (ci, cq) abwickeln → out[] in µm. Rückgabe: mittlerer Abstand zum Mittelpunkt (Radius);
     *  span = überstrichene Phase (max − min, rad), spread = Streuung des Abstands relativ zum Radius. */
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

    /** Phase des Zoom-Punkts b um den Kreismittelpunkt (circleCenter) abwickeln → µm. Überstreicht die Phase weniger als
     *  kArcMinRad, ist der Fit schlecht bestimmt (kurzer Bogen): Dann gilt der letzte gute Mittelpunkt dieses Punkts,
     *  solange die IQ-Werte auf dessen Kreis liegen (Streuung höchstens doppelt so groß wie um den Fit), und short_arc
     *  meldet dem Aufrufer, das Gewicht des Punkts zu halbieren. Rückgabe: mittlerer Abstand zum Mittelpunkt (Radius),
     *  spread: Streuung des Abstands relativ zum Radius (klein = sauberes Echo, ≈ 0,5 = nur Rauschen). */
    float binDisplacement(uint8_t b, uint16_t start, uint16_t N, float *out, bool &short_arc, float &spread,
                          float *c_out = nullptr) {
        const float *base = hist_zoom_ + (size_t)b * kHistoryLen * 2U;  /* Ring dieses Punkts, fortlaufend im PSRAM */
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
                have_good_[b] = false;  /* Szene hat sich geändert: alter Mittelpunkt passt nicht mehr */
            }
        }
        if (c_out) {  /* benutzter Mittelpunkt (V3.1) */
            c_out[0] = ci;
            c_out[1] = cq;
        }
        return radius;
    }

#ifdef A121_DSP_COMBINE
    /** V3.1: Die gewählten Punkte (use) um ihre Kreismittelpunkte verschieben (binDisplacement, mit Rückfall auf den
     *  letzten guten Mittelpunkt) und zur 1. Hauptkomponente kombinieren (combinePrincipal). Punkte mit Rauschen statt
     *  Echo (Streuung ≥ kEchoSpreadMax) bleiben draußen, außer es gibt keinen anderen. Das Wegsignal des Kanals steht
     *  danach in work_x_ (µm). Rückgabe: dominanter Zoom-Punkt; amp/short_arc/spread wie binDisplacement. */
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
        if (n_clean > 0 && n_clean < K) {  /* nur saubere Punkte behalten */
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

    /** Nullphasen-Bandpass 0,6–5 Hz (kBpSos) wie scipy `sosfiltfilt`. Ergebnis: work_ext_[kFiltPad .. kFiltPad + N). */
    void sosFiltFilt(const float *x, uint16_t N) { sosFiltFiltSos(x, N, work_ext_, kBpSos); }

    /** Streuung der IQ-Werte jedes Zoom-Punkts im Fenster (≈ Amplitude² × Phasenhub², um den ersten Wert verschoben). */
    void zoomStrength(uint16_t start, uint16_t N, float *strength) const {
        for (uint8_t b = 0; b < kZoomPoints; b++) {
            const float *base = hist_zoom_ + (size_t)b * kHistoryLen * 2U;
            const float *z0 = base + (size_t)start * 2U;  /* um den ersten Wert verschoben (Auslöschung) */
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
    /** V6: Bewegungskarte des Fensters start .. start + N aus den kMotionPoints stärksten Punkten unter use: Wegsignal wie
     *  in der Hauptschleife (binDisplacement, Trend, Bandpass), Effektivwert je 1-s-Block (ohne Dämpfung gateMotion),
     *  dann motionMapFromRms. Nutzt work_x_ und work_ext_ als Arbeitsfeld (überschreibt das Wegsignal der Hauptschleife);
     *  binDisplacement pflegt dabei good_ci_/good_cq_/have_good_ wie dort. Ohne Heap: 3 × kMaxBlocks Werte auf dem Stack. */
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
            if (sel[b] && use[b]) pts[n_pts++] = b;  /* selectStrongest füllt bei wenigen Punkten in use mit unbenutzten auf */
#ifdef A121_DSP_REACQ_PERIOD
        /* der stärkste Punkt kommt zuletzt: sein gefiltertes Wegsignal bleibt in work_ext_ für die Periodizitätsprüfung */
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
                const uint16_t a = k * kGateBlk, e = (k + 1U == nb) ? N : (uint16_t)(a + kGateBlk);  /* wie gateMotion */
                float acc = 0.0f;
                for (uint16_t n = a; n < e; n++) acc += h[n] * h[n];
                rms[i * nb + k] = sqrtf(acc / (float)(e - a));
            }
        }
        MotionMap mm = motionMapFromRms(rms, n_pts, nb);
#ifdef A121_DSP_REACQ_PERIOD
        /* Variante: ein zu starker Block zählt nur als bewegt, wenn das Signal um ihn (3 s am stärksten Punkt) nicht
         * periodisch ist. Der Ausschnitt liegt in dec_ (wird in der Hauptschleife vor der Nutzung neu gefüllt). */
        if (mm.mask) {
            const float *h = work_ext_ + kFiltPad;  /* Weg des stärksten Punkts (zuletzt gerechnet) */
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

    void extractRoomTargets(const ReportSnapshot &s, VitalReport &rep) {
        rep.num_room_targets = 0;
        /* Rauschpegel getrennt für Amplitude und Bewegung. Reines Rauschen hat Bewegung ≈ Amplitude (das Standbild
         * mittelt es weg); am Datensatz 2026-11-01 beide ≈ 55, die Person bis 3300 / 2600. +1 gegen Division durch 0
         * in den ersten Frames. */
        const float noise_amp = lowerQuartile(s.room_amp) + 1.0f;
        const float noise_mot = lowerQuartile(s.room_mot) + 1.0f;
        rep.room_noise_amp    = noise_amp;
        rep.room_noise_motion = noise_mot;

        /* Ziel = lokales Maximum von max(Amplitude, Bewegung) relativ zum Rauschen, ab kRoomDetK. So zählen feste Echos
         * (Wand, Möbel: Amplitude hoch, Bewegung ≈ Rauschen) und schwache bewegte Echos. Behalten werden die
         * kMaxRoomTargets stärksten nach Amplitude + 8 × Bewegung (Bewegung zählt stärker), egal in welcher Entfernung. */
        auto key = [](const RoomTarget &t) { return t.amplitude + 8.0f * t.motion_energy; };
        forEachRoomPeak(s.room_amp, s.room_mot, noise_amp, noise_mot, [&](uint16_t r, float d, bool moving) {
            const RoomTarget t = {roomDistM(r, d), s.room_amp[r], s.room_mot[r], moving};
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
        rep.person_in_room = best_m >= 0.0f;
        rep.target_in_zoom = best_m >= 0.0f && best_m >= zoom_min_m && best_m <= zoom_max_m;
        if (best_m < 0.0f || (best_m >= zoom_min_m && best_m <= zoom_max_m)) {
            out_of_window_frames_ = 0;
            return;
        }
        /* Start (noch kein Neubeginn seit begin()): nach 2 s zoomen, also beim 2. Report (V2.4); sonst 3 bzw. 5 */
        const bool start = s.tot_frames == s.frame_count && s.frame_count <= kStartZoomFrames;
        const uint16_t needed = start ? 2U : ((s.frame_count < 600U) ? 3U : 5U);
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
#ifdef A121_DSP_VITERBI
    ViterbiTracker vit_;                                           /* V3.2, Report-Seite */
#endif
#ifdef A121_DSP_HARM_REMOVE
    static_assert(kHrWin / kDecim + 1U <= kGuideFft, "Fenster nach Dezimierung passt nicht in kGuideFft");
    a121_fft::ComplexFft<kGuideFft> cfft_;                        /* V3.3, Report-Seite */
    float    guide_disp_[kGuideFft];
    float    guide_work_[2U * kGuideFft];
    float    fb_prev_              = 0.0f;  /* Atemfrequenz des vorigen Reports (0 = keine Atemspitze) */
    float    conf_prev_            = 0.0f;  /* Konfidenz des vorigen Reports (0 = ungültig) */
#endif
    BreathDetector breath_det_;                                    /* V3.4, nur Push-Seite */
    BreathEvent    breath_ev_;
    bool     breath_pending_       = false;
    bool     breath_gate_          = false;  /* V3.4, Push-Seite */
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
    uint8_t  alt_count_            = 0;     /* Reports in Folge mit Alternative zur Oberwelle */
    BreathStateTracker br_track_;           /* Atemzustand (V1.4) */
    uint32_t tot_delayed_ = 0, tot_sat_ = 0, tot_tickgap_ = 0;    /* Sensor-Zähler seit begin() (V1.5, Summen seit V2.1) */
    uint32_t prev_delayed_ = 0, prev_sat_ = 0, prev_tickgap_ = 0; /* … beim zuletzt gerechneten Report */
    uint32_t tot_frames_ = 0;                                      /* Frames seit begin() (V2.3) */
    uint32_t last_valid_tot_ = 0;                                  /* tot_frames des letzten gültigen Reports */
    bool     have_valid_ = false;
    bool     prov_ = true;                                         /* Verfolgung noch vorläufig (Fenster < 8 s, V2.4) */
    uint8_t  state_ = 0xFFU;                                       /* letzter Zustand der Messung (V3.5) */
    uint8_t  person_run_ = 0;                                      /* Reports in Folge mit Zoom-Ziel im Raumprofil */
    uint32_t state_since_tot_ = 0;
    ReportSnapshot snap_ = {};                                     /* für computeVitalReport(rep) ohne eigenen Stand */
    a121_fft::RealFft<kFftN> fft_;                                  /* Spektren je Zoom-Punkt (V2.2) */
    float    hann_[kHrWin / kDecim + 1U] = {};
    uint16_t hann_m_ = 0;
#ifdef A121_DSP_REACQ
    float    hann_p_[kHrWin / kDecim + 1U] = {};  /* V6: Hann für das kürzere Pulsfenster (Endstück nach Bewegung) */
    uint16_t hann_p_m_ = 0;
    ReacqMemory mem_;                              /* V6: zuletzt gesicherter Puls */
#endif
    uint32_t last_tick_ = 0;
    bool     have_last_tick_ = false;
    uint8_t  hist_sat_[kHistoryLen] = {};   /* data_saturated je Frame der Historie */
    float    hr_hist_[5]           = {};    /* hr_fused der letzten 5 Reports (Sprung-Abschlag) */
    uint8_t  hr_hist_n_            = 0;

    float good_ci_[kZoomPoints]        = {};  /* letzter gut bestimmter Kreismittelpunkt je Zoom-Punkt (Bogen ≥ 2 rad) */
    float good_cq_[kZoomPoints]        = {};
    bool  have_good_[kZoomPoints]      = {};
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
    float *hist_lb_phi_ = nullptr;
    float *work_x_      = nullptr;
    float *work_ext_    = nullptr;  /* sosfiltfilt mit Rand: kHrWin + 2 * kFiltPad */
#ifdef A121_DSP_COMBINE
    float *comb_y_      = nullptr;  /* kombinierter Kanal [I,Q] × kHrWin (V3.1) */
#endif
    float *best_x_      = nullptr;
    float *best_h_      = nullptr;
    float *br_x_        = nullptr;  /* Wegsignal des Atem-Punkts (stärkste Atmung unter den Punkten mit sauberem Echo) */
    float *dec_         = nullptr;
    float *spec_        = nullptr;  /* P(f) je Punkt */
    float *sum_         = nullptr;  /* S(f) = P(f) + 0,5 · P(2f) je Punkt */
    float *comb_        = nullptr;
    float *scratch_     = nullptr;
    float *br_comb_     = nullptr;
    float *pen_         = nullptr;  /* comb_ mit Abschlag auf Atem-Oberwellen (Kandidatenwahl) */
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

/*
 * Report-Zeilen (`iq3`), gemeinsam für Firmware, PC-/Handy-Nachrechnung (host/dsp_replay) und Prüfstand
 * (host/vital_sim.py). Neue Felder nur hinten anhängen; @VITAL behält seine 15 Felder (docs/ANDROID_APP.md).
 *   @DSP,t_ms,ok,hr_bin,konfidenz_pct,compute_us,motion_pct,hr_harm,point_agree_pct,br_state,n_delayed,n_sat,n_tickgap
 *        (+ mit A121_DSP_COMBINE n_eff als 15. Feld; + mit A121_DSP_REACQ: n_eff (ggf. leer), win_s, reacq)
 *   @VITAL,t_ms,br,hr_fused,hr_spec,hr_acf,hr_ibi,ibi_ms,rmssd_ms,konfidenz,resp_um,heart_um,snr_db,lb_std_mrad,
 *          target_mm,n_room
 *   @REZOOM,zoom_start_mm,zoom_end_mm
 * compute_us getrennt, weil die Nachrechnung die Rechenzeit des ESP32 übernimmt.
 */
#define A121_DSP_FORMATS 1  /* host/dsp_replay erkennt daran Header ab V0.4 (ältere bekommen dort die alten Formate) */
static constexpr size_t kReportLineMax = 256U;

inline size_t clampLen(int len, size_t cap) { return (len < 0) ? 0U : std::min((size_t)len, cap - 1U); }

inline size_t formatDspLine(char *buf, size_t cap, uint32_t t_ms, bool ok, const VitalReport &rep, uint32_t compute_us) {
#ifdef A121_DSP_REACQ
    /* V6: Feld 14 n_eff (Zahl mit A121_DSP_COMBINE, sonst leer), Feld 15 win_s, Feld 16 reacq; nur mit Schalter */
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
    /* V3.1: effektive Punktzahl als 15. Feld, nur mit Schalter (Standardzeile unverändert) */
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
/** @STATE,t_ms,zustand,seit_ms (V3.5): je Report nach @VITAL bzw. @ROOM. */
inline size_t formatStateLine(char *buf, size_t cap, uint32_t t_ms, const VitalReport &r) {
    return clampLen(snprintf(buf, cap, "@STATE,%lu,%u,%lu", (unsigned long)t_ms, (unsigned)r.state,
                             (unsigned long)r.state_since_ms),
                    cap);
}

/** @BREATH,t_ms,tick,intervall_ms,tiefe_um,rate (V3.4): je erkanntem Atemzug, direkt nach @IQ3/@RAW des Frames.
 *  rate = 60 / Median der letzten (bis zu) 4 Abstände, /min. */
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
