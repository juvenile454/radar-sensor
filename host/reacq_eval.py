#!/usr/bin/env python3
"""Measure pulse dropouts (ok == 0) on recordings: how long is the pulse missing, especially after motion? (V6, plan
2026-11-02-pulse-reacquisition). Baseline against which the motion map and pulse memory of the DSP are measured.

A dropout is a run of @DSP reports with ok == 0 after a valid report. Runs in which more than half of the reports
have state 0 (KEINE_PERSON, no person) do not count. Cause: "bewegung" (motion; within the dropout a report with motion_pct > 30,
state 4 or win_s == 0 with at least 6 s of data since start/rezoom (snap_frame ≥ 360), i.e. pulse
switched off by the motion map; win_s exists only with A121_DSP_REACQ), otherwise "keine_atemspitze" (no breathing peak; br_state == 1 in the majority of the reports), otherwise "rest".

  .venv/bin/python host/reacq_eval.py <ordner>… [--replay <header-ordner>] [--define NAME[=WERT]]… [--keine-person-zaehlen]
      [--ohne-karte]

--keine-person-zaehlen switches the person filter off (all runs of ok == 0 count, also with state 0 in the majority).
--ohne-karte does not count win_s == 0 as motion (baseline rule: only motion_pct > 30 or state 4), so that runs without and
with A121_DSP_REACQ are compared by the same rule (ruling R16).

Without --replay, stream.log is evaluated unchanged. With --replay it is recomputed (needs @RAW) with the a121_esp_dsp.h from the
folder. With --define, a copy of the header (from --replay, otherwise firmware/esp32s3/src) with
prepended #define lines is written to a temp directory and used for that, e.g. --define A121_DSP_REACQ.
The time is the receive time of the @DSP lines; @VITAL (hr_fused) exists only for valid reports and belongs to the
preceding @DSP.
"""

from __future__ import annotations

import argparse
import dataclasses
import math
import pathlib
import sys
import tempfile

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HOST))
from vital_eval import count_jumps, load_recording, replay_lines  # noqa: E402

ROOT = HOST.parent
FIRMWARE_SRC = ROOT / "firmware" / "esp32s3" / "src"
HEADER_NAME = "a121_esp_dsp.h"

STATE_NO_PERSON = 0     # @STATE KEINE_PERSON (no person)
STATE_MOTION = 4        # @STATE BEWEGUNG (motion)
BR_NO_PEAK = 1          # br_state: no breathing peak
MOTION_PCT_THR = 30.0   # motion_pct above this = motion
MIN_WIN_FRAMES = 360    # kHrMinWin: a pulse window exists only from 6 s of data since start/rezoom
CAUSES = ("bewegung", "keine_atemspitze", "rest")


@dataclasses.dataclass
class Dropout:
    start_s: float   # time of the first invalid report
    dur_s: float     # until the next valid report (at the end of the recording: until the last report)
    cause: str


def dropouts(t_s, ok, motion_pct, br_state, state, keine_person_zaehlen: bool = False, win_s=None,
             frames=None, ohne_karte: bool = False) -> list[Dropout]:
    """Runs of ok == 0 after a valid report (see module header); keine_person_zaehlen: also count runs with state 0
    in the majority. NaN in state/motion_pct/br_state/win_s meets none of the conditions; win_s None = field missing
    (ruling R10: win_s == 0 counts as motion). frames: frames since start/rezoom (@DSP snap_frame); win_s == 0 counts
    only from MIN_WIN_FRAMES, otherwise it is the ramp-up time after a rezoom (no window), not motion. None = do not
    check. ohne_karte: ignore win_s (baseline rule, ruling R16)."""
    t_s, ok = np.asarray(t_s, float), np.asarray(ok, float) > 0
    motion_pct, br_state, state = (np.asarray(x, float) for x in (motion_pct, br_state, state))
    win_s = np.full(ok.size, np.nan) if win_s is None or ohne_karte else np.asarray(win_s, float)
    frames = np.full(ok.size, np.inf) if frames is None else np.asarray(frames, float)
    karte_aus = (win_s == 0) & (frames >= MIN_WIN_FRAMES)  # pulse switched off by the motion map
    out: list[Dropout] = []
    seen_valid = False
    i, n = 0, ok.size
    while i < n:
        if ok[i]:
            seen_valid = True
            i += 1
            continue
        j = i
        while j < n and not ok[j]:
            j += 1  # [i, j) invalid
        if seen_valid:
            run = slice(i, j)
            size = j - i
            if keine_person_zaehlen or np.sum(state[run] == STATE_NO_PERSON) * 2 <= size:
                end = t_s[j] if j < n else t_s[-1]
                if (np.any(motion_pct[run] > MOTION_PCT_THR) or np.any(state[run] == STATE_MOTION)
                        or np.any(karte_aus[run])):
                    cause = "bewegung"
                elif np.sum(br_state[run] == BR_NO_PEAK) * 2 > size:
                    cause = "keine_atemspitze"
                else:
                    cause = "rest"
                out.append(Dropout(float(t_s[i]), float(end - t_s[i]), cause))
        i = j
    return out


