#!/usr/bin/env python3
"""Synthetischer Prüfstand für die Vital-DSP (Audit 2026-11-01, docs/VITAL_AUDIT.md).

NUR SIMULATION: Das Brustmodell (mehrere Streuzentren, Gauß-Hüllkurve je Profil, komplexes Rauschen, nicht-sinusförmige
Atmung, Herzschlag als Pulsfolge mit Streuung) ist eine Annahme. Ergebnisse zeigen Schwächen der Logik und die Richtung
einer Änderung, sie ersetzen keine Messung mit Referenzgerät.

Modi:
  demod   Ein Range-Punkt z = C + A·exp(j·4π·x/λ) + Rauschen: Herzband-Fehler nach Phasen-Demodulation mit
          wahrem Kreismittelpunkt, Fenster-Mittelwert (a121_esp_dsp.h bis 2026-11-01) und Kåsa-Kreisfit (seit V1.1).
  bench   Übersetzt firmware/esp32s3/src/a121_esp_dsp.h (oder --header) für den PC, erzeugt iq3-Frames (105 Punkte)
          für mehrere Szenarien und vergleicht die Reports mit der Simulationswahrheit (Puls, Atmung, RMSSD, Konfidenz,
          Reaktionszeit, Atemzustand).

  .venv/bin/python host/vital_sim.py demod
  .venv/bin/python host/vital_sim.py bench [--out <ordner>] [--scen base harm4 ...] [--header <a121_esp_dsp.h>]
  .venv/bin/python host/vital_sim.py bench --check     # Regressionstest: Exit 1, wenn ein Grenzwert verletzt ist
  .venv/bin/python host/vital_sim.py record --out <ordner> [--scen anhalt]   # synthetische Aufnahme im App-Format

--check prüft nur den angegebenen Header (Standard: Firmware) gegen LIMITS. Jede DSP-Änderung verschärft die Grenzwerte,
die sie verbessert (docs/VITAL_AUDIT.md, V0.5).

Die erzeugten Frames (je Szenario ≈ 3 MB) landen in einem temporären Ordner oder unter --out (nicht im Repo).
"""

from __future__ import annotations

import argparse
import inspect
import pathlib
import warnings
import subprocess
import sys
import tempfile

import numpy as np
from scipy import signal

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DSP_HEADER = REPO_ROOT / "firmware" / "esp32s3" / "src" / "a121_esp_dsp.h"

LAM = 299792458.0 / 60.5e9
UM_PER_RAD = LAM / (4 * np.pi) * 1e6
FS = 60.0
STEP_M = 0.0025
ZOOM_N, ROOM_N, ROOM_START_PT, ROOM_STEP = 24, 80, 120, 12
HR_WIN = 900  # Pulsfenster in Frames (A121_HR_WIN, V2.5)
DEFINES: list[str] = []  # weitere Schalter für den Header (--define, V3-Varianten)


# ---------------------------------------------------------------------------------------------------------
# Signalmodell
# ---------------------------------------------------------------------------------------------------------

def heart_beats(rng, T, hr_fn, hrv_ms):
    beats, tb = [], 0.2
    while tb < T + 1:
        beats.append(tb)
        tb += 60.0 / hr_fn(tb) + rng.normal(0, hrv_ms / 1000.0)
    return np.array(beats)


def heart_wave(t, beats, pp_um):
    """Je Schlag ein glatter Puls (zwei Gauß-Anteile); Spitze-Spitze auf pp_um normiert."""
    h = np.zeros_like(t)
    for bt in beats:
        h += np.exp(-0.5 * ((t - bt) / 0.06) ** 2) - 0.45 * np.exp(-0.5 * ((t - bt - 0.18) / 0.09) ** 2)
    return (h - h.mean()) / (np.ptp(h) + 1e-12) * pp_um


def breath_phase(t, rr_fn):
    rr = np.array([rr_fn(x) for x in t])
    return 2 * np.pi * np.cumsum(rr / 60.0) / FS, rr


def breath_wave(ph, pp_um):
    """Nicht-sinusförmige Atmung (natürliche 2./3. Oberwelle), Spitze-Spitze ≈ pp_um."""
    b = np.sin(ph) + 0.15 * np.sin(2 * ph + 0.5) + 0.05 * np.sin(3 * ph + 1.0)
    return b / 2.3 * pp_um


# ---------------------------------------------------------------------------------------------------------
# Modus demod
# ---------------------------------------------------------------------------------------------------------

def kasa_center(z):
    x, y = z.real, z.imag
    M = np.column_stack([x, y, np.ones_like(x)])
    s, *_ = np.linalg.lstsq(M, -(x ** 2 + y ** 2), rcond=None)
    return complex(-0.5 * s[0], -0.5 * s[1])


def demod_case(rng, pp_mm, clutter_ratio, rr=15.0, hr=72.0, win_s=15.0, T=180.0):
    t = np.arange(int(T * FS)) / FS
    ph, _ = breath_phase(t, lambda _t: rr)
    ph = ph + 0.4 * np.sin(2 * np.pi * 0.02 * t)
    xb = breath_wave(ph, pp_mm * 1000.0)
    xh = heart_wave(t, heart_beats(rng, T, lambda _t: hr, 40.0), 100.0)
    A = 1000.0
    C = clutter_ratio * A * np.exp(1j * 0.7)
    z = C + A * np.exp(1j * ((xb + xh) / UM_PER_RAD + 0.3))
    z += 20.0 * (rng.normal(size=t.size) + 1j * rng.normal(size=t.size)) / np.sqrt(2)
    sos = signal.butter(4, [0.8, 3.0], "band", fs=FS, output="sos")
    win, hop = int(win_s * FS), int(5 * FS)
    out = {}
    for name in ("wahr", "mittel", "kasa"):
        errs, ratios = [], []
        for s0 in range(0, t.size - win + 1, hop):
            zz = z[s0:s0 + win]
            c = {"wahr": C, "mittel": zz.mean(), "kasa": kasa_center(zz)}[name]
            h = signal.sosfiltfilt(sos, signal.detrend(np.unwrap(np.angle(zz - c)) * UM_PER_RAD))
            href = signal.sosfiltfilt(sos, signal.detrend(xh[s0:s0 + win]))
            m = slice(int(FS), win - int(FS))
            errs.append(np.sqrt(np.mean((h[m] - href[m]) ** 2)))
            f, p = signal.periodogram(h * np.hanning(win), fs=FS, nfft=8192)

            def pw(f0):
                return p[(f > f0 - 0.03) & (f < f0 + 0.03)].sum()

            harm = [mm * rr / 60 for mm in range(3, 12)]
            p_r = sum(pw(fh) for fh in harm if 0.85 < fh < 2.9 and abs(fh - hr / 60) > 0.08)
            ratios.append(p_r / (pw(hr / 60) + 1e-12))
        out[name] = (float(np.median(errs)), float(10 * np.log10(np.median(ratios) + 1e-12)))
    return out


def run_demod() -> int:
    rng = np.random.default_rng(1)
    print("Herzsignal 100 µm Spitze-Spitze, Puls 72 /min, Atmung 15 /min, 15-s-Fenster (wie a121_esp_dsp.h).")
    print("Fehler = Effektivwert(Herzband gemessen − wahr) in µm; OW = Leistung der Atem-Oberwellen m = 3..11 im")
    print("Herzband relativ zur Herzlinie (dB, > 0 dB: Oberwellen stärker als der Puls).\n")
    print(f"{'Atem-Hub':>9} {'Clutter/A':>9} | {'wahr':>17} | {'Mittelwert':>17} | {'Kåsa':>17}")
    for cr in (0.3, 1.5):
        for pp in (0.5, 1.0, 2.0, 3.0, 5.0):
            r = demod_case(rng, pp, cr)
            cells = " | ".join(f"{r[k][0]:6.1f} µm {r[k][1]:5.1f} dB" for k in ("wahr", "mittel", "kasa"))
            print(f"{pp:>7.1f}mm {cr:>9.1f} | {cells}")
    return 0


