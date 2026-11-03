"""ARCHIVED (V5.3, docs/VITAL_AUDIT.md), no longer maintained.
Own DSP chain on the PC (Kåsa + DACM + Hilbert projection + HPS). New analyses only with host/vital_eval.py and
variants of the firmware header firmware/esp32s3/src/a121_esp_dsp.h (--replay); what proves itself here moves into the
header. Function of the script unchanged.

Vital recording (breathing/pulse) with the A121 — raw data + extended offline/online analysis.

Improvements over the base version:
1. Sensor configuration: profile 1 (40 mm FWHM instead of 75 mm with profile 2) for sharper separation
   of sternum and shoulder/abdomen, HWAAS 32, IdleState.READY (less wake-up phase jitter),
   override_baudrate=921600 against the CH342 reconnect timeout.
2. Multi-bin quality selection: picks in the chest cluster the bins with the highest amplitude stability
   (lowest multipath fading μ/|σ|) instead of a blind argmax(|z|).
3. I/Q circle centering (Kåsa fit) + regularized DACM phase demodulation: prevents artificial
   phase jumps at destructive interference (|z| -> 0).
4. Time-varying breathing-harmonic projection: estimates the instantaneous breathing phase θ_b(t) by
   Hilbert transform and projects cos(m·θ_b), sin(m·θ_b) (m=1..5) out of the displacement signal.
5. Acceleration spectrum d²x/dt² + Harmonic Product Spectrum (HPS, f0·2f0·3f0): uses the
   steep systolic edge (harmonic comb of the heartbeat) to separate it from smooth breathing residue.

Without a reference (smartwatch/oximeter/ECG) every pulse value output is an experimental
rhythm candidate in the radar signal, not a statement about accuracy.

Recording at the sensor:
  sg dialout -c ".venv/bin/python host/a121_vitalaufnahme.py <PORT> <OUT_OHNE_ENDUNG> <DAUER_S> '<Beschreibung>'"

Offline re-analysis of an existing .npz recording:
  .venv/bin/python host/a121_vitalaufnahme.py --offline <EINGABE.npz> <OUT_OHNE_ENDUNG>
"""
import json
import sys
import time

import numpy as np
from acconeer.exptool import a121
from scipy import signal

START_POINT, NUM_POINTS, STEP = 60, 120, 1  # fallback 0.15–0.45 m in 2.5 mm steps (centered live by auto-scan)
POINT_M = 0.0025
FRAME_RATE = 100.0
MM_PER_RAD = 2.4776236 / (2 * np.pi)  # PERCEIVED_WAVELENGTH / (2π) ≈ 0.3943 mm/rad


def connect(port: str, cfg: a121.SensorConfig | None = None, tries: int = 3) -> a121.Client:
    # override_baudrate=921600 avoids the 2 Mbaud switch, which on the CH342 regularly runs into
    # a LinkError: recv timeout on the next open.
    client = None
    for i in range(tries):
        try:
            client = a121.Client.open(serial_port=port, override_baudrate=921600)
            if cfg is not None:
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


