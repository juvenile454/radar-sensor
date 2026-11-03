#!/usr/bin/env python3
"""Check the phase across the session restart at a rezoom (docs/VITAL_AUDIT.md, V2.3 step 1, experiment M5).

Question: does the phase stay continuous at equal distances when the ESP32 sets up the session again for a rezoom?
Only then is it worth taking over the overlapping zoom points at the rezoom and filling the gap (V2.3 step 4).

Needs a recording with `iq3 … raw` (stream.log with @RAW). Per @REZOOM: mean of the last k frames before and of the
first k frames after, per distance (overlapping zoom points, all room points):
  Sprung    (jump) weighted circular mean of the phase difference (weight = amplitude), raw and with loopback derotation
            (each frame rotated back with the unit phasor of the loopback)
  Streuung  (spread) circular standard deviation of the difference across the points: small with a large jump = common rotation
            that a single rotation could compensate
  Amplitude median of the ratio after/before
Breathing moves the phase during the gap (≈ 0.4 s); keep k small and compare with the phase trend before the rezoom.

  .venv/bin/python host/rezoom_check.py <stream.log | aufnahme-ordner> [--k 5]
"""

from __future__ import annotations

import argparse
import base64
import math
import pathlib
import sys

import numpy as np

ROOM_N = 80
STEP_MM = 2.5
CONT_RAD = 0.1  # "continuous": |jump| below this


def _raw_frame(line: str) -> np.ndarray | None:
    p = line.split(",", 4)
    if len(p) < 5:
        return None
    try:
        iq = np.frombuffer(base64.b64decode(p[4]), "<i2").astype(float)
    except ValueError:
        return None
    if iq.size != 2 * int(p[3]):
        return None
    return iq[0::2] + 1j * iq[1::2]


def _segments(lines: list[str]) -> tuple[list[dict], list[tuple[int, int, int]]]:
    """Frames per section between restarts; rezooms = (section before, start before, start after)."""
    segs: list[dict] = []
    rezooms: list[tuple[int, int, int]] = []
    start = None
    for ln in lines:
        if ln.startswith("@IQ3_START,"):
            start = round(float(ln.split(",")[1]) / STEP_MM)
            segs.append({"start": start, "frames": []})
        elif ln.startswith("@REZOOM,") and segs:
            new = round(float(ln.split(",")[1]) / STEP_MM)
            rezooms.append((len(segs) - 1, segs[-1]["start"], new))
            segs.append({"start": new, "frames": []})
        elif ln.startswith(("@RECAL,", "@RECONNECT,")) and segs:
            segs.append({"start": segs[-1]["start"], "frames": []})
        elif ln.startswith("@RAW,") and segs:
            f = _raw_frame(ln)
            if f is not None:
                segs[-1]["frames"].append(f)
    return segs, rezooms


def _jump(before: np.ndarray, after: np.ndarray) -> tuple[float, float, float]:
    """before/after: complex means per point. Returns (jump, spread, amplitude ratio)."""
    if before.size == 0:
        return math.nan, math.nan, math.nan
    w = np.abs(before)
    e = after * np.conj(before)
    e = e / np.maximum(np.abs(e), 1e-12)
    m = np.sum(w * e) / max(np.sum(w), 1e-12)
    r = min(float(np.abs(m)), 1.0)
    spread = math.sqrt(max(-2.0 * math.log(r), 0.0)) if r > 0 else math.inf
    ratio = float(np.median(np.abs(after) / np.maximum(np.abs(before), 1e-12)))
    return float(np.angle(m)), spread, ratio


def analyze(lines: list[str], k: int = 5) -> list[dict]:
    segs, rezooms = _segments(lines)
    out = []
    for si, old, new in rezooms:
        fb, fa = segs[si]["frames"][-k:], segs[si + 1]["frames"][:k]
        res = {"old_start_pt": old, "new_start_pt": new, "frames_before": len(fb), "frames_after": len(fa)}
        if not fb or not fa:
            res.update(overlap=0, zoom_jump_rad=math.nan, zoom_jump_derot_rad=math.nan, zoom_spread_rad=math.nan,
                       zoom_amp_ratio=math.nan, room_jump_rad=math.nan, room_jump_derot_rad=math.nan)
            out.append(res)
            continue
        B, A = np.stack(fb), np.stack(fa)
        nz = B.shape[1] - 1 - ROOM_N
        lo, hi = max(old, new), min(old, new) + nz
        res["overlap"] = max(hi - lo, 0)
        for tag, Bx, Ax in (("", B, A), ("_derot", B * np.conj(B[:, :1]) / np.abs(B[:, :1]),
                                          A * np.conj(A[:, :1]) / np.abs(A[:, :1]))):
            zb = Bx[:, 1 + lo - old:1 + hi - old].mean(axis=0) if hi > lo else np.zeros(0, complex)
            za = Ax[:, 1 + lo - new:1 + hi - new].mean(axis=0) if hi > lo else np.zeros(0, complex)
            j, s, ratio = _jump(zb, za)
            res["zoom_jump" + tag + "_rad"] = j
            if not tag:
                res["zoom_spread_rad"], res["zoom_amp_ratio"] = s, ratio
            rj, _, _ = _jump(Bx[:, 1 + nz:].mean(axis=0), Ax[:, 1 + nz:].mean(axis=0))
            res["room_jump" + tag + "_rad"] = rj
        out.append(res)
    return out


def summary(res: list[dict]) -> str:
    def cont(key: str) -> int:
        return sum(1 for r in res if not math.isnan(r[key]) and abs(r[key]) < CONT_RAD)

    lines = ["Rezoom  alt→neu (Punkt)  Überlapp  Sprung roh / derotiert  Streuung  Amplitude  Raum roh / derotiert"]
    for i, r in enumerate(res, 1):
        lines.append(f"{i:6d}  {r['old_start_pt']:4d}→{r['new_start_pt']:<4d}      {r['overlap']:5d}    "
                     f"{r['zoom_jump_rad']:+6.3f} / {r['zoom_jump_derot_rad']:+6.3f} rad   {r['zoom_spread_rad']:6.3f}  "
                     f"{r['zoom_amp_ratio']:9.3f}  {r['room_jump_rad']:+6.3f} / {r['room_jump_derot_rad']:+6.3f}")
    with_ov = [r for r in res if r["overlap"] > 0]
    lines.append(f"{len(res)} Rezooms, davon {len(with_ov)} mit überlappenden Zoom-Punkten; "
                 f"stetig (|Sprung| < 0,1 rad): {cont('zoom_jump_rad')} von {len(with_ov)} roh, "
                 f"{cont('zoom_jump_derot_rad')} von {len(with_ov)} mit Loopback-Derotation; "
                 f"Raum: {cont('room_jump_rad')} / {cont('room_jump_derot_rad')} von {len(res)}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=pathlib.Path, help="stream.log oder Aufnahme-Ordner")
    ap.add_argument("--k", type=int, default=5, help="Frames je Seite (Standard 5)")
    a = ap.parse_args()
    path = a.log / "stream.log" if a.log.is_dir() else a.log
    res = analyze(path.read_text(errors="replace").splitlines(), a.k)
    if not res:
        print("Keine @REZOOM-Zeile mit @RAW-Frames gefunden (Aufnahme mit `iq3 … raw`?).")
        return 1
    print(summary(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