# ---------------------------------------------------------------------------------------------------------
# Modus bench: Firmware-Header auf synthetischen iq3-Frames
# ---------------------------------------------------------------------------------------------------------

def gauss_env(dd, fwhm):
    return np.exp(-0.5 * (dd / (fwhm / 2.3548)) ** 2)


def motion_list(motion) -> list[tuple[float, float, float]]:
    """motion als Liste von (t0, t1, pp_mm): None → [], ein Tupel → [Tupel], eine Liste bleibt."""
    if not motion:
        return []
    return [tuple(motion)] if np.isscalar(motion[0]) else [tuple(m) for m in motion]


def episodes_of(kw: dict) -> list[tuple[float, float]]:
    """Bewegungsepisoden (Beginn, Ende) eines Szenarios aus motion, shift (t0, t0 + Dauer) und jolts (t0, t1)."""
    eps = [(m[0], m[1]) for m in motion_list(kw.get("motion"))]
    if kw.get("shift"):
        eps.append((kw["shift"][0], kw["shift"][0] + kw["shift"][1]))
    if kw.get("jolts"):
        eps.append((kw["jolts"][0], kw["jolts"][1]))
    return sorted(eps)


def make_frames(path: pathlib.Path, seed, T=120.0, hr_fn=lambda t: 72.0, rr_fn=lambda t: 15.0, breath_pp_mm=2.5,
                heart_pp_um=100.0, hrv_ms=35.0, motion=None, hold=None, zoom_start_pt=244, person=True, leave=None,
                shift=None, jolts=None):
    """Schreibt <path>.iq (int16 real/imag je Punkt, 105 Punkte je Frame) und <path>_truth.npz.

    person=False: nur Tisch, Wand und Möbel (leerer Platz vor dem Sensor). leave=t: Die Person geht bei t s (ihre
    Streuzentren blenden in 1 s aus, Tisch, Wand und Möbel bleiben).
    V6, Bewegungsarten: motion = (t0, t1, pp_mm) oder Liste davon (Zufallsweg des Oberkörpers je Episode);
    shift = (t0, dauer_s, dd_mm, amp_faktor): alle Brust-Streuzentren wandern ab t0 in dauer_s linear um dd_mm und
    ihre Amplitude auf amp_faktor (Lageänderung); jolts = (t0, t1, pp_mm): zufällige Stöße am Brust-Echo (Länge
    0,2–0,5 s, Abstand 0,5–3 s, Halbsinus mit zufälligem Vorzeichen und 50–100 % der Höhe pp_mm)."""
    rng = np.random.default_rng(seed)
    n = int(T * FS)
    t = np.arange(n) / FS
    ph, rr = breath_phase(t, rr_fn)
    b = breath_wave(ph, breath_pp_mm * 1e-3)
    if hold:  # Atem anhalten: Atmung weich aus- und wieder einblenden
        w = np.where((t > hold[0]) & (t < hold[1]), 0.0, 1.0)
        b = b * np.convolve(w, np.ones(int(FS)) / FS, mode="same")
    beats = heart_beats(rng, T, hr_fn, hrv_ms)
    h = heart_wave(t, beats, heart_pp_um * 1e-6)
    body = np.zeros(n)
    for m0, m1, m_pp in motion_list(motion):  # Zufallsbewegung des Oberkörpers (eine Episode oder mehrere)
        k = (t > m0) & (t < m1)
        walk = np.convolve(np.cumsum(rng.normal(0, 1, k.sum())), np.ones(15) / 15, mode="same")
        body[k] = walk / (np.ptp(walk) + 1e-9) * m_pp * 1e-3
    if jolts:  # Stöße (z. B. Tippen): Halbsinus, zufälliges Vorzeichen; Zufallszahlen nur hier gezogen
        tc = jolts[0]
        while True:
            tc += rng.uniform(0.5, 3.0)
            dur = rng.uniform(0.2, 0.5)
            amp = jolts[2] * 1e-3 * rng.uniform(0.5, 1.0) * rng.choice([-1.0, 1.0])
            if tc + dur > jolts[1]:
                break
            k = (t >= tc) & (t < tc + dur)
            body[k] += amp * np.sin(np.pi * (t[k] - tc) / dur)
            tc += dur
    sh = np.clip((t - shift[0]) / shift[1], 0.0, 1.0) if shift else None  # Lageänderung: 0 → 1
    # Streuzentren: Abstand (m), Amplitude, Atem-Anteil, Herz-Anteil, Phase. Vorderbrust, Bauch, Schulter, Tisch, Wand, Möbel
    scat = [(0.640, 1500, 1.0, 1.0, 0.3), (0.668, 900, 1.3, 0.5, 1.9), (0.730, 800, 0.3, 0.2, 4.0),
            (0.450, 600, 0.0, 0.0, 2.2), (2.200, 1500, 0.0, 0.0, 5.1), (1.300, 500, 0.0, 0.0, 0.9)]
    if not person:
        scat = scat[3:]
    zd = (zoom_start_pt + np.arange(ZOOM_N)) * STEP_M
    rd = (ROOM_START_PT + np.arange(ROOM_N) * ROOM_STEP) * STEP_M
    Z = np.zeros((n, ZOOM_N), complex)
    R = np.zeros((n, ROOM_N), complex)
    pres = np.clip(1.0 - (t - leave), 0.0, 1.0) if leave is not None else np.ones(n)  # Person da (1) / weg (0)
    for r0, A, bs, hs, p0 in scat:
        r = r0 + bs * b + hs * h + (body if bs > 0 else 0.0)
        if shift and bs > 0:
            r = r + sh * shift[2] * 1e-3
        rot = np.exp(1j * (p0 - 4 * np.pi * r / LAM))
        a_t = A * (1 + 0.08 * bs * np.sin(ph + 0.7))  # Amplitude schwankt etwas mit der Atmung
        if bs > 0:
            a_t = a_t * pres
            if shift:
                a_t = a_t * (1.0 + (shift[3] - 1.0) * sh)
        Z += (a_t * rot)[:, None] * gauss_env(zd[None, :] - r[:, None], 0.040)   # Profil 1
        R += (a_t * rot)[:, None] * gauss_env(rd[None, :] - r[:, None], 0.140)   # Profil 3
    Z += 25 * (rng.normal(size=Z.shape) + 1j * rng.normal(size=Z.shape)) / np.sqrt(2)
    R += 40 * (rng.normal(size=R.shape) + 1j * rng.normal(size=R.shape)) / np.sqrt(2)
    lb_phi = 0.003 * rng.normal(size=n) + 0.02 * np.sin(2 * np.pi * t / 300)  # gemeinsamer Phasenfehler
    L = 3000 * np.exp(1j * (1.1 + lb_phi)) + 5 * (rng.normal(size=n) + 1j * rng.normal(size=n))
    Z *= np.exp(1j * lb_phi)[:, None]
    R *= np.exp(1j * lb_phi)[:, None]
    X = np.concatenate([L[:, None], Z, R], axis=1)
    iq = np.empty(X.shape + (2,), "<i2")
    iq[..., 0] = np.clip(np.round(X.real), -32768, 32767)
    iq[..., 1] = np.clip(np.round(X.imag), -32768, 32767)
    iq.tofile(f"{path}.iq")
    np.savez(f"{path}_truth.npz", t=t, rr=rr, beats=beats, zoom_start_pt=zoom_start_pt)


