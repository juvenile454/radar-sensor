"""ARCHIVED (V5.3, docs/VITAL_AUDIT.md), no longer maintained.
Analysis with the DSP chain from a121_vitalaufnahme.py. New analyses only with host/vital_eval.py and variants of the
firmware header firmware/esp32s3/src/a121_esp_dsp.h (--replay); what proves itself here moves into the header.
Function of the script unchanged.

Analysis of a live-monitor/vital recording with a breath-hold section (experiment M3, recording B).

Uses:
- phase-stability bin selection (avoids multipath cancellation / I/Q zero crossings as at bin 79)
- I/Q circle fit + regularized DACM phase demodulation
- Hilbert breathing-harmonic projection for the breathing section
- spectrum, acceleration HPS (f0·2f0·3f0), autocorrelation and peak counting (IBI)

  .venv/bin/python host/a121_anhalteanalyse.py <aufnahme.npz> <ausgabe_ohne_endung>

Output: key figures on stdout, <ausgabe>.png and <ausgabe>_seg.npy (section boundaries in frames).
Without a reference pulse the result is only a rhythm indication, not a pulse measurement.
"""
import sys
import numpy as np
from scipy import signal
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from a121_vitalaufnahme import (
    dacm_displacement_mm,
    hps_cardiac_peaks,
    project_out_respiration_harmonics,
    select_stable_chest_iq,
)

z = np.load(sys.argv[1])
iq = z["iq"]
fs = float(z["fs"])
d = z["distances_m"]
print("frames", iq.shape, "fs", fs, "Dauer", len(iq) / fs, "s, zero-frames", int((np.abs(iq).sum(axis=1) == 0).sum()))

# Phase-stable bin vs. naive amplitude maximum
k_raw, k_best, z_iq = select_stable_chest_iq(iq, d)
saved_bin = int(z["target_bin"]) if "target_bin" in z else k_raw
print(f"Gespeicherter target_bin in Datei: Bin {saved_bin} ({d[saved_bin]*1000:.1f} mm)")

disp = signal.detrend(dacm_displacement_mm(z_iq))
t = np.arange(len(disp)) / fs

sos_h = signal.butter(4, [0.8, 2.5], btype="band", fs=fs, output="sos")
br, disp_clean = project_out_respiration_harmonics(disp, fs=fs, max_harm=5)
heart = signal.sosfiltfilt(sos_h, disp_clean)

# For comparison: naive bin without circle centering (may show the zero-crossing artifact)
zz_naive = iq[:, max(k_raw - 2, 0):k_raw + 3].mean(axis=1)
disp_naive = signal.detrend(np.unwrap(np.angle(zz_naive)) * (5.0 / (4.0 * np.pi)))
heart_naive = signal.sosfiltfilt(sos_h, disp_naive)

