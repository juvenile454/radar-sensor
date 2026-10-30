"""Auswertung einer Live-Monitor-/Vitalaufnahme mit Atem-anhalten-Abschnitt (Experiment M3, Aufnahme B).

Findet den ruhigsten Abschnitt (geringste Brustbewegung) automatisch und vergleicht dort und im
Atem-Abschnitt das Pulsband 0,8–2,5 Hz per Spektrum, Autokorrelation und Spitzenzählung.

  .venv/bin/python host/a121_anhalteanalyse.py <aufnahme.npz> <ausgabe_ohne_endung>

Ausgabe: Kennzahlen auf stdout, <ausgabe>.png und <ausgabe>_seg.npy (Abschnittsgrenzen in Frames).
Ohne Referenzpuls ist das Ergebnis nur ein Rhythmus-Nachweis, keine Pulsmessung.
"""
import sys, numpy as np
from scipy import signal
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt

z = np.load(sys.argv[1]); iq = z["iq"]; fs = float(z["fs"]); d = z["distances_m"]
print("frames", iq.shape, "fs", fs, "Dauer", len(iq)/fs, "s, zero-frames", int((np.abs(iq).sum(axis=1) == 0).sum()))
amp = np.abs(iq).mean(axis=0); k = int(np.argmax(amp)); print("Ziel", d[k], "target_bin gespeichert", int(z["target_bin"]), d[int(z["target_bin"])])
zz = iq[:, max(k-2, 0):k+3].mean(axis=1)
disp = signal.detrend(np.unwrap(np.angle(zz)) * 5.0/(4*np.pi))
t = np.arange(len(disp))/fs
br = signal.sosfiltfilt(signal.butter(2, [0.1, 0.6], btype="band", fs=fs, output="sos"), disp)
heart = signal.sosfiltfilt(signal.butter(4, [0.8, 2.5], btype="band", fs=fs, output="sos"), disp)
# Atemaktivität: gleitende Spitze-Spitze (4 s) der Verschiebung ohne Trend
w = int(4*fs); act = np.array([np.ptp(signal.detrend(disp[max(0, i-w//2):i+w//2])) for i in range(0, len(disp))])
for s0 in range(0, int(len(disp)/fs), 5):
    seg = slice(int(s0*fs), int((s0+5)*fs))
    print(f"  {s0:2d}-{s0+5:2d} s: Bewegung p-p {np.ptp(signal.detrend(disp[seg])):.2f} mm, Pulsband rms {heart[seg].std()*1000:.0f} µm")
quiet = act < np.percentile(act, 30) * 1.5
# längster zusammenhängender ruhiger Abschnitt
best, cur, start = (0, 0), 0, 0
for i, q in enumerate(quiet):
    if q:
        if cur == 0: start = i
        cur += 1
        if cur > best[1]-best[0]: best = (start, i+1)
    else: cur = 0
a, b = best; print(f"ruhigster Abschnitt: {a/fs:.1f}–{b/fs:.1f} s ({(b-a)/fs:.1f} s)")
np.save(sys.argv[2] + "_seg.npy", np.array([a, b]))
def analyse(x, name):
    f, p = signal.welch(x, fs=fs, nperseg=len(x), nfft=int(fs*120), window="hann")
    m = (f >= 0.8) & (f <= 2.5)
    idx, _ = signal.find_peaks(p[m]); idx = idx[np.argsort(p[m][idx])[::-1]][:3]
    print(f"  {name}: Spektral-Kandidaten /min:", [(round(f[m][i]*60, 1), round(p[m][i]/p[m].max(), 2)) for i in idx])
    ac = np.correlate(x - x.mean(), x - x.mean(), "full")[len(x)-1:]; ac /= ac[0]
    lo, hi = int(fs*60/150), int(fs*60/48)
    j = lo + int(np.argmax(ac[lo:hi])); print(f"  {name}: Autokorrelation max bei {j/fs:.3f} s → {60*fs/j:.1f} /min, r={ac[j]:.2f}")
    pk, _ = signal.find_peaks(x, distance=int(fs*0.4), prominence=x.std()*0.5)
    if len(pk) > 2:
        ibi = np.diff(pk)/fs; print(f"  {name}: {len(pk)} Spitzen, Abstand {ibi.mean():.3f} s ± {ibi.std():.3f} → {60/ibi.mean():.1f} /min")
    return f, p
hold = heart[a:b]
fH, pH = analyse(hold, "Anhalten")
seg_b = slice(0, a) if a > fs*10 else slice(b, len(heart))
fB, pB = analyse(heart[seg_b], "Atmen")
fig, ax = plt.subplots(3, 1, figsize=(11, 9))
ax[0].plot(t, disp, lw=0.8); ax[0].plot(t, br); ax[0].axvspan(a/fs, b/fs, color="y", alpha=0.3); ax[0].set_ylabel("mm"); ax[0].set_title("Brustverschiebung (gelb: ruhigster Abschnitt)")
ax[1].plot(t, heart*1000, lw=0.8); ax[1].axvspan(a/fs, b/fs, color="y", alpha=0.3); ax[1].set_ylabel("µm"); ax[1].set_title("Pulsband 0,8–2,5 Hz")
m = (fH > 0.5) & (fH < 3)
ax[2].plot(fH[m]*60, pH[m]/pH[m].max(), label="Anhalten"); ax[2].plot(fB[m]*60, pB[m]/pB[m].max(), label="Atmen"); ax[2].legend(); ax[2].set_xlabel("/min"); ax[2].set_title("Spektrum Pulsband (normiert)")
fig.tight_layout(); fig.savefig(sys.argv[2] + ".png", dpi=90); print("Plot", sys.argv[2] + ".png")