SCENARIOS = {
    "base":   ("Atmung 15, Puls 72", dict()),
    "harm4":  ("Puls = 4 × Atmung (18 → 72)", dict(rr_fn=lambda t: 18.0, hr_fn=lambda t: 72.0)),
    "step":   ("Puls 66 → 90 bei 60–70 s", dict(T=150.0, rr_fn=lambda t: 14.0,
                                                  hr_fn=lambda t: 66.0 if t < 60 else 66 + 24 * min(1, (t - 60) / 10))),
    "brady":  ("Puls 50, Atmung 12", dict(rr_fn=lambda t: 12.0, hr_fn=lambda t: 50.0)),
    "tachy":  ("Puls 120, Atmung 19", dict(rr_fn=lambda t: 19.0, hr_fn=lambda t: 120.0)),
    "flach":  ("Atemhub 0,6 mm", dict(breath_pp_mm=0.6, rr_fn=lambda t: 16.0, hr_fn=lambda t: 75.0)),
    "tief":   ("Atemhub 4 mm", dict(breath_pp_mm=4.0, rr_fn=lambda t: 13.0, hr_fn=lambda t: 70.0)),
    "beweg":  ("10 s Bewegung ±3 mm bei 50–60 s", dict(motion=(50, 60, 6.0))),
    "anhalt": ("Atem anhalten 40–60 s", dict(hold=(40, 60), rr_fn=lambda t: 16.0, hr_fn=lambda t: 76.0)),
    "leer":   ("ruhendes Echo, keine Atmung/Puls", dict(breath_pp_mm=0.0, heart_pp_um=0.0)),
    "niemand": ("keine Person, nur Möbel und Wand", dict(person=False)),
    # Puls genau auf einer Atem-Oberwelle (120 = 6 × 20, die 3. Oberwelle 60 ist im Modell stärker als der Puls):
    # spektral nicht trennbar; der Wert darf falsch sein, die Konfidenz muss es zeigen (VITAL_AUDIT.md, Prüfen Nr. 2)
    "tachy6": ("Puls 120 = 6 × Atmung 20", dict(rr_fn=lambda t: 20.0, hr_fn=lambda t: 120.0)),
    # Metronom 6 /min (R3): unter dem Spektralband der Atmung (ab 7 /min), nur Atemzug für Atemzug (V3.4) misst sie
    "takt6":  ("Metronom 6 /min (R3), Puls 64", dict(rr_fn=lambda t: 6.0, hr_fn=lambda t: 64.0)),
    # Person steht auf und geht (Abschluss-Review C1): danach weder gültiger Puls noch „Apnoe" (Prüfen Nr. 1 und 4)
    "weg":    ("Person geht bei 60 s", dict(leave=60.0)),
    # V6, Puls nach Bewegung wiederfinden (Plan 2026-11-02): neue Szenarien hinten, damit die Seeds der übrigen bleiben
    "kurz":   ("Bewegung 3 s (±2 mm) bei 60–63 s", dict(motion=(60, 63, 4.0))),
    "zappel": ("3 × Bewegung 2 s bei 40/60/80 s", dict(motion=[(40, 42, 4.0), (60, 62, 4.0), (80, 82, 4.0)])),
    "lang":   ("Bewegung 20 s (±3 mm) bei 50–70 s", dict(motion=(50, 70, 6.0), T=150.0)),
    "lage":   ("Lageänderung 8 mm, Echo 70 % bei 60 s", dict(shift=(60, 2, 8.0, 0.7))),
    "beweg_hr": ("Bewegung 60–70 s, Puls 70 → 85", dict(motion=(60, 70, 6.0), T=150.0,
                                                     hr_fn=lambda t: 70.0 if t < 60 else 70 + 15 * min(1, (t - 60) / 10))),
    "beweg_weg": ("Bewegung 50–55 s, Person geht bei 60 s", dict(motion=(50, 55, 4.0), leave=60)),
    "tippen": ("Stöße 0,3 mm (Tippen) 60–120 s", dict(jolts=(60, 120, 0.3), T=150.0)),
}

# Grenzwerte für --check: (Kennzahl, Vergleich, Grenze). Stand V0.5 = Werte des Headers vom 2026-11-01 mit kleinem
# Spielraum (Anteil −3 Prozentpunkte, MAE +0,5 /min, Reaktion +1 s). Jede DSP-Aufgabe verschärft, was sie verbessert.
LIMITS = {
    "base":    [("hr_p5", ">=", 97.0), ("hr_mae", "<=", 0.8), ("br_mae", "<=", 0.5), ("apnoe_fehl", "<=", 0),
                ("rmssd_ratio", ">=", 0.7), ("rmssd_ratio", "<=", 1.3)],                                       # V1.6
    "harm4":   [("hr_p5", ">=", 97.0), ("hr_mae", "<=", 0.8), ("br_mae", "<=", 0.5), ("konf", "<=", 60.0)],  # V1.3
    "step":    [("hr_p5", ">=", 97.0), ("hr_mae", "<=", 1.2), ("reaktion_s", "<=", 10.0), ("br_mae", "<=", 0.5),
                ("apnoe_fehl", "<=", 0), ("rmssd_ratio", ">=", 0.7), ("rmssd_ratio", "<=", 1.3)],              # V1.6
    "brady":   [("hr_p5", ">=", 91.0), ("hr_mae", "<=", 3.7), ("br_mae", "<=", 0.6)],                  # V1.2/V1.3
    "tachy":   [("hr_p5", ">=", 97.0), ("hr_mae", "<=", 1.4), ("br_mae", "<=", 0.5)],                  # V1.3
    "flach":   [("hr_p5", ">=", 97.0), ("hr_mae", "<=", 0.9), ("br_mae", "<=", 0.6), ("apnoe_fehl", "<=", 0),
                ("rmssd_ratio", ">=", 0.7), ("rmssd_ratio", "<=", 1.3)],                                       # V1.6
    "tief":    [("hr_p5", ">=", 97.0), ("hr_mae", "<=", 0.9), ("br_mae", "<=", 0.5), ("apnoe_fehl", "<=", 0),
                ("rmssd_ratio", ">=", 0.7), ("rmssd_ratio", "<=", 1.3)],                                       # V1.6
    "beweg":   [("hr_p5", ">=", 95.0), ("hr_mae", "<=", 1.3), ("br_mae", "<=", 0.7)],                  # V1.3
    "anhalt":  [("hr_p5", ">=", 97.0), ("hr_mae", "<=", 0.9), ("br_mae", "<=", 1.3), ("apnoe_verzug_s", "<=", 12.0),
                ("apnoe_fehl", "<=", 0)],                                                                 # V1.4
    "leer":    [("n_valid", "<=", 0)],                                                                    # V1.3
    "niemand": [("n_valid", "<=", 0)],                                                                    # V1.3
    "tachy6":  [("konf_falsch", "<=", 60.0)],                                                             # V1.3
    # Atmung 6 /min liegt unter dem Spektralband (Atem-MAE ≈ 8 /min, keine Grenze); Puls und Atemzüge schon (V3.4)
    "takt6":   [("hr_p5", ">=", 97.0), ("hr_mae", "<=", 0.8)],                                           # V3.4
    # Person geht (Abschluss-Review C1): Puls bis dahin wie base, danach kein gültiger Wert und keine Apnoe
    "weg":     [("hr_p5", ">=", 97.0), ("hr_mae", "<=", 0.8), ("weg_gueltig_n", "<=", 0), ("weg_apnoe_n", "<=", 0)],
    # V6, Basislinie ohne Schalter (heutiger Header, gemessen 2026-11-02): falsche Reports (> 10 /min) in den 30 s nach
    # der Bewegung höchstens wie gemessen. Gemessene Wiederfindzeit (wiederfind_s) im Kommentar, keine Grenze: kurz 14 s,
    # zappel 1 s, lang 11 s, lage 1 s, beweg_hr 12 s, beweg_weg 3 s, tippen 5 s. Die Ziele stehen in LIMITS_REACQ.
    "kurz":    [("falsch_nach_beweg_n", "<=", 2)],
    "zappel":  [("falsch_nach_beweg_n", "<=", 10)],
    "lang":    [("falsch_nach_beweg_n", "<=", 3)],
    "lage":    [("falsch_nach_beweg_n", "<=", 0)],
    "beweg_hr": [("falsch_nach_beweg_n", "<=", 0)],
    "beweg_weg": [("falsch_nach_beweg_n", "<=", 2)],
    "tippen":  [("falsch_nach_beweg_n", "<=", 4)],
    # über alle Szenarien mit Person: Konfidenz der Reports mit > 10 /min Fehler bzw. ≤ 5 /min (V1.3)
    "alle":    [("konf_falsch", "<=", 40.0), ("konf_richtig", ">=", 70.0)],
}

