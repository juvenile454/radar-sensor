#!/usr/bin/env python3
"""
a121_esp_ultimate.py — Live-Aufnahme, PC-Prüfung und Raumbild für `iq3` auf dem ESP32-S3
(3-Subsweep Sparse-IQ über den Exploration Server auf LPUART1, ESP32 GPIO15/16).

Modi:
  1. `--mode iq3`: startet `iq3 <sek> <start_mm>` am ESP32, speichert alle Zeilen als `.log` und
     erzeugt daraus `.npz` + `.png` (wie `--mode plot`). Port automatisch über /dev/serial/by-id.
       sg dialout -c ".venv/bin/python host/a121_esp_ultimate.py --mode iq3 --duration 60"

  2. `--mode verify-cpp`: übersetzt `firmware/esp32s3/src/a121_esp_dsp.h` für den PC und spielt eine
     `iq3raw_*.npz`-Aufnahme (105 Punkte je Frame, wie der ESP32 sie bekommt) Frame für Frame durch.
     Ausgabe sind dieselben Zeilen wie auf dem ESP32 (`@ROOMPROF`, `@ROOM`, `@VITAL`). Ein Rezoom
     wird nur als `@REZOOM_HINT` gemeldet, nicht ausgeführt (die Aufnahme hat ein festes Zoom-Fenster).
     `--zoom-start-mm` setzt das Fenster absichtlich woandershin, um die Zielsuche zu prüfen.
       .venv/bin/python host/a121_esp_ultimate.py --mode verify-cpp --out <ausgabe_ohne_endung>

  3. `--mode plot --log <datei.log>`: wertet ein Log aus (Live oder verify-cpp) → `.npz` + `.png`:
     Entfernungs-Zeit-Bild der Bewegung (0,30–2,67 m) mit Raum-Zielen und Zoom-Fenster,
     festes Echo-Profil des Raums und Atem-/Puls-Verlauf.

Zeitachse: `tick` (Sensor-Zeitstempel in ms) aus `@ROOMPROF`/`@ROOM`; er läuft auch über Rechenpausen
lückenlos. `@VITAL` bekommt die Zeit der `@ROOM`-Zeile desselben Reports.
Puls-Werte sind Rhythmus-Kandidaten; ohne Referenzmessung keine Aussage über Genauigkeit.
"""

from __future__ import annotations

import argparse
import datetime as dt
import glob
import pathlib
import subprocess
import sys
import tempfile
import time

import numpy as np


DATA_DIR = pathlib.Path("~/Data/radar-sensor").expanduser()
DEFAULT_NPZ = DATA_DIR / "iq3raw_normal_120s_2026-11-01.npz"
REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DSP_HEADER = REPO_ROOT / "firmware" / "esp32s3" / "src" / "a121_esp_dsp.h"

VITAL_FIELDS = ["t_ms", "br", "hr_fused", "hr_spec", "hr_acf", "hr_ibi", "ibi_ms", "rmssd_ms", "conf",
                "resp_um", "heart_um", "snr_db", "lb_std_mrad", "target_mm", "n_room"]


def find_esp32_port() -> str | None:
    matches = sorted(glob.glob("/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00"))
    return matches[0] if matches else None


def default_out(tag: str) -> pathlib.Path:
    return DATA_DIR / f"{dt.datetime.now():%Y-%m-%d_%H%M%S}_{tag}"


# ---------------------------------------------------------------------------------------------------------
# PC-Prüfung des DSP-Headers
# ---------------------------------------------------------------------------------------------------------

ARDUINO_SHIM = """#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static inline bool psramFound() { return true; }
static inline void *ps_malloc(size_t sz) { return malloc(sz); }
static inline unsigned long micros() { return 0; }
"""

