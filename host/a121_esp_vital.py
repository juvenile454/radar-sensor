"""ARCHIVED (V5.3, docs/VITAL_AUDIT.md), no longer maintained.
Own pulse/breathing analysis for the I2C command `vital` (distance detector). New analyses only with
host/vital_eval.py and variants of the firmware header firmware/esp32s3/src/a121_esp_dsp.h (--replay); what
proves itself here moves into the header. Function of the script unchanged.

Vital-data recording and analysis via ESP32-S3 + A121 I2C distance detector.

Uses the optimized `vital [start_mm end_mm]` command of the ESP32-S3 firmware:
- 400 kHz I2C + hardware-interrupt polling (`GPIO18` / `MCU_INT`) -> ~37 Hz sampling rate
- `close_range_leakage_cancellation = false` (`0x43 = 0`), `PROFILE_1` (`0x45 = 1`, 40 mm FWHM),
  `max_step_length = 1` (`0x42 = 1`, 2.5 mm), `FIXED_STRENGTH = -25 dB`
- gated scatterer tracking (±15 mm gate) for primary (front chest) and secondary (upper body) scatterer
- integration of gated frame-to-frame differences (Δd, ΔS with 0.001 dB resolution) against target-swap jumps
- Hilbert-based breathing-harmonic projection + multi-feature spectrum fusion

Live recording:
  sg dialout -c ".venv/bin/python host/a121_esp_vital.py <ESP_PORT> <AUSGABE_OHNE_ENDUNG> [START_MM END_MM ANZAHL]"

Offline analysis of an existing recording:
  .venv/bin/python host/a121_esp_vital.py --offline <EINGABE.npz> <AUSGABE_OHNE_ENDUNG>

Without a reference pulse (pulse oximeter/smartwatch/ECG) the result is a rhythm candidate, not a pulse measurement.
"""
import re
import sys
import time
import numpy as np
import serial
from scipy import interpolate, signal
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LINE_RE = re.compile(r"^#(\d+)\s+(\d+)\s*ms,\s*T\s*[-\d]+\s*°C,\s*(\d+)\s*Ziel\(e\)(.*)")
PEAK_RE = re.compile(r"\|\s*(\d+)\s*mm\s*([-\d.]+)\s*dB")


def capture_esp_vital(port: str, start_mm: int, end_mm: int, n_frames: int) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.25
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.4)
    s.reset_input_buffer()

    def send_wait(cmd: str, wait_s: float = 3.0) -> str:
        s.write((cmd + "\n").encode())
        t_end = time.monotonic() + wait_s
        out = []
        while time.monotonic() < t_end:
            line = s.readline().decode("utf-8", errors="replace")
            if line:
                out.append(line)
                if "Status 0x" in line or "FEHLER" in line or "nicht bereit" in line:
                    break
        return "".join(out)

    print(f"Konfiguriere A121 über ESP32-S3: vital {start_mm} {end_mm} …")
    cfg_out = send_wait(f"vital {start_mm} {end_mm}", 4.0)
    print(cfg_out.strip())
    if "FEHLER" in cfg_out or "Timeout" in cfg_out or "nicht bereit" in cfg_out:
        s.close()
        raise RuntimeError(f"A121 vital-Konfiguration fehlgeschlagen: {cfg_out.strip()}")

    print(f"Starte Messung ({n_frames} Frames) …")
    t_list, n_list, d_list, s_list = [], [], [], []
    t0 = time.monotonic()
    s.write(f"dist {n_frames}\n".encode())

    while len(t_list) < n_frames and (time.monotonic() - t0) < (n_frames * 0.08 + 10.0):
        line = s.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        m = LINE_RE.match(line)
        if m:
            t_now = time.monotonic() - t0
            n_peaks = int(m.group(3))
            rest = m.group(4)
            peaks = PEAK_RE.findall(rest)
            dists = [np.nan] * 10
            strs = [np.nan] * 10
            for idx, (d_s, st_s) in enumerate(peaks[:10]):
                dists[idx] = float(d_s)
                strs[idx] = float(st_s)
            t_list.append(t_now)
            n_list.append(n_peaks)
            d_list.append(dists)
            s_list.append(strs)

    s.close()
    return (
        np.array(t_list, dtype=np.float64),
        np.array(n_list, dtype=np.int32),
        np.array(d_list, dtype=np.float64),
        np.array(s_list, dtype=np.float64),
    )


