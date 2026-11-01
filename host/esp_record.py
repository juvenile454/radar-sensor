#!/usr/bin/env python3
"""ESP32-S3-Ausgabe eines Befehls aufzeichnen und prüfen, im selben Format wie die Android-App.

Aufnahme-Ordner (docs/ANDROID_APP.md):
  stream.log   Zeilen genau wie vom ESP32 (ohne Zeilenende-\\r)
  rxtime.csv   line,rx_ms (Empfangszeit je Zeile, ms seit Aufnahmebeginn)
  meta.json    Befehl, @HELLO, Gerät, Start, Quelle

Beispiele:
  .venv/bin/python host/esp_record.py --cmd "iq3 20 500 raw" --name iq3raw_20s
  .venv/bin/python host/esp_record.py --check ~/Data/radar-sensor/app-fixtures/iq3raw_20s
  .venv/bin/python host/esp_record.py --check <ordner> --npz <ziel.npz>   # @RAW → iq3raw-npz für --mode verify-cpp

Die Prüfung zählt Zeilen je Tag, Frame-Lücken (@IQ3/@DOPPLER-Zähler), Tick-Rücksprünge und vergleicht bei @RAW die
Loopback-/Zoom-Amplitude aus den Rohpunkten mit den Werten in @IQ3 desselben Frames.
"""

from __future__ import annotations

import argparse
import base64
import collections
import datetime as dt
import json
import pathlib
import sys
import time

import numpy as np

DATA_DIR = pathlib.Path("~/Data/radar-sensor").expanduser()
FIXTURE_DIR = DATA_DIR / "app-fixtures"
BY_ID = pathlib.Path("/dev/serial/by-id")
ROOM_POINTS = 80
ZOOM_WIDTHS = (24, 48)  # Zoom-Punkte je Frame (A121_ZOOM_POINTS; 48 vorbereitet, V2.3), aus @RAW abgelesen


def find_port() -> str:
    ports = sorted(p for p in BY_ID.glob("usb-Espressif*") if p.name.endswith("-if00"))
    if len(ports) != 1:
        raise SystemExit(f"FEHLER: genau ein ESP32 unter {BY_ID} erwartet, gefunden: {[p.name for p in ports]}")
    return str(ports[0])


def read_until_quiet(ser, quiet_s: float = 0.4, max_s: float = 3.0) -> list[str]:
    lines, t0, last = [], time.monotonic(), time.monotonic()
    while time.monotonic() - t0 < max_s and time.monotonic() - last < quiet_s:
        raw = ser.readline()
        if raw:
            lines.append(raw.decode("utf-8", errors="replace").rstrip("\r\n"))
            last = time.monotonic()
    return lines


def record(port: str, cmd: str, out: pathlib.Path, timeout_s: float, reset_after_s: float | None) -> pathlib.Path:
    import serial  # pyserial

    out.mkdir(parents=True, exist_ok=False)
    # DTR/RTS nicht anfassen: Linux setzt beim Öffnen beide, das ist ohne Reset. DTR=0 bei RTS=1 (auch kurz, etwa
    # durch dtr=False vor open()) startet den ESP32-S3 über USB-Serial/JTAG neu (2026-11-01 per @HELLO up_ms geprüft).
    ser = serial.Serial(port, 115200, timeout=0.2)
    try:
        ser.write(b"\n")
        read_until_quiet(ser)
        ser.write(b"hello\n")
        hello = next((ln for ln in read_until_quiet(ser) if ln.startswith("@HELLO")), None)
        meta = {
            "command": cmd, "hello": hello, "source": "pc", "port": port,
            "start": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        }
        ser.reset_input_buffer()
        n = 0
        t0 = time.monotonic()
        reset_sent = False
        with open(out / "stream.log", "w") as f_log, open(out / "rxtime.csv", "w") as f_rx:
            f_rx.write("line,rx_ms\n")
            ser.write((cmd + "\n").encode("ascii"))
            while time.monotonic() - t0 < timeout_s:
                if reset_after_s is not None and not reset_sent and time.monotonic() - t0 >= reset_after_s:
                    ser.write(b"\x12")  # Server absichtlich neu starten → @RECONNECT prüfen
                    reset_sent = True
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                if not line:
                    continue
                f_log.write(line + "\n")
                f_rx.write(f"{n},{(time.monotonic() - t0) * 1000:.1f}\n")
                n += 1
                if line == "@END" or line.startswith("FEHLER"):
                    break
        meta["lines"] = n
        meta["duration_s"] = round(time.monotonic() - t0, 2)
        (out / "meta.json").write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n")
    finally:
        ser.close()
    return out