# Gibt dieselben Zeilen aus wie cmdIq3() in main.cpp (ohne @IQ3 je Frame und @DSP).
CPP_HARNESS = r"""
#include <Arduino.h>
#include "a121_esp_dsp.h"
using namespace a121_dsp;

int main(int argc, char **argv) {
    if (argc < 4) return 1;
    FILE *f_iq = fopen(argv[1], "rb"), *f_tick = fopen(argv[2], "rb");
    const int32_t zoom_pt = atoi(argv[3]);
    if (!f_iq || !f_tick) return 2;
    UltimateEspDsp dsp;
    if (!dsp.begin(zoom_pt, 120, kRoomStep)) return 3;
    static char line[kRoomLineMax];
    printf("@IQ3_START,%ld,%ld,60.0\n", (long)(zoom_pt * 25 / 10), (long)((zoom_pt + kZoomPoints) * 25 / 10));
    Complex16 pts[kTotal3SubPts];
    uint32_t tick = 0, n = 0;
    while (fread(pts, sizeof(pts[0]), kTotal3SubPts, f_iq) == kTotal3SubPts && fread(&tick, 4, 1, f_tick) == 1) {
        float um = 0.0f;
        dsp.pushFrame(pts, kTotal3SubPts, 20, 0.0f, um);
        n++;
        const uint32_t t_ms = (uint32_t)((uint64_t)n * 1000U / 60U);
        if (n % 12 == 0) {
            formatRoomProfileLine(line, sizeof(line), t_ms, tick, dsp);
            puts(line);
        }
        if (n % 60 == 0) {
            VitalReport rep;
            const bool ok = dsp.computeVitalReport(rep);
            formatRoomLine(line, sizeof(line), t_ms, tick, rep);
            puts(line);
            if (ok)
                printf("@VITAL,%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.1f,%.1f,%.1f,%.2f,%.0f,%u\n",
                       (unsigned long)t_ms, rep.br_bpm, rep.hr_fused_bpm, rep.hr_spec_bpm, rep.hr_acf_bpm,
                       rep.hr_ibi_bpm, rep.ibi_median_ms, rep.rmssd_ms, rep.confidence_pct, rep.resp_depth_um,
                       rep.heart_amp_um, rep.zoom_snr_db, rep.loopback_phase_std_mrad, rep.target_dist_m * 1000.0f,
                       (unsigned)rep.num_room_targets);
            if (rep.needs_rezoom)
                printf("@REZOOM_HINT,%lu,%ld,%ld\n", (unsigned long)t_ms, (long)(rep.recommended_zoom_start_pt * 25 / 10),
                       (long)((rep.recommended_zoom_start_pt + kZoomPoints) * 25 / 10));
        }
    }
    printf("@END\n");
    return 0;
}
"""


def run_verify_cpp_dsp(npz_path: pathlib.Path, zoom_start_mm: int | None, out: pathlib.Path | None) -> int:
    if not npz_path.is_file():
        print(f"FEHLER: Aufnahme nicht gefunden: {npz_path}", file=sys.stderr)
        return 1
    data = np.load(npz_path, allow_pickle=False)
    if "X" not in data.files or data["X"].shape[1] != 105:
        print("FEHLER: Erwartet wird eine iq3raw-Aufnahme (Feld X mit 105 Punkten je Frame).", file=sys.stderr)
        return 1
    X = data["X"]
    zoom_pt = int(round(zoom_start_mm / 2.5)) if zoom_start_mm is not None else int(data["zoom_start_pt"])
    iq = np.empty(X.shape + (2,), dtype="<i2")
    iq[..., 0] = np.round(X.real)
    iq[..., 1] = np.round(X.imag)
    tick = data["tick"].astype("<u4")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        (tmp / "Arduino.h").write_text(ARDUINO_SHIM)
        (tmp / "harness.cpp").write_text(CPP_HARNESS)
        iq.tofile(tmp / "iq.bin")
        tick.tofile(tmp / "tick.bin")
        subprocess.run(["g++", "-O2", "-std=c++17", "-Wall", f"-I{tmp}", f"-I{DSP_HEADER.parent}",
                        str(tmp / "harness.cpp"), "-o", str(tmp / "harness"), "-lm"], check=True)
        res = subprocess.run([str(tmp / "harness"), str(tmp / "iq.bin"), str(tmp / "tick.bin"), str(zoom_pt)],
                             check=True, text=True, capture_output=True)
    lines = res.stdout.splitlines()
    print(f"{npz_path.name}: {len(X)} Frames, Zoom-Fenster ab {zoom_pt * 2.5:.0f} mm")
    for ln in lines:
        if ln.startswith(("@ROOM,", "@REZOOM_HINT")):
            print(ln)
    if out is not None:
        log = out.with_suffix(".log")
        log.write_text("\n".join(lines) + "\n")
        print(f"Log: {log}")
        return analyze_log(log, out)
    return 0