def summary(t_s, ok, hr, state, drops: list[Dropout]) -> dict:
    """anteil_gueltig_pct (reports with ok, excluding state 0), ursachen {cause: n, median_s, summe_s}, spruenge,
    summe_s (dropout time across all causes)."""
    ok, state = np.asarray(ok, float) > 0, np.asarray(state, float)
    person = state != STATE_NO_PERSON  # NaN (no @STATE) counts as person
    share = 100.0 * float(np.mean(ok[person])) if person.any() else math.nan
    causes = {}
    for c in CAUSES:
        d = [x.dur_s for x in drops if x.cause == c]
        causes[c] = {"n": len(d), "median_s": float(np.median(d)) if d else math.nan, "summe_s": float(sum(d))}
    return {"anteil_gueltig_pct": share, "ursachen": causes, "spruenge": count_jumps(hr, ok),
            "summe_s": float(sum(x.dur_s for x in drops))}


def header_with_defines(src_dir: pathlib.Path, defines: list[str], dest_dir: pathlib.Path) -> pathlib.Path:
    """Copy of a121_esp_dsp.h from src_dir with prepended #define lines into dest_dir; returns dest_dir.
    "NAME" → `#define NAME`, "NAME=WERT" → `#define NAME WERT`."""
    head = "".join(f"#define {d.replace('=', ' ', 1)}\n" for d in defines)
    (dest_dir / HEADER_NAME).write_text(head + (src_dir / HEADER_NAME).read_text(encoding="utf-8"), encoding="utf-8")
    return dest_dir


def evaluate_folder(folder: pathlib.Path, header_dir: pathlib.Path | None,
                    keine_person_zaehlen: bool = False, ohne_karte: bool = False) -> tuple[dict, list[Dropout]]:
    """Evaluate one recording (recomputed if requested). @VITAL exists only for valid reports; each line belongs to the
    preceding @DSP (like vital_eval.evaluate), hence assignment by receive time, not by index."""
    lines = replay_lines(folder, header_dir) if header_dir else None
    rec = load_recording(folder, lines)
    d = rec.dsp
    t_s = d["rx_ms"] / 1000.0
    hr = np.full(t_s.size, math.nan)
    if t_s.size and rec.vital["rx_ms"].size:
        idx = np.searchsorted(d["rx_ms"], rec.vital["rx_ms"], side="right") - 1
        good = idx >= 0
        hr[idx[good]] = rec.vital["hr_fused"][good]
    ok = (d["ok"] > 0) & np.isfinite(hr)
    drops = dropouts(t_s, ok, d["motion_pct"], d["br_state"], d["state"], keine_person_zaehlen, d["win_s"],
                     d["snap_frame"], ohne_karte)
    return summary(t_s, ok, hr, d["state"], drops), drops


def _f(x: float) -> str:
    return "–" if math.isnan(x) else f"{x:.1f}"


def format_folder(name: str, s: dict) -> str:
    c = s["ursachen"]
    total = sum(v["n"] for v in c.values())
    out = (f"{name}: gültig {_f(s['anteil_gueltig_pct'])} % (ohne Zustand 0), Ausfälle {total}, "
           f"Bewegungs-Ausfälle {c['bewegung']['n']}, Ausfallzeit gesamt {_f(s['summe_s'])} s, "
           f"Sprünge > 15 /min {s['spruenge']}")
    out += "\n\n| Ursache | n | Median [s] | Summe [s] |\n|---|---|---|---|\n"
    for k in CAUSES:
        v = c[k]
        out += f"| {k} | {v['n']} | {_f(v['median_s'])} | {_f(v['summe_s'])} |\n"
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folders", nargs="+", type=pathlib.Path)
    ap.add_argument("--replay", type=pathlib.Path, default=None, help="Ordner mit a121_esp_dsp.h: neu rechnen")
    ap.add_argument("--keine-person-zaehlen", dest="keine_person_zaehlen", action="store_true",
                    help="auch Ausfälle mit Zustand KEINE_PERSON in der Mehrheit zählen (ohne Personenfilter)")
    ap.add_argument("--ohne-karte", dest="ohne_karte", action="store_true",
                    help="win_s == 0 nicht als Bewegung zählen (Basisregel wie ohne A121_DSP_REACQ, Ruling R16)")
    ap.add_argument("--define", action="append", default=[], metavar="NAME[=WERT]",
                    help="#define vor dem Header (Kopie in Temp-Ordner); mehrfach möglich")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        header_dir = a.replay
        if a.define:
            header_dir = header_with_defines(a.replay or FIRMWARE_SRC, a.define, pathlib.Path(tmp))
        if header_dir is not None:
            what = f" mit `{a.replay or FIRMWARE_SRC}`" + (f" und {' '.join(a.define)}" if a.define else "")
            print(f"Neu gerechnet{what}.\n")
        if a.ohne_karte:
            print("Ursache Bewegung nach der Basisregel (win_s == 0 zählt nicht, --ohne-karte).\n")
        for folder in a.folders:
            s, _ = evaluate_folder(folder, header_dir, a.keine_person_zaehlen, a.ohne_karte)
            print(format_folder(folder.name, s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