PERSON_FREE = ("leer", "niemand")
# V2.4: erster gültiger Wert nach höchstens 6 s (wachsendes Fenster). base, harm4, brady, tachy6: Im 6-s-Fenster liegt
# die stärkste Spitze dort auf 2 × Puls oder am Bandrand, Autokorrelation/Einzelschläge bestätigen sie nicht, der
# vorläufige Wert wird verworfen; Grenze dort wie vor V2.4 (8 s).
# V6 (neue Szenarien, Stand 2026-11-02 gemessen): Die Streuzentren sind dieselben wie in base, nur andere Zufallszahlen;
# im 6-s-Fenster liegt die stärkste Spitze mal auf der Oberwelle (dann kommt der erste Wert erst nach 7–8 s).
FIRST_VALUE_S = {"base": 8.0, "harm4": 8.0, "brady": 8.0, "tachy6": 8.0, "takt6": 10.0,
                 "zappel": 7.0, "lang": 7.0, "lage": 8.0, "beweg_hr": 8.0, "beweg_weg": 8.0, "tippen": 7.0}
# Abschluss-Review I1: erster richtiger Wert (≤ 5 /min) und falsche Werte (> 10 /min) in den ersten 15 s, Stand der
# Messung als Grenze (keine Ziele): brady meldet 8–13 s den doppelten Puls (98,6, Konfidenz 18–43 %), tief nach 6 s die
# 3. Atem-Oberwelle (40,3 statt 71, Konfidenz 28 %); Autokorrelation und Einzelschläge sehen auf demselben Signal dasselbe
FIRST_OK_S = {"base": 8.0, "harm4": 8.0, "brady": 14.0, "tief": 7.0, "tachy6": 8.0, "takt6": 10.0,
              "zappel": 7.0, "lang": 7.0, "lage": 8.0, "beweg_hr": 8.0, "beweg_weg": 8.0, "tippen": 7.0}   # V6, wie oben
# V6: zappel meldet in den ersten 15 s (vor der ersten Bewegung bei 40 s) dreimal einen falschen Puls, wie brady
EARLY_WRONG_N = {"brady": 6, "tief": 1, "zappel": 3}
for _scen in LIMITS:
    if _scen not in PERSON_FREE + ("alle",):
        LIMITS[_scen].append(("erst_s", "<=", FIRST_VALUE_S.get(_scen, 6.0)))
        LIMITS[_scen].append(("erst_ok_s", "<=", FIRST_OK_S.get(_scen, 6.0)))
        LIMITS[_scen].append(("frueh_falsch_n", "<=", EARLY_WRONG_N.get(_scen, 0)))
# V3.5: Zustand BEWEGUNG im Bewegungsabschnitt (beweg), sonst nirgends
LIMITS["beweg"].append(("zustand_beweg_n", ">=", 5.0))
for _scen in LIMITS:
    if _scen != "alle":
        LIMITS[_scen].append(("zustand_beweg_fehl", "<=", 0))
# V3.4 Atemzug für Atemzug: R3-Kriterium ±1 /min als MAE, Anteil ±1 /min wie gemessen (anhalt: die zwei Atemzüge nach
# dem Anhalten; flach mit 48 Zoom-Punkten 93 %), erster Wert nach höchstens 2,5 Atemzügen (flach: Telemetrie-Strahl
# schwingt bei 0,6 mm Atemhub erst nach ≈ 9 s ein, 4,1 Atemzüge); ohne Person kein @BREATH
BREATH_FIRST_N = {"flach": 4.5}
BREATH_P1 = {"anhalt": 90.0, "flach": 90.0, "zappel": 90.0}   # V6 zappel: 90 % gemessen (3 von 30 Atemzügen > 1 /min daneben)
for _scen in LIMITS:
    if _scen in PERSON_FREE:
        LIMITS[_scen].append(("atemzug_n", "<=", 0))
    elif _scen != "alle":
        LIMITS[_scen] += [("atemzug_mae", "<=", 1.0), ("atemzug_p1", ">=", BREATH_P1.get(_scen, 100.0)),
                          ("atemzug_erst_n", "<=", BREATH_FIRST_N.get(_scen, 2.5))]

# V6: Ziele für „Puls nach Bewegung wiederfinden" (Plan 2026-11-02), nur angewendet, wenn der Schalter A121_DSP_REACQ
# gesetzt ist (--define A121_DSP_REACQ). Ohne die DSP-Änderung verletzt der heutige Header sie, das ist erwartet.
# Ruling R15: falsch_in_beweg_n (Fehlwerte > 10 /min mitten in der Bewegung) ≤ 0 überall; ohne Schalter nur berichtet.
# beweg_hr: erster gültiger Wert nach der Bewegung höchstens 5 /min neben dem wahren Puls (85, nicht 70; Spec).
LIMITS_REACQ = {
    "kurz":      [("wiederfind_s", "<=", 8.0), ("falsch_nach_beweg_n", "<=", 0), ("nie_gefunden_n", "<=", 0)],
    "zappel":    [("wiederfind_s", "<=", 8.0), ("falsch_nach_beweg_n", "<=", 0), ("nie_gefunden_n", "<=", 0)],
    "lang":      [("wiederfind_s", "<=", 10.0), ("falsch_nach_beweg_n", "<=", 0), ("nie_gefunden_n", "<=", 0)],
    "lage":      [("wiederfind_s", "<=", 8.0), ("falsch_nach_beweg_n", "<=", 0), ("nie_gefunden_n", "<=", 0)],
    "beweg_hr":  [("falsch_nach_beweg_n", "<=", 0), ("nie_gefunden_n", "<=", 0), ("erst_nach_beweg_fehler", "<=", 5.0)],
    "beweg_weg": [("falsch_nach_beweg_n", "<=", 0), ("weg_gueltig_n", "<=", 0), ("nie_gefunden_n", "<=", 0)],
    "tippen":    [("falsch_nach_beweg_n", "<=", 0), ("abschnitt_gueltig_pct", ">=", 80.0), ("abschnitt_mae", "<=", 2.0),
                  ("nie_gefunden_n", "<=", 0)],
}
for _scen in LIMITS_REACQ:
    LIMITS_REACQ[_scen].append(("falsch_in_beweg_n", "<=", 0))


def active_limits() -> dict:
    """LIMITS, mit Schalter A121_DSP_REACQ (DEFINES) zusätzlich LIMITS_REACQ."""
    if not any(x.partition("=")[0] == "A121_DSP_REACQ" for x in DEFINES):
        return LIMITS
    return {k: LIMITS.get(k, []) + LIMITS_REACQ.get(k, []) for k in {**LIMITS, **LIMITS_REACQ}}


def check_limits(results: dict, limits: dict) -> list[str]:
    """Verletzte Grenzwerte als Meldungen; NaN (z. B. kein gültiger Report) gilt als verletzt. Szenarien ohne
    Ergebnis werden übersprungen."""
    msgs = []
    for scen, rules in limits.items():
        if scen not in results:
            continue
        for metric, op, lim in rules:
            v = results[scen].get(metric, np.nan)
            ok = (v >= lim) if op == ">=" else (v <= lim)
            if not (isinstance(v, (int, float)) and not np.isnan(v) and ok):
                shown = f"{v:.1f}" if isinstance(v, float) else str(v)
                msgs.append(f"{scen}: {metric} = {shown}, Grenze {op} {lim}")
    return msgs

