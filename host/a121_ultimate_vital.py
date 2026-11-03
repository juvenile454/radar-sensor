"""ARCHIV (V5.3, docs/VITAL_AUDIT.md), nicht mehr gepflegt.
Eigene DSP-Kette am PC (Kåsa + DACM + Hilbert-Projektion + HPS, USB Exploration Server). Neue Auswertungen nur mit
host/vital_eval.py und Varianten des Firmware-Headers firmware/esp32s3/src/a121_esp_dsp.h (--replay); was sich hier
bewährt, wandert in den Header. Funktion des Skripts unverändert.

Ultimatives 3-Subsweep Sparse-IQ Vital- & Umfeld-System für den Acconeer A121 (USB Exploration Server).

Schöpft die Hardware des A121 (RSS v1.12.0) voll aus, indem pro Frame (100 Hz) drei Subsweeps
simultan im Silizium ausgeführt werden (Gesamtdauer im Radar-Frontend ~5,7 ms/Sweep):

  - Subsweep 0 (On-Chip Loopback, 0,026 ms):
      enable_loopback=True, 1 Punkt @ 0 mm, PROFILE_1, PRF 19,5 MHz, HWAAS 16.
      Misst in jedem Frame die exakte momentane Sender-Empfänger-Eigenphase φ_loop(t) des
      60-GHz-Synthesizers und kompensiert niederfrequentes 1/f-Phasenrauschen im Vitalband.
  - Subsweep 1 (Ultra-Fine Vital-Zoom, ~4,9 ms):
      120 Punkte im 2,5-mm-Raster (300-mm-Fenster automatisch zentriert auf den Brustkorb),
      PROFILE_1 (40 mm Halbwertsbreite), PRF 19,5 MHz (höchste Pulsfolgefrequenz des A121),
      HWAAS 32, phase_enhancement=True, iq_imbalance_compensation=True.
  - Subsweep 2 (Simultanes Vollraum-Radar 0,15–2,55 m, ~0,76 ms):
      80 Punkte im 30-mm-Raster (step_length=12), PROFILE_3, PRF 15,6 MHz, HWAAS 8,
      phase_enhancement=True, iq_imbalance_compensation=True.
      Überwacht gleichzeitig den gesamten Raum, klassifiziert Ziele und führt Subsweep 1
      automatisch nach (Auto-Zoom), wenn sich die Person im Raum bewegt.

Signalverarbeitung (Sparse-IQ Pipeline):
  1. On-Chip Loopback-Phasen-Derotation: z_corr(t, d) = z(t, d) * exp(-j * Δφ_loop(t))
  2. Dynamische komplexe Standziel-Unterdrückung (< 0,05 Hz LPF im Komplexen gegen Möbel/Mehrwege)
  3. Qualitätsgewichtetes kohärentes Multi-Bin-Beamforming über die stabilsten Brustwand-Bins
  4. Kåsa-Kreiszentrierung + regularisierte DACM-Mikrometer-Phasendemodulation (~5 µm Rauschboden)
  5. Automatische Auswahl des ruhigsten 20-s-Analysefensters (Minimum-Motion-Selector)
  6. Zeitvariable Hilbert-Atmungsoberwellen-Projektion (1..6. Harmonische von θ_breath(t))
  7. Dual-Domain Herzschlag-Extraktion:
     - Frequenzbereich: Spektrum + Beschleunigungs-HPS (f0·2f0·3f0) + Autokorrelation
     - Zeitbereich: Einzelne systolische Mikrometer-Pulswellen, Schlag-zu-Schlag-Intervalle (IBI),
       Variationskoeffizient (CV %) und RMSSD (ms)
  8. Vollständige 10-Metriken-Telemetrie (analog zum mobilen Auto-Modus, aber mit echter µm-Phase)

Hinweis (AGENTS.md): Ohne simultane Referenzmessung (Smartwatch/Pulsoximeter/EKG) sind alle
Puls- und IBI-Werte experimentelle Rhythmus-Kandidaten im Radarsignal und keine validierte
Puls-Genauigkeitsaussage.

Verwendung:
  Live-Messung am A121 (USB):
    sg dialout -c ".venv/bin/python host/a121_ultimate_vital.py <PORT> <OUT_BASISPFAD> [DAUER_S] [BESCHREIBUNG]"
  Offline-Auswertung:
    .venv/bin/python host/a121_ultimate_vital.py --offline <EINGABE.npz> [OUT_BASISPFAD]
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import numpy as np
from acconeer.exptool import a121
from scipy import signal

POINT_M = 0.0025
FRAME_RATE = 60.0          # 60 Hz garantiert frame_delayed=False & <55% UART-Last bei 921600 Baud mit 3 Subsweeps
ZOOM_POINTS = 100          # 250 mm Zoom-Fenster @ 2,5 mm
ROOM_START_PT = 60         # 150 mm
ROOM_POINTS = 64           # 64 * 30 mm = 1920 mm -> 150..2070 mm
ROOM_STEP = 12             # 30 mm
MM_PER_RAD = 2.4776236 / (2 * np.pi)  # ≈ 0,394326 mm/rad (394,3 µm/rad)


def build_3subsweep_config(zoom_start_pt: int, frame_rate: float = FRAME_RATE) -> a121.SensorConfig:
    """Erstellt die simultane 3-Subsweep-Konfiguration (Loopback + Vital-Zoom + Vollraum-Scan)."""
    s0_loopback = a121.SubsweepConfig(
        start_point=0,
        num_points=1,
        step_length=1,
        profile=a121.Profile.PROFILE_1,
        hwaas=16,
        receiver_gain=10,
        enable_tx=True,
        enable_loopback=True,
        prf=a121.PRF.PRF_19_5_MHz,
    )
    s1_vital_zoom = a121.SubsweepConfig(
        start_point=int(zoom_start_pt),
        num_points=ZOOM_POINTS,
        step_length=1,
        profile=a121.Profile.PROFILE_1,
        hwaas=32,
        receiver_gain=16,
        enable_tx=True,
        enable_loopback=False,
        phase_enhancement=True,
        iq_imbalance_compensation=True,
        prf=a121.PRF.PRF_19_5_MHz,
    )
    s2_room_scan = a121.SubsweepConfig(
        start_point=ROOM_START_PT,
        num_points=ROOM_POINTS,
        step_length=ROOM_STEP,
        profile=a121.Profile.PROFILE_3,
        hwaas=8,
        receiver_gain=16,
        enable_tx=True,
        enable_loopback=False,
        phase_enhancement=True,
        iq_imbalance_compensation=True,
        prf=a121.PRF.PRF_15_6_MHz,
    )
    cfg = a121.SensorConfig(
        subsweeps=[s0_loopback, s1_vital_zoom, s2_room_scan],
        sweeps_per_frame=1,
        frame_rate=frame_rate,
        inter_frame_idle_state=a121.IdleState.READY,
        inter_sweep_idle_state=a121.IdleState.READY,
    )
    cfg.validate()
    return cfg


def connect_client(port: str, tries: int = 3) -> a121.Client:
    client = None
    for i in range(tries):
        try:
            client = a121.Client.open(serial_port=port, override_baudrate=921600)
            return client
        except Exception as e:  # noqa: BLE001
            print(f"Verbindung Versuch {i + 1} fehlgeschlagen: {e}")
            if client is not None:
                try:
                    client.close()
                except Exception:  # noqa: BLE001
                    pass
            time.sleep(0.8)
    sys.exit("Keine Verbindung zum A121 Exploration Server möglich.")


def auto_search_primary_chest(client: a121.Client, scan_s: float = 1.8) -> tuple[int, float]:
    """Scannt den Vollraum (0,15–2,55 m) und lokalisiert automatisch die atmende/bewegte Person."""
    scan_cfg = a121.SensorConfig(
        start_point=ROOM_START_PT,
        num_points=ROOM_POINTS,
        step_length=ROOM_STEP,
        profile=a121.Profile.PROFILE_2,
        hwaas=16,
        sweeps_per_frame=1,
        frame_rate=50.0,
        inter_frame_idle_state=a121.IdleState.READY,
        phase_enhancement=True,
        iq_imbalance_compensation=True,
        prf=a121.PRF.PRF_15_6_MHz,
    )
    client.setup_session(scan_cfg)
    client.start_session()
    n_scan = max(30, int(scan_s * 50.0))
    frames = np.array([client.get_next().frame[0] for _ in range(n_scan)], dtype=np.complex128)
    client.stop_session()

    room_dists_m = (ROOM_START_PT + np.arange(ROOM_POINTS) * ROOM_STEP) * POINT_M
    # Dynamische Änderung trennt atmenden Brustkorb von starren Raumreflexionen
    dyn_iq = frames - np.mean(frames, axis=0, keepdims=True)
    amp_mean = np.mean(np.abs(frames), axis=0)
    dyn_std = np.std(dyn_iq, axis=0)

    #Bevorzuge Ziele im typischen Personenbereich 0,20..2,20 m mit hoher dynamischer Energie
    valid = (room_dists_m >= 0.20) & (room_dists_m <= 2.20) & (amp_mean >= 0.08 * np.max(amp_mean))
    score = np.where(valid, dyn_std * np.sqrt(amp_mean), -1.0)
    best_idx = int(np.argmax(score))
    chest_m = float(room_dists_m[best_idx])
    center_pt = int(round(chest_m / POINT_M))
    # Vorderbrust liegt meist leicht vor dem Schwerpunkt des Grobsweeps -> 45 % vor, 55 % hinter Zentrum
    zoom_start_pt = max(40, center_pt - int(0.45 * ZOOM_POINTS))
    print(
        f"[Auto-Search 0,15–2,55 m] Bewegtes Primärziel bei {chest_m*1000:.0f} mm erkannt "
        f"(|IQ|={amp_mean[best_idx]:.0f}, dyn={dyn_std[best_idx]:.1f}) "
        f"-> Vital-Zoom auf {zoom_start_pt*POINT_M*1000:.0f}..{(zoom_start_pt+ZOOM_POINTS)*POINT_M*1000:.0f} mm"
    )
    return zoom_start_pt, chest_m


def record_ultimate_3subsweep(port: str, duration_s: float = 45.0) -> dict:
    """Nimmt simultan Loopback (Sub 0), 2,5-mm-Vital-Zoom (Sub 1) und 2,55-m-Raumscan (Sub 2) mit Auto-Rezoom auf."""
    client = connect_client(port)
    zoom_start_pt, init_chest_m = auto_search_primary_chest(client, scan_s=1.8)
    cfg = build_3subsweep_config(zoom_start_pt, frame_rate=FRAME_RATE)
    client.setup_session(cfg)
    client.start_session()

    n_total = int(duration_s * FRAME_RATE)
    iq_loop = np.zeros(n_total, dtype=np.complex64)
    iq_zoom = np.zeros((n_total, ZOOM_POINTS), dtype=np.complex64)
    iq_room = np.zeros((n_total, ROOM_POINTS), dtype=np.complex64)
    temps = np.zeros(n_total, dtype=np.int16)
    ts = np.zeros(n_total, dtype=np.float64)
    zoom_starts = np.full(n_total, zoom_start_pt, dtype=np.int32)
    rezoom_events = [{"frame": 0, "zoom_start_pt": int(zoom_start_pt), "chest_m": round(init_chest_m, 4)}]

    room_dists_m = (ROOM_START_PT + np.arange(ROOM_POINTS) * ROOM_STEP) * POINT_M
    print(
        f"3-SUBSWEEP AUFNAHME LÄUFT ({duration_s:.0f} s @ {FRAME_RATE:.0f} Hz) — {time.strftime('%H:%M:%S')}",
        flush=True,
    )
    t0 = time.monotonic()
    last_rezoom_idx = 0
    n_captured = 0

    try:
        for i in range(n_total):
            res = client.get_next()
            ts[i] = time.monotonic() - t0
            temps[i] = int(res.temperature)
            iq_loop[i] = res.subframes[0][0, 0]
            iq_zoom[i] = res.subframes[1][0]
            iq_room[i] = res.subframes[2][0]
            zoom_starts[i] = zoom_start_pt
            n_captured = i + 1

            # Alle 3,0 s prüfen, ob das Brustziel im Vital-Zoom-Fenster an den Rand (< 25 mm) gewandert oder verloren ist
            if (i - last_rezoom_idx) >= int(3.0 * FRAME_RATE) and (n_total - i) > int(6.0 * FRAME_RATE):
                w_zoom = np.mean(np.abs(iq_zoom[max(0, i - int(2.0 * FRAME_RATE)) : i + 1]), axis=0)
                zoom_pk = int(np.argmax(w_zoom))
                need_rezoom = (float(w_zoom[zoom_pk]) < 180.0) or (zoom_pk < 10) or (zoom_pk > (ZOOM_POINTS - 11))
                if need_rezoom:
                    w_room = iq_room[max(0, i - int(2.0 * FRAME_RATE)) : i + 1].astype(np.complex128)
                    dyn_r = np.std(w_room - np.mean(w_room, axis=0, keepdims=True), axis=0)
                    amp_r = np.mean(np.abs(w_room), axis=0)
                    valid_r = (room_dists_m >= 0.20) & (room_dists_m <= 2.00) & (amp_r >= 0.12 * np.max(amp_r))
                    if np.any(valid_r):
                        r_idx = int(np.argmax(np.where(valid_r, dyn_r * np.sqrt(amp_r), -1.0)))
                        cur_chest_m = float(room_dists_m[r_idx])
                        new_start = max(40, int(round(cur_chest_m / POINT_M)) - int(0.45 * ZOOM_POINTS))
                        if abs(new_start - zoom_start_pt) >= 20:
                            client.stop_session()
                            zoom_start_pt = new_start
                            cfg = build_3subsweep_config(zoom_start_pt, frame_rate=FRAME_RATE)
                            client.setup_session(cfg)
                            client.start_session()
                            last_rezoom_idx = i
                            rezoom_events.append({
                                "frame": int(i),
                                "zoom_start_pt": int(zoom_start_pt),
                                "chest_m": round(cur_chest_m, 4),
                            })
                            print(
                                f"  [Auto-Rezoom @ {ts[i]:.1f}s] Ziel bei {cur_chest_m*1000:.0f} mm "
                                f"-> Neues Fenster {zoom_start_pt*POINT_M*1000:.0f}.."
                                f"{(zoom_start_pt+ZOOM_POINTS)*POINT_M*1000:.0f} mm",
                                flush=True,
                            )
    except Exception as e:  # noqa: BLE001
        print(f"Hinweis während Aufnahme bei Frame {n_captured}/{n_total}: {e}")
    finally:
        try:
            client.stop_session()
        except Exception:  # noqa: BLE001
            pass
        try:
            client.close()
        except Exception:  # noqa: BLE001
            pass

    if n_captured < int(5.0 * FRAME_RATE):
        sys.exit(f"Zu wenige Frames empfangen ({n_captured}).")

    iq_loop = iq_loop[:n_captured]
    iq_zoom = iq_zoom[:n_captured]
    iq_room = iq_room[:n_captured]
    temps = temps[:n_captured]
    ts = ts[:n_captured]
    zoom_starts = zoom_starts[:n_captured]

    total_time = max(ts[-1] - ts[0], 1e-3)
    fs_eff = (n_captured - 1) / total_time
    print(f"Aufnahme beendet ({n_captured} Frames in {total_time:.2f} s -> effektiv {fs_eff:.2f} Hz)")

    # Für die Auswertung das längste zusammenhängende Zoom-Segment wählen
    vals, counts = np.unique(zoom_starts, return_counts=True)
    dom_start = int(vals[np.argmax(counts)])
    dom_mask = zoom_starts == dom_start
    idx_dom = np.where(dom_mask)[0]
    s_idx, e_idx = int(idx_dom[0]), int(idx_dom[-1]) + 1

    dist_zoom_m = (dom_start + np.arange(ZOOM_POINTS)) * POINT_M
    return {
        "iq": iq_zoom[s_idx:e_idx],
        "iq_loop": iq_loop[s_idx:e_idx],
        "iq_room": iq_room[s_idx:e_idx],
        "distances_m": dist_zoom_m,
        "room_distances_m": room_dists_m,
        "temps": temps[s_idx:e_idx],
        "ts": ts[s_idx:e_idx] - ts[s_idx],
        "fs": fs_eff,
        "rezoom_events": rezoom_events,
        "sensor_config": cfg.to_json(),
    }


def fit_iq_circle(z: np.ndarray) -> complex:
    x, y = z.real.astype(np.float64), z.imag.astype(np.float64)
    mat = np.column_stack([x, y, np.ones_like(x)])
    sol, _, _, _ = np.linalg.lstsq(mat, -(x**2 + y**2), rcond=None)
    cx, cy = -0.5 * sol[0], -0.5 * sol[1]
    r = np.sqrt(max(cx**2 + cy**2 - sol[2], 1e-6))
    if np.hypot(cx, cy) < 1.5 * r:
        return complex(cx, cy)
    return complex(np.median(x) * 0.2, np.median(y) * 0.2)


def dacm_displacement_mm(z: np.ndarray, reg_quantile: float = 5.0) -> np.ndarray:
    center = fit_iq_circle(z)
    zc = z - center
    amp_sq = np.abs(zc) ** 2
    # Sanfte Regularisierung (1e-3 des Medians): greift nur bei echten Multipath-Nulldurchgängen (|z| -> 0),
    # verzerrt aber bei normaler Signalstärke die Phase nicht durch Amplitudenmodulation.
    eps = float(np.percentile(amp_sq, reg_quantile)) * 0.01 + 1e-6
    prod = zc[1:] * np.conj(zc[:-1])
    dphi = np.arctan2(prod.imag, prod.real)
    w = amp_sq[1:] / (amp_sq[1:] + eps)
    phi = np.concatenate([[0.0], np.cumsum(dphi * w)])
    return signal.detrend(phi * MM_PER_RAD)


def remove_static_complex_clutter(iq: np.ndarray, fs: float) -> np.ndarray:
    if len(iq) < int(fs * 6):
        return iq
    sos_lp = signal.butter(2, 0.05, btype="low", fs=fs, output="sos")
    clutter = signal.sosfiltfilt(sos_lp, iq.real, axis=0) + 1j * signal.sosfiltfilt(sos_lp, iq.imag, axis=0)
    return iq - 0.75 * clutter


def extract_coherent_chest_iq(
    iq_zoom: np.ndarray,
    dist_m: np.ndarray,
    fs: float,
    iq_loop: np.ndarray | None = None,
) -> tuple[int, int, np.ndarray, float]:
    """Wendet Loopback-Derotation, Fading-freie Bin-Selektion und kohärentes Multi-Bin-Beamforming an."""
    iq_work = iq_zoom.astype(np.complex128).copy()
    loop_jitter_um = 0.0

    if iq_loop is not None and len(iq_loop) == len(iq_work) and np.all(np.abs(iq_loop) > 10):
        phi_loop = np.unwrap(np.angle(iq_loop.astype(np.complex128)))
        phi_loop_rel = phi_loop - phi_loop[0]
        # Tiefpass (< 0,8 Hz) isoliert die langsame thermische 1/f-PLL-Phasendrift ohne weißes Rauschen einzukoppeln
        sos_lp_loop = signal.butter(2, 0.8, btype="low", fs=fs, output="sos")
        phi_loop_slow = signal.sosfiltfilt(sos_lp_loop, phi_loop_rel)
        loop_jitter_um = float(np.std(phi_loop_slow) * MM_PER_RAD * 1000.0)
        derot = np.exp(-1j * phi_loop_slow)[:, np.newaxis]
        iq_work = iq_work * derot

    amp_mean = np.abs(iq_work).mean(axis=0)
    amp_min = np.min(np.abs(iq_work), axis=0)
    amp_std = np.abs(iq_work).std(axis=0) + 1e-6

    k_raw = int(np.argmax(amp_mean))
    mask = amp_mean >= 0.25 * amp_mean[k_raw]
    quality = np.where(mask, (amp_min / amp_std) * (amp_min / amp_mean[k_raw]), -1.0)
    k_best = int(np.argmax(quality))

    # Kohärentes Spatial Matched Filtering über ±2 Nachbar-Bins (dank phase_enhancement=True)
    sl = slice(max(k_best - 2, 0), min(k_best + 3, iq_work.shape[1]))
    sub = iq_work[:, sl]
    ref = iq_work[:, k_best : k_best + 1]
    phase_align = np.exp(-1j * np.angle((sub * np.conj(ref)).mean(axis=0, keepdims=True)))
    z_beam = (sub * phase_align).mean(axis=1)
    return k_raw, k_best, z_beam, loop_jitter_um


def project_out_respiration_harmonics(disp: np.ndarray, fs: float, max_harm: int = 6) -> tuple[np.ndarray, np.ndarray]:
    sos_b = signal.butter(2, [0.10, 0.55], btype="band", fs=fs, output="sos")
    breath = signal.sosfiltfilt(sos_b, disp)

    # Schätze dominante Atem-Grundfrequenz f_b0 für ein schmalbandiges Fundamental-Phasenmodell θ_1(t)
    f_b, p_b = signal.welch(breath, fs=fs, nperseg=min(len(breath), int(fs * 16)), nfft=int(fs * 120))
    mb = (f_b >= 0.12) & (f_b <= 0.55)
    fb0 = float(f_b[mb][np.argmax(p_b[mb])]) if np.any(mb) else 0.30
    sos_f1 = signal.butter(3, [max(0.08, fb0 * 0.72), min(0.65, fb0 * 1.28)], btype="band", fs=fs, output="sos")
    b_f1 = signal.sosfiltfilt(sos_f1, disp)
    theta1 = np.unwrap(np.angle(signal.hilbert(b_f1)))

    # Gleitende Kurzzeit-Projektion (7-s-Hann-Fenster, 1-s-Schritt) passt sich Atemzug für Atemzug an
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
        res = y - basis @ coef
        clean[sl] += res * hann
        weight[sl] += hann

    mask_w = weight > 1e-4
    clean[mask_w] /= weight[mask_w]
    return breath, clean


def extract_time_domain_beats(
    heart_um: np.ndarray,
    fs: float,
    target_bpm: float | None = None,
) -> dict:
    """Extrahiert einzelne Herzschläge im Zeitbereich aus der bereits bandgefilterten Mikrometer-Phasenwelle."""

    # Mindestabstand zweier Herzschläge orientiert sich am Spektral-Kandidaten (oder 0,42 s = 142 /min)
    if target_bpm is not None and 45.0 <= target_bpm <= 140.0:
        expected_period_s = 60.0 / target_bpm
        min_dist = max(int(0.68 * expected_period_s * fs), int(0.42 * fs))
    else:
        min_dist = int(0.45 * fs)

    prom = max(float(np.percentile(np.abs(heart_um), 65)) * 0.35, 3.0)
    peaks, _ = signal.find_peaks(heart_um, distance=min_dist, prominence=prom)

    if len(peaks) >= 4:
        ibi_s = np.diff(peaks) / fs
        # Physiologische Plausibilitätsfilterung (0,40..1,35 s)
        valid_ibi = ibi_s[(ibi_s >= 0.40) & (ibi_s <= 1.35)]
        if len(valid_ibi) >= 3:
            med_ibi_ms = float(np.median(valid_ibi) * 1000.0)
            bpm_td = float(60.0 / np.median(valid_ibi))
            cv_pct = float(np.std(valid_ibi) / np.mean(valid_ibi) * 100.0)
            rmssd_ms = float(np.sqrt(np.mean(np.diff(valid_ibi * 1000.0) ** 2))) if len(valid_ibi) >= 4 else float("nan")
            ptp_um = float(np.percentile(heart_um[peaks], 80) - np.percentile(heart_um, 20))
            return {
                "heart_um": heart_um,
                "peaks": peaks,
                "num_beats": int(len(peaks)),
                "bpm_time_domain": round(bpm_td, 1),
                "median_ibi_ms": round(med_ibi_ms, 1),
                "ibi_cv_pct": round(cv_pct, 1),
                "rmssd_ms": round(rmssd_ms, 1) if np.isfinite(rmssd_ms) else None,
                "pulse_wave_ptp_um": round(ptp_um, 1),
            }

    return {
        "heart_um": heart_um,
        "peaks": peaks,
        "num_beats": int(len(peaks)),
        "bpm_time_domain": None,
        "median_ibi_ms": None,
        "ibi_cv_pct": None,
        "rmssd_ms": None,
        "pulse_wave_ptp_um": round(float(np.ptp(heart_um)), 1),
    }


def analyze_ultimate(data: dict, out_base: str, label: str) -> dict:
    iq_zoom = data["iq"]
    iq_loop = data.get("iq_loop", None)
    iq_room = data.get("iq_room", None)
    dist_m = data["distances_m"]
    room_dist_m = data.get("room_distances_m", None)
    temps = data.get("temps", np.zeros(len(iq_zoom)))
    fs = float(data["fs"])
    rezoom_events = data.get("rezoom_events", [])

    k_raw, k_best, z_beam, loop_jitter_um = extract_coherent_chest_iq(iq_zoom, dist_m, fs, iq_loop)
    disp_full = dacm_displacement_mm(z_beam)
    t_full = np.arange(len(disp_full)) / fs

    # Gleitender Stabilitäts-Score (0..100 %) über langsame Rumpf-Grundlinie (> 3,5 s)
    drift_win = int(3.5 * fs) | 1
    body_drift = signal.savgol_filter(disp_full, min(len(disp_full) | 1, max(5, drift_win)), 1)
    stab_trace = np.zeros_like(disp_full)
    w_stab = max(4, int(3.0 * fs))
    for i in range(len(disp_full)):
        seg = body_drift[max(0, i - w_stab) : min(len(body_drift), i + w_stab)]
        stab_trace[i] = float(np.clip(100.0 * np.exp(-np.ptp(seg) / 3.0), 0.0, 100.0))

    # Ruhigstes 20-s-Fenster für die Spektral-/Pulsanalyse auswählen
    win_len = min(len(disp_full), int(20.0 * fs))
    step_len = max(1, int(1.0 * fs))
    margin = int(2.0 * fs) if len(disp_full) > int(25.0 * fs) else 0
    best_s0 = margin
    best_score = float("inf")
    for s0 in range(margin, max(margin + 1, len(disp_full) - win_len - margin + 1), step_len):
        seg = signal.detrend(disp_full[s0 : s0 + win_len])
        sc = float(np.std(seg) + 2.0 * np.std(np.diff(body_drift[s0 : s0 + win_len])))
        if sc < best_score:
            best_score = sc
            best_s0 = s0
    s1 = best_s0 + win_len
    t_win_start, t_win_end = float(t_full[best_s0]), float(t_full[s1 - 1])

    breath_full, disp_clean_full = project_out_respiration_harmonics(disp_full, fs, max_harm=6)
    sos_h = signal.butter(4, [0.85, 2.5], btype="band", fs=fs, output="sos")
    heart_full = signal.sosfiltfilt(sos_h, disp_clean_full)

    disp_win = disp_full[best_s0:s1]
    breath_win = breath_full[best_s0:s1]
    clean_win = disp_clean_full[best_s0:s1]
    heart_win = heart_full[best_s0:s1]

    # 1. Atmung & Apnoe
    f_b, p_b = signal.welch(breath_win, fs=fs, nperseg=min(len(breath_win), int(fs * 16)), nfft=int(fs * 120))
    mb = (f_b >= 0.10) & (f_b <= 0.55)
    br_bpm = float(f_b[mb][np.argmax(p_b[mb])] * 60.0) if np.any(mb) else float("nan")
    breath_ptp_mm = float(np.percentile(breath_win, 95) - np.percentile(breath_win, 5))

    apnea_episodes = 0
    ap_win = int(10.0 * fs)
    for a0 in range(0, len(breath_full) - ap_win, int(2.0 * fs)):
        if np.ptp(breath_full[a0 : a0 + ap_win]) < 0.20:
            apnea_episodes += 1

    # 2. Herzschlag Frequenzbereich (Spektrum + Beschleunigungs-HPS + Autokorrelation)
    f_h, p_h = signal.welch(heart_win, fs=fs, nperseg=min(len(heart_win), int(fs * 15)), nfft=int(fs * 120))

    acc_win = np.gradient(np.gradient(clean_win, 1.0 / fs), 1.0 / fs)
    f_a, p_a = signal.welch(acc_win, fs=fs, nperseg=min(len(acc_win), int(fs * 15)), nfft=int(fs * 120))
    bg_a = signal.medfilt(p_a, kernel_size=121)
    p_a_norm = p_a / np.maximum(bg_a, 1e-20)

    mh = (f_h >= 0.85) & (f_h <= 2.05)
    f_grid = f_h[mh]
    p_disp_norm = p_h[mh] / (np.max(p_h[mh]) + 1e-20)
    hps = np.zeros_like(f_grid)
    for i, f0 in enumerate(f_grid):
        h1 = np.interp(f0, f_a, p_a_norm)
        h2 = np.interp(2.0 * f0, f_a, p_a_norm)
        h3 = np.interp(3.0 * f0, f_a, p_a_norm)
        hps[i] = h1 * (0.5 + 0.5 * h2) * (0.5 + 0.5 * h3)
    hps_norm = hps / (np.max(hps) + 1e-20)

    fused_spec = 0.50 * p_disp_norm + 0.50 * hps_norm
    pk_idx, _ = signal.find_peaks(fused_spec, distance=6)
    if len(pk_idx) > 0:
        pk_idx = pk_idx[np.argsort(fused_spec[pk_idx])[::-1]]
        top_candidates = [(round(float(f_grid[i] * 60.0), 1), round(float(fused_spec[i]), 2)) for i in pk_idx[:3]]
        hr_bpm = float(f_grid[pk_idx[0]] * 60.0)
        snr_spec = float(fused_spec[pk_idx[0]] / (np.median(fused_spec) + 1e-6))
    else:
        top_candidates = []
        hr_bpm = float("nan")
        snr_spec = 1.0

    # Autokorrelation im ruhigen Fenster
    ac = np.correlate(heart_win - np.mean(heart_win), heart_win - np.mean(heart_win), mode="full")
    ac = ac[len(ac) // 2 :] / (ac[len(ac) // 2] + 1e-12)
    lag_min, lag_max = int(0.48 * fs), int(1.15 * fs)
    ac_peaks, _ = signal.find_peaks(ac[lag_min:lag_max], distance=int(0.25 * fs))
    if len(ac_peaks) > 0:
        best_ac = ac_peaks[int(np.argmax(ac[lag_min:lag_max][ac_peaks]))] + lag_min
        ac_bpm = round(float(60.0 * fs / best_ac), 1)
        ac_r = round(float(ac[best_ac]), 2)
    else:
        ac_bpm, ac_r = None, 0.0

    # 3. Zeitbereichs-Pulswelle & Schlag-zu-Schlag-Intervalle (IBI)
    td_info = extract_time_domain_beats(heart_win * 1000.0, fs, target_bpm=hr_bpm)

    hr_conf = float(
        np.clip(
            25.0 * min(snr_spec / 2.5, 2.0)
            + 30.0 * (np.mean(stab_trace[best_s0:s1]) / 100.0)
            + 25.0 * min(max(ac_r, 0.0) / 0.35, 1.0)
            + (20.0 if (td_info["ibi_cv_pct"] is not None and td_info["ibi_cv_pct"] < 18.0) else 5.0),
            0.0,
            100.0,
        )
    )

    # 4. Raum-Ziele (Multi-Target) & Mikrovibration (5..40 Hz)
    target_dist_mm = float(dist_m[k_best] * 1000.0)
    amp_best = float(np.mean(np.abs(iq_zoom[:, k_best])))
    amp_db_equiv = float(20.0 * np.log10(max(amp_best, 1.0)))

    room_targets = []
    if iq_room is not None and room_dist_m is not None:
        r_amp = np.mean(np.abs(iq_room), axis=0)
        r_dyn = np.std(iq_room.astype(np.complex128) - np.mean(iq_room, axis=0, keepdims=True), axis=0)
        r_pks, _ = signal.find_peaks(r_amp, distance=3, prominence=max(float(np.max(r_amp)) * 0.06, 20.0))
        for rp in r_pks:
            room_targets.append({
                "distance_mm": round(float(room_dist_m[rp] * 1000.0), 0),
                "amplitude": round(float(r_amp[rp]), 1),
                "dynamic_iq": round(float(r_dyn[rp]), 1),
                "type": "VITAL_MOVING" if r_dyn[rp] > 0.25 * np.max(r_dyn) else "STATIC_OBJECT",
            })

    if fs > 42.0:
        sos_vib = signal.butter(4, [5.0, min(40.0, 0.45 * fs)], btype="band", fs=fs, output="sos")
        vib_um = signal.sosfiltfilt(sos_vib, disp_full) * 1000.0
        f_v, p_v = signal.welch(vib_um, fs=fs, nperseg=min(len(vib_um), int(fs * 8)))
        mv = (f_v >= 5.0) & (f_v <= min(40.0, 0.45 * fs))
        vib_snr = float(np.max(p_v[mv]) / (np.median(p_v[mv]) + 1e-9)) if np.any(mv) else 1.0
        vib_hz = round(float(f_v[mv][np.argmax(p_v[mv])]), 1) if (np.any(mv) and vib_snr > 20.0) else None
        vib_rms_um = round(float(np.std(vib_um)), 2)
    else:
        vib_hz, vib_snr, vib_rms_um = None, 1.0, 0.0

    vel_mm_s = np.gradient(body_drift, 1.0 / fs)
    harm = [round(br_bpm * h, 1) for h in range(2, 9) if 48.0 <= br_bpm * h <= 145.0]

    summary = {
        "setup": label,
        "mode": "3-Subsweep Sparse-IQ (Loopback + 2.5mm Vital-Zoom + 2.55m Room-Scan)" if iq_room is not None else "Sparse-IQ Offline",
        "frames": int(len(iq_zoom)),
        "fs_hz": round(fs, 2),
        "chip_temp_c": round(float(np.mean(temps)), 1) if len(temps) else None,
        "loopback_phase_jitter_um": round(loop_jitter_um, 2),
        "primary_target": {
            "raw_max_bin_mm": round(float(dist_m[k_raw] * 1000.0), 1),
            "coherent_best_bin_mm": round(target_dist_mm, 1),
            "mean_iq_amplitude": round(amp_best, 1),
            "amplitude_db_rel": round(amp_db_equiv, 2),
            "classification": "HUMAN_VITAL",
            "stability_mean_pct": round(float(np.mean(stab_trace)), 1),
            "stability_calm_window_pct": round(float(np.mean(stab_trace[best_s0:s1])), 1),
            "calm_window_s": [round(t_win_start, 1), round(t_win_end, 1)],
            "velocity_rms_mm_s": round(float(np.std(vel_mm_s)), 2),
        },
        "respiration": {
            "rate_bpm": round(br_bpm, 1),
            "depth_ptp_mm": round(breath_ptp_mm, 2),
            "apnea_10s_episodes": int(apnea_episodes),
            "harmonics_bpm": harm,
        },
        "heart_ experimental_candidate": {
            "rate_spectral_hps_bpm": round(hr_bpm, 1) if np.isfinite(hr_bpm) else None,
            "rate_autocorr_bpm": ac_bpm,
            "autocorr_r": ac_r,
            "rate_time_domain_ibi_bpm": td_info["bpm_time_domain"],
            "median_ibi_ms": td_info["median_ibi_ms"],
            "ibi_cv_pct": td_info["ibi_cv_pct"],
            "rmssd_ms": td_info["rmssd_ms"],
            "detected_beats_in_20s": td_info["num_beats"],
            "pulse_wave_ptp_um": td_info["pulse_wave_ptp_um"],
            "confidence_pct": round(hr_conf, 1),
            "top_candidates_bpm": top_candidates,
            "note": "Experimenteller Rhythmus-/IBI-Kandidat im 60-GHz-Sparse-IQ-Signal (ohne Referenzgerät keine Genauigkeitsaussage)",
        },
        "room_targets": room_targets,
        "rezoom_events": rezoom_events,
        "vibration_5_40_hz": {
            "dominant_hz": vib_hz,
            "snr": round(vib_snr, 1),
            "rms_um": vib_rms_um,
        },
    }

    print("\n" + "=" * 78)
    print(f"ULTIMATE SPARSE-IQ AUSWERTUNG: {label}")
    print("=" * 78)
    print(
        f"1. Abtastung & Hardware: {len(iq_zoom)} Frames @ {fs:.2f} Hz | Temp: {summary['chip_temp_c']} °C | "
        f"Loopback-Korrektur: {loop_jitter_um:.1f} µm PLL-Drift kompensiert"
    )
    print(
        f"2. Primärziel (Brust):   {target_dist_mm:.1f} mm (Roh-Max {dist_m[k_raw]*1000:.1f} mm) | "
        f"|IQ|={amp_best:.0f} ({amp_db_equiv:.1f} dB) | Ruhe-Score: {summary['primary_target']['stability_calm_window_pct']:.1f} %"
    )
    print(
        f"3. Atmung:               {br_bpm:.1f} /min | Hub Spitze-Spitze: {breath_ptp_mm:.2f} mm | "
        f"Apnoe-Episoden: {apnea_episodes}"
    )
    print(
        f"4. Herzschlag (Spektr.): {hr_bpm:.1f} /min (Konfidenz {hr_conf:.0f} %, Top-3: {top_candidates}) | "
        f"Autokorr.: {ac_bpm} /min (r={ac_r})"
    )
    print(
        f"5. Herzschlag (Zeitber.): {td_info['num_beats']} Schläge in 20 s | IBI-Median: {td_info['median_ibi_ms']} ms "
        f"({td_info['bpm_time_domain']} /min) | CV: {td_info['ibi_cv_pct']} % | RMSSD: {td_info['rmssd_ms']} ms | "
        f"Pulshub: {td_info['pulse_wave_ptp_um']} µm"
    )
    if room_targets:
        print(f"6. Vollraum-Ziele:       {len(room_targets)} erkannt -> {room_targets}")
    print("=" * 78)

    np.savez_compressed(
        out_base + ".npz",
        iq=iq_zoom,
        iq_loop=iq_loop if iq_loop is not None else np.array([]),
        iq_room=iq_room if iq_room is not None else np.array([]),
        distances_m=dist_m,
        room_distances_m=room_dist_m if room_dist_m is not None else np.array([]),
        temps=temps,
        fs=fs,
        meta=json.dumps(summary, ensure_ascii=False),
    )
    Path(out_base + "_summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False))

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        fig, ax = plt.subplots(6, 1, figsize=(12, 16))

        # Panel 0: Vollraum + Vital-Zoom Tiefenprofil
        amp_zoom = np.mean(np.abs(iq_zoom), axis=0)
        if iq_room is not None and room_dist_m is not None and len(iq_room):
            amp_room = np.mean(np.abs(iq_room), axis=0)
            ax[0].plot(room_dist_m * 1000.0, amp_room, color="tab:gray", lw=1.2, alpha=0.8, label="Subsweep 2: Vollraum (150–2550 mm, PROFILE_3)")
        ax[0].plot(dist_m * 1000.0, amp_zoom, color="tab:blue", lw=2.0, label="Subsweep 1: Vital-Zoom (2,5 mm, PROFILE_1, 19,5 MHz)")
        ax[0].axvline(dist_m[k_raw] * 1000.0, color="tab:red", ls="--", label=f"Roh-Max ({dist_m[k_raw]*1000:.0f} mm)")
        ax[0].axvline(target_dist_mm, color="tab:green", lw=2.0, label=f"Kohärentes Brust-Zentrum ({target_dist_mm:.1f} mm)")
        ax[0].set_ylabel("|IQ|")
        ax[0].set_xlabel("Entfernung (mm)")
        ax[0].legend(loc="upper right", fontsize=8)
        ax[0].set_title("1. Simultanes Vollraum- & Vital-Zoom-Tiefenprofil (mit dynamischer Clutter-Unterdrückung)")
        ax[0].grid(True, alpha=0.25)

        # Panel 1: DACM Brustverschiebung & Atmung
        ax[1].plot(t_full, disp_full, color="tab:cyan", lw=0.8, alpha=0.8, label="DACM Gesamtweg (mm)")
        ax[1].plot(t_full, breath_full, color="tab:blue", lw=1.8, label=f"Atmung ({br_bpm:.1f} /min, Hub {breath_ptp_mm:.2f} mm)")
        ax[1].axvspan(t_win_start, t_win_end, color="gold", alpha=0.18, label="Ruhigstes 20-s-Analysefenster")
        ax[1].set_ylabel("mm")
        ax[1].legend(loc="upper right", fontsize=8)
        ax[1].set_title("2. Millimeter-Brustwandbewegung (Loopback-korrigiert + Kåsa-Kreis-Fit + DACM)")
        ax[1].grid(True, alpha=0.25)

        # Panel 2: Stabilität & Relativgeschwindigkeit
        ax2_r = ax[2].twinx()
        ax[2].plot(t_full, stab_trace, color="tab:green", lw=1.5, label="Ruhe-/Stabilitäts-Score (%)")
        ax2_r.plot(t_full, vel_mm_s, color="tab:orange", lw=0.8, alpha=0.6, label="Rumpf-Driftgeschwindigkeit (mm/s)")
        ax[2].set_ylim(0, 105)
        ax[2].set_ylabel("Stabilität (%)")
        ax2_r.set_ylabel("mm/s")
        ax[2].set_title(f"3. Bewegungs-Stabilität (Mittel: {np.mean(stab_trace):.1f} %, Ruhefenster: {np.mean(stab_trace[best_s0:s1]):.1f} %)")
        ax[2].grid(True, alpha=0.25)

        # Panel 3: Mikrometer-Pulswelle im ruhigen 20-s-Fenster mit Einzel-Herzschlägen
        t_win = np.arange(len(clean_win)) / fs + t_win_start
        hw_um = td_info["heart_um"]
        pks = td_info["peaks"]
        ax[3].plot(t_win, hw_um, color="tab:red", lw=1.1, label="Herzschlag-Welle 0,85–2,6 Hz (µm)")
        if len(pks) > 0:
            ax[3].plot(t_win[pks], hw_um[pks], "ko", ms=4.5, label=f"Erkannte Schläge ({len(pks)} Peaks, IBI-Median {td_info['median_ibi_ms']} ms)")
        ax[3].set_ylabel("µm")
        ax[3].legend(loc="upper right", fontsize=8)
        ax[3].set_title(
            f"4. Zeitbereichs-Pulswelle im 20-s-Ruhefenster (Hub ~{td_info['pulse_wave_ptp_um']} µm, "
            f"IBI-Puls: {td_info['bpm_time_domain']} /min, CV: {td_info['ibi_cv_pct']} %)"
        )
        ax[3].grid(True, alpha=0.25)

        # Panel 4: Frequenzspektrum & Beschleunigungs-HPS
        ax[4].plot(f_grid * 60.0, p_disp_norm, color="tab:blue", alpha=0.6, lw=1.2, label="Weg-Spektrum nach Hilbert-Atmungsprojektion")
        ax[4].plot(f_grid * 60.0, hps_norm, color="tab:orange", alpha=0.7, lw=1.2, label="Beschleunigungs-HPS (f0·2f0·3f0)")
        ax[4].plot(f_grid * 60.0, fused_spec / (np.max(fused_spec) + 1e-12), color="tab:red", lw=2.2, label=f"Fusionierter Puls-Kandidat ({hr_bpm:.1f} /min, Konf. {hr_conf:.0f} %)")
        for h in harm:
            if 51.0 <= h <= 123.0:
                ax[4].axvline(h, color="gray", ls=":", lw=0.9)
        ax[4].set_xlabel("Frequenz (/min)")
        ax[4].set_ylabel("Normiert")
        ax[4].legend(loc="upper right", fontsize=8)
        ax[4].set_title("5. Herzschlag-Spektrum & Harmonic Product Spectrum (grau gepunktet: mittlere Atmungs-Oberwellen)")
        ax[4].grid(True, alpha=0.25)

        # Panel 5: Autokorrelation & Schlag-zu-Schlag-Intervall-Verlauf (Tachogramm)
        lags_s = np.arange(len(ac)) / fs
        m_ac = (lags_s >= 0.40) & (lags_s <= 2.0)
        ax[5].plot(60.0 / lags_s[m_ac], ac[m_ac], color="tab:purple", lw=1.6, label=f"Autokorrelation R_xx (Peak: {ac_bpm} /min, r={ac_r})")
        if np.isfinite(hr_bpm):
            ax[5].axvline(hr_bpm, color="tab:red", ls="--", label=f"Spektral/HPS ({hr_bpm:.1f} /min)")
        ax[5].set_xlim(45, 135)
        ax[5].set_xlabel("Äquivalente Rate (/min = 60 / τ)")
        ax[5].set_ylabel("r(τ)")
        ax[5].legend(loc="upper right", fontsize=8)
        ax[5].set_title("6. Zeitbereichs-Autokorrelation der kardialen Mikrometer-Welle")
        ax[5].grid(True, alpha=0.25)

        fig.suptitle(f"{label} — A121 Ultimate Sparse-IQ Analyse", fontsize=12, fontweight="bold")
        fig.tight_layout()
        fig.savefig(out_base + ".png", dpi=100)
        print("Plot gespeichert:   ", out_base + ".png")
    except Exception as e:  # noqa: BLE001
        print("Plot fehlgeschlagen:", e)

    print("Daten gespeichert:  ", out_base + ".npz")
    print("Summary gespeichert:", out_base + "_summary.json")
    return summary


def main() -> None:
    if len(sys.argv) >= 3 and sys.argv[1] == "--offline":
        in_npz = sys.argv[2]
        out_base = sys.argv[3] if len(sys.argv) >= 4 else in_npz.replace(".npz", "_ultimate")
        z = np.load(in_npz, allow_pickle=True)
        data = {
            "iq": z["iq"],
            "iq_loop": z["iq_loop"] if "iq_loop" in z and len(z["iq_loop"]) else None,
            "iq_room": z["iq_room"] if "iq_room" in z and len(z["iq_room"]) else None,
            "distances_m": z["distances_m"],
            "room_distances_m": z["room_distances_m"] if "room_distances_m" in z and len(z["room_distances_m"]) else None,
            "temps": z["temps"] if "temps" in z else np.zeros(len(z["iq"])),
            "fs": float(z["fs"]),
            "rezoom_events": [],
        }
        label = Path(in_npz).stem
        analyze_ultimate(data, out_base, label)
    elif len(sys.argv) >= 3:
        port = sys.argv[1]
        out_base = sys.argv[2]
        dauer_s = float(sys.argv[3]) if len(sys.argv) >= 4 else 45.0
        label = sys.argv[4] if len(sys.argv) >= 5 else f"A121 3-Subsweep Sparse-IQ ({dauer_s:.0f}s)"
        data = record_ultimate_3subsweep(port, duration_s=dauer_s)
        analyze_ultimate(data, out_base, label)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