def auto_locate_chest_start_point(client: a121.Client, scan_s: float = 1.5) -> int:
    """Scans 0.15–2.25 m (30 mm grid) for 1.5 s and automatically finds the strongest moving target (chest)."""
    scan_cfg = a121.SensorConfig(
        start_point=60,
        num_points=70,
        step_length=12,
        profile=a121.Profile.PROFILE_2,
        hwaas=16,
        sweeps_per_frame=1,
        frame_rate=40.0,
        inter_frame_idle_state=a121.IdleState.READY,
        phase_enhancement=True,
        iq_imbalance_compensation=True,
        prf=a121.PRF.PRF_15_6_MHz,
    )
    client.setup_session(scan_cfg)
    client.start_session()
    n_scan = max(20, int(scan_s * 40.0))
    frames = np.array([client.get_next().frame[0] for _ in range(n_scan)], dtype=np.complex128)
    client.stop_session()

    dists_m = (60 + np.arange(70) * 12) * POINT_M
    # Dynamic IQ change (separates a breathing person from rigid furniture/walls)
    dyn_iq = frames - np.mean(frames, axis=0, keepdims=True)
    dyn_score = np.std(dyn_iq, axis=0) * np.sqrt(np.mean(np.abs(frames), axis=0))
    best_idx = int(np.argmax(dyn_score))
    chest_m = float(dists_m[best_idx])
    center_pt = int(round(chest_m / POINT_M))
    start_pt = max(40, center_pt - NUM_POINTS // 2)
    print(
        f"Auto-Scan (0,15–2,25 m): Bewegtes Primärziel bei {chest_m*1000:.0f} mm erkannt "
        f"-> Zentriere Vital-Zoom auf {start_pt*POINT_M*1000:.0f}..{(start_pt+NUM_POINTS)*POINT_M*1000:.0f} mm"
    )
    return start_pt


def remove_static_complex_clutter(iq: np.ndarray, fs: float) -> np.ndarray:
    """Dynamic complex stationary-target suppression (subtracts slow/static background phasors < 0.05 Hz)."""
    if len(iq) < int(fs * 6):
        return iq
    sos_lp = signal.butter(2, 0.05, btype="low", fs=fs, output="sos")
    static_clutter = signal.sosfiltfilt(sos_lp, iq.real, axis=0) + 1j * signal.sosfiltfilt(sos_lp, iq.imag, axis=0)
    # Subtract 75 % of the quasi-static background phasor (keeps the working circle, damps furniture/multipath clutter)
    return iq - 0.75 * static_clutter


def fit_iq_circle(z: np.ndarray) -> complex:
    """Algebraic circle fit (Kåsa) in the complex I/Q plane for static clutter compensation."""
    x, y = z.real.astype(np.float64), z.imag.astype(np.float64)
    mat = np.column_stack([x, y, np.ones_like(x)])
    sol, _, _, _ = np.linalg.lstsq(mat, -(x**2 + y**2), rcond=None)
    cx, cy = -0.5 * sol[0], -0.5 * sol[1]
    r = np.sqrt(max(cx**2 + cy**2 - sol[2], 1e-6))
    # Subtract only if the estimated offset is plausibly within the signal strength
    if np.hypot(cx, cy) < 1.5 * r:
        return complex(cx, cy)
    return complex(np.median(x) * 0.2, np.median(y) * 0.2)


def dacm_displacement_mm(z: np.ndarray, reg_quantile: float = 5.0) -> np.ndarray:
    """Circle-centered, regularized DACM phase demodulation (robust against |z| -> 0 dropouts)."""
    center = fit_iq_circle(z)
    zc = z - center
    # Soft regularization prevents artificial π jumps at true zero crossings (|z| -> 0),
    # without modulating the phase through amplitude fluctuations at normal signal strength.
    amp_sq = np.abs(zc) ** 2
    eps = float(np.percentile(amp_sq, reg_quantile)) * 0.01 + 1e-6
    prod = zc[1:] * np.conj(zc[:-1])
    dphi = np.arctan2(prod.imag, prod.real)
    w = amp_sq[1:] / (amp_sq[1:] + eps)
    phi = np.concatenate([[0.0], np.cumsum(dphi * w)])
    return signal.detrend(phi * MM_PER_RAD)


def select_stable_chest_iq(iq: np.ndarray, dist: np.ndarray, fs: float = 50.0) -> tuple[int, int, np.ndarray]:
    """Selects both the raw amplitude maximum and the most stable bin in the chest echo (least fading)."""
    _ = fs
    amp_mean = np.abs(iq).mean(axis=0)
    amp_min = np.min(np.abs(iq), axis=0)
    amp_std = np.abs(iq).std(axis=0) + 1e-6
    k_raw = int(np.argmax(amp_mean))

    # Candidates: all bins with at least 25 % of the maximum amplitude
    mask = amp_mean >= 0.25 * amp_mean[k_raw]
    # Quality measure: high minimum |z| (no nulls/multipath cancellation) and small CV = std/mean
    quality = np.where(mask, (amp_min / amp_std) * (amp_min / amp_mean[k_raw]), -1.0)
    k_best = int(np.argmax(quality))

    # Coherent averaging over ±2 neighbor bins with phase alignment to the selected bin
    sl = slice(max(k_best - 2, 0), min(k_best + 3, iq.shape[1]))
    sub = iq[:, sl]
    ref = iq[:, k_best : k_best + 1]
    phase_align = np.exp(-1j * np.angle((sub * np.conj(ref)).mean(axis=0, keepdims=True)))
    z_aligned = (sub * phase_align).mean(axis=1)
    print(
        f"Ziel-Auswahl: Roh-Max Bin {k_raw} ({dist[k_raw]*1000:.0f} mm, min|z|={amp_min[k_raw]:.0f}) "
        f"-> Stabilster Bin {k_best} ({dist[k_best]*1000:.0f} mm, min|z|={amp_min[k_best]:.0f}, "
        f"CV={amp_std[k_best]/amp_mean[k_best]:.2f})"
    )
    return k_raw, k_best, z_aligned


def project_out_respiration_harmonics(disp: np.ndarray, fs: float, max_harm: int = 6) -> tuple[np.ndarray, np.ndarray]:
    """Estimates the narrow-band fundamental breathing phase θ_1(t) by Hilbert transform and projects 1..max_harm harmonics out in a sliding fashion."""
    sos_b = signal.butter(2, [0.1, 0.55], btype="band", fs=fs, output="sos")
    breath = signal.sosfiltfilt(sos_b, disp)

    f_b, p_b = signal.welch(breath, fs=fs, nperseg=min(len(breath), int(fs * 16)), nfft=int(fs * 120))
    mb = (f_b >= 0.12) & (f_b <= 0.55)
    fb0 = float(f_b[mb][np.argmax(p_b[mb])]) if np.any(mb) else 0.30
    sos_f1 = signal.butter(3, [max(0.08, fb0 * 0.72), min(0.65, fb0 * 1.28)], btype="band", fs=fs, output="sos")
    b_f1 = signal.sosfiltfilt(sos_f1, disp)
    theta1 = np.unwrap(np.angle(signal.hilbert(b_f1)))

    wlen = min(len(disp), int(7.0 * fs))
    step = max(1, int(1.0 * fs))
    if len(disp) <= wlen + step:
        env = np.abs(signal.hilbert(breath)) / (np.std(breath) + 1e-9)
        cols = [np.ones_like(disp), np.linspace(-1.0, 1.0, len(disp))]
        for m in range(1, max_harm + 1):
            cols.extend([np.cos(m * theta1), np.sin(m * theta1), env * np.cos(m * theta1), env * np.sin(m * theta1)])
        basis = np.column_stack(cols)
        coef, _, _, _ = np.linalg.lstsq(basis, disp, rcond=None)
        return breath, disp - basis @ coef

    clean = np.zeros_like(disp)
    weight = np.zeros_like(disp)
    hann = np.hanning(wlen)
    w_sqrt = np.sqrt(hann)[:, np.newaxis]
    t_loc = np.linspace(-1.0, 1.0, wlen)
    starts = list(range(0, len(disp) - wlen + 1, step))
    if starts[-1] != len(disp) - wlen:
        starts.append(len(disp) - wlen)
    for st in starts:
        sl = slice(st, st + wlen)
        th = theta1[sl]
        y = disp[sl]
        cols = [np.ones(wlen), t_loc]
        for m in range(1, max_harm + 1):
            cols.extend([np.cos(m * th), np.sin(m * th)])
        basis = np.column_stack(cols)
        coef, _, _, _ = np.linalg.lstsq(basis * w_sqrt, y * hann, rcond=None)
        clean[sl] += (y - basis @ coef) * hann
        weight[sl] += hann
    mask_w = weight > 1e-4
    clean[mask_w] /= weight[mask_w]
    return breath, clean


def spectrum_peaks(x: np.ndarray, fs: float, fmin: float, fmax: float, n: int = 3):
    f, p = signal.welch(x, fs=fs, nperseg=min(len(x), int(fs * 20)), nfft=int(fs * 120))
    m = (f >= fmin) & (f <= fmax)
    fi, pi = f[m], p[m]
    idx, _ = signal.find_peaks(pi)
    if len(idx) == 0:
        return f, p, []
    idx = idx[np.argsort(pi[idx])[::-1]][:n]
    return f, p, [(round(float(fi[i] * 60), 1), round(float(pi[i] / pi.max()), 2)) for i in idx]


def hps_cardiac_peaks(disp_clean: np.ndarray, fs: float, fmin: float = 0.85, fmax: float = 2.0, n: int = 3):
    """Harmonic Product Spectrum (f0 * 2f0 * 3f0) on the 2nd derivative (acceleration d²x/dt²)."""
    acc = np.gradient(np.gradient(disp_clean, 1.0 / fs), 1.0 / fs)
    f, p = signal.welch(acc, fs=fs, nperseg=min(len(acc), int(fs * 16)), nfft=int(fs * 120))
    bg = signal.medfilt(p, kernel_size=121)
    p_norm = p / np.maximum(bg, 1e-20)

    f_grid = f[(f >= fmin) & (f <= fmax)]
    hps = np.zeros_like(f_grid)
    for i, f0 in enumerate(f_grid):
        h1 = np.interp(f0, f, p_norm)
        h2 = np.interp(2.0 * f0, f, p_norm)
        h3 = np.interp(3.0 * f0, f, p_norm)
        hps[i] = h1 * (0.5 + 0.5 * h2) * (0.5 + 0.5 * h3)

    idx, _ = signal.find_peaks(hps)
    if len(idx) == 0:
        return f_grid, hps, []
    idx = idx[np.argsort(hps[idx])[::-1]][:n]
    return f_grid, hps, [(round(float(f_grid[i] * 60), 1), round(float(hps[i] / hps.max()), 2)) for i in idx]


def analyze_and_save(iq: np.ndarray, dist: np.ndarray, fs: float, out: str, label: str, cfg_json: str) -> None:
    k_raw, k_best, z = select_stable_chest_iq(iq, dist, fs)
    disp = dacm_displacement_mm(z)
    breath, disp_clean = project_out_respiration_harmonics(disp, fs, max_harm=5)

    sos_h = signal.butter(4, [0.85, 2.5], btype="band", fs=fs, output="sos")
    heart = signal.sosfiltfilt(sos_h, disp_clean)

    f, p, bpk = spectrum_peaks(disp, fs, 0.1, 0.6, 1)
    br = bpk[0][0] if bpk else float("nan")
    _, _, hpk_band = spectrum_peaks(heart, fs, 0.85, 2.2, 3)
    f_hps, p_hps, hpk_hps = hps_cardiac_peaks(disp_clean, fs, 0.85, 2.0, 3)

    print(f"Verschiebung Spitze-Spitze: {np.ptp(disp):.2f} mm")
    print(f"ATMUNG: {br:.1f} /min, Amplitude (Band) Spitze-Spitze {np.ptp(breath):.2f} mm")
    print("PULS-Kandidaten (nach Atmungs-Projektion + DACM, /min, rel.):",
          [(round(b, 1), round(s, 2)) for b, s in hpk_band])
    print("PULS-Kandidaten (Beschleunigungs-HPS f0·2f0·3f0, /min, rel.):",
          [(round(b, 1), round(s, 2)) for b, s in hpk_hps])
    harm = [round(br * h, 1) for h in range(2, 9) if 48 <= br * h <= 150]
    print("  Mittlere Atmungs-Oberwellen (zum Vergleich, /min):", harm)

    win, step = int(15 * fs), int(5 * fs)
    sliding_br, sliding_hr = [], []
    for s0 in range(0, len(disp) - win + 1, step):
        _, _, b_w = spectrum_peaks(disp[s0 : s0 + win], fs, 0.12, 0.55, 1)
        _, _, h_w = hps_cardiac_peaks(disp_clean[s0 : s0 + win], fs, 0.85, 2.0, 1)
        sliding_br.append(round(b_w[0][0], 1) if b_w else None)
        sliding_hr.append(round(h_w[0][0], 1) if h_w else None)
    print("  Atmung gleitend (15-s-Fenster, /min):", sliding_br)
    print("  Puls-Kandidat gleitend (15-s-Fenster, /min):", sliding_hr)

    np.savez_compressed(
        out + ".npz",
        iq=iq,
        distances_m=dist,
        fs=fs,
        target_bin=k_best,
        target_bin_raw=k_raw,
        meta=json.dumps({"setup": label, "time": time.strftime("%Y-%m-%dT%H:%M:%S"), "sensor_config": cfg_json}),
    )
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        t = np.arange(len(disp)) / fs
        amp = np.abs(iq).mean(axis=0)
        fig, ax = plt.subplots(4, 1, figsize=(11, 11))
        ax[0].plot(dist, amp, label="Mittelwert |IQ|")
        ax[0].axvline(dist[k_raw], color="r", ls="--", label=f"Roh-Max ({dist[k_raw]*1000:.0f} mm)")
        ax[0].axvline(dist[k_best], color="g", lw=1.8, label=f"Stabilster Bin ({dist[k_best]*1000:.0f} mm)")
        ax[0].set_xlabel("m")
        ax[0].legend()
        ax[0].set_title("Amplitude über Abstand & Qualitäts-Bin-Auswahl")

        ax[1].plot(t, disp, lw=0.8, label="DACM gesamt")
        ax[1].plot(t, breath, lw=1.8, label=f"Atmung {br:.1f}/min")
        ax[1].set_ylabel("mm")
        ax[1].legend()
        ax[1].set_title("Brustverschiebung (I/Q-kreiszentriert + DACM)")

        ax[2].plot(t, heart * 1000, lw=0.8, color="tab:green")
        ax[2].set_ylabel("µm")
        ax[2].set_xlabel("s")
        ax[2].set_title("Pulsband 0,85–2,5 Hz (nach Projektion der momentanen Atmungs-Oberwellen)")

        fh, ph = signal.welch(heart, fs=fs, nperseg=min(len(heart), int(fs * 20)), nfft=int(fs * 120))
        m = (fh >= 0.5) & (fh <= 2.5)
        ax[3].plot(fh[m] * 60, ph[m] / np.max(ph[m]), label="Pulsband-Spektrum (normiert)")
        ax[3].plot(f_hps * 60, p_hps / np.max(p_hps), lw=2.0, color="tab:red", label="Beschleunigungs-HPS (f0·2f0·3f0)")
        for h in harm:
            ax[3].axvline(h, color="gray", ls=":", lw=0.8)
        ax[3].set_xlabel("/min")
        ax[3].legend()
        ax[3].set_title("Normiertes Spektrum & Harmonic Product Spectrum (grau: mittlere Atmungs-Oberwellen)")
        fig.suptitle(label)
        fig.tight_layout()
        fig.savefig(out + ".png", dpi=90)
        print("Plot:", out + ".png")
    except Exception as e:  # noqa: BLE001
        print("Plot fehlgeschlagen:", e)
    print("gespeichert:", out + ".npz")


def main_live(port: str, out: str, dauer_s: str, label: str) -> None:
    dauer = float(dauer_s)
    client = connect(port, None)
    start_pt = auto_locate_chest_start_point(client, scan_s=1.5)
    cfg = a121.SensorConfig(
        start_point=start_pt,
        num_points=NUM_POINTS,
        step_length=STEP,
        profile=a121.Profile.PROFILE_1,
        hwaas=32,
        sweeps_per_frame=1,
        frame_rate=FRAME_RATE,
        inter_frame_idle_state=a121.IdleState.READY,
        inter_sweep_idle_state=a121.IdleState.READY,
        phase_enhancement=True,
        iq_imbalance_compensation=True,
        prf=a121.PRF.PRF_19_5_MHz,
    )
    client.setup_session(cfg)
    client.start_session()
    n = int(dauer * FRAME_RATE)
    t0 = time.time()
    print(f"AUFNAHME LÄUFT ({dauer:.0f} s @ {FRAME_RATE:.0f} Hz) — {time.strftime('%H:%M:%S')}", flush=True)
    iq = np.array([client.get_next().frame[0] for _ in range(n)])
    t_end = time.time()
    client.stop_session()
    client.close()
    fs = n / (t_end - t0)
    print(f"Aufnahme fertig {time.strftime('%H:%M:%S')}, {n} Frames, effektiv {fs:.2f} Hz")
    dist = (start_pt + np.arange(NUM_POINTS) * STEP) * POINT_M
    analyze_and_save(iq, dist, fs, out, label, cfg.to_json())


def main_offline(in_npz: str, out: str) -> None:
    z = np.load(in_npz)
    iq = z["iq"]
    dist = z["distances_m"]
    fs = float(z["fs"])
    meta = json.loads(str(z["meta"])) if "meta" in z else {}
    label = meta.get("setup", in_npz) + " (Neuauswertung: DACM + Atmungs-Projektion + HPS)"
    cfg_json = meta.get("sensor_config", "{}")
    print(f"OFFLINE-ANALYSE: {in_npz} ({len(iq)} Frames @ {fs:.2f} Hz)")
    analyze_and_save(iq, dist, fs, out, label, cfg_json)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--offline":
        main_offline(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 5:
        main_live(*sys.argv[1:])
    else:
        sys.exit(__doc__)