# Breathing activity: moving peak-to-peak (4 s) of the breathing component + displacement
w = int(4 * fs)
act = np.array([np.ptp(signal.detrend(disp[max(0, i - w // 2):i + w // 2])) for i in range(len(disp))])
for s0 in range(0, int(len(disp) / fs), 5):
    seg = slice(int(s0 * fs), int((s0 + 5) * fs))
    print(
        f"  {s0:2d}-{s0+5:2d} s: Bewegung p-p {np.ptp(signal.detrend(disp[seg])):.2f} mm, "
        f"Pulsband rms {heart[seg].std()*1000:.0f} µm (naiv Bin {k_raw}: {heart_naive[seg].std()*1000:.0f} µm)"
    )

quiet = act < np.percentile(act, 35) * 1.35
best, cur, start = (0, 0), 0, 0
for i, q in enumerate(quiet):
    if q:
        if cur == 0:
            start = i
        cur += 1
        if cur > best[1] - best[0]:
            best = (start, i + 1)
    else:
        cur = 0
a, b = best
print(f"ruhigster Abschnitt: {a/fs:.1f}–{b/fs:.1f} s ({(b-a)/fs:.1f} s)")
np.save(sys.argv[2] + "_seg.npy", np.array([a, b]))


def analyse(x_heart, x_disp_clean, name):
    f, p = signal.welch(x_heart, fs=fs, nperseg=len(x_heart), nfft=int(fs * 120), window="hann")
    m = (f >= 0.8) & (f <= 2.5)
    idx, _ = signal.find_peaks(p[m])
    idx = idx[np.argsort(p[m][idx])[::-1]][:3]
    print(f"  {name}: Spektral-Kandidaten /min:", [(round(float(f[m][i] * 60), 1), round(float(p[m][i] / p[m].max()), 2)) for i in idx])
    _, _, hps_pks = hps_cardiac_peaks(x_disp_clean, fs=fs, fmin=0.85, fmax=2.3)
    print(f"  {name}: Beschleunigungs-HPS (f0·2f0·3f0) /min:", hps_pks)
    ac = np.correlate(x_heart - x_heart.mean(), x_heart - x_heart.mean(), "full")[len(x_heart) - 1:]
    ac /= max(float(ac[0]), 1e-12)
    lo, hi = int(fs * 60 / 150), int(fs * 60 / 48)
    j = lo + int(np.argmax(ac[lo:hi]))
    print(f"  {name}: Autokorrelation max bei {j/fs:.3f} s -> {60*fs/j:.1f} /min, r={ac[j]:.2f}")
    pk, _ = signal.find_peaks(x_heart, distance=int(fs * 0.48), prominence=x_heart.std() * 0.45)
    if len(pk) > 2:
        ibi = np.diff(pk) / fs
        print(
            f"  {name}: {len(pk)} Spitzen, Abstand {ibi.mean():.3f} s ± {ibi.std():.3f} "
            f"(CV {ibi.std()/ibi.mean()*100:.1f} %) -> {60/ibi.mean():.1f} /min"
        )
    return f, p, pk


hold = heart[a:b]
fH, pH, pkH = analyse(hold, disp_clean[a:b], "Anhalten")
seg_b = slice(0, a) if a > fs * 10 else slice(b, len(heart))
fB, pB, _ = analyse(heart[seg_b], disp_clean[seg_b], "Atmen (mit Hilbert-Projektion)")

fig, ax = plt.subplots(3, 1, figsize=(11, 9))
ax[0].plot(t, disp, lw=0.9, label=f"DACM Verschiebung (Bin {k_best}, {d[k_best]*1000:.0f} mm)")
ax[0].plot(t, br, lw=1.2, label="Atmung (0,1–0,6 Hz)")
ax[0].axvspan(a / fs, b / fs, color="y", alpha=0.3, label="ruhigster Abschnitt (Atem anhalten)")
ax[0].set_ylabel("mm")
ax[0].set_title("Brustverschiebung (gelb: ruhigster Abschnitt)")
ax[0].legend(loc="upper right")
ax[0].grid(True, alpha=0.3)

ax[1].plot(t, heart_naive * 1000, lw=0.6, color="0.7", alpha=0.7, label=f"Naiv Bin {k_raw} (mit Multipath-Einbruch)")
ax[1].plot(t, heart * 1000, lw=0.9, color="tab:green", label=f"Stabil Bin {k_best} + DACM + Projektion")
if len(pkH) > 0:
    ax[1].plot(t[a:b][pkH], hold[pkH] * 1000, "ro", ms=3.5, label=f"Pulse im Anhalte-Abschnitt ({len(pkH)})")
ax[1].axvspan(a / fs, b / fs, color="y", alpha=0.3)
ax[1].set_ylabel("µm")
ax[1].set_title("Pulsband 0,8–2,5 Hz: Stabiler Bin + DACM vs. naiver Bin")
ax[1].legend(loc="upper right")
ax[1].grid(True, alpha=0.3)

m = (fH > 0.5) & (fH < 3.0)
ax[2].plot(fH[m] * 60, pH[m] / pH[m].max(), lw=1.2, label="Anhalten")
ax[2].plot(fB[m] * 60, pB[m] / pB[m].max(), lw=1.2, label="Atmen (nach Oberwellen-Projektion)")
ax[2].legend(loc="upper right")
ax[2].set_xlabel("/min")
ax[2].set_title("Spektrum Pulsband (normiert)")
ax[2].grid(True, alpha=0.3)

fig.tight_layout()
fig.savefig(sys.argv[2] + ".png", dpi=110)
print("Plot", sys.argv[2] + ".png")
