#!/usr/bin/env python3
"""Auswertung von iq3-Aufnahmen gegen eine Referenz (EKG-Brustgurt, Metronom, Marker); docs/AUDIT_VITAL.md, V0.4.

Aufnahme-Ordner im Format der App bzw. host/esp_record.py (docs/ANDROID_APP.md):
  stream.log   Zeilen wie vom ESP32 (@IQ3_START, @DSP, @VITAL, @REZOOM, @RECAL, …)
  rxtime.csv   line,rx_ms (Empfangszeit je Zeile, ms seit Aufnahmebeginn)
  ref_hr.csv   rx_ms,hr_bpm,rr_ms,contact (Brustgurt über BLE, eine Zeile je RR-Wert; ohne RR: rr_ms leer)
  events.csv   rx_ms,text (Marker: ein, anhalten_start/_ende, bewegung_start/_ende, zaehl_start/_ende, metronom_<n>)

Ausgabe je Sitzung und gesamt: Puls (MAE, Anteil ±5/±10, Bland-Altman, Verfügbarkeit, Konfidenz-Kalibrierung),
Atmung gegen Metronom, Apnoe-Erkennung gegen Marker (Verzug, Fehlalarme), Zeit bis zum ersten Wert (Start/Rezoom),
Verzug gegenüber der Referenz.

  .venv/bin/python host/vital_eval.py <ordner>… [--md bericht.md]
  .venv/bin/python host/vital_eval.py <ordner>… --replay firmware/esp32s3/src   # stream.log mit diesem Header neu rechnen

Mit --replay wird stream.log (braucht @RAW, also `iq3 … raw`) mit host/dsp_replay/ und dem a121_esp_dsp.h aus dem
angegebenen Ordner neu gerechnet; so lassen sich Header-Varianten auf denselben Daten vergleichen. Testdaten (Tag B)
nur zur Abnahme einer Phase auswerten.
"""

from __future__ import annotations

import argparse
import dataclasses
import io
import math
import pathlib
import sys
import tempfile

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HOST))
from a121_esp_ultimate import VITAL_FIELDS, run_replay_cpp  # noqa: E402

# @DSP-Felder nach dem Tag; neue Felder kommen nur hinten dazu (fehlende = NaN)
DSP_FIELDS = ["t_ms", "ok", "hr_bin", "conf", "compute_us", "motion_pct", "hr_harm",
              "point_agree_pct", "br_state", "n_delayed", "n_sat", "n_tickgap", "snap_frame"]
BR_STATE_APNEA = 2
START_TAGS = ("@IQ3_START", "@REZOOM", "@RECAL")


@dataclasses.dataclass
class Recording:
    folder: pathlib.Path
    vital: dict[str, np.ndarray]     # rx_ms + VITAL_FIELDS
    dsp: dict[str, np.ndarray]       # rx_ms + DSP_FIELDS
    beats_ms: np.ndarray             # Schlagzeiten der Referenz (rx-Zeitachse)
    rr_ms: np.ndarray                # RR-Intervall, das am jeweiligen Schlag endet
    events: list[tuple[float, str]]
    starts_ms: list[float]           # rx_ms von @IQ3_START, @REZOOM, @RECAL (Neubeginn der Historie)
    end_ms: float                    # letzte Empfangszeit


# ---------------------------------------------------------------------------------------------------------
# Einlesen
# ---------------------------------------------------------------------------------------------------------

def _read_csv(path: pathlib.Path) -> list[list[str]]:
    if not path.is_file():
        return []
    rows = [ln.split(",") for ln in path.read_text().splitlines()[1:] if ln.strip()]
    return rows


def _num(s: str) -> float:
    try:
        return float(s)
    except ValueError:
        return math.nan


