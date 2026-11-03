#!/usr/bin/env python3
"""Pulsausfälle (ok == 0) auf Aufnahmen messen: wie lange fehlt der Puls, besonders nach Bewegung? (V6, Plan
2026-11-02-puls-wiederfinden). Basislinie, an der Bewegungskarte und Pulsgedächtnis der DSP gemessen werden.

Ein Ausfall ist eine Folge von @DSP-Reports mit ok == 0 nach einem gültigen Report. Folgen, in denen mehr als die Hälfte
der Reports Zustand 0 (KEINE_PERSON) hat, zählen nicht. Ursache: "bewegung" (im Ausfall ein Report mit motion_pct > 30,
Zustand 4 oder win_s == 0 bei mindestens 6 s Daten seit Start/Rezoom (snap_frame ≥ 360), d. h. Puls
von der Bewegungskarte abgeschaltet; win_s gibt es nur mit A121_DSP_REACQ), sonst "keine_atemspitze" (br_state == 1 in der Mehrheit der Reports), sonst "rest".

  .venv/bin/python host/reacq_eval.py <ordner>… [--replay <header-ordner>] [--define NAME[=WERT]]… [--keine-person-zaehlen]

--keine-person-zaehlen schaltet den Personenfilter ab (alle Läufe von ok == 0 zählen, auch mit Zustand 0 in der Mehrheit).

Ohne --replay wird stream.log unverändert ausgewertet. Mit --replay wird es (braucht @RAW) mit dem a121_esp_dsp.h aus dem
Ordner neu gerechnet. Mit --define wird eine Kopie des Headers (aus --replay, sonst firmware/esp32s3/src) mit
vorangestellten #define-Zeilen in ein Temp-Verzeichnis geschrieben und dafür benutzt, z. B. --define A121_DSP_REACQ.
Die Zeit ist die Empfangszeit der @DSP-Zeilen; @VITAL (hr_fused) gibt es nur für gültige Reports und gehört zum
vorangehenden @DSP.
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

STATE_NO_PERSON = 0     # @STATE KEINE_PERSON
STATE_MOTION = 4        # @STATE BEWEGUNG
BR_NO_PEAK = 1          # br_state: keine Atemspitze
MOTION_PCT_THR = 30.0   # motion_pct darüber = Bewegung
MIN_WIN_FRAMES = 360    # kHrMinWin: erst ab 6 s Daten seit Start/Rezoom gibt es ein Pulsfenster
CAUSES = ("bewegung", "keine_atemspitze", "rest")


@dataclasses.dataclass
class Dropout:
    start_s: float   # Zeit des ersten ungültigen Reports
    dur_s: float     # bis zum nächsten gültigen Report (am Ende der Aufnahme: bis zum letzten Report)
    cause: str


def dropouts(t_s, ok, motion_pct, br_state, state, keine_person_zaehlen: bool = False, win_s=None,
             frames=None) -> list[Dropout]:
    """Läufe von ok == 0 nach einem gültigen Report (siehe Modulkopf); keine_person_zaehlen: auch Läufe mit Zustand 0
    in der Mehrheit zählen. NaN in state/motion_pct/br_state/win_s erfüllt keine der Bedingungen; win_s None = Feld fehlt
    (Ruling R10: win_s == 0 zählt als Bewegung). frames: Frames seit Start/Rezoom (@DSP snap_frame); win_s == 0 zählt
    nur ab MIN_WIN_FRAMES, sonst ist es die Anlaufzeit nach einem Rezoom (kein Fenster), keine Bewegung. None = nicht
    prüfen."""
    t_s, ok = np.asarray(t_s, float), np.asarray(ok, float) > 0
    motion_pct, br_state, state = (np.asarray(x, float) for x in (motion_pct, br_state, state))
    win_s = np.full(ok.size, np.nan) if win_s is None else np.asarray(win_s, float)
    frames = np.full(ok.size, np.inf) if frames is None else np.asarray(frames, float)
    karte_aus = (win_s == 0) & (frames >= MIN_WIN_FRAMES)  # Puls von der Bewegungskarte abgeschaltet
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
            j += 1  # [i, j) ungültig
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
    """anteil_gueltig_pct (Reports mit ok, ohne Zustand 0), ursachen {Ursache: n, median_s, summe_s}, spruenge."""
    ok, state = np.asarray(ok, float) > 0, np.asarray(state, float)
    person = state != STATE_NO_PERSON  # NaN (kein @STATE) zählt als Person
    share = 100.0 * float(np.mean(ok[person])) if person.any() else math.nan
    causes = {}
    for c in CAUSES:
        d = [x.dur_s for x in drops if x.cause == c]
        causes[c] = {"n": len(d), "median_s": float(np.median(d)) if d else math.nan, "summe_s": float(sum(d))}
    return {"anteil_gueltig_pct": share, "ursachen": causes, "spruenge": count_jumps(hr, ok)}


def header_with_defines(src_dir: pathlib.Path, defines: list[str], dest_dir: pathlib.Path) -> pathlib.Path:
    """Kopie von a121_esp_dsp.h aus src_dir mit vorangestellten #define-Zeilen nach dest_dir; Rückgabe dest_dir.
    "NAME" → `#define NAME`, "NAME=WERT" → `#define NAME WERT`."""
    head = "".join(f"#define {d.replace('=', ' ', 1)}\n" for d in defines)
    (dest_dir / HEADER_NAME).write_text(head + (src_dir / HEADER_NAME).read_text(encoding="utf-8"), encoding="utf-8")
    return dest_dir


def evaluate_folder(folder: pathlib.Path, header_dir: pathlib.Path | None,
                    keine_person_zaehlen: bool = False) -> tuple[dict, list[Dropout]]:
    """Eine Aufnahme (ggf. neu gerechnet) auswerten. @VITAL gibt es nur für gültige Reports; jede Zeile gehört zum
    vorangehenden @DSP (wie vital_eval.evaluate), daher Zuordnung über die Empfangszeit, nicht über den Index."""
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
                     d["snap_frame"])
    return summary(t_s, ok, hr, d["state"], drops), drops


def _f(x: float) -> str:
    return "–" if math.isnan(x) else f"{x:.1f}"


def format_folder(name: str, s: dict) -> str:
    c = s["ursachen"]
    total = sum(v["n"] for v in c.values())
    out = (f"{name}: gültig {_f(s['anteil_gueltig_pct'])} % (ohne Zustand 0), Ausfälle {total}, "
           f"Bewegungs-Ausfälle {c['bewegung']['n']}, Sprünge > 15 /min {s['spruenge']}")
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
        for folder in a.folders:
            s, _ = evaluate_folder(folder, header_dir, a.keine_person_zaehlen)
            print(format_folder(folder.name, s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