# ---------------------------------------------------------------------------------------------------------
# Live-Aufnahme
# ---------------------------------------------------------------------------------------------------------

def run_live_iq3(port: str, duration_s: int, start_mm: int, out: pathlib.Path) -> int:
    import serial

    log = out.with_suffix(".log")
    print(f"Verbinde mit ESP32-S3 auf {port} …")
    with serial.Serial(port, 115200, timeout=1.0) as ser, log.open("w") as f:
        ser.reset_input_buffer()
        cmd = f"iq3 {duration_s} {start_mm}\n"
        print(f"Sende: {cmd.strip()}  (Log: {log})")
        ser.write(cmd.encode("ascii"))
        t0 = time.monotonic()
        while (time.monotonic() - t0) < (duration_s + 15):
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            f.write(line + "\n")
            if not line.startswith(("@IQ3,", "@ROOMPROF,")):
                print(line)
            if line == "@END" or line.startswith("FEHLER"):
                break
    return analyze_log(log, out)


# ---------------------------------------------------------------------------------------------------------
# Auswertung eines Logs
# ---------------------------------------------------------------------------------------------------------

def parse_log(path: pathlib.Path) -> dict:
    prof_t, prof_amp, prof_mot = [], [], []
    start_mm = step_mm = None
    room_rows = []          # (t_s, dist_mm, amp, mot, moving)
    room_noise = []         # (t_s, rausch_amp, rausch_bew)
    vital_rows = []
    zoom_events = []        # (t_s, start_mm, end_mm, art)
    tick0 = None
    t_last = 0.0

    def tick_time(t_ms: int, tick: int) -> float:
        nonlocal tick0
        if tick0 is None:
            tick0 = (tick, t_ms)
        return (tick - tick0[0] + tick0[1]) / 1000.0

    for raw in path.read_text(errors="replace").splitlines():
        p = raw.strip().split(",")
        tag = p[0]
        try:
            if tag == "@ROOMPROF":
                t = tick_time(int(p[1]), int(p[2]))
                start_mm, step_mm, n = float(p[3]), float(p[4]), int(p[5])
                vals = np.array(p[6:6 + 2 * n], dtype=float)
                if len(vals) != 2 * n:
                    continue
                prof_t.append(t)
                prof_amp.append(vals[:n])
                prof_mot.append(vals[n:])
                t_last = t
            elif tag == "@ROOM":
                t = tick_time(int(p[1]), int(p[2]))
                room_noise.append((t, float(p[3]), float(p[4])))
                n = int(p[5])
                for k in range(n):
                    d, a, m, mv = p[6 + 4 * k:10 + 4 * k]
                    room_rows.append((t, float(d), float(a), float(m), int(mv)))
                t_last = t
            elif tag == "@VITAL":
                row = [float(x) for x in p[1:1 + len(VITAL_FIELDS)]]
                row[0] = t_last if room_noise else row[0] / 1000.0  # Zeit der @ROOM-Zeile desselben Reports
                vital_rows.append(row)
            elif tag in ("@IQ3_START", "@REZOOM"):
                zoom_events.append((t_last, float(p[1]), float(p[2]), tag[1:]))
            elif tag == "@REZOOM_HINT":
                zoom_events.append((t_last, float(p[2]), float(p[3]), "REZOOM_HINT"))
        except (ValueError, IndexError):
            continue  # abgeschnittene Zeile (z. B. beim Abbruch)

    return {
        "prof_t": np.array(prof_t),
        "prof_amp": np.array(prof_amp).reshape(len(prof_t), -1),
        "prof_mot": np.array(prof_mot).reshape(len(prof_t), -1),
        "start_mm": start_mm, "step_mm": step_mm,
        "room": np.array(room_rows).reshape(-1, 5),
        "room_noise": np.array(room_noise).reshape(-1, 3),
        "vital": np.array(vital_rows).reshape(-1, len(VITAL_FIELDS)),
        "zoom_events": zoom_events,
    }


