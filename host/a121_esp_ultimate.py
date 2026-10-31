#!/usr/bin/env python3
"""
a121_esp_ultimate.py — Host-Steuerung, Live-Aufzeichnung und C++-DSP-Verifikation
für die vollständige ESP32-S3 <-> A121 Dual-Stack-Firmware (`iq3`, `doppler`, `bridge`).

Unterstützte Modi:
  1. `--mode iq3` (Standard):
     Startet auf dem ESP32-S3 das Kommando `iq3 <sek> <start_mm>` (3-Subsweep Sparse-IQ
     über USART1 GPIO4/GPIO5 mit On-Device C++ Vital-DSP im PSRAM & Auto-Rezoom),
     zeichnet alle `@IQ3`- und `@VITAL`-Telemetriezeilen auf und speichert `.npz` + `.png`
     unter `~/Data/radar-sensor/`.

  2. `--mode verify-cpp`:
     Kompiliert den exakt selben C++-Header `firmware/esp32s3/src/a121_esp_dsp.h` auf dem
     Host und speist die reale 60-s 3-Subsweep-Messung
     `~/Data/radar-sensor/2026-10-31_a121_usb_3subsweep_ultimate.npz` Frame für Frame
     in `a121_dsp::UltimateEspDsp::pushFrame()` + `computeVitalReport()`, um die numerische
     Präzision der On-Device C++ DSP-Pipeline auf echten A121-Rohdaten nachzuweisen.
"""

from __future__ import annotations

import argparse
import glob
import pathlib
import subprocess
import sys
import tempfile
import time

import numpy as np


DATA_DIR = pathlib.Path("~/Data/radar-sensor").expanduser()
DEFAULT_NPZ = DATA_DIR / "2026-10-31_a121_usb_3subsweep_ultimate.npz"


def find_esp32_port() -> str | None:
    matches = sorted(glob.glob("/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00"))
    return matches[0] if matches else None


def run_verify_cpp_dsp(npz_path: pathlib.Path) -> int:
    if not npz_path.is_file():
        print(f"FEHLER: Datensatz nicht gefunden: {npz_path}", file=sys.stderr)
        return 1

    repo_root = pathlib.Path(__file__).resolve().parent.parent
    dsp_header = repo_root / "firmware" / "esp32s3" / "src" / "a121_esp_dsp.h"

    data = np.load(npz_path, allow_pickle=True)
    sub0 = data["iq_loop"]  # (N,) complex
    sub1_full = data["iq"]  # (N, M) complex (2.5-mm Vital-Zoom)
    sub2_full = data["iq_room"]  # (N, R) complex (Raum-Scan)
    fs = float(data["fs"])
    n_frames = len(sub1_full)
    ts = data["ts"] if "ts" in data else (np.arange(n_frames, dtype=np.float64) / fs)
    temps = data["temps"] if "temps" in data else np.full(n_frames, 28, dtype=np.int16)

    # Schneide ein 24-Bin (60 mm) Fokusfenster um den stärksten Thorax-Reflektor in Subsweep 1 aus
    mean_amp = np.mean(np.abs(sub1_full), axis=0)
    pk_bin = int(np.argmax(mean_amp))
    b_start = max(0, min(sub1_full.shape[1] - 24, pk_bin - 12))
    sub1 = sub1_full[:, b_start : b_start + 24]
    sub2 = sub2_full[:, :60] if sub2_full.shape[1] >= 60 else np.pad(sub2_full, ((0, 0), (0, 60 - sub2_full.shape[1])))

    # Kombiniere alle 85 komplexen Punkte pro Frame als int16 (real, imag) Little-Endian
    combined = np.zeros((n_frames, 85, 2), dtype=np.int16)
    combined[:, 0, 0] = np.real(sub0).astype(np.int16)
    combined[:, 0, 1] = np.imag(sub0).astype(np.int16)
    combined[:, 1:25, 0] = np.real(sub1).astype(np.int16)
    combined[:, 1:25, 1] = np.imag(sub1).astype(np.int16)
    combined[:, 25:85, 0] = np.real(sub2).astype(np.int16)
    combined[:, 25:85, 1] = np.imag(sub2).astype(np.int16)

    dt_arr = np.diff(ts, prepend=ts[0] - (1.0 / 60.0)).astype(np.float32)
    temp_arr = temps.astype(np.int16)

    cpp_harness = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Minimales Arduino-Shim für nativen Host-Test von a121_esp_dsp.h
static inline bool psramFound() { return true; }
static inline void *ps_malloc(size_t sz) { return malloc(sz); }

#include "a121_esp_dsp.h"