# Prüfstand-Programm: Frames einspielen, je 60 Frames ein Report, Ausgabe als @DSP-/@VITAL-Zeilen über die
# Formatfunktionen des Headers (dsp_replay.h bringt sie auch für ältere Header mit, z. B. die Basislinie).
HARNESS = r"""
#include <Arduino.h>
#include "dsp_replay.h"
using namespace a121_dsp;
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static UltimateEspDsp dsp;
    if (!dsp.begin(atoi(argv[2]), 120, kRoomStep)) return 3;
    Complex16 pts[kTotal3SubPts];
    char line[kRoomLineMax];
    uint32_t n = 0;
    while (fread(pts, sizeof(pts[0]), kTotal3SubPts, f) == kTotal3SubPts) {
        float um;
        dsp.pushFrame(pts, kTotal3SubPts, 20, 0.0f, um);
#ifdef A121_DSP_BREATH
        BreathEvent bev;
        if (dsp.takeBreathEvent(bev)) {
            formatBreathLine(line, sizeof(line), (uint32_t)lround(n * 1000.0 / 60.0), n, bev);
            puts(line);
        }
#endif
        if (++n % 60) continue;
        VitalReport r;
        const bool ok = dsp.computeVitalReport(r);
#ifdef A121_DSP_BREATH
        dsp.setBreathGate(breathGateOpen(r.state));
#endif
        const uint32_t t_ms = (uint32_t)lround(n * 1000.0 / 60.0);
        formatDspLine(line, sizeof(line), t_ms, ok, r, r.compute_us);
        puts(line);
        if (ok) {
            formatVitalLine(line, sizeof(line), t_ms, r);
            puts(line);
        }
#ifdef A121_DSP_STATE
        formatStateLine(line, sizeof(line), t_ms, r);
        puts(line);
#endif
    }
    return 0;
}
"""

SHIM = """#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
static inline bool psramFound() { return true; }
static inline void *ps_malloc(size_t sz) { return malloc(sz); }
static inline unsigned long micros() {
    return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
"""

def build(tmp: pathlib.Path, header: pathlib.Path = DSP_HEADER) -> dict[str, pathlib.Path]:
    (tmp / "inc").mkdir(exist_ok=True)
    (tmp / "inc" / "Arduino.h").write_text(SHIM)
    (tmp / "harness.cpp").write_text(HARNESS)
    src = header.read_text()
    variants = {"original": src}
    exes = {}
    for name, text in variants.items():
        d = tmp / name
        d.mkdir(exist_ok=True)
        (d / "a121_esp_dsp.h").write_text(text)
        exe = tmp / f"harness_{name}"
        # Shim zuerst (chrono-micros), dann Header-Ordner, Firmware-Ordner als Rückfall, dsp_replay für die Formate
        subprocess.run(["g++", "-O2", "-std=c++17", f"-DA121_ZOOM_POINTS={ZOOM_N}", f"-DA121_HR_WIN={HR_WIN}",
                        *(f"-D{x}" for x in DEFINES),
                        f"-I{tmp / 'inc'}", f"-I{d}",
                        f"-I{header.parent}",
                        f"-I{DSP_HEADER.parent}", f"-I{REPLAY_DIR}", str(tmp / "harness.cpp"), "-o", str(exe), "-lm"],
                       check=True)
        exes[name] = exe
    return exes


# ---------------------------------------------------------------------------------------------------------
# Modus record: synthetische Aufnahme im App-Format (stream.log, rxtime.csv, ref_hr.csv, events.csv, meta.json)
# ---------------------------------------------------------------------------------------------------------

REPLAY_DIR = REPO_ROOT / "host" / "dsp_replay"


def write_skeleton(iq_path: pathlib.Path, zoom_start_pt: int, out: pathlib.Path, vital_placeholders: bool = True) -> None:
    """Log wie `iq3 … raw` vom ESP32, aber nur mit Platzhaltern für alles, was die DSP rechnet: @IQ3 (Zähler, tick,
    Temperatur), @RAW, je 12 Frames @ROOMPROF und je 60 Frames @DSP/@ROOM/@VITAL. host/dsp_replay füllt sie mit dem
    Header aus (ungültige Reports bekommen keine @VITAL-Zeile)."""
    import base64

    raw = np.fromfile(iq_path, "<i2").reshape(-1, 1 + ZOOM_N + ROOM_N, 2)
    zs_mm, ze_mm = zoom_start_pt * 25 // 10, (zoom_start_pt + ZOOM_N) * 25 // 10
    with open(out, "w") as f:
        f.write(f"@IQ3_START,{zs_mm},{ze_mm},60.0,1\n")
        for i, fr in enumerate(raw):
            t_ms = round(i * 1000 / FS)
            f.write(f"@IQ3,{t_ms},20,0.0,0.00,0.0,0,{i},{t_ms}\n")
            f.write(f"@RAW,{i},{t_ms},{1 + ZOOM_N + ROOM_N},{base64.b64encode(fr.tobytes()).decode()}\n")
            if (i + 1) % 12 == 0:
                f.write(f"@ROOMPROF,{t_ms},{t_ms}\n")
            if (i + 1) % 60 == 0:
                f.write(f"@DSP,{t_ms},0,0,0.0,0,0,0\n@ROOM,{t_ms},{t_ms}\n")
                if vital_placeholders:
                    f.write(f"@VITAL,{t_ms}\n")
        f.write("@END\n")


def replay_skeleton(skeleton: pathlib.Path, out_log: pathlib.Path, header: pathlib.Path = DSP_HEADER,
                    defines: tuple[str, ...] | None = None) -> str:
    """Platzhalter-Log mit host/dsp_replay und dem Header durchrechnen; out_log = Zeilen wie live. Rückgabe: Bericht
    der Nachrechnung. defines: z. B. ("-DA121_ZOOM_POINTS=48",); ohne Angabe die Zoom-Breite von ZOOM_N."""
    if defines is None:
        defines = (f"-DA121_ZOOM_POINTS={ZOOM_N}",)
    with tempfile.TemporaryDirectory() as d:
        exe = pathlib.Path(d) / "dsp_replay"
        subprocess.run(["g++", "-O2", "-std=c++17", "-ffp-contract=off", *defines, f"-I{REPLAY_DIR}",
                        f"-I{header.parent}", f"-I{DSP_HEADER.parent}", str(REPLAY_DIR / "dsp_replay_main.cpp"), "-o",
                        str(exe), "-lm"], check=True)
        r = subprocess.run([str(exe), str(skeleton), str(out_log)], capture_output=True, text=True)
        return r.stdout + r.stderr


def polar_notifications(rng, beats_s: np.ndarray, T: float) -> list[tuple[float, int, list[float]]]:
    """Wie ein EKG-Gurt (BLE Heart Rate Service): etwa jede Sekunde eine Benachrichtigung mit den RR-Werten der
    Schläge seit der letzten (1/1024 s aufgelöst), Latenz 20–80 ms nach dem letzten Schlag."""
    out, last = [], 0
    rr_all = np.diff(beats_s)
    for k in range(1, int(T)):
        t_n = k + rng.uniform(0.02, 0.08)
        idx = np.nonzero((beats_s[1:] <= t_n - 0.02) & (np.arange(1, beats_s.size) > last))[0] + 1
        rr = [round(rr_all[j - 1] * 1024) * 1000.0 / 1024 for j in idx]
        if idx.size:
            last = int(idx[-1])
        hr = round(60000.0 / np.mean(rr)) if rr else (out[-1][1] if out else 0)
        out.append((t_n * 1000.0, int(hr), rr))
    return out