def track_scatterer(
    t: np.ndarray,
    tu: np.ndarray,
    d_mat: np.ndarray,
    s_mat: np.ndarray,
    d_init: float,
    gate_mm: float = 15.0,
    max_step_mm: float = 6.0,
    alpha: float = 0.12,
):
    """Tracks a scatterer with a sliding gate (±gate_mm) and integrates gated frame-to-frame deltas."""
    d_trk = np.full(len(t), np.nan)
    s_trk = np.full(len(t), np.nan)
    state_d = d_init
    prev_d = d_init
    for i in range(len(t)):
        rd, rs = d_mat[i], s_mat[i]
        m = (~np.isnan(rd)) & (np.abs(rd - state_d) <= gate_mm) & (rs > -24.5)
        if np.any(m):
            cand_d, cand_s = rd[m], rs[m]
            cost = np.abs(cand_d - prev_d) - 0.3 * cand_s
            idx = int(np.argmin(cost))
            chosen_d, chosen_s = float(cand_d[idx]), float(cand_s[idx])
            if np.abs(chosen_d - prev_d) <= max_step_mm or np.isnan(d_trk[max(0, i - 1)]):
                d_trk[i] = chosen_d
                s_trk[i] = chosen_s
                state_d = (1.0 - alpha) * state_d + alpha * chosen_d
                prev_d = chosen_d
            else:
                state_d = (1.0 - alpha * 0.25) * state_d + (alpha * 0.25) * chosen_d
                prev_d = state_d

    valid = ~np.isnan(d_trk)
    if valid.sum() < 10:
        z = np.zeros_like(tu)
        return z, z, z, z, 0.0
    dd_raw = np.diff(d_trk, prepend=d_trk[0])
    ds_raw = np.diff(s_trk, prepend=s_trk[0])
    consec = valid & np.roll(valid, 1)
    dd_clean = np.where(consec & (np.abs(dd_raw) <= max_step_mm), dd_raw, 0.0)
    ds_clean = np.where(consec & (np.abs(ds_raw) <= 3.0), ds_raw, 0.0)
    d_int = np.cumsum(dd_clean)
    s_int = np.cumsum(ds_clean)
    return (
        np.interp(tu, t[valid], d_trk[valid]),
        np.interp(tu, t[valid], s_trk[valid]),
        np.interp(tu, t, d_int),
        np.interp(tu, t, s_int),
        float(np.mean(valid)),
    )


def find_initial_scatterers(d_mat: np.ndarray, s_mat: np.ndarray) -> tuple[float, float]:
    """Automatically finds the two dominant scatterer distances (front chest & secondary scatterer)."""
    d_filt = np.where((~np.isnan(d_mat)) & (s_mat > -24.5), d_mat, np.nan)
    if np.all(np.isnan(d_filt)):
        raise RuntimeError("Keine gültigen Ziele in der Aufnahme erkannt.")
    # Front chest wall: 35th percentile of the nearest valid scatterer per frame
    d_closest = np.nanmin(d_filt, axis=1)
    d1 = float(np.percentile(d_closest[~np.isnan(d_closest)], 35))
    valid_sec = (~np.isnan(d_filt)) & (d_filt >= d1 + 35.0) & (d_filt <= d1 + 165.0)
    if np.any(valid_sec):
        sec_vals = d_filt[valid_sec]
        bins = np.arange(np.floor(sec_vals.min()) - 4, np.ceil(sec_vals.max()) + 8, 6.0)
        hist, edges = np.histogram(sec_vals, bins=bins)
        d2 = float(0.5 * (edges[np.argmax(hist)] + edges[np.argmax(hist) + 1]))
    else:
        d2 = d1 + 55.0
    return d1, d2