def decode_raw(b64: str, n: int) -> np.ndarray:
    a = np.frombuffer(base64.b64decode(b64), dtype="<i2")
    if a.size != 2 * n:
        raise ValueError(f"@RAW: {a.size // 2} statt {n} Punkte")
    return a[0::2].astype(np.float64) + 1j * a[1::2].astype(np.float64)


def check(folder: pathlib.Path, npz: pathlib.Path | None) -> int:
    lines = (folder / "stream.log").read_text(errors="replace").splitlines()
    tags = collections.Counter(ln.split(",", 1)[0] if ln.startswith("@") else "(text)" for ln in lines)
    frames: dict[str, list[int]] = {"@IQ3": [], "@DOPPLER": []}
    ticks: dict[str, list[int]] = {"@IQ3": [], "@DOPPLER": []}
    # Index des ersten Frames nach @RECONNECT (Server neu: tick beginnt neu) bzw. nach @IQ3_START/@DOPPLER_START
    # (neuer Stream, z. B. ESP32 nach Abziehen neu gebootet: tick und Frame-Zähler beginnen neu)
    seg_start: dict[str, set[int]] = {"@IQ3": set(), "@DOPPLER": set()}
    stream_start: dict[str, set[int]] = {"@IQ3": set(), "@DOPPLER": set()}
    reconnects = 0
    iq3_amp: dict[int, tuple[float, float]] = {}
    raw_frames, raw_ticks, raw_X = [], [], []
    zoom_start_mm = None
    bad = 0
    for ln in lines:
        p = ln.split(",")
        try:
            if p[0] == "@RECONNECT":
                reconnects += 1
                for tag in seg_start:
                    seg_start[tag].add(len(ticks[tag]))
            elif p[0] == "@IQ3" and len(p) >= 9:
                frames["@IQ3"].append(int(p[7]))
                ticks["@IQ3"].append(int(p[8]))
                iq3_amp[int(p[7])] = (float(p[3]), float(p[5]))
            elif p[0] == "@DOPPLER" and len(p) >= 7:
                frames["@DOPPLER"].append(int(p[5]))
                ticks["@DOPPLER"].append(int(p[6]))
            elif p[0] == "@RAW":
                raw_frames.append(int(p[1]))
                raw_ticks.append(int(p[2]))
                raw_X.append(decode_raw(p[4], int(p[3])))
            elif p[0] in ("@IQ3_START", "@DOPPLER_START"):
                tag = p[0].removesuffix("_START")
                seg_start[tag].add(len(ticks[tag]))
                stream_start[tag].add(len(frames[tag]))
                if p[0] == "@IQ3_START" and zoom_start_mm is None:
                    zoom_start_mm = float(p[1])
        except (ValueError, IndexError):
            bad += 1

    print(f"{folder.name}: {len(lines)} Zeilen, {bad} nicht lesbar")
    for tag, c in sorted(tags.items()):
        print(f"  {tag:14s} {c}")
    ok = bad == 0
    for tag in ("@IQ3", "@DOPPLER"):
        fr, tk = np.array(frames[tag]), np.array(ticks[tag], dtype=np.int64)
        if fr.size < 2:
            continue
        # frame = Zähler des ESP32 (Lücke = Verlust zwischen ESP32 und Empfänger),
        # tick = Sensorzeit in ms (beginnt nach @RECONNECT neu, große Abstände = Lücke am Sensor/Server)
        new_stream = np.array([i + 1 in stream_start[tag] for i in range(fr.size - 1)], dtype=bool)
        dfr = np.diff(fr)[~new_stream]
        gaps = int(np.sum(dfr != 1))
        lost = int(np.sum(dfr - 1))
        new_seg = np.array([i + 1 in seg_start[tag] for i in range(tk.size - 1)], dtype=bool)
        dt_tick = np.diff(tk)
        back = int(np.sum((dt_tick < 0) & ~new_seg))
        dt_ok = dt_tick[~new_seg]
        span_s = dt_ok.sum() / 1000.0
        rate = dt_ok.size / span_s if span_s > 0 else float("nan")
        restarts = int(new_stream.sum())
        print(f"  {tag}: {fr.size} Frames in {restarts + 1} Stream(s), Lücken {gaps} (fehlend {lost}), "
              f"Neuverbindungen {reconnects}, Tick-Rücksprünge {back}, Rate nach Tick {rate:.2f} Hz, "
              f"Tick-Abstand Median {np.median(dt_ok):.1f} ms, max {dt_ok.max()} ms")
        ok &= gaps == 0 and back == 0
    if raw_X:
        X = np.stack(raw_X)
        zoom_points = X.shape[1] - 1 - ROOM_POINTS
        zoom_center = 1 + zoom_points // 2  # wie b0 in cmdIq3()
        diffs = []
        for fno, x in zip(raw_frames, raw_X):
            if fno in iq3_amp:
                lb, za = iq3_amp[fno]
                diffs.append(max(abs(abs(x[0]) - lb), abs(abs(x[zoom_center]) - za)))
        maxdiff = max(diffs) if diffs else float("nan")
        same_tick = all(t == ticks["@IQ3"][frames["@IQ3"].index(f)] for f, t in zip(raw_frames[:50], raw_ticks[:50])
                        if f in frames["@IQ3"])
        print(f"  @RAW: {X.shape[0]} Frames × {X.shape[1]} Punkte ({zoom_points} Zoom); |IQ| vs. @IQ3 (Loopback/Zoom) max. Abweichung "
              f"{maxdiff:.3f} (Rundung %.1f → ≤ 0,05); Tick gleich: {same_tick}")
        ok &= zoom_points in ZOOM_WIDTHS and maxdiff <= 0.051 and same_tick
        if npz is not None:
            zoom_pt = int(round((zoom_start_mm or 500) / 2.5))
            np.savez_compressed(npz, X=X.astype(np.complex64), tick=np.array(raw_ticks, dtype=np.uint32),
                                frame=np.array(raw_frames, dtype=np.uint32), zoom_start_pt=zoom_pt,
                                source=str(folder))
            print(f"  → {npz} (für host/a121_esp_ultimate.py --mode verify-cpp --npz ...)")
    print("PRÜFUNG:", "OK" if ok else "AUFFÄLLIG")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cmd", help="Befehl an den ESP32, z. B. 'iq3 20 500 raw'")
    ap.add_argument("--name", help="Ordnername unter app-fixtures (Standard: Datum + Befehl)")
    ap.add_argument("--port", default=None)
    ap.add_argument("--timeout", type=float, default=None, help="Abbruch nach s (Standard: aus dem Befehl + 20 s)")
    ap.add_argument("--reset-after", type=float, default=None, help="nach s Strg+R senden (Server-Neustart-Test)")
    ap.add_argument("--check", type=pathlib.Path, default=None, help="vorhandene Aufnahme nur prüfen")
    ap.add_argument("--npz", type=pathlib.Path, default=None, help="@RAW als iq3raw-npz speichern")
    a = ap.parse_args()

    if a.check:
        return check(a.check, a.npz)
    if not a.cmd:
        ap.error("--cmd oder --check angeben")
    parts = a.cmd.split()
    dur = float(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 60.0
    timeout = a.timeout or dur + 20.0
    name = a.name or dt.datetime.now().strftime("%Y-%m-%d_%H%M%S_") + "_".join(parts)
    out = record(a.port or find_port(), a.cmd, FIXTURE_DIR / name, timeout, a.reset_after)
    print(f"Aufnahme: {out}")
    return check(out, a.npz)


if __name__ == "__main__":
    sys.exit(main())
