"""Vitalaufnahme (Atmung/Puls) mit dem A121 — Rohdaten + Offline-Auswertung.

Nimmt Sparse IQ mit fester Framerate auf, sucht die Brust (stärkstes Echo im Bereich),
wandelt die Phase in Verschiebung um (λ ≈ 5 mm → 0,398 mm/rad) und schätzt:
- Atmung: Spektralmaximum 0,1–0,6 Hz
- Puls:   Spektralmaxima 0,8–2,5 Hz (48–150 /min), gesamt und in gleitenden 20-s-Fenstern
Ergebnis: .npz + .png nach ~/Data/radar-sensor/.

Experimentell: Herzschlag ist ~0,1–0,5 mm, Atmungsoberwellen fallen ins Pulsband.
Ohne Referenz (Smartwatch/Oximeter) ist ein Pulswert keine Aussage.

  sg dialout -c ".venv/bin/python host/a121_vitalaufnahme.py <PORT> <OUT_OHNE_ENDUNG> <DAUER_S> '<Beschreibung>'"
"""
import json
import sys
import time

import numpy as np
from acconeer.exptool import a121
from scipy import signal

START_POINT, NUM_POINTS, STEP = 60, 120, 1  # 0,15–0,45 m in 2,5-mm-Schritten
POINT_M = 0.0025
FRAME_RATE = 50.0
MM_PER_RAD = 5.0 / (4 * np.pi)  # λ/(4π), λ ≈ 5,0 mm bei 60 GHz


def connect(port: str, cfg: a121.SensorConfig, tries: int = 3) -> a121.Client:
    # Bekannt: erste Verbindung nach einer beendeten Session scheitert oft mit recv timeout.
    client = None
    for i in range(tries):
        try:
            client = a121.Client.open(serial_port=port)
            client.setup_session(cfg)
            return client
        except Exception as e:  # noqa: BLE001
            print(f"Verbindung Versuch {i + 1} fehlgeschlagen: {e}")
            if client is not None:
                try:
                    client.close()
                except Exception:  # noqa: BLE001
                    pass
            time.sleep(1)
    sys.exit("keine Verbindung")


def spectrum_peaks(x, fs, fmin, fmax, n=3):
    f, p = signal.welch(x, fs=fs, nperseg=min(len(x), int(fs * 20)), nfft=int(fs * 120))
    m = (f >= fmin) & (f <= fmax)
    fi, pi = f[m], p[m]
    idx, _ = signal.find_peaks(pi)
    idx = idx[np.argsort(pi[idx])[::-1]][:n]
    return f, p, [(float(fi[i] * 60), float(pi[i] / pi.max())) for i in idx]