def beats_from_ref(rx_ms: np.ndarray, rr_ms: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Schlagzeiten aus ref_hr.csv: Zeilen mit gleichem rx_ms sind eine Benachrichtigung; ihr letzter Schlag liegt bei
    rx_ms, die früheren rückwärts aufsummiert. Rückgabe (Schlagzeiten, RR je Schlag), sortiert."""
    beats, rrs = [], []
    i = 0
    while i < rx_ms.size:
        j = i
        while j + 1 < rx_ms.size and rx_ms[j + 1] == rx_ms[i]:
            j += 1
        rr = rr_ms[i:j + 1]
        rr = rr[np.isfinite(rr)]
        t = rx_ms[i] - np.concatenate([np.cumsum(rr[::-1])[::-1][1:], [0.0]]) if rr.size else np.array([])
        beats.extend(t.tolist())
        rrs.extend(rr.tolist())
        i = j + 1
    order = np.argsort(beats, kind="stable")
    return np.asarray(beats, float)[order], np.asarray(rrs, float)[order]


def parse_lines(lines: list[str], rx: np.ndarray) -> tuple[dict, dict, list[float]]:
    """@VITAL und @DSP mit Empfangszeit; Zeitpunkte, an denen die Historie neu beginnt."""
    vital, dsp, starts = [], [], []
    for i, ln in enumerate(lines):
        t_rx = rx[i] if i < rx.size else math.nan
        tag = ln.split(",", 1)[0]
        if tag == "@VITAL":
            v = [_num(x) for x in ln.split(",")[1:]]
            if len(v) >= len(VITAL_FIELDS):
                vital.append([t_rx] + v[:len(VITAL_FIELDS)])
        elif tag == "@DSP":
            v = [_num(x) for x in ln.split(",")[1:]][:len(DSP_FIELDS)]
            dsp.append([t_rx] + v + [math.nan] * (len(DSP_FIELDS) - len(v)))
        elif tag in START_TAGS:
            starts.append(float(t_rx))
    va = np.array(vital, float).reshape(-1, len(VITAL_FIELDS) + 1)
    da = np.array(dsp, float).reshape(-1, len(DSP_FIELDS) + 1)
    vd = {k: va[:, i] for i, k in enumerate(["rx_ms"] + VITAL_FIELDS)}
    dd = {k: da[:, i] for i, k in enumerate(["rx_ms"] + DSP_FIELDS)}
    return vd, dd, starts


def load_recording(folder: pathlib.Path, lines: list[str] | None = None) -> Recording:
    """lines: statt stream.log neu gerechnete Zeilen (--replay); deren rx_ms kommt über t_ms aus dem Original."""
    folder = pathlib.Path(folder)
    orig = (folder / "stream.log").read_text(errors="replace").splitlines()
    rx = np.array([_num(r[1]) for r in _read_csv(folder / "rxtime.csv")], float)
    vd, dd, starts = parse_lines(orig, rx)
    if lines is not None:
        vd, dd, _ = parse_lines(lines, np.full(len(lines), math.nan))
        ref_t, ref_rx = _t_to_rx(orig, rx)
        vd["rx_ms"] = np.interp(vd["t_ms"], ref_t, ref_rx) if ref_t.size else vd["t_ms"]
        dd["rx_ms"] = np.interp(dd["t_ms"], ref_t, ref_rx) if ref_t.size else dd["t_ms"]
    ref = _read_csv(folder / "ref_hr.csv")
    beats, rr = beats_from_ref(np.array([_num(r[0]) for r in ref], float),
                               np.array([_num(r[2]) if len(r) > 2 else math.nan for r in ref], float))
    events = [(_num(r[0]), ",".join(r[1:]).strip()) for r in _read_csv(folder / "events.csv")]
    end = float(np.nanmax(rx)) if rx.size else 0.0
    return Recording(folder, vd, dd, beats, rr, events, starts, end)


def _t_to_rx(lines: list[str], rx: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Zuordnung ESP32-Zeit (t_ms der @DSP-Zeilen) → Empfangszeit, für neu gerechnete Zeilen."""
    t, r = [], []
    for i, ln in enumerate(lines):
        if ln.startswith("@DSP,") and i < rx.size:
            t.append(_num(ln.split(",")[1]))
            r.append(rx[i])
    return np.array(t, float), np.array(r, float)


# ---------------------------------------------------------------------------------------------------------
# Referenzwerte
# ---------------------------------------------------------------------------------------------------------

def ref_hr_at(beats_ms: np.ndarray, t_end_ms: float, win_s: float = 15.0, rr_ms: np.ndarray | None = None) -> float:
    """Referenzpuls im Fenster (t_end − win, t_end] wie das Pulsfenster der DSP: 60 000 / mittleres IBI. Mit rr_ms
    zählen die vom Gurt gemessenen Intervalle der Schläge im Fenster (unabhängig von der BLE-Latenz der Zeitpunkte),
    sonst die Abstände der Schlagzeiten. NaN bei weniger als 3 Schlägen."""
    m = (beats_ms > t_end_ms - win_s * 1000.0) & (beats_ms <= t_end_ms)
    if m.sum() < 3:
        return math.nan
    if rr_ms is not None:
        ibi = rr_ms[m][1:]
    else:
        ibi = np.diff(beats_ms[m])
    return 60000.0 / float(np.mean(ibi))


def br_ref_at(marks_ms: np.ndarray, t_end_ms: float, win_s: float = 15.0, tol: float = 0.05) -> float:
    """Metronom-Atemfrequenz am Fensterende: Intervalle der Einatem-Marker im Fenster, mindestens die letzten zwei.
    NaN, wenn die Intervalle um mehr als tol auseinanderliegen (Taktwechsel) oder zu wenige Marker da sind."""
    before = marks_ms[marks_ms <= t_end_ms]
    if before.size < 2:
        return math.nan
    in_win = before[before > t_end_ms - win_s * 1000.0]
    use = before[-max(in_win.size, 2):]
    if in_win.size < 2:
        use = before[-3:] if before.size >= 3 else before[-2:]
    iv = np.diff(use)
    if iv.max() > (1.0 + tol) * iv.min():
        return math.nan
    return 60000.0 / float(np.mean(iv))


def intervals(events: list[tuple[float, str]], name: str, end_ms: float) -> list[tuple[float, float]]:
    """Abschnitte aus <name>_start / <name>_ende; ein offener Abschnitt endet bei end_ms."""
    out, start = [], None
    for t, text in sorted(events):
        if text == f"{name}_start" and start is None:
            start = t
        elif text == f"{name}_ende" and start is not None:
            out.append((start, t))
            start = None
    if start is not None:
        out.append((start, end_ms))
    return out


# ---------------------------------------------------------------------------------------------------------
# Kennzahlen
# ---------------------------------------------------------------------------------------------------------

def metrics(est: np.ndarray, ref: np.ndarray, valid: np.ndarray) -> dict:
    """Puls gegen Referenz. coverage_pct = Anteil gültiger Reports an allen; Fehler nur über gültige Reports mit
    Referenz. Bland-Altman-Grenzen = bias ± 1,96 SD."""
    est, ref, valid = np.asarray(est, float), np.asarray(ref, float), np.asarray(valid, bool)
    use = valid & np.isfinite(ref) & np.isfinite(est)
    e = est[use] - ref[use]
    n = int(e.size)
    nan = math.nan
    sd = float(np.std(e, ddof=1)) if n > 1 else nan
    bias = float(np.mean(e)) if n else nan
    return {
        "n": n,
        "coverage_pct": 100.0 * float(np.mean(valid)) if valid.size else nan,
        "mae": float(np.mean(np.abs(e))) if n else nan,
        "rmse": float(np.sqrt(np.mean(e ** 2))) if n else nan,
        "bias": bias,
        "loa_low": bias - 1.96 * sd if n > 1 else nan,
        "loa_high": bias + 1.96 * sd if n > 1 else nan,
        "p5": 100.0 * float(np.mean(np.abs(e) <= 5.0)) if n else nan,
        "p10": 100.0 * float(np.mean(np.abs(e) <= 10.0)) if n else nan,
    }


def calibration(conf: np.ndarray, abs_err: np.ndarray, edges=(0, 30, 50, 70, 90, 101)) -> list[tuple]:
    """Je Konfidenzklasse [lo, hi): (lo, hi, Anzahl, Anteil innerhalb ±5 /min in %). Leere Klasse: Anteil NaN."""
    conf, abs_err = np.asarray(conf, float), np.asarray(abs_err, float)
    out = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (conf >= lo) & (conf < hi) & np.isfinite(abs_err)
        n = int(m.sum())
        out.append((lo, hi, n, 100.0 * float(np.mean(abs_err[m] <= 5.0)) if n else math.nan))
    return out


def apnea_eval(t_ms: np.ndarray, state: np.ndarray, holds: list[tuple[float, float]], grace_s: float = 10.0) -> dict:
    """Atemzustand (br_state je Report) gegen Anhalte-Abschnitte. Erkannt = Zustand 2 innerhalb des Abschnitts
    (bis grace_s danach); Verzug ab Abschnittsbeginn. Fehlalarm = zusammenhängende Folge mit Zustand 2, die keinen
    Abschnitt (± grace_s) berührt."""
    t_ms, apn = np.asarray(t_ms, float), np.asarray(state) == BR_STATE_APNEA
    delays = []
    for a, b in holds:
        hit = t_ms[apn & (t_ms >= a) & (t_ms <= b + grace_s * 1000.0)]
        if hit.size:
            delays.append((hit[0] - a) / 1000.0)
    false_alarms = 0
    i = 0
    while i < apn.size:
        if not apn[i]:
            i += 1
            continue
        j = i
        while j + 1 < apn.size and apn[j + 1]:
            j += 1
        t0, t1 = t_ms[i], t_ms[j]
        if not any(t1 >= a and t0 <= b + grace_s * 1000.0 for a, b in holds):
            false_alarms += 1
        i = j + 1
    return {"episodes": len(holds), "detected": len(delays), "delays_s": delays, "false_alarms": false_alarms}


def first_value_delays(starts_ms: list[float], vital_rx_ms: np.ndarray) -> list[float]:
    """Je Start (iq3, Rezoom, Neu-Kalibrierung): Sekunden bis zum nächsten @VITAL, auch wenn dazwischen ein weiterer
    Start liegt (so lange gab es keinen Wert). Starts ohne folgenden Wert fehlen in der Liste."""
    out = []
    for s in starts_ms:
        v = vital_rx_ms[vital_rx_ms > s]
        if v.size:
            out.append((float(v[0]) - s) / 1000.0)
    return out


def best_lag_s(t_ms: np.ndarray, est: np.ndarray, ref_fn, max_lag_s: float = 20.0, step_s: float = 0.5) -> float:
    """Verzug der Schätzung gegenüber der Referenz: Verschiebung, bei der MAE(est(t), ref(t − lag)) am kleinsten ist."""
    best, best_err = math.nan, math.inf
    for lag in np.arange(0.0, max_lag_s + 1e-9, step_s):
        r = np.asarray(ref_fn(t_ms - lag * 1000.0), float)
        m = np.isfinite(r) & np.isfinite(est)
        if m.sum() < 5:
            continue
        err = float(np.mean(np.abs(est[m] - r[m])))
        if err < best_err - 1e-9:
            best, best_err = float(lag), err
    return best


# ---------------------------------------------------------------------------------------------------------
# Bericht
# ---------------------------------------------------------------------------------------------------------

def evaluate(rec: Recording, win_s: float = 15.0) -> dict:
    """Alle Kennzahlen einer Sitzung. Bezugszeit eines Reports = Empfang seiner @DSP-Zeile (Ende des Fensters)."""
    d = rec.dsp
    t = d["rx_ms"]
    ok = d["ok"] > 0
    # Pulswert je Report: @VITAL gehört zur vorangehenden @DSP-Zeile
    hr = np.full(t.size, math.nan)
    br = np.full(t.size, math.nan)
    if t.size and rec.vital["rx_ms"].size:
        idx = np.searchsorted(t, rec.vital["rx_ms"], side="right") - 1
        good = idx >= 0
        hr[idx[good]] = rec.vital["hr_fused"][good]
        br[idx[good]] = rec.vital["br"][good]
    ok &= np.isfinite(hr)
    have_ref = rec.beats_ms.size >= 3
    rr = rec.rr_ms if rec.rr_ms.size == rec.beats_ms.size else None
    ref = np.array([ref_hr_at(rec.beats_ms, x, win_s, rr) for x in t]) if have_ref else np.full(t.size, math.nan)
    res = {"reports": int(t.size), "valid": int(ok.sum()), "have_ref": have_ref}
    res["hr"] = metrics(hr, ref, ok) if have_ref else None
    if have_ref:
        err = np.abs(hr - ref)
        res["calibration"] = calibration(np.where(ok, d["conf"], math.nan), np.where(ok, err, math.nan))
        if np.nanmax(ref) - np.nanmin(ref) >= 15.0:  # Verzug nur, wenn sich der Puls deutlich ändert (R4)
            res["lag_s"] = best_lag_s(t[ok], hr[ok],
                                      lambda tt: np.array([ref_hr_at(rec.beats_ms, x, win_s, rr) for x in tt]))
    marks = np.array([tm for tm, text in rec.events if text == "ein"], float)
    if marks.size >= 3:
        bref = np.array([br_ref_at(marks, x, win_s) for x in t])
        m = ok & np.isfinite(bref) & np.isfinite(br)
        e = np.abs(br[m] - bref[m])
        res["br"] = {"n": int(m.sum()), "mae": float(np.mean(e)) if e.size else math.nan,
                     "p1": 100.0 * float(np.mean(e <= 1.0)) if e.size else math.nan}
    holds = intervals(rec.events, "anhalten", rec.end_ms)
    if holds:
        if np.isfinite(d["br_state"]).any():
            res["apnea"] = apnea_eval(t, d["br_state"], holds)
        else:
            res["apnea"] = "kein br_state in @DSP (Firmware vor V1.4)"
    res["first_s"] = first_value_delays(rec.starts_ms, rec.vital["rx_ms"])
    return res


def _f(x, fmt="{:.1f}") -> str:
    return "–" if x is None or (isinstance(x, float) and math.isnan(x)) else fmt.format(x)


def format_report(name: str, r: dict) -> str:
    out = io.StringIO()
    w = out.write
    w(f"## {name}\n\n")
    w(f"Reports {r['reports']}, gültig {r['valid']}")
    fs = r["first_s"]
    if fs:
        w(f"; erster Wert nach Start {_f(fs[0])} s")
        if len(fs) > 1:
            w(f", nach Rezoom/Neu-Kalibrierung Median {_f(float(np.median(fs[1:])))} s ({len(fs) - 1}×)")
    w("\n\n")
    if r["hr"] is None:
        w("Puls: keine Referenz (ref_hr.csv fehlt oder leer).\n\n")
    else:
        h = r["hr"]
        w("| Puls | Wert |\n|---|---|\n")
        w(f"| Reports mit Referenz | {h['n']} |\n| Verfügbarkeit | {_f(h['coverage_pct'])} % |\n")
        w(f"| MAE / RMSE | {_f(h['mae'])} / {_f(h['rmse'])} /min |\n| Bias (Radar − Gurt) | {_f(h['bias'])} /min |\n")
        w(f"| Bland-Altman-Grenzen | {_f(h['loa_low'])} … {_f(h['loa_high'])} /min |\n")
        w(f"| innerhalb ±5 / ±10 /min | {_f(h['p5'])} / {_f(h['p10'])} % |\n")
        if "lag_s" in r:
            w(f"| Verzug gegenüber Gurt | {_f(r['lag_s'])} s |\n")
        w("\n| Konfidenz | Reports | innerhalb ±5 /min |\n|---|---|---|\n")
        for lo, hi, n, p in r["calibration"]:
            w(f"| {lo}–{min(hi, 100)} % | {n} | {_f(p)} % |\n")
        w("\n")
    if "br" in r:
        b = r["br"]
        w(f"Atmung gegen Metronom: {b['n']} Reports, MAE {_f(b['mae'])} /min, innerhalb ±1 /min {_f(b['p1'])} %.\n\n")
    if "apnea" in r:
        a = r["apnea"]
        if isinstance(a, str):
            w(f"Atem anhalten: {a}.\n\n")
        else:
            d = ", ".join(_f(x) for x in a["delays_s"]) or "–"
            w(f"Atem anhalten: {a['detected']} von {a['episodes']} erkannt, Verzug {d} s, Fehlalarme {a['false_alarms']}.\n\n")
    return out.getvalue()


def replay_lines(folder: pathlib.Path, header_dir: pathlib.Path) -> list[str]:
    """stream.log mit dem Header aus header_dir neu rechnen (host/dsp_replay); Rückgabe: neue Zeilen."""
    with tempfile.TemporaryDirectory() as d:
        out = pathlib.Path(d) / "neu"
        run_replay_cpp(folder / "stream.log", out, header_dir)
        return out.with_suffix(".log").read_text().splitlines()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folders", nargs="+", type=pathlib.Path)
    ap.add_argument("--replay", type=pathlib.Path, default=None, help="Ordner mit a121_esp_dsp.h: neu rechnen")
    ap.add_argument("--md", type=pathlib.Path, default=None, help="Bericht zusätzlich als Markdown-Datei")
    ap.add_argument("--win", type=float, default=15.0, help="Referenzfenster in s (wie das Pulsfenster)")
    a = ap.parse_args()
    parts = ["# Vital-Auswertung gegen Referenz\n\n"]
    if a.replay:
        parts.append(f"Neu gerechnet mit `{a.replay}`.\n\n")
    all_hr = []
    for folder in a.folders:
        lines = replay_lines(folder, a.replay) if a.replay else None
        rec = load_recording(folder, lines)
        r = evaluate(rec, a.win)
        parts.append(format_report(folder.name, r))
        if r["hr"] is not None:
            all_hr.append(r)
    if len(all_hr) > 1:
        n = sum(r["hr"]["n"] for r in all_hr)
        mae = sum(r["hr"]["mae"] * r["hr"]["n"] for r in all_hr if r["hr"]["n"]) / max(n, 1)
        parts.append(f"## Gesamt\n\n{len(all_hr)} Sitzungen, {n} Reports mit Referenz, Puls-MAE {_f(mae)} /min.\n")
    text = "".join(parts)
    print(text)
    if a.md:
        a.md.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
