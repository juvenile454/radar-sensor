"""Vollautomatischer Mobiler Radar-Modus (ESP32-S3 + A121): Auto-Search -> Auto-Lock -> Auto-Zoom + 10-fach Multi-Info.

Nutzt das `auto [sek]`-Kommando der ESP32-S3-Firmware:
- Startet automatisch im Weitbereich (`WIDE_SEARCH`, 150–2500 mm, Profile 2) und erfasst die Raum-Ziele.
- Prüft im Zustand `TARGET_LOCK` die Stabilität des vordersten starken Ziels (~1,5 s).
- Schaltet automatisch in `VITAL_ZOOM` ([d0 - 135 mm, d0 + 155 mm], Profile 1/2, Step 2,5 mm, ~45 Hz)
  und führt das Fenster bei Positionsänderungen automatisch nach (`@Z`).
- Berechnet gleichzeitig 10 Messgrößen (wie für das spätere Android-App-Dashboard):
  1. Auto-State & Zoom-Fenster (SEARCH / LOCK / ZOOM, Profil, Abtastrate Hz)
  2. Primär-Entfernung d0 (mm) + Raum-Mehr-Ziel-Landkarte (bis 10 Echos)
  3. Relativgeschwindigkeit v (mm/s bzw. cm/s)
  4. Hand-/Ziel-Stabilitäts-Score (0–100 %, Ruhe-Ampel)
  5. Automatische Ziel-Klassifikation (Mensch/Vital vs. Statisches Objekt/Wand vs. Vibration vs. Bewegung)
  6. Atemfrequenz (/min), Atemhub (mm) & Atemphase
  7. Atem-Anhalte- / Apnoe-Detektor (Sekunden)
  8. Herzschlag-Kandidat (/min) + Konfidenz-Index (0–100 %) nach Hilbert-Oberwellenprojektion
  9. Technische Vibrations-Frequenz (2,5–20 Hz / RPM)
  10. Reflexionsgüte (dB + entfernungsnormierter RCS-Index) & Chip-Temperatur (°C)

Live-Lauf:
  sg dialout -c ".venv/bin/python host/a121_esp_auto.py <ESP_PORT> <AUSGABE_OHNE_ENDUNG> [DAUER_S]"

Offline-Auswertung:
  .venv/bin/python host/a121_esp_auto.py --offline <EINGABE.npz> <AUSGABE_OHNE_ENDUNG>

Ohne Referenzpuls (Smartwatch/Pulsoximeter/EKG) ist der Herzschlag-Wert ein Rhythmus-Kandidat, keine Messung.
"""
import json
import sys
import time
import numpy as np
import serial
from scipy import signal
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def capture_auto_stream(port: str, duration_s: int = 40):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.25
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.35)
    s.reset_input_buffer()

    print(f"Starte vollautomatischen Modus am ESP32-S3 (`auto {duration_s}`) …")
    s.write(f"auto {duration_s}\n".encode())

    t_list, state_list, win_list, prof_list, temp_list, n_list = [], [], [], [], [], []
    d_list, s_list = [], []
    zoom_events = []
    t0 = time.monotonic()
    last_print = t0

    while (time.monotonic() - t0) < (duration_s + 4.0):
        line = s.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line == "@END" or line.startswith("@E"):
            if line.startswith("@E"):
                print("Firmware-Meldung:", line)
            break
        if line.startswith("@Z "):
            parts = line.split()
            if len(parts) >= 8:
                z_ev = {
                    "t_s": round(time.monotonic() - t0, 2),
                    "start_mm": int(parts[2]),
                    "end_mm": int(parts[3]),
                    "profile": int(parts[4]),
                    "step": int(parts[5]),
                    "cal_ms": int(parts[6]),
                    "status": parts[7],
                }
                zoom_events.append(z_ev)
                print(
                    f"  [Auto-Zoom @ {z_ev['t_s']:5.2f} s] Fenster {z_ev['start_mm']}–{z_ev['end_mm']} mm "
                    f"(Profile {z_ev['profile']}, Step {z_ev['step']*2.5:.1f} mm, Kalibrierung {z_ev['cal_ms']} ms)"
                )
        elif line.startswith("@F "):
            segments = line.split("|")
            hdr = segments[0].split()
            if len(hdr) < 8:
                continue
            t_now = time.monotonic() - t0
            st_str = hdr[2]
            w_start, w_end = int(hdr[3]), int(hdr[4])
            prof = int(hdr[5])
            temp_c = int(hdr[6])
            n_p = int(hdr[7])
            dists = [np.nan] * 10
            strs = [np.nan] * 10
            for idx, seg in enumerate(segments[1:11]):
                sp = seg.strip().split()
                if len(sp) >= 2:
                    dists[idx] = float(sp[0])
                    strs[idx] = float(sp[1])
            t_list.append(t_now)
            state_list.append(st_str)
            win_list.append([w_start, w_end])
            prof_list.append(prof)
            temp_list.append(temp_c)
            n_list.append(n_p)
            d_list.append(dists)
            s_list.append(strs)

            if (t_now - (last_print - t0)) >= 4.0:
                last_print = time.monotonic()
                valid_s = [s_v for s_v in strs if not np.isnan(s_v)]
                if valid_s:
                    best_i = int(np.nanargmax(strs))
                    d0_str = f"{dists[best_i]:.0f} mm ({strs[best_i]:+.2f} dB)"
                else:
                    d0_str = "kein Ziel"
                print(f"  t={t_now:5.1f} s | State={st_str:6s} [{w_start:4d}–{w_end:4d} mm, P{prof}] | Hauptziel: {d0_str} | n={n_p}")

    s.close()
    return (
        np.array(t_list, dtype=np.float64),
        np.array(state_list, dtype="<U8"),
        np.array(win_list, dtype=np.int32),
        np.array(prof_list, dtype=np.int32),
        np.array(temp_list, dtype=np.int32),
        np.array(n_list, dtype=np.int32),
        np.array(d_list, dtype=np.float64),
        np.array(s_list, dtype=np.float64),
        zoom_events,
    )