def record_scenario(name: str, folder: pathlib.Path, header: pathlib.Path = DSP_HEADER) -> pathlib.Path:
    """Synthetische Aufnahme eines Szenarios im App-Format, NUR SIMULATION. Gurt-Referenz aus den wahren Schlägen,
    Einatem-Marker „ein" je Atemzug (wie Metronom-Atmung), Anhalte-Marker beim Szenario anhalt."""
    import json

    folder.mkdir(parents=True, exist_ok=True)
    kw = dict(SCENARIOS[name][1])
    seed = list(SCENARIOS).index(name) + 1
    with tempfile.TemporaryDirectory() as d:
        base = pathlib.Path(d) / name
        make_frames(base, seed=seed, **kw)
        tr = np.load(f"{base}_truth.npz")
        skel = pathlib.Path(d) / "skeleton.log"
        write_skeleton(pathlib.Path(f"{base}.iq"), int(tr["zoom_start_pt"]), skel)
        computed = pathlib.Path(d) / "computed.log"
        replay_skeleton(skel, computed, header)
        raw = {int(ln.split(",", 2)[1]): ln for ln in skel.read_text().splitlines() if ln.startswith("@RAW,")}
        lines = []
        for ln in computed.read_text().splitlines():  # @RAW wie live direkt hinter das @IQ3 desselben Frames
            lines.append(ln)
            if ln.startswith("@IQ3,"):
                lines.append(raw[int(ln.split(",")[7])])
    (folder / "stream.log").write_text("\n".join(lines) + "\n")
    with open(folder / "rxtime.csv", "w") as f:  # Empfang 3 ms nach der ESP32-Zeit der Zeile
        f.write("line,rx_ms\n")
        t_ms = 0.0
        for i, ln in enumerate(lines):
            p = ln.split(",")
            if p[0] in ("@IQ3", "@DSP", "@ROOM", "@VITAL", "@ROOMPROF") and len(p) > 1:
                t_ms = float(p[1])
            f.write(f"{i},{t_ms + 3.0:.1f}\n")
    T = float(tr["t"][-1]) + 1.0 / FS
    rng = np.random.default_rng(seed + 100)
    with open(folder / "ref_hr.csv", "w") as f:
        f.write("rx_ms,hr_bpm,rr_ms,contact\n")
        for rx, hr, rr in polar_notifications(rng, tr["beats"], T):
            for v in rr or [None]:
                f.write(f"{rx:.1f},{hr},{'' if v is None else f'{v:.1f}'},1\n")
    hold = kw.get("hold")
    ph = 2 * np.pi * np.cumsum(tr["rr"] / 60.0) / FS
    onsets = tr["t"][1:][np.diff(np.floor(ph / (2 * np.pi))) > 0]
    events = [(t * 1000.0, "ein") for t in onsets if not (hold and hold[0] <= t <= hold[1])]
    if hold:
        events += [(hold[0] * 1000.0, "anhalten_start"), (hold[1] * 1000.0, "anhalten_ende")]
    with open(folder / "events.csv", "w") as f:
        f.write("rx_ms,text\n")
        for t, text in sorted(events):
            f.write(f"{t:.1f},{text}\n")
    meta = {"source": "sim", "command": "iq3 0 raw (synthetisch)", "scenario": name, "description": SCENARIOS[name][0],
            "note": "NUR SIMULATION (host/vital_sim.py record)", "lines": len(lines), "duration_s": T}
    (folder / "meta.json").write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n")
    return folder


def truth_at(tr, t_end, win=15.0):
    """Wahre Werte im 15-s-Fenster vor dem Report (wie das Pulsfenster der DSP)."""
    b = tr["beats"]
    bb = b[(b > t_end - win) & (b <= t_end)]
    ibi = np.diff(bb) * 1000.0
    hr = 60000.0 / ibi.mean() if len(ibi) > 1 else np.nan
    rmssd = np.sqrt(np.mean(np.diff(ibi) ** 2)) if len(ibi) > 3 else np.nan
    m = (tr["t"] > t_end - win) & (tr["t"] <= t_end)
    return hr, float(tr["rr"][m].mean()), rmssd


def early_metrics(t_s: np.ndarray, valid: np.ndarray, err: np.ndarray) -> dict:
    """Erster richtiger Report (Fehler ≤ 5 /min) und falsche Reports (> 10 /min) in den ersten 15 s (Abschluss-Review
    I1: erst_s zählt den ersten gültigen Report, auch einen falschen)."""
    ok = valid & (np.nan_to_num(err, nan=np.inf) <= 5.0)
    bad = valid & (np.nan_to_num(err, nan=0.0) > 10.0) & (t_s <= 15.0)
    return {"erst_ok_s": float(t_s[ok][0]) if ok.any() else np.nan, "frueh_falsch_n": int(bad.sum())}


def reacq_metrics(t_s: np.ndarray, valid: np.ndarray, err: np.ndarray, episodes: list[tuple[float, float]]) -> dict:
    """Wiederfinden nach Bewegung (V6). wiederfind_s: Median über die Episoden der Zeit vom Episodenende bis zum ersten
    gültigen Report mit Fehler ≤ 5 /min; nur Episoden, die wiedergefunden wurden, zählen, NaN wenn keine. nie_gefunden_n:
    Zahl der Episoden ohne einen solchen Report bis Szenarioende (der Median allein würde sie verdecken).
    falsch_nach_beweg_n: gültige Reports mit Fehler > 10 /min in (Ende, Ende + 30 s]; falsch_in_beweg_n: dasselbe in
    (Beginn, Ende] (Ruling R15: Fehlwerte mitten in der Bewegung, z. B. ein Endstück, das die Bewegungskarte nicht als
    bewegt sieht); überlappende Zeitfenster mehrerer Episoden werden nicht doppelt gezählt."""
    e = np.nan_to_num(err, nan=0.0)
    ok = valid & (np.nan_to_num(err, nan=np.inf) <= 5.0)
    bad = valid & (e > 10.0)
    found, never = [], 0
    in_win, in_mot = np.zeros(len(t_s), bool), np.zeros(len(t_s), bool)
    for a, b in episodes:
        after = ok & (t_s > b)
        if after.any():
            found.append(float(t_s[after][0] - b))
        else:
            never += 1
        in_win |= (t_s > b) & (t_s <= b + 30.0)
        in_mot |= (t_s > a) & (t_s <= b)
    return {"wiederfind_s": float(np.median(found)) if found else np.nan, "nie_gefunden_n": never,
            "falsch_nach_beweg_n": int(np.sum(bad & in_win)), "falsch_in_beweg_n": int(np.sum(bad & in_mot))}


def first_after_error(t_s: np.ndarray, valid: np.ndarray, hr: np.ndarray, t_end: float, hr_fn) -> float:
    """|erster gültiger Wert nach t_end − wahrer Puls hr_fn(t) zu seiner Zeit| (V6, beweg_hr: Spec „erster gültiger
    Wert nach der Bewegung innerhalb ±5 /min von 85“; Bezug ist der Momentanpuls des Modells, nicht das 15-s-Mittel,
    in dem die Rampe 70 → 85 noch steckt). NaN, wenn danach kein gültiger Report kommt."""
    after = valid & (t_s > t_end)
    if not after.any():
        return np.nan
    i = int(np.flatnonzero(after)[0])
    return float(abs(hr[i] - hr_fn(t_s[i])))


def section_metrics(t_s: np.ndarray, valid: np.ndarray, err: np.ndarray, a: float, b: float) -> dict:
    """Anteil gültiger Reports und Puls-MAE der gültigen Reports im Abschnitt a < t ≤ b (V6, z. B. Tippen)."""
    sel = (t_s > a) & (t_s <= b)
    v = valid & sel
    return {"abschnitt_gueltig_pct": float(100 * v.sum() / sel.sum()) if sel.any() else np.nan,
            "abschnitt_mae": float(np.mean(err[v])) if v.any() else np.nan}


def conf_stats(conf: np.ndarray, err: np.ndarray) -> dict:
    """Konfidenz-Median der falschen (> 10 /min) und der richtigen (≤ 5 /min) Reports. Keine falschen Reports →
    konf_falsch = 0 (nichts war falsch hoch bewertet)."""
    bad, good = err > 10, err <= 5
    return {"konf_falsch": float(np.median(conf[bad])) if bad.any() else 0.0,
            "konf_richtig": float(np.median(conf[good])) if good.any() else np.nan, "n_falsch": int(bad.sum())}


def pooled(results: dict) -> dict:
    """Konfidenz über alle Szenarien mit Person (ohne leer/niemand und ohne exakte Oberwellen-Fälle wie tachy6, deren
    Werte falsch sein dürfen)."""
    err = np.concatenate([r["_err"] for k, r in results.items() if k not in PERSON_FREE and k != "tachy6"] or [[]])
    conf = np.concatenate([r["_conf"] for k, r in results.items() if k not in PERSON_FREE and k != "tachy6"] or [[]])
    return conf_stats(conf, err)


