#!/usr/bin/env python3
"""Weg-Rauschen an einem statischen Ziel (V4.1, docs/VITAL_AUDIT.md; Messplan in docs/EXPERIMENTS.md).

Rauschboden = RMS des Wegs (Phase × λ/4π, linear entzerrt) im Pulsband 0,6–5 Hz je Zoom-Punkt, berichtet als Median
der starken Punkte (mittlere Amplitude ≥ halbe stärkste) und bester Punkt. Drei Arten der Loopback-Derotation:
  aus       ohne Derotation
  roh       Loopback-Phase je Frame abziehen (wie die Firmware, a121_esp_dsp.h)
  tiefpass  Loopback-Phase erst tiefpassgefiltert (Butterworth 2. Ordnung, 0,8 Hz, nullphasig; wie früher am PC)

Eingaben: `iq3 … raw`-Aufnahme (Ordner mit stream.log oder die Datei; Punkt 0 Loopback, dann die Zoom-Punkte, die
80 Raum-Punkte fallen weg; nur bis zum ersten @REZOOM/@RECAL/@RECONNECT) oder eine npz aus `record`.

  .venv/bin/python host/noise_floor.py eval <aufnahme>…
  sg dialout -c ".venv/bin/python host/noise_floor.py record --out ~/Data/radar-sensor/v41/p1_h32_60hz.npz \\
      --start-mm 600 --profile 1 --hwaas 32 --rate 60"

`record` schaltet den ESP32 in `bridge` (transparente Brücke zum Exploration Server, docs/HARDWARE.md) und nimmt mit
exptool 7.18.2 eine 2-Subsweep-Session auf (Loopback + Zoom, frei einstellbar; `sess run` liefert keine Phase).
Port automatisch über /dev/serial/by-id (genau ein ESP32). Ergebnisse sind Sensor-Rauschen, keine Vitalwerte.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time
from dataclasses import dataclass, field

import numpy as np
from scipy import signal

HOST = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HOST))

RAD_TO_UM = 299792458.0 / 60.5e9 / (4.0 * np.pi) * 1e6  # ≈ 394,37 µm/rad, wie kRadToUm im Header
STEP_MM = 2.5
ROOM_N = 80
BAND = (0.6, 5.0)
MODES = ("aus", "roh", "tiefpass")
LOOP_LP_HZ = 0.8


@dataclass
class Recording:
    frames: np.ndarray  # N × P komplex, Zoom-Punkte
    loop: np.ndarray | None  # N komplex, Loopback (None = ohne)
    fs: float
    config: dict = field(default_factory=dict)


def noise_floor(frames: np.ndarray, fs: float, loop: np.ndarray | None, mode: str,
                band: tuple[float, float] = BAND) -> np.ndarray:
    """Weg-RMS in µm im Band je Punkt (Spalte von frames)."""
    if mode not in MODES:
        raise ValueError(f"Derotation {mode!r}: erlaubt {MODES}")
    z = np.asarray(frames, dtype=np.complex128)
    if mode != "aus":
        if loop is None:
            raise ValueError("Derotation braucht den Loopback")
        phi = np.unwrap(np.angle(np.asarray(loop, dtype=np.complex128)))
        if mode == "tiefpass":
            phi = signal.sosfiltfilt(signal.butter(2, LOOP_LP_HZ, "low", fs=fs, output="sos"), phi)
        z = z * np.exp(-1j * phi)[:, None]
    d = signal.detrend(np.unwrap(np.angle(z), axis=0), axis=0) * RAD_TO_UM
    y = signal.sosfiltfilt(signal.butter(4, band, "bandpass", fs=fs, output="sos"), d, axis=0)
    return np.sqrt(np.mean(y**2, axis=0))


def strong_points(frames: np.ndarray) -> np.ndarray:
    amp = np.mean(np.abs(frames), axis=0)
    return amp >= 0.5 * amp.max()


# --------------------------------------------------------------------------------------------- Laden / Speichern

def save(path: pathlib.Path, rec: Recording) -> None:
    np.savez_compressed(path, frames=rec.frames, loop=rec.loop if rec.loop is not None else np.zeros(0, np.complex64),
                        fs=rec.fs, config=json.dumps(rec.config))


def _load_log(path: pathlib.Path) -> Recording:
    from esp_record import decode_raw

    frames, cfg, fs = [], {"quelle": "iq3 raw"}, 60.0
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("@IQ3_START,"):
            if frames:
                break
            p = line.split(",")
            cfg["zoom_start_mm"] = float(p[1])
            if len(p) > 3:
                fs = float(p[3])
        elif line.startswith(("@REZOOM,", "@RECAL,", "@RECONNECT,")) and frames:
            cfg["abgeschnitten_bei"] = line.split(",")[0]
            break
        elif line.startswith("@RAW,"):
            p = line.split(",", 4)
            frames.append(decode_raw(p[4], int(p[3])))
    if not frames:
        raise SystemExit(f"FEHLER: keine @RAW-Zeilen in {path} (Aufnahme mit `iq3 … raw`?)")
    a = np.array(frames)
    zoom = a.shape[1] - 1 - ROOM_N
    return Recording(a[:, 1:1 + zoom].astype(np.complex64), a[:, 0].astype(np.complex64), fs, cfg)


def load(path: pathlib.Path) -> Recording:
    path = pathlib.Path(path)
    if path.is_dir():
        path = path / "stream.log"
    if path.suffix == ".npz":
        z = np.load(path, allow_pickle=False)
        loop = z["loop"] if z["loop"].size else None
        return Recording(z["frames"], loop, float(z["fs"]), json.loads(str(z["config"])))
    return _load_log(path)


def report(path: pathlib.Path) -> str:
    rec = load(path)
    s = strong_points(rec.frames)
    cfg = ", ".join(f"{k}={v}" for k, v in rec.config.items())
    out = [f"{path.stem if path.suffix else path.name}: {rec.frames.shape[0] / rec.fs:.1f} s, {rec.frames.shape[1]} Punkte "
           f"({int(s.sum())} stark), {rec.fs:g} Hz" + (f" [{cfg}]" if cfg else "")]
    for mode in MODES:
        if mode != "aus" and rec.loop is None:
            continue
        r = noise_floor(rec.frames, rec.fs, rec.loop, mode)
        out.append(f"  {mode:<9} {np.median(r[s]):.3f} µm RMS (bester Punkt {r.min():.3f} µm, Punkt {int(r.argmin())})")
    return "\n".join(out)


# --------------------------------------------------------------------------------------------- Aufnahme über bridge

def _prf(a121, value: str):
    return {"19.5": a121.PRF.PRF_19_5_MHz, "15.6": a121.PRF.PRF_15_6_MHz, "13.0": a121.PRF.PRF_13_0_MHz,
            "8.7": a121.PRF.PRF_8_7_MHz, "6.5": a121.PRF.PRF_6_5_MHz, "5.2": a121.PRF.PRF_5_2_MHz}[value]


def build_config(start_mm: float, points: int = 24, profile: int = 1, hwaas: int = 32, gain: int = 16,
                 prf: str = "15.6", rate: float = 60.0, loop: bool = True, loop_profile: int = 1,
                 loop_prf: str = "19.5", loop_gain: int = 16, loop_hwaas: int = 8):
    """Loopback (Punkt 0) + Zoom (Schritt 2,5 mm); Standard wie das iq3-Preset der Firmware (a121_server_client.h),
    jede Größe einstellbar. Regeln von exptool: 19,5 MHz nur mit Profil 1, Loopback nicht mit Profil 2, PRF eines
    Subsweeps nicht kleiner als die des folgenden (Loopback 15,6 MHz also nur mit Zoom ≤ 15,6 MHz)."""
    from acconeer.exptool import a121

    subs = []
    if loop:
        subs.append(a121.SubsweepConfig(start_point=0, num_points=1, step_length=1,
                                        profile=a121.Profile(loop_profile), hwaas=loop_hwaas,
                                        receiver_gain=loop_gain, enable_loopback=True,
                                        prf=_prf(a121, loop_prf)))
    subs.append(a121.SubsweepConfig(start_point=int(round(start_mm / STEP_MM)), num_points=points, step_length=1,
                                    profile=a121.Profile(profile), hwaas=hwaas, receiver_gain=gain,
                                    phase_enhancement=True, iq_imbalance_compensation=True,
                                    prf=_prf(a121, prf)))
    return a121.SensorConfig(subsweeps=subs, sweeps_per_frame=1, frame_rate=rate,
                             inter_frame_idle_state=a121.IdleState.READY,
                             inter_sweep_idle_state=a121.IdleState.READY)


def record(a: argparse.Namespace) -> int:  # pragma: no cover - braucht das Gerät
    import serial
    from acconeer.exptool import a121
    from esp_record import find_port

    port = a.port or find_port()
    cfg = build_config(a.start_mm, a.points, a.profile, a.hwaas, a.gain, a.prf, a.rate, not a.no_loop,
                       a.loop_profile, a.loop_prf, a.loop_gain)
    cfg.validate()
    with serial.Serial(port, 115200, timeout=0.5) as ser:
        ser.write(b"\nbridge\n")
        time.sleep(0.8)
        ser.reset_input_buffer()
    client = a121.Client.open(serial_port=port)
    try:
        client.setup_session(cfg)
        client.start_session()
        n = int(a.seconds * a.rate)
        frames = np.zeros((n, a.points), np.complex64)
        loop = None if a.no_loop else np.zeros(n, np.complex64)
        flags = 0
        for i in range(n):
            res = client.get_next()
            sub = res.subframes
            if loop is not None:
                loop[i] = sub[0][0, 0]
            frames[i] = sub[-1][0]
            flags += int(res.data_saturated or res.frame_delayed or res.calibration_needed)
        client.stop_session()
    finally:
        client.close()
        with serial.Serial(port, 115200, timeout=0.5) as ser:
            ser.write(b"+++EXIT")
    conf = {k: getattr(a, k) for k in ("start_mm", "points", "profile", "hwaas", "gain", "prf", "rate", "loop_profile",
                                       "loop_prf", "loop_gain")}
    conf["loop"] = not a.no_loop
    conf["frames_mit_flags"] = flags
    save(a.out, Recording(frames, loop, a.rate, conf))
    print(f"{a.out}: {n} Frames, {flags} mit Flags (gesättigt/verspätet/Kalibrierung)")
    print(report(a.out))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    ev = sub.add_parser("eval", help="Rauschboden je Aufnahme")
    ev.add_argument("paths", nargs="+", type=pathlib.Path)
    rc = sub.add_parser("record", help="Aufnahme über ESP32-bridge + exptool")
    rc.add_argument("--out", type=pathlib.Path, required=True)
    rc.add_argument("--port", default=None)
    rc.add_argument("--seconds", type=float, default=60.0)
    rc.add_argument("--start-mm", type=float, required=True)
    rc.add_argument("--points", type=int, default=24)
    rc.add_argument("--profile", type=int, default=1)
    rc.add_argument("--hwaas", type=int, default=32)
    rc.add_argument("--gain", type=int, default=16)
    rc.add_argument("--prf", default="15.6", help="MHz: 19.5 15.6 13.0 8.7 6.5 5.2")
    rc.add_argument("--rate", type=float, default=60.0)
    rc.add_argument("--no-loop", action="store_true")
    rc.add_argument("--loop-profile", type=int, default=1)
    rc.add_argument("--loop-prf", default="19.5")
    rc.add_argument("--loop-gain", type=int, default=16)
    a = ap.parse_args(argv)
    if a.cmd == "record":
        return record(a)
    for p in a.paths:
        print(report(p))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