def main(port: str, out: str, dauer_s: str, label: str) -> None:
    dauer = float(dauer_s)
    cfg = a121.SensorConfig(start_point=START_POINT, num_points=NUM_POINTS, step_length=STEP,
                            profile=a121.Profile.PROFILE_2, hwaas=16, sweeps_per_frame=1,
                            frame_rate=FRAME_RATE, phase_enhancement=True)
    client = connect(port, cfg)
    client.start_session()
    n = int(dauer * FRAME_RATE)
    t0 = time.time()
    print(f"AUFNAHME LÄUFT ({dauer:.0f} s) — {time.strftime('%H:%M:%S')}", flush=True)
    iq = np.array([client.get_next().frame[0] for _ in range(n)])
    t_end = time.time()
    client.stop_session()
    client.close()
    fs = n / (t_end - t0)
    print(f"Aufnahme fertig {time.strftime('%H:%M:%S')}, {n} Frames, effektiv {fs:.2f} Hz")

    dist = (START_POINT + np.arange(NUM_POINTS) * STEP) * POINT_M
    amp = np.abs(iq).mean(axis=0)
    k = int(np.argmax(amp))
    print(f"Ziel (stärkstes Echo, roh): {dist[k]:.3f} m, Amp {amp[k]:.0f}, Median {np.median(amp):.0f}")

    # Phase am Ziel-Bin, Mittel über ±2 Nachbarn (komplex) für weniger Rauschen
    sl = slice(max(k - 2, 0), min(k + 3, NUM_POINTS))
    z = iq[:, sl].mean(axis=1)
    disp = np.unwrap(np.angle(z)) * MM_PER_RAD
    disp = signal.detrend(disp)
    print(f"Verschiebung Spitze-Spitze: {np.ptp(disp):.2f} mm")

    sos_b = signal.butter(2, [0.1, 0.6], btype="band", fs=fs, output="sos")
    sos_h = signal.butter(4, [0.8, 2.5], btype="band", fs=fs, output="sos")
    breath = signal.sosfiltfilt(sos_b, disp)
    heart = signal.sosfiltfilt(sos_h, disp)

    f, p, bpk = spectrum_peaks(disp, fs, 0.1, 0.6, 1)
    br = bpk[0][0] if bpk else float("nan")
    _, _, hpk = spectrum_peaks(heart, fs, 0.8, 2.5, 3)
    print(f"ATMUNG: {br:.1f} /min, Amplitude (Band) Spitze-Spitze {np.ptp(breath):.2f} mm")
    print("PULS-Kandidaten gesamt (/min, rel. Stärke):",
          [(round(b, 1), round(s, 2)) for b, s in hpk])
    harm = [round(br * h, 1) for h in range(2, 9) if 48 <= br * h <= 150]
    print("  Atmungs-Oberwellen im Pulsband (/min):", harm)

    win, step = int(20 * fs), int(5 * fs)
    sliding = []
    for s0 in range(0, len(heart) - win + 1, step):
        _, _, pk = spectrum_peaks(heart[s0:s0 + win], fs, 0.8, 2.5, 1)
        sliding.append(round(pk[0][0], 1) if pk else None)
    print("  Puls gleitend (20-s-Fenster, 5-s-Schritt, /min):", sliding)

    np.savez_compressed(out + ".npz", iq=iq, distances_m=dist, fs=fs, target_bin=k,
                        meta=json.dumps({"setup": label, "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                                         "sensor_config": cfg.to_json()}))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        t = np.arange(len(disp)) / fs
        fig, ax = plt.subplots(4, 1, figsize=(11, 11))
        ax[0].plot(dist, amp); ax[0].axvline(dist[k], color="r"); ax[0].set_xlabel("m"); ax[0].set_title("Amplitude über Abstand")
        ax[1].plot(t, disp, lw=0.8, label="gesamt"); ax[1].plot(t, breath, label=f"Atmung {br:.1f}/min")
        ax[1].set_ylabel("mm"); ax[1].legend(); ax[1].set_title("Brustverschiebung")
        ax[2].plot(t, heart * 1000, lw=0.8); ax[2].set_ylabel("µm"); ax[2].set_xlabel("s"); ax[2].set_title("Pulsband 0,8–2,5 Hz")
        fh, ph = signal.welch(heart, fs=fs, nperseg=min(len(heart), int(fs * 20)), nfft=int(fs * 120))
        m = (fh > 0.05) & (fh < 3)
        ax[3].semilogy(f[m] * 60, p[m], label="gesamt"); ax[3].semilogy(fh[m] * 60, ph[m], label="Pulsband")
        for h in harm:
            ax[3].axvline(h, color="gray", ls=":", lw=0.8)
        ax[3].set_xlabel("/min"); ax[3].legend(); ax[3].set_title("Spektrum (grau: Atmungs-Oberwellen)")
        fig.suptitle(label); fig.tight_layout(); fig.savefig(out + ".png", dpi=90)
        print("Plot:", out + ".png")
    except Exception as e:  # noqa: BLE001
        print("Plot fehlgeschlagen:", e)
    print("gespeichert:", out + ".npz")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