def analyze_log(log: pathlib.Path, out: pathlib.Path) -> int:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    d = parse_log(log)
    if len(d["prof_t"]) == 0:
        print("FEHLER: keine @ROOMPROF-Zeilen im Log (Firmware zu alt?).", file=sys.stderr)
        return 1
    t = d["prof_t"]
    n_pts = d["prof_amp"].shape[1]
    dist_m = (d["start_mm"] + d["step_mm"] * np.arange(n_pts)) / 1000.0
    # Rauschpegel je Profil wie in der Firmware (nth_element bei n/4): unteres Viertel der Punkte
    noise_amp = np.sort(d["prof_amp"], axis=1)[:, n_pts // 4, None] + 1.0
    noise_mot = np.sort(d["prof_mot"], axis=1)[:, n_pts // 4, None] + 1.0
    mot_db = 20.0 * np.log10(np.maximum(d["prof_mot"] / noise_mot, 1e-3))
    amp_db = 20.0 * np.log10(np.maximum(d["prof_amp"] / noise_amp, 1e-3))

    np.savez_compressed(
        out.with_suffix(".npz"), prof_t=t, dist_m=dist_m, prof_amp=d["prof_amp"], prof_mot=d["prof_mot"],
        room=d["room"], room_cols=np.array(["t_s", "dist_mm", "amp", "mot", "moving"]), room_noise=d["room_noise"],
        vital=d["vital"], vital_cols=np.array(VITAL_FIELDS),
        zoom_events=np.array([(a, b, c) for a, b, c, _ in d["zoom_events"]]).reshape(-1, 3),
        zoom_kinds=np.array([k for *_, k in d["zoom_events"]]), source_log=str(log))

    fig = plt.figure(figsize=(14, 9))
    gs = fig.add_gridspec(2, 2, width_ratios=[4, 1], height_ratios=[3, 1], hspace=0.25, wspace=0.05)
    ax_w = fig.add_subplot(gs[0, 0])
    ax_p = fig.add_subplot(gs[0, 1], sharey=ax_w)
    ax_v = fig.add_subplot(gs[1, 0], sharex=ax_w)

    im = ax_w.pcolormesh(t, dist_m, mot_db.T, shading="nearest", cmap="magma", vmin=0, vmax=36)
    fig.colorbar(im, ax=ax_p, pad=0.02, label="Bewegung über Rauschen (dB)")
    # Zoom-Fenster als Stufenlinien (Start und Ende)
    ev = [(a, b, c) for a, b, c, k in d["zoom_events"] if k in ("IQ3_START", "REZOOM")]
    if ev:
        te = [e[0] for e in ev] + [t[-1]]
        for col in (1, 2):
            ax_w.step(te, [e[col] / 1000 for e in ev] + [ev[-1][col] / 1000], where="post", color="cyan", lw=1,
                      label="Zoom-Fenster" if col == 1 else None)
    hints = [e for e in d["zoom_events"] if e[3] == "REZOOM_HINT"]
    if hints:  # empfohlenes Fenster als senkrechter Balken (nur verify-cpp; live steht dort @REZOOM)
        ax_w.vlines([h[0] for h in hints], [h[1] / 1000 for h in hints], [h[2] / 1000 for h in hints],
                    colors="deepskyblue", lw=3, zorder=5, label="Rezoom-Empfehlung")
    r = d["room"]
    if len(r):
        mv = r[:, 4] > 0
        ax_w.plot(r[mv, 0], r[mv, 1] / 1000, "o", ms=4, mfc="lime", mec="k", mew=0.5, label="Ziel bewegt")
        ax_w.plot(r[~mv, 0], r[~mv, 1] / 1000, "o", ms=4, mfc="none", mec="w", mew=0.8, label="Ziel fest")
    ax_w.set_ylabel("Entfernung (m)")
    ax_w.set_title(f"Raum entlang der Sensorachse: Bewegung über Zeit — {log.name}")
    ax_w.legend(loc="upper right", fontsize=8, framealpha=0.6)
    plt.setp(ax_w.get_xticklabels(), visible=False)

    ax_p.plot(np.median(amp_db, axis=0), dist_m, "k-", lw=1, label="Echo (Median)")
    ax_p.plot(np.percentile(mot_db, 90, axis=0), dist_m, "r-", lw=1, label="Bewegung (90 %)")
    ax_p.axvline(20 * np.log10(2.0), color="gray", ls=":", lw=1, label="Zielschwelle 2×")
    ax_p.set_xlabel("dB über Rauschen")
    ax_p.legend(fontsize=7, loc="lower right")
    plt.setp(ax_p.get_yticklabels(), visible=False)

    v = d["vital"]
    if len(v):
        ax_v.plot(v[:, 0], v[:, 2], "r.-", lw=1, ms=3, label="Puls verfolgt (/min)")
        ax_v.plot(v[:, 0], np.where(v[:, 1] > 0, v[:, 1], np.nan), "b.-", lw=1, ms=3, label="Atmung (/min)")  # 0 = keine Atemspitze
        ax_v.legend(fontsize=8, loc="upper right")
    else:
        ax_v.text(0.5, 0.5, "keine @VITAL-Zeilen", transform=ax_v.transAxes, ha="center")
    ax_v.set_xlabel("Zeit (s, Sensor-Zeitstempel)")
    ax_v.set_ylabel("/min")
    ax_v.grid(alpha=0.3)
    png = out.with_suffix(".png")
    fig.savefig(png, dpi=110, bbox_inches="tight")

    n_mv = int((r[:, 4] > 0).sum()) if len(r) else 0
    print(f"{len(t)} Raumprofile über {t[-1] - t[0]:.1f} s, {len(d['room_noise'])} Reports, "
          f"{len(r)} Ziel-Einträge ({n_mv} bewegt), {len(v)} @VITAL")
    print(f"Ergebnis: {out.with_suffix('.npz')}\n          {png}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="iq3 am ESP32-S3: Live-Aufnahme, PC-Prüfung des DSP-Headers, Raumbild")
    parser.add_argument("--mode", choices=["iq3", "verify-cpp", "plot"], default="verify-cpp")
    parser.add_argument("--port", default=None, help="Serieller Port des ESP32-S3 (Standard: /dev/serial/by-id)")
    parser.add_argument("--duration", type=int, default=30, help="Messdauer in s für --mode iq3 (Standard 30)")
    parser.add_argument("--start-mm", type=int, default=500, help="Start des Zoom-Fensters in mm für --mode iq3")
    parser.add_argument("--npz", type=pathlib.Path, default=DEFAULT_NPZ, help="iq3raw-Aufnahme für --mode verify-cpp")
    parser.add_argument("--zoom-start-mm", type=int, default=None,
                        help="verify-cpp: Zoom-Fenster absichtlich hierhin setzen (prüft die Zielsuche)")
    parser.add_argument("--log", type=pathlib.Path, default=None, help="Log für --mode plot")
    parser.add_argument("--out", type=pathlib.Path, default=None, help="Ausgabe ohne Endung (.log/.npz/.png)")
    args = parser.parse_args()
    out = args.out.with_suffix("") if args.out else None

    if args.mode == "verify-cpp":
        return run_verify_cpp_dsp(args.npz, args.zoom_start_mm, out)
    if args.mode == "plot":
        if args.log is None:
            parser.error("--mode plot braucht --log")
        return analyze_log(args.log, out or args.log.with_suffix(""))
    port = args.port or find_esp32_port()
    if not port:
        print("FEHLER: Kein ESP32-S3 unter /dev/serial/by-id gefunden.", file=sys.stderr)
        return 1
    return run_live_iq3(port, args.duration, args.start_mm, out or default_out("iq3_raum"))


if __name__ == "__main__":
    raise SystemExit(main())