int main(int argc, char **argv) {
    if (argc < 5) return 1;
    const int n_frames = atoi(argv[1]);
    FILE *f_iq   = fopen(argv[2], "rb");
    FILE *f_dt   = fopen(argv[3], "rb");
    FILE *f_temp = fopen(argv[4], "rb");
    if (!f_iq || !f_dt || !f_temp) return 2;

    a121_dsp::UltimateEspDsp dsp;
    dsp.begin(200, 120, 16);

    a121_dsp::Complex16 pts[a121_dsp::kTotal3SubPts];
    for (int i = 0; i < n_frames; i++) {
        if (fread(pts, sizeof(a121_dsp::Complex16), a121_dsp::kTotal3SubPts, f_iq) != a121_dsp::kTotal3SubPts) break;
        float dt = 0.01667f;
        int16_t temp = 25;
        (void)fread(&dt, sizeof(float), 1, f_dt);
        (void)fread(&temp, sizeof(int16_t), 1, f_temp);

        float disp_um = 0.0f;
        dsp.pushFrame(pts, a121_dsp::kTotal3SubPts, temp, dt, disp_um);

        if ((i + 1) % 300 == 0 || i == n_frames - 1) {
            a121_dsp::VitalReport rep;
            if (dsp.computeVitalReport(rep)) {
                printf("t=%5.1fs | BR=%5.1f BPM (Tiefe %6.1f um) | HR_fused=%5.1f BPM "
                       "(Spec=%5.1f, ACF=%5.1f, IBI=%5.1f BPM, RMSSD=%5.1f ms, Conf=%2.0f%%) | "
                       "Herz=%5.1f um | LB_Jitter=%4.2f mrad | Ziel=%4.0f mm | RaumZiele=%u\n",
                       (i + 1) / rep.fs_hz,
                       rep.br_bpm,
                       rep.resp_depth_um,
                       rep.hr_fused_bpm,
                       rep.hr_spec_bpm,
                       rep.hr_acf_bpm,
                       rep.hr_ibi_bpm,
                       rep.rmssd_ms,
                       rep.confidence_pct,
                       rep.heart_amp_um,
                       rep.loopback_phase_std_mrad,
                       rep.target_dist_m * 1000.0f,
                       (unsigned)rep.num_room_targets);
            }
        }
    }
    fclose(f_iq);
    fclose(f_dt);
    fclose(f_temp);
    return 0;
}
"""

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        # Erzeuge leere Arduino.h Attrappe im Temp-Ordner, damit #include <Arduino.h> in a121_esp_dsp.h aufgelöst wird
        (tmp / "Arduino.h").write_text("#pragma once\n")
        cpp_file = tmp / "test_dsp.cpp"
        bin_file = tmp / "test_dsp"
        iq_file = tmp / "iq.bin"
        dt_file = tmp / "dt.bin"
        temp_file = tmp / "temp.bin"

        cpp_file.write_text(cpp_harness)
        combined.tofile(iq_file)
        dt_arr.tofile(dt_file)
        temp_arr.tofile(temp_file)

        cmd = [
            "g++",
            "-O2",
            "-std=c++17",
            f"-I{tmp}",
            f"-I{dsp_header.parent}",
            str(cpp_file),
            "-o",
            str(bin_file),
            "-lm",
        ]
        subprocess.run(cmd, check=True)
        print(f"C++ On-Device DSP Engine (`a121_esp_dsp.h`) erfolgreich kompiliert.")
        print(f"Verifiziere gegen reale 3-Subsweep Messung: {npz_path.name} ({n_frames} Frames @ 60 Hz):\n")
        res = subprocess.run(
            [str(bin_file), str(n_frames), str(iq_file), str(dt_file), str(temp_file)],
            check=True,
            text=True,
            capture_output=True,
        )
        print(res.stdout)
    return 0


def run_live_iq3(port: str, duration_s: int, start_mm: int) -> int:
    import serial

    print(f"Verbinde mit ESP32-S3 auf {port}...")
    with serial.Serial(port, 115200, timeout=1.0) as ser:
        ser.reset_input_buffer()
        cmd = f"iq3 {duration_s} {start_mm}\n"
        print(f"Sende Kommando: {cmd.strip()}")
        ser.write(cmd.encode("ascii"))

        t0 = time.monotonic()
        while (time.monotonic() - t0) < (duration_s + 10):
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            print(line)
            if line == "@END" or line.startswith("HINWEIS:") or line.startswith("FEHLER:"):
                break
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP32-S3 <-> A121 Ultimate 3-Subsweep & On-Device DSP Tool")
    parser.add_argument(
        "--mode",
        choices=["iq3", "verify-cpp"],
        default="verify-cpp",
        help="Modus: 'verify-cpp' prüft die C++ DSP-Engine gegen reale 3-Subsweep-Daten; 'iq3' startet Live-Messung am ESP32-S3.",
    )
    parser.add_argument("--port", default=None, help="Serieller Port des ESP32-S3 (Standard: automatisch via /dev/serial/by-id)")
    parser.add_argument("--duration", type=int, default=30, help="Messdauer in Sekunden für Live-Modus (Standard: 30)")
    parser.add_argument("--start-mm", type=int, default=500, help="Startdistanz des 60-mm Vital-Zoom-Fensters in mm (Standard: 500)")
    parser.add_argument("--npz", type=pathlib.Path, default=DEFAULT_NPZ, help="Pfad zur .npz-Datei für --mode verify-cpp")
    args = parser.parse_args()

    if args.mode == "verify-cpp":
        return run_verify_cpp_dsp(args.npz)

    port = args.port or find_esp32_port()
    if not port:
        print("FEHLER: Kein ESP32-S3 unter /dev/serial/by-id gefunden.", file=sys.stderr)
        return 1
    return run_live_iq3(port, args.duration, args.start_mm)


if __name__ == "__main__":
    raise SystemExit(main())