def extract_room_map(states: np.ndarray, d_mat: np.ndarray, s_mat: np.ndarray) -> list[dict]:
    """Extrahiert aus allen Frames (insb. SEARCH/LOCK) die erkannten Raum-Echos als Distanz-Cluster."""
    valid = (~np.isnan(d_mat)) & (s_mat > -24.0)
    all_d = d_mat[valid]
    all_s = s_mat[valid]
    if len(all_d) == 0:
        return []
    bins = np.arange(150.0, 2600.0, 40.0)
    hist, edges = np.histogram(all_d, bins=bins)
    centers = 0.5 * (edges[:-1] + edges[1:])
    pks, _ = signal.find_peaks(hist, distance=2, height=3)
    clusters = []
    for p in pks:
        c_d = centers[p]
        m = np.abs(all_d - c_d) <= 45.0
        if np.any(m):
            clusters.append({
                "distance_mm": round(float(np.median(all_d[m])), 1),
                "strength_db": round(float(np.median(all_s[m])), 2),
                "hits": int(np.sum(m)),
            })
    clusters.sort(key=lambda x: x["distance_mm"])
    return clusters


def analyze_auto_capture(
    t: np.ndarray,
    states: np.ndarray,
    wins: np.ndarray,
    profs: np.ndarray,
    temps: np.ndarray,
    n_peaks: np.ndarray,
    d_mat: np.ndarray,
    s_mat: np.ndarray,
    zoom_events: list[dict],
    out_prefix: str,
) -> dict:
    if len(t) < 20:
        raise RuntimeError("Zu wenige Frames für die Analyse.")

    dur = float(t[-1] - t[0])
    fs_all = (len(t) - 1) / dur
    zoom_mask = states == "ZOOM"
    if np.sum(zoom_mask) >= 40:
        t_z = t[zoom_mask]
        fs_zoom = float((len(t_z) - 1) / max(t_z[-1] - t_z[0], 1e-3))
    else:
        fs_zoom = fs_all

    # 1. Raum-Landkarte (Multi-Target Map)
    room_clusters = extract_room_map(states, d_mat, s_mat)

    # 2. Primär-Streuer-Tracking über den gesamten Verlauf (fokusiert auf das Hauptziel im Zoom-Fenster)
    s_clean = np.where(np.isnan(s_mat), -999.0, s_mat)
    best_j = np.argmax(s_clean, axis=1)
    d_strong = d_mat[np.arange(len(t)), best_j]
    d_init = float(np.nanmedian(d_strong[zoom_mask])) if np.any(zoom_mask) else float(np.nanmedian(d_strong))

    fs = 45.0
    tu = np.arange(t[0], t[-1], 1.0 / fs)
    d_trk = np.full(len(t), np.nan)
    s_trk = np.full(len(t), np.nan)
    state_d = d_init
    prev_d = d_init
    for i in range(len(t)):
        rd, rs = d_mat[i], s_mat[i]
        if np.all(np.isnan(rd)):
            continue
        s_max_frame = float(np.nanmax(rs))
        gate = 40.0 if states[i] != "ZOOM" else 16.0
        m = (~np.isnan(rd)) & (np.abs(rd - state_d) <= gate) & (rs >= max(-18.0, s_max_frame - 8.0))
        if np.any(m):
            cand_d, cand_s = rd[m], rs[m]
            cost = np.abs(cand_d - prev_d) - 0.35 * cand_s
            idx = int(np.argmin(cost))
            chosen_d, chosen_s = float(cand_d[idx]), float(cand_s[idx])
            if np.abs(chosen_d - prev_d) <= 6.0 or np.isnan(d_trk[max(0, i - 1)]):
                d_trk[i] = chosen_d
                s_trk[i] = chosen_s
                state_d = 0.88 * state_d + 0.12 * chosen_d
                prev_d = chosen_d
            else:
                state_d = 0.97 * state_d + 0.03 * chosen_d
                prev_d = state_d

    valid = ~np.isnan(d_trk)
    consec = valid & np.roll(valid, 1)
    dd_raw = np.diff(d_trk, prepend=d_trk[0])
    ds_raw = np.diff(s_trk, prepend=s_trk[0])
    dd_clean = np.where(consec & (np.abs(dd_raw) <= 6.0), dd_raw, 0.0)
    ds_clean = np.where(consec & (np.abs(ds_raw) <= 3.0), ds_raw, 0.0)
    d_int = np.cumsum(dd_clean)
    s_int = np.cumsum(ds_clean)

    dt_u = np.interp(tu, t[valid], d_trk[valid])
    st_u = np.interp(tu, t[valid], s_trk[valid])
    dint_u = np.interp(tu, t, d_int)
    sint_u = np.interp(tu, t, s_int)

    # 3. Relativgeschwindigkeit v(t) [mm/s] & Stabilitäts-Score [0..100 %] (relativ zur Atem-Grundlinie)
    d_base = signal.savgol_filter(dt_u, int(3.5 * fs) | 1, 1)
    vel_mm_s = np.gradient(signal.savgol_filter(dt_u, int(1.8 * fs) | 1, 2), 1.0 / fs)
    w2 = int(2.5 * fs)
    mot_d = np.array([np.ptp(d_base[max(0, i - w2 // 2):min(len(tu), i + w2 // 2)]) for i in range(len(tu))])
    mot_s = np.array([np.std(np.diff(st_u[max(0, i - w2 // 2):min(len(tu), i + w2 // 2)])) for i in range(len(tu))])
    stab_t = np.clip(100.0 - 9.0 * np.maximum(0.0, mot_d - 1.5) - 25.0 * np.maximum(0.0, mot_s - 0.35), 0.0, 100.0)

    # Ruhigstes 18-s-Fenster im ZOOM-Bereich auswählen
    zoom_u = np.interp(tu, t, zoom_mask.astype(float)) > 0.5
    mot_score = (100.0 - stab_t) + np.where(zoom_u, 0.0, 200.0)
    win_len = min(len(tu), int(18.0 * fs))
    if len(tu) > win_len + int(2.0 * fs):
        cs = np.concatenate([[0.0], np.cumsum(mot_score)])
        qa = int(np.argmin((cs[win_len:] - cs[:-win_len]) / win_len))
        qb = qa + win_len
    else:
        qa, qb = 0, len(tu)

    # 4. Atmung & Apnoe-Detektor
    sos_b = signal.butter(2, [0.12, 0.50], btype="band", fs=fs, output="sos")
    sos_h = signal.butter(4, [0.85, 2.00], btype="band", fs=fs, output="sos")
    sos_v = signal.butter(4, [5.00, 19.0], btype="band", fs=fs, output="sos")

    br_full = signal.sosfiltfilt(sos_b, dint_u - np.mean(dint_u))
    br_env = np.abs(signal.hilbert(br_full))
    apnea_mask = (stab_t >= 70.0) & (br_env < 0.35)
    # Längste Apnoe-Episode
    best_ap, cur_ap = 0, 0
    for ap in apnea_mask:
        cur_ap = cur_ap + 1 if ap else 0
        if cur_ap > best_ap:
            best_ap = cur_ap
    apnea_s = round(float(best_ap / fs), 1)

    d_seg = dint_u[qa:qb] - np.mean(dint_u[qa:qb])
    s_seg = st_u[qa:qb] - np.mean(st_u[qa:qb])
    br_seg = signal.sosfiltfilt(sos_b, d_seg)
    fb, pb = signal.welch(br_seg, fs=fs, nperseg=len(br_seg), nfft=int(120 * fs))
    mb = (fb >= 0.12) & (fb <= 0.50)
    br_bpm = float(fb[mb][np.argmax(pb[mb])] * 60.0)
    br_amp_mm = float(np.percentile(br_seg, 95) - np.percentile(br_seg, 5))

    # 5. Gleitende 7-s-Hann-Hilbert-Oberwellenprojektion (Schmalband-Grundphase θ_1(t)) & Herzschlag-Kandidat
    fb0 = br_bpm / 60.0
    bw_lo = max(0.08, fb0 * 0.72)
    bw_hi = min(0.55, fb0 * 1.28)
    sos_b1 = signal.butter(2, [bw_lo, bw_hi], btype="band", fs=fs, output="sos")
    br_fund_full = signal.sosfiltfilt(sos_b1, dint_u - np.mean(dint_u))
    an_br_full = signal.hilbert(br_fund_full - np.mean(br_fund_full))
    th_b_full = np.angle(an_br_full)
    env_b_full = np.abs(an_br_full) / (np.std(br_fund_full) + 1e-9)
    cols_b = [np.ones_like(th_b_full)]
    for k in range(1, 7):
        cols_b.extend([
            np.cos(k * th_b_full),
            np.sin(k * th_b_full),
            env_b_full * np.cos(k * th_b_full),
            env_b_full * np.sin(k * th_b_full),
        ])
    basis_full = np.column_stack(cols_b)

    def clean_sliding(sig_in: np.ndarray) -> np.ndarray:
        sig0 = sig_in - np.mean(sig_in)
        n = len(sig0)
        win = min(n, int(round(7.0 * fs)))
        step = max(1, int(round(1.0 * fs)))
        if win < int(3.5 * fs):
            c, _, _, _ = np.linalg.lstsq(basis_full, sig0, rcond=None)
            return sig0 - basis_full @ c
        hann = np.hanning(win)
        num = np.zeros(n, dtype=np.float64)
        den = np.zeros(n, dtype=np.float64)
        starts = list(range(0, n - win + 1, step))
        if starts[-1] != n - win:
            starts.append(n - win)
        for s0 in starts:
            s1 = s0 + win
            sw = np.sqrt(hann + 1e-6)
            Bw = basis_full[s0:s1] * sw[:, None]
            yw = sig0[s0:s1] * sw
            c, _, _, _ = np.linalg.lstsq(Bw, yw, rcond=None)
            res = sig0[s0:s1] - basis_full[s0:s1] @ c
            num[s0:s1] += hann * res
            den[s0:s1] += hann
        return num / np.maximum(den, 1e-9)

    d_cln_full = clean_sliding(dint_u)
    s_cln_full = clean_sliding(st_u)
    hr_d_full = signal.sosfiltfilt(sos_h, d_cln_full)
    hr_s_full = signal.sosfiltfilt(sos_h, s_cln_full)
    hr_d = hr_d_full[qa:qb]
    hr_s = hr_s_full[qa:qb]

    fh, pd = signal.welch(hr_d, fs=fs, nperseg=min(len(hr_d), int(15 * fs)), nfft=int(120 * fs))
    _, ps = signal.welch(hr_s, fs=fs, nperseg=min(len(hr_s), int(15 * fs)), nfft=int(120 * fs))
    m = (fh >= 0.85) & (fh <= 2.00)
    pd_n = pd[m] / (np.max(pd[m]) + 1e-12)
    ps_n = ps[m] / (np.max(ps[m]) + 1e-12)
    p_fused = (pd_n + 0.08) * (ps_n + 0.08)
    p_fused /= np.max(p_fused) + 1e-12

    pks_f, _ = signal.find_peaks(p_fused)
    pks_f = pks_f[np.argsort(p_fused[pks_f])[::-1]][:3]
    hr_candidates = [(round(float(fh[m][i] * 60), 1), round(float(p_fused[i]), 2)) for i in pks_f]
    hr_best = hr_candidates[0][0] if hr_candidates else 0.0

    # Autokorrelation auf dem kombinierten normierten Zeitsignal
    hr_comb = hr_d / (np.std(hr_d) + 1e-9) + hr_s / (np.std(hr_s) + 1e-9)
    ac = np.correlate(hr_comb - np.mean(hr_comb), hr_comb - np.mean(hr_comb), "full")[len(hr_comb) - 1:]
    ac /= max(float(ac[0]), 1e-12)
    lo_ac, hi_ac = int(fs * 60 / 130), int(fs * 60 / 50)
    j_ac = lo_ac + int(np.argmax(ac[lo_ac:hi_ac]))
    hr_ac_bpm = round(float(60.0 * fs / j_ac), 1)
    ac_r = round(float(ac[j_ac]), 2)

    # Konfidenz-Score (0..100 %)
    second_pk = hr_candidates[1][1] if len(hr_candidates) > 1 else 0.5
    prom_score = np.clip((1.0 - second_pk) * 100.0, 15.0, 100.0)
    harm_dist = min(abs(hr_best - br_bpm * k) for k in range(2, 7))
    harm_factor = 1.0 if harm_dist >= 3.0 else 0.65
    stab_mean = float(np.mean(stab_t[qa:qb]))
    hr_conf = round(float(np.clip((0.45 * prom_score + 0.35 * stab_mean + 20.0 * max(ac_r, 0.0)) * harm_factor, 0.0, 98.0)), 1)

    # 6. Technische Vibrations-Analyse (5,0–19 Hz)
    vib_sig = signal.sosfiltfilt(sos_v, s_seg)
    fv, pv = signal.welch(vib_sig, fs=fs, nperseg=min(len(vib_sig), int(8 * fs)), nfft=int(40 * fs))
    mv = (fv >= 5.0) & (fv <= 19.0)
    vib_snr = float(np.max(pv[mv]) / (np.median(pv[mv]) + 1e-12))
    vib_hz = round(float(fv[mv][np.argmax(pv[mv])]), 2) if vib_snr > 25.0 else 0.0

    # 7. Automatische Ziel-Klassifikation
    std_s_calm = float(np.std(s_seg))
    std_d_calm = float(np.std(d_seg))
    mean_abs_v = float(np.mean(np.abs(vel_mm_s[qa:qb])))
    if mean_abs_v > 18.0:
        target_class = "BEWEGTES ZIEL / HAND-BEWEGUNG"
    elif std_s_calm < 0.04 and std_d_calm < 0.5:
        target_class = "STATISCHES OBJEKT (Wand / Möbel)"
    elif vib_snr > 25.0 and std_s_calm > 0.1:
        target_class = f"TECHNISCHE VIBRATION ({vib_hz:.1f} Hz / {vib_hz*60:.0f} RPM)"
    else:
        target_class = "MENSCH (Vital-Ziel erkannt)"

    # 8. Reflexionsgüte & normierter RCS-Index
    med_d_m = float(np.median(dt_u[qa:qb])) / 1000.0
    med_s_db = float(np.median(st_u[qa:qb]))
    rcs_norm_db = round(med_s_db + 40.0 * np.log10(max(med_d_m, 0.1)), 1)

    summary = {
        "frames_total": int(len(t)),
        "duration_s": round(dur, 2),
        "fs_zoom_hz": round(fs_zoom, 2),
        "zoom_events": zoom_events,
        "primary_distance_mm": round(float(np.median(dt_u[qa:qb])), 1),
        "primary_strength_db": round(med_s_db, 2),
        "rcs_normalized_db_at_1m": rcs_norm_db,
        "velocity_mean_mm_s": round(float(np.mean(vel_mm_s[qa:qb])), 2),
        "stability_score_pct": round(stab_mean, 1),
        "target_classification": target_class,
        "breathing_bpm": round(br_bpm, 1),
        "breathing_depth_mm": round(br_amp_mm, 2),
        "apnea_max_duration_s": apnea_s,
        "heart_rate_candidate_bpm": hr_best,
        "heart_rate_candidates": hr_candidates,
        "heart_rate_autocorr_bpm": hr_ac_bpm,
        "heart_rate_confidence_pct": hr_conf,
        "vibration_hz": vib_hz,
        "temperature_c": int(np.median(temps)),
        "room_targets": room_clusters,
    }

    print("\n" + "=" * 72)
    print("  MOBILER AUTO-RADAR-REPORT (10-FACH MULTI-INFO FÜR ANDROID-APP)")
    print("=" * 72)
    print(f" 1. Auto-Zoom & Rate   : {len(zoom_events)} Fenster-Anpassung(en), Zoom-Rate {fs_zoom:.1f} Hz ({len(t)} Frames)")
    print(f" 2. Hauptziel-Abstand  : {summary['primary_distance_mm']:.1f} mm ({len(room_clusters)} Echo-Cluster im Raum erkannt)")
    for idx_c, cl in enumerate(room_clusters):
        print(f"      Echo #{idx_c+1}: {cl['distance_mm']:6.1f} mm | {cl['strength_db']:+6.2f} dB ({cl['hits']} Treffer)")
    print(f" 3. Relativgeschwindig.: {summary['velocity_mean_mm_s']:+.2f} mm/s (im Ruhefenster)")
    print(f" 4. Ziel-Stabilität    : {summary['stability_score_pct']:.1f} % ({'GRÜN: Vital-Lock stabil' if stab_mean >= 70 else 'GELB: Bewegung'})")
    print(f" 5. Ziel-Klassifikation: {summary['target_classification']}")
    print(f" 6. Atmung             : {summary['breathing_bpm']:.1f} /min (Brusthub ~{summary['breathing_depth_mm']:.2f} mm)")
    print(f" 7. Apnoe-/Ruhe-Detekt.: max. {summary['apnea_max_duration_s']:.1f} s ohne Atemhub")
    print(f" 8. Puls-Kandidat      : {summary['heart_rate_candidate_bpm']:.1f} /min (Konfidenz {summary['heart_rate_confidence_pct']:.0f} %, Autokorr. {hr_ac_bpm:.1f} /min, r={ac_r:.2f})")
    print(f"      Top-3 Kandidaten : {hr_candidates} | Atmungs-Oberwellen: {[round(br_bpm*k, 1) for k in range(3, 6)]}")
    print(f" 9. Technik-Vibration  : {f'{vib_hz:.2f} Hz ({vib_hz*60:.0f} RPM)' if vib_hz > 0 else 'keine Maschinen-Vibration (> 2,5 Hz)'}")
    print(f"10. Echo-Güte & Temp.  : {summary['primary_strength_db']:+.2f} dB (normiert @ 1 m: {rcs_norm_db:+.1f} dB), Chip-Temp. {summary['temperature_c']} °C")
    print("=" * 72)

    # 5-Panel Dashboard-Plot
    fig, ax = plt.subplots(5, 1, figsize=(12, 13))
    w_s_u = np.interp(tu, t, wins[:, 0])
    w_e_u = np.interp(tu, t, wins[:, 1])
    ax[0].fill_between(tu - tu[0], w_s_u, w_e_u, color="tab:cyan", alpha=0.15, label="Aktives A121-Hardware-Fenster (Auto-Zoom)")
    ax[0].plot(t - t[0], d_mat[:, 0], ".", ms=2, color="0.6", alpha=0.5, label="Roh-Peaks")
    ax[0].plot(tu - tu[0], dt_u, lw=1.5, color="tab:blue", label=f"Verfolgtes Hauptziel (~{summary['primary_distance_mm']:.0f} mm)")
    ax[0].axvspan(tu[qa] - tu[0], tu[qb - 1] - tu[0], color="y", alpha=0.2, label="Ruhigstes Vital-Fenster")
    ax[0].set_ylabel("Abstand (mm)")
    ax[0].set_title(f"1. Auto-Search -> Auto-Lock -> Auto-Zoom ({fs_zoom:.1f} Hz im Zoom) | {summary['target_classification']}")
    ax[0].legend(loc="upper right")
    ax[0].grid(True, alpha=0.3)

    ax[1].plot(tu - tu[0], stab_t, lw=1.5, color="tab:green", label=f"Stabilitäts-Score (Mittel im Fenster: {stab_mean:.0f} %)")
    ax[1].plot(tu - tu[0], np.clip(np.abs(vel_mm_s), 0, 100), lw=1.0, color="tab:red", alpha=0.7, label="|Geschwindigkeit| (mm/s)")
    ax[1].axvspan(tu[qa] - tu[0], tu[qb - 1] - tu[0], color="y", alpha=0.2)
    ax[1].set_ylim(-5, 105)
    ax[1].set_ylabel("%  /  mm/s")
    ax[1].set_title("2. Mobiler Hand-/Ziel-Stabilitäts-Score & Relativgeschwindigkeit")
    ax[1].legend(loc="upper right")
    ax[1].grid(True, alpha=0.3)

    ax[2].plot(tu - tu[0], dint_u - np.mean(dint_u), lw=0.9, color="tab:blue", alpha=0.7, label="Integrierte Feinbewegung Δd (mm)")
    ax[2].plot(tu - tu[0], br_full, lw=2.0, color="tab:orange", label=f"Atmung ({br_bpm:.1f} /min, Hub {br_amp_mm:.1f} mm)")
    ax[2].axvspan(tu[qa] - tu[0], tu[qb - 1] - tu[0], color="y", alpha=0.2)
    ax[2].set_ylabel("mm")
    ax[2].set_title("3. Kontinuierliche Brustbewegung & Atmung")
    ax[2].legend(loc="upper right")
    ax[2].grid(True, alpha=0.3)

    ax[3].plot(tu - tu[0], hr_d_full, lw=0.9, color="tab:blue", label="Pulsband Weg Δd nach Hilbert-Projektion (mm)")
    ax[3].plot(tu - tu[0], hr_s_full * 0.5, lw=0.9, color="tab:green", label="Pulsband Echo-Stärke (0,5× dB)")
    ax[3].axvspan(tu[qa] - tu[0], tu[qb - 1] - tu[0], color="y", alpha=0.2)
    ylim_h = max(0.25, float(np.percentile(np.abs(np.concatenate([hr_d, hr_s * 0.5])), 99) * 1.45))
    ax[3].set_ylim(-ylim_h, ylim_h)
    ax[3].set_ylabel("Amplitude")
    ax[3].set_title("4. Pulsband-Zeitverlauf (0,85–2,00 Hz)")
    ax[3].legend(loc="upper right")
    ax[3].grid(True, alpha=0.3)

    ax[4].plot(fh[m] * 60, pd_n, lw=1.2, alpha=0.55, color="tab:blue", label=f"Track Δd ({fh[m][np.argmax(pd[m])]*60:.1f} /min)")
    ax[4].plot(fh[m] * 60, ps_n, lw=1.2, alpha=0.55, color="tab:green", label=f"Track Stärke dB ({fh[m][np.argmax(ps[m])]*60:.1f} /min)")
    for k in range(3, 6):
        if 51 <= br_bpm * k <= 120:
            ax[4].axvline(br_bpm * k, color="gray", ls="--", alpha=0.6, label=f"{k}× Atmung" if k == 3 else None)
    ax[4].plot(
        fh[m] * 60,
        p_fused,
        lw=2.4,
        color="red",
        label=f"Fusionierter Puls-Kandidat: {hr_best:.1f} /min (Konfidenz {hr_conf:.0f} %)",
    )
    ax[4].set_xlabel("Frequenz (/min)")
    ax[4].set_ylabel("Normierte Leistung")
    ax[4].set_title("5. Multi-Feature Spektralfusion nach Hilbert-Atmungsoberwellen-Projektion")
    ax[4].legend(loc="upper right")
    ax[4].grid(True, alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_prefix + ".png", dpi=110)
    with open(out_prefix + "_summary.json", "w", encoding="utf-8") as f_json:
        json.dump(summary, f_json, indent=2, ensure_ascii=False)
    print(f"Plot gespeichert   : {out_prefix}.png")
    print(f"JSON-Report        : {out_prefix}_summary.json")
    return summary


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "--offline":
        in_npz, out_pref = sys.argv[2], sys.argv[3]
        z = np.load(in_npz, allow_pickle=True)
        z_ev = json.loads(str(z["zoom_events_json"])) if "zoom_events_json" in z else []
        analyze_auto_capture(
            z["t"],
            z["states"],
            z["wins"],
            z["profs"],
            z["temps"],
            z["n_peaks"],
            z["distances_mm"],
            z["strengths_db"],
            z_ev,
            out_pref,
        )
        sys.exit(0)

    if len(sys.argv) < 3:
        sys.exit(__doc__)

    port_arg = sys.argv[1]
    out_arg = sys.argv[2]
    dur_arg = int(sys.argv[3]) if len(sys.argv) > 3 else 38

    t_a, st_a, win_a, pr_a, tmp_a, n_a, d_a, s_a, z_evs = capture_auto_stream(port_arg, dur_arg)
    np.savez_compressed(
        out_arg + ".npz",
        t=t_a,
        states=st_a,
        wins=win_a,
        profs=pr_a,
        temps=tmp_a,
        n_peaks=n_a,
        distances_mm=d_a,
        strengths_db=s_a,
        zoom_events_json=json.dumps(z_evs),
    )
    print(f"Rohdaten gespeichert: {out_arg}.npz")
    analyze_auto_capture(t_a, st_a, win_a, pr_a, tmp_a, n_a, d_a, s_a, z_evs, out_arg)