def harness_reports(exe: pathlib.Path, base: pathlib.Path, zoom_start_pt: int) -> dict[str, np.ndarray]:
    """Reports des Prüfstand-Programms je @DSP-Zeile: alle @DSP-Felder (fehlende = NaN), dazu t_s, valid und die
    @VITAL-Felder des Reports (ungültige Reports: NaN)."""
    from vital_eval import parse_lines
    from a121_esp_ultimate import VITAL_FIELDS

    lines = subprocess.run([str(exe), f"{base}.iq", str(zoom_start_pt)], capture_output=True, text=True,
                           check=True).stdout.splitlines()
    vd, dd, _ = parse_lines(lines, np.full(len(lines), np.nan))
    d = dict(dd)
    d["t_s"] = dd["t_ms"] / 1000.0
    d["valid"] = dd["ok"]
    pos = {t: i for i, t in enumerate(vd["t_ms"])}
    for k in VITAL_FIELDS[1:]:
        d[k] = np.array([vd[k][pos[t]] if t in pos else np.nan for t in dd["t_ms"]])
    # Namen wie früher im Prüfstand
    d["hr"], d["rmssd"], d["us"] = d["hr_fused"], d["rmssd_ms"], d["compute_us"]
    st = {int(p[1]): int(p[2]) for p in (ln.split(",") for ln in lines if ln.startswith("@STATE,")) if len(p) >= 3}
    d["state"] = np.array([st.get(int(t), -1) for t in dd["t_ms"]], float)  # -1: Header ohne @STATE (vor V3.5)
    # @BREATH (V3.4): t_ms, Frame, Abstand_ms, Tiefe_um, Rate je Atemzug
    d["breath"] = np.array([[float(x) for x in ln.split(",")[1:6]] for ln in lines if ln.startswith("@BREATH,")],
                           float).reshape(-1, 5)
    return d


def evaluate(exe: pathlib.Path, base: pathlib.Path, name: str) -> dict:
    from vital_eval import count_jumps
    warnings.filterwarnings("ignore", "Mean of empty slice")
    warnings.filterwarnings("ignore", "All-NaN slice")
    tr = np.load(f"{base}_truth.npz")
    d = harness_reports(exe, base, int(tr["zoom_start_pt"]))
    v = d["valid"] > 0
    tru = np.array([truth_at(tr, x) for x in d["t_s"]])
    e_hr = np.abs(d["hr"] - tru[:, 0])[v]
    # Atem-MAE ohne Reports im Zustand „angehalten" (br_state 2, dort meldet die DSP bewusst 0 statt einer Rate)
    e_br = np.where(np.nan_to_num(d["br_state"], nan=-1) == 2, np.nan, np.abs(d["br"] - tru[:, 1]))[v]
    if not v.any():  # kein gültiger Report (Szenarien ohne Person): Kennzahlen NaN ohne Warnungen
        e_hr = e_br = np.array([np.nan])
    res = {
        "gueltig": f"{int(v.sum())}/{len(v)}",
        "n_valid": int(v.sum()),
        "erst_s": float(d["t_s"][v][0]) if v.any() else np.nan,
        "hr_mae": float(np.nanmean(e_hr)), "hr_p5": float(100 * np.nanmean(e_hr <= 5)),
        "br_mae": float(np.nanmean(e_br)),
        # RMSSD nur aus Reports mit Wert (V1.6: 0 = nicht berechnet, < 90 % gültige Schläge)
        "rmssd": float(np.nanmedian(d["rmssd"][v][d["rmssd"][v] > 0])) if (d["rmssd"][v] > 0).any() else np.nan,
        "rmssd_wahr": float(np.nanmedian(tru[v, 2])),
        "rmssd_anteil": float(100 * np.mean(d["rmssd"][v] > 0)) if v.any() else np.nan,
        "konf": float(np.nanmedian(d["conf"][v])),
        **{k: x for k, x in conf_stats(d["conf"][v], e_hr).items() if k != "konf_richtig"},
        "_err": e_hr, "_conf": d["conf"][v],
        "us": float(np.median(d["us"])),
        "spruenge": count_jumps(d["hr"], v),   # V3.2: Sprünge > 15 /min zwischen aufeinanderfolgenden gültigen Reports
    }
    res["rmssd_ratio"] = res["rmssd"] / res["rmssd_wahr"] if res["rmssd_wahr"] > 0 else np.nan
    res.update(early_metrics(d["t_s"], v, np.abs(d["hr"] - tru[:, 0])))
    # Zustand BEWEGUNG (V3.5): Reports im Bewegungsabschnitt (bis 15 s danach, so lange liegt er im Fenster) und außerhalb
    kw = SCENARIOS[name][1]
    eps = episodes_of(kw)   # Bewegung, Lageänderung und Stöße (V6)
    in_mot = np.zeros(len(d["t_s"]), bool)
    for a, b in eps:
        in_mot |= (d["t_s"] > a) & (d["t_s"] <= b + 15.0)
    res["zustand_beweg_n"] = int(np.sum((d["state"] == 4) & in_mot))
    res["zustand_beweg_fehl"] = int(np.sum((d["state"] == 4) & ~in_mot))
    res["episoden"] = eps
    if eps:  # V6: Wiederfinden nach Bewegung; Abschnitt der Stöße (tippen)
        res.update(reacq_metrics(d["t_s"], v, np.abs(d["hr"] - tru[:, 0]), eps))
        hr_fn = kw.get("hr_fn", inspect.signature(make_frames).parameters["hr_fn"].default)
        res["erst_nach_beweg_fehler"] = first_after_error(d["t_s"], v, d["hr"], max(b for _a, b in eps), hr_fn)
    if kw.get("jolts"):
        res.update(section_metrics(d["t_s"], v, np.abs(d["hr"] - tru[:, 0]), kw["jolts"][0], kw["jolts"][1]))
    if name == "step":  # Reaktionszeit ab Rampenende (70 s) bis |Puls − 90| ≤ 5
        ok = (d["t_s"] >= 70) & v & (np.abs(d["hr"] - 90) <= 5)
        res["reaktion_s"] = float(d["t_s"][ok][0] - 70.0) if ok.any() else np.nan
    # Atemzustand (V1.4): Verzug bis Zustand 2 ab Beginn des Anhaltens, Fehlalarme außerhalb (vital_eval.apnea_eval)
    from vital_eval import apnea_eval
    hold = SCENARIOS[name][1].get("hold")
    holds = [(hold[0] * 1000.0, hold[1] * 1000.0)] if hold else []
    ap = apnea_eval(d["t_ms"], np.nan_to_num(d["br_state"], nan=-1), holds)
    res["apnoe_fehl"] = ap["false_alarms"]
    if holds:
        res["apnoe_verzug_s"] = ap["delays_s"][0] if ap["delays_s"] else np.nan
    # Atemzug für Atemzug (V3.4): Rate je @BREATH gegen die wahre Atemfrequenz; erster Wert in Atemzügen ab Start
    b = d["breath"]
    res["atemzug_n"] = int(len(b))
    if len(b):
        rr = np.interp(b[:, 0] / 1000.0, tr["t"], tr["rr"])
        e = np.abs(b[:, 4] - rr)
        res.update(atemzug_mae=float(e.mean()), atemzug_p1=float(100 * np.mean(e <= 1.0)),
                   atemzug_erst_s=float(b[0, 0] / 1000.0), atemzug_erst_n=float(b[0, 0] / 1000.0 * rr[0] / 60.0))
    else:
        res.update(atemzug_mae=np.nan, atemzug_p1=np.nan, atemzug_erst_s=np.nan, atemzug_erst_n=np.nan)
    leave = SCENARIOS[name][1].get("leave")
    if leave is not None:  # gültig: ab 16 s nach dem Gehen (nur noch leerer Platz im Fenster); Apnoe: ab 2 s (niemand da)
        res["weg_gueltig_n"] = int(np.sum(v & (d["t_s"] >= leave + 16.0)))
        apnea = (d["state"] == 5) | (np.nan_to_num(d["br_state"], nan=-1) == 2)
        res["weg_apnoe_n"] = int(np.sum(apnea & (d["t_s"] >= leave + 2.0)))
    if name == "anhalt":  # gemeldete Atemfrequenz, während der Atem ganz angehalten ist (Fenster 45–60 s)
        m = (d["t_s"] >= 55) & (d["t_s"] <= 62) & v
        res["atem_beim_anhalten"] = np.round(d["br"][m], 1).tolist()
    return res