def analyze_esp_vital(t: np.ndarray, n_peaks: np.ndarray, d_mat: np.ndarray, s_mat: np.ndarray, out_prefix: str) -> None:
    dur = float(t[-1] - t[0])
    fs_mean = (len(t) - 1) / dur
    det_rate = float(np.mean(n_peaks > 0) * 100.0)
    print(f"Frames: {len(t)} in {dur:.2f} s ({fs_mean:.2f} Hz), Trefferquote (n>=1): {det_rate:.1f} %")

    fs = 40.0
    tu = np.arange(t[0], t[-1], 1.0 / fs)
    init_a, init_b = find_initial_scatterers(d_mat, s_mat)
    dtA, stA, dintA, sintA, covA = track_scatterer(t, tu, d_mat, s_mat, d_init=init_a, gate_mm=15.0)
    dtB, stB, dintB, sintB, covB = track_scatterer(t, tu, d_mat, s_mat, d_init=init_b, gate_mm=18.0)
    med_sA = float(np.median(stA))
    med_sB = float(np.median(stB))
    print(f"Track A (Vorderbrust ~{np.median(dtA):.1f} mm, {med_sA:+.2f} dB): {covA*100:.1f} % Gated")
    print(f"Track B (Sekundär    ~{np.median(dtB):.1f} mm, {med_sB:+.2f} dB): {covB*100:.1f} % Gated")

    # Find the quietest contiguous 20 s section (lowest macro-motion energy in distance + strength)
    w_mot = int(3.0 * fs)
    mot_d = np.array([
        np.ptp(dintA[max(0, i - w_mot // 2):min(len(dintA), i + w_mot // 2)])
        for i in range(len(dintA))
    ])
    mot_s = np.array([
        np.ptp(stA[max(0, i - w_mot // 2):min(len(stA), i + w_mot // 2)])
        for i in range(len(stA))
    ])
    mot_score = mot_d / 5.0 + mot_s / 3.0
    win_len = min(len(tu), int(20.0 * fs))
    if len(tu) > win_len + int(2.0 * fs):
        cs = np.concatenate([[0.0], np.cumsum(mot_score)])
        win_means = (cs[win_len:] - cs[:-win_len]) / win_len
        qa = int(np.argmin(win_means))
        qb = qa + win_len
    else:
        qa, qb = 0, len(tu)
    print(f"Ruhigster Abschnitt ohne Makro-Bewegung: {tu[qa]-tu[0]:.1f}–{tu[qb-1]-tu[0]:.1f} s ({(qb-qa)/fs:.1f} s)")

    sos_b = signal.butter(2, [0.12, 0.50], btype="band", fs=fs, output="sos")
    sos_h = signal.butter(4, [0.85, 2.00], btype="band", fs=fs, output="sos")

    # Common narrow-band breathing phase θ_1(t) + sliding 7 s Hann-Hilbert projection
    d_segA = dintA[qa:qb] - np.mean(dintA[qa:qb])
    brA_full = signal.sosfiltfilt(sos_b, dintA - np.mean(dintA))
    brA = signal.sosfiltfilt(sos_b, d_segA)
    fb, pb = signal.welch(brA, fs=fs, nperseg=len(brA), nfft=int(120 * fs))
    mb = (fb >= 0.12) & (fb <= 0.50)
    br_bpm = float(fb[mb][np.argmax(pb[mb])] * 60.0)
    fb0 = br_bpm / 60.0
    sos_b1 = signal.butter(2, [max(0.08, fb0 * 0.72), min(0.55, fb0 * 1.28)], btype="band", fs=fs, output="sos")
    br_fund_full = signal.sosfiltfilt(sos_b1, dintA - np.mean(dintA))
    analytic_br = signal.hilbert(br_fund_full - np.mean(br_fund_full))
    theta_b = np.angle(analytic_br)
    env_b = np.abs(analytic_br) / (np.std(br_fund_full) + 1e-9)
    cols_b = [np.ones_like(theta_b)]
    for m_h in range(1, 7):
        cols_b.extend([
            np.cos(m_h * theta_b),
            np.sin(m_h * theta_b),
            env_b * np.cos(m_h * theta_b),
            env_b * np.sin(m_h * theta_b),
        ])
    basis_b = np.column_stack(cols_b)

    def process_chan(y_full: np.ndarray):
        sig0 = y_full - np.mean(y_full)
        n = len(sig0)
        win = min(n, int(round(7.0 * fs)))
        step = max(1, int(round(1.0 * fs)))
        hann = np.hanning(win)
        num = np.zeros(n, dtype=np.float64)
        den = np.zeros(n, dtype=np.float64)
        starts = list(range(0, n - win + 1, step))
        if starts[-1] != n - win:
            starts.append(n - win)
        for s0 in starts:
            s1 = s0 + win
            sw = np.sqrt(hann + 1e-6)
            Bw = basis_b[s0:s1] * sw[:, None]
            yw = sig0[s0:s1] * sw
            coef, _, _, _ = np.linalg.lstsq(Bw, yw, rcond=None)
            res = sig0[s0:s1] - basis_b[s0:s1] @ coef
            num[s0:s1] += hann * res
            den[s0:s1] += hann
        y_cln_full = num / np.maximum(den, 1e-9)
        hr_full = signal.sosfiltfilt(sos_h, y_cln_full)
        hr_seg = hr_full[qa:qb]
        f, p = signal.welch(hr_seg, fs=fs, nperseg=min(len(hr_seg), int(15 * fs)), nfft=int(120 * fs))
        return hr_full, hr_seg, f, p

    hr_dintA, _, f, p_dintA = process_chan(dintA)
    hr_stA, _, _, p_stA = process_chan(stA)
    hr_dtB, _, _, p_dtB = process_chan(dtB)
    hr_stB, _, _, p_stB = process_chan(stB)
    print(f"ATMUNG (ruhiger Abschnitt): {br_bpm:.1f} /min (Oberwellen: {[round(br_bpm * k, 1) for k in range(2, 7)]})")

    # Weighting of track B depends on the echo-strength gap to track A
    use_track_b = (covB >= 0.65) and ((med_sA - med_sB) <= 12.0)
    if not use_track_b:
        print(f"  Hinweis: Track B ist {med_sA - med_sB:.1f} dB schwächer als Track A -> Fusion nutzt primär Track A (Δd + dB).")

    m = (f >= 0.85) & (f <= 2.00)
    p_fused = np.ones_like(f)
    ch_specs = [
        ("TrackA_d", p_dintA, "tab:blue", 1.0),
        ("TrackA_s", p_stA, "tab:green", 1.0),
        ("TrackB_d", p_dtB, "tab:orange", 0.7 if use_track_b else 0.0),
        ("TrackB_s", p_stB, "tab:purple", 0.7 if use_track_b else 0.0),
    ]
    for name, p_ch, _, w_ch in ch_specs:
        pn = p_ch / (np.max(p_ch[m]) + 1e-12)
        if w_ch > 0:
            p_fused *= (pn + 0.08) ** w_ch
        pk_c, _ = signal.find_peaks(pn[m])
        pk_c = pk_c[np.argsort(pn[m][pk_c])[::-1]][:3]
        print(f"  {name:8s} Puls-Kandidaten (/min): {[(round(float(f[m][k]*60), 1), round(float(pn[m][k]), 2)) for k in pk_c]}")

    pk_f, _ = signal.find_peaks(p_fused[m])
    pk_f = pk_f[np.argsort(p_fused[m][pk_f])[::-1]][:3]
    fused_pks = [(round(float(f[m][k] * 60), 1), round(float(p_fused[m][k] / p_fused[m].max()), 2)) for k in pk_f]
    print(f"PULS-KANDIDAT (Multi-Feature-Fusion, /min): {fused_pks}")

    fig, ax = plt.subplots(4, 1, figsize=(12, 11))
    ax[0].plot(tu, dtA, label=f"Track A: vordere Brustwand (~{np.median(dtA):.0f} mm, {med_sA:+.1f} dB, {covA*100:.0f}% Gated)", lw=1.2)
    ax[0].plot(tu, dtB, label=f"Track B: Sekundär (~{np.median(dtB):.0f} mm, {med_sB:+.1f} dB, {covB*100:.0f}% Gated)", lw=1.2)
    ax[0].axvspan(tu[qa], tu[qb - 1], color="y", alpha=0.22, label="ruhigster Auswerteabschnitt")
    ax[0].set_ylabel("mm")
    ax[0].set_title(f"A121 über ESP32-S3 (I2C, Profile 1, {fs_mean:.1f} Hz): Gated Scatterer Tracking ohne Ziel-Sprünge")
    ax[0].legend(loc="upper right")
    ax[0].grid(True, alpha=0.3)

    ax[1].plot(tu, dintA - np.mean(dintA), label="Integrierte Frame-zu-Frame-Verschiebung Δd (Track A)", lw=1.0, color="tab:blue")
    ax[1].plot(tu, brA_full, label=f"Atmungskomponente ({br_bpm:.1f} /min)", lw=2.0, color="tab:orange")
    ax[1].axvspan(tu[qa], tu[qb - 1], color="y", alpha=0.22)
    ax[1].set_ylabel("mm (relativ)")
    ax[1].set_title("Kontinuierliche Brustbewegung (gelb: ruhiger Abschnitt ohne Makro-Bewegung)")
    ax[1].legend(loc="upper right")
    ax[1].grid(True, alpha=0.3)

    ax[2].plot(tu, hr_dintA, label="Pulsband Weg (Track A Δd, mm)", lw=1.0, color="tab:blue")
    ax[2].plot(tu, hr_stA * 0.5, label="Pulsband Echo-Stärke (Track A, 0,5× dB)", lw=1.0, color="tab:green", alpha=0.8)
    ax[2].axvspan(tu[qa], tu[qb - 1], color="y", alpha=0.22)
    ax[2].set_ylabel("Amplitude")
    ax[2].set_xlabel("Zeit (s)")
    ax[2].set_title("Extrahiertes Pulsband (0,85–2,00 Hz)")
    ax[2].legend(loc="upper right")
    ax[2].grid(True, alpha=0.3)

    mp = (f * 60 >= 50) & (f * 60 <= 125)
    for name, p_ch, col, w_ch in ch_specs:
        if w_ch <= 0:
            continue
        pk_top = float(f[m][np.argmax(p_ch[m])] * 60)
        ax[3].plot(f[mp] * 60, p_ch[mp] / np.max(p_ch[m]), lw=1.2, alpha=0.55, label=f"{name} ({pk_top:.1f} /min)", color=col)
    for k in range(3, 6):
        if 50 <= br_bpm * k <= 125:
            ax[3].axvline(br_bpm * k, color="gray", ls="--", alpha=0.6, label=f"{k}× Atmung" if k == 3 else None)
    if fused_pks:
        ax[3].plot(
            f[mp] * 60,
            p_fused[mp] / np.max(p_fused[m]),
            color="red",
            lw=2.5,
            label=f"Fusioniertes Spektrum (Peak: {fused_pks[0][0]:.1f} /min)",
        )
    ax[3].set_xlabel("Frequenz (/min)")
    ax[3].set_ylabel("Normierte Leistung")
    ax[3].set_title("Multi-Feature Spektralfusion im ruhigen Abschnitt (Abstand + 0,001-dB-Stärke)")
    ax[3].legend(loc="upper right")
    ax[3].grid(True, alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_prefix + ".png", dpi=110)
    print(f"Plot: {out_prefix}.png")


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "--offline":
        in_npz, out_pref = sys.argv[2], sys.argv[3]
        z = np.load(in_npz)
        d_m = z["distances_mm"] if "distances_mm" in z else z["d_mm"]
        s_d = z["strengths_db"] if "strengths_db" in z else z["s_db"]
        n_p = z["n_peaks"] if "n_peaks" in z else np.sum(~np.isnan(d_m), axis=1)
        analyze_esp_vital(z["t"], n_p, d_m, s_d, out_pref)
        sys.exit(0)

    if len(sys.argv) < 3:
        sys.exit(__doc__)

    port_arg = sys.argv[1]
    out_arg = sys.argv[2]
    s_mm = int(sys.argv[3]) if len(sys.argv) > 3 else 450
    e_mm = int(sys.argv[4]) if len(sys.argv) > 4 else 850
    n_cnt = int(sys.argv[5]) if len(sys.argv) > 5 else 1800

    t_arr, n_arr, d_arr, s_arr = capture_esp_vital(port_arg, s_mm, e_mm, n_cnt)
    np.savez_compressed(
        out_arg + ".npz",
        t=t_arr,
        n_peaks=n_arr,
        distances_mm=d_arr,
        strengths_db=s_arr,
        n=n_arr,
        d_mm=d_arr,
        s_db=s_arr,
    )
    print(f"gespeichert: {out_arg}.npz")
    analyze_esp_vital(t_arr, n_arr, d_arr, s_arr, out_arg)