def run_bench(out_dir: pathlib.Path | None, names: list[str], header: pathlib.Path = DSP_HEADER,
              check: bool = False) -> int:
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        data = out_dir or tmp
        data.mkdir(parents=True, exist_ok=True)
        exes = build(tmp, header)
        results = {}
        for nm in names:  # fester Zufallswert je Szenario, unabhängig von --scen
            make_frames(data / nm, seed=list(SCENARIOS).index(nm) + 1, **SCENARIOS[nm][1])
        print("NUR SIMULATION. Puls-MAE und Anteil ≤ 5 /min gegen die wahre mittlere Herzfrequenz im 15-s-Fenster;")
        print("Konfidenz = Median aller gültigen Reports, 'bei Fehler' = Median der Reports mit > 10 /min Abweichung.")
        print("Szenarien 'leer'/'niemand': Puls-Werte sind bedeutungslos, dort zählt nur die Zahl gültiger Reports (Soll 0).")
        print(f"{ZOOM_N} Zoom-Punkte je Frame ({ZOOM_N * 2.5:.0f} mm), Pulsfenster {HR_WIN // 60} s.")
        sw = [f"A121_HR_WIN={HR_WIN}"] if HR_WIN != 900 else []
        sw += [f"A121_ZOOM_POINTS={ZOOM_N}"] if ZOOM_N != 24 else []
        if sw + DEFINES:
            print("Schalter:", " ".join(sw + DEFINES))
        for var, exe in exes.items():
            print(f"\n=== DSP-Header {var}")
            print(f"{'Szenario':8s} {'Beschreibung':32s} {'gültig':>7s} {'erst':>5s} {'Puls-MAE':>9s} {'≤5/min':>7s}"
                  f" {'Atem-MAE':>9s} {'RMSSD/wahr (Anteil)':>19s} {'Konf':>5s} {'b.Fehler':>8s}  Zusatz")
            for nm in names:
                r = evaluate(exe, data / nm, nm)
                if var == "original":
                    results[nm] = r
                extra = ""
                if "reaktion_s" in r:
                    extra = f"Reaktion {r['reaktion_s']:.0f} s"
                if "atem_beim_anhalten" in r:
                    extra = f"Atem beim Anhalten {r['atem_beim_anhalten']}, angehalten erkannt nach {r['apnoe_verzug_s']:.0f} s"
                if r["episoden"]:
                    extra += (f"Zustand BEWEGUNG in {r['zustand_beweg_n']} Reports ({r['episoden'][0][0]:.0f}–"
                              f"{max(b for _a, b in r['episoden']) + 15:.0f} s), außerhalb {r['zustand_beweg_fehl']}"
                              f"; wiederfind {r['wiederfind_s']:.0f} s (nie {r['nie_gefunden_n']}), falsch danach "
                              f"{r['falsch_nach_beweg_n']}, falsch in der Bewegung {r['falsch_in_beweg_n']}"
                              f", erster Wert danach {r['erst_nach_beweg_fehler']:.1f} daneben")
                    if "abschnitt_gueltig_pct" in r:
                        extra += f", Abschnitt gültig {r['abschnitt_gueltig_pct']:.0f} % MAE {r['abschnitt_mae']:.1f}"
                elif r["zustand_beweg_fehl"]:
                    extra += f" Zustand BEWEGUNG ohne Bewegung: {r['zustand_beweg_fehl']}"
                if r.get("apnoe_fehl"):
                    extra += f" Apnoe-Fehlalarme {r['apnoe_fehl']}"
                if r["spruenge"]:
                    extra += f" Sprünge > 15 /min: {r['spruenge']}"
                kf = f"{r['konf_falsch']:7.0f}" if r["n_falsch"] else "      –"
                print(f"{nm:8s} {SCENARIOS[nm][0]:32s} {r['gueltig']:>7s} {r['erst_s']:4.0f}s {r['hr_mae']:7.1f}   {r['hr_p5']:5.0f} %"
                      f" {r['br_mae']:7.1f}   {r['rmssd']:5.0f}/{r['rmssd_wahr']:3.0f} ms ({r['rmssd_anteil']:3.0f} %) {r['konf']:4.0f}"
                      f" {kf}  {extra}")
            print(f"Rechenzeit je Report am PC (Median): {r['us'] / 1000:.1f} ms (ESP32 live vor V2.2: ≈ 270–320 ms)")
            print("\nAtemzug für Atemzug (V3.4, @BREATH): Rate = 60 / Median der letzten 4 Abstände gegen die wahre "
                  "Atemfrequenz; erster Wert in Atemzügen ab Start")
            print(f"{'Szenario':8s} {'Atemzüge':>8s} {'MAE':>6s} {'±1/min':>7s} {'erst':>6s} {'(Atemzüge)':>10s}")
            for nm in names:
                r = evaluate(exe, data / nm, nm) if var != "original" else results[nm]
                print(f"{nm:8s} {r['atemzug_n']:8d} {r['atemzug_mae']:6.2f} {r['atemzug_p1']:6.0f} % "
                      f"{r['atemzug_erst_s']:5.1f}s {r['atemzug_erst_n']:8.1f}")
        results["alle"] = pooled(results)
        p = results["alle"]
        print(f"Alle Szenarien mit Person: Konfidenz-Median bei > 10 /min Fehler {p['konf_falsch']:.0f} % "
              f"({p['n_falsch']} Reports), bei ≤ 5 /min {p['konf_richtig']:.0f} %")
    if not check:
        return 0
    msgs = check_limits(results, active_limits())
    print(f"\nPrüfung gegen LIMITS ({header}):")
    for m in msgs:
        print("  VERLETZT", m)
    print("  ERGEBNIS:", "VERSCHLECHTERT" if msgs else "OK (alle Grenzwerte eingehalten)")
    return 1 if msgs else 0


def main() -> int:
    global ZOOM_N, HR_WIN, DEFINES
    ap = argparse.ArgumentParser(description="Synthetischer Prüfstand für die Vital-DSP (nur Simulation)")
    ap.add_argument("mode", choices=["demod", "bench", "record"])
    ap.add_argument("--out", type=pathlib.Path, default=None, help="bench: Ordner für die erzeugten Frames")
    ap.add_argument("--scen", nargs="*", default=list(SCENARIOS), choices=list(SCENARIOS))
    ap.add_argument("--header", type=pathlib.Path, default=DSP_HEADER, help="bench: zu prüfender DSP-Header")
    ap.add_argument("--check", action="store_true", help="bench: gegen LIMITS prüfen, Exit 1 bei Verschlechterung")
    ap.add_argument("--zoom", type=int, default=ZOOM_N, choices=[24, 48],
                    help="bench/record: Zoom-Punkte je Frame (Header mit A121_ZOOM_POINTS übersetzt, V2.3)")
    ap.add_argument("--hr-win", type=int, default=HR_WIN, choices=[600, 720, 900],
                    help="bench: Pulsfenster in Frames (A121_HR_WIN, Vergleich 10 / 12 / 15 s, V2.5)")
    ap.add_argument("--define", action="append", default=[], metavar="NAME[=WERT]",
                    help="bench: Schalter für den Header, z. B. A121_DSP_VITERBI (mehrfach möglich, V3-Varianten)")
    a = ap.parse_args()
    ZOOM_N = a.zoom
    HR_WIN = a.hr_win
    for x in a.define:  # Fensterlänge und Zoom-Breite auch über --define, dann passen Frames und Ausgabe dazu
        name, _, val = x.partition("=")
        if name == "A121_HR_WIN" and val:
            HR_WIN = int(val)
        elif name == "A121_ZOOM_POINTS" and val:
            ZOOM_N = int(val)
        else:
            DEFINES.append(x)
    if a.mode == "demod":
        return run_demod()
    if a.mode == "record":
        if a.out is None:
            ap.error("record braucht --out <ordner>; je Szenario entsteht <ordner>/sim_<szenario>")
        for nm in a.scen:
            print("Aufnahme:", record_scenario(nm, a.out / f"sim_{nm}", a.header.resolve()))
        return 0
    return run_bench(a.out, a.scen, a.header.resolve(), a.check)


if __name__ == "__main__":
    sys.exit(main())
