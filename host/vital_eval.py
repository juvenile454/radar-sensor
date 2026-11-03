#!/usr/bin/env python3
"""Evaluation of iq3 recordings against a reference (ECG chest strap, metronome, markers); docs/VITAL_AUDIT.md, V0.4.

Recording folder in the format of the app or host/esp_record.py (docs/ANDROID_APP.md):
  stream.log   lines as from the ESP32 (@IQ3_START, @DSP, @VITAL, @REZOOM, @RECAL, …)
  rxtime.csv   line,rx_ms (receive time per line, ms since recording start)
  ref_hr.csv   rx_ms,hr_bpm,rr_ms,contact (chest strap via BLE, one line per RR value; without RR: rr_ms empty)
  events.csv   rx_ms,text (markers: ein, anhalten_start/_ende, bewegung_start/_ende, zaehl_start/_ende, metronom_<n>)

Output per session and in total: pulse (MAE, share within ±5/±10, Bland-Altman, availability, confidence calibration),
breathing against metronome, apnea detection against markers (delay, false alarms), time to the first value (start/rezoom),
delay relative to the reference.

  .venv/bin/python host/vital_eval.py <ordner>… [--md bericht.md]
  .venv/bin/python host/vital_eval.py <ordner>… --replay firmware/esp32s3/src   # recompute stream.log with this header

With --replay, stream.log (needs @RAW, i.e. `iq3 … raw`) is recomputed with host/dsp_replay/ and the a121_esp_dsp.h from the
given folder; this allows comparing header variants on the same data. Evaluate test data (day B)
only for the acceptance of a phase.
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

# @DSP fields after the tag; new fields are only appended at the end (missing = NaN)
DSP_FIELDS = ["t_ms", "ok", "hr_bin", "conf", "compute_us", "motion_pct", "hr_harm",
              "point_agree_pct", "br_state", "n_delayed", "n_sat", "n_tickgap", "snap_frame",
              "n_eff", "win_s", "reacq"]
# n_eff: effective number of points, only with compile switch A121_DSP_COMBINE (V3.1), otherwise NaN; win_s (pulse window in s) and reacq
# (0 normal, 1 short window after motion, 2 reacquired via memory) only with A121_DSP_REACQ (V6), otherwise NaN
BR_STATE_APNEA = 2
START_TAGS = ("@IQ3_START", "@REZOOM", "@RECAL")


@dataclasses.dataclass
class Recording:
    folder: pathlib.Path
    vital: dict[str, np.ndarray]     # rx_ms + VITAL_FIELDS
    dsp: dict[str, np.ndarray]       # rx_ms + DSP_FIELDS
    beats_ms: np.ndarray             # beat times of the reference (rx time axis)
    rr_ms: np.ndarray                # RR interval that ends at the respective beat
    events: list[tuple[float, str]]
    starts_ms: list[float]           # rx_ms of @IQ3_START, @REZOOM, @RECAL (restart of the history)
    end_ms: float                    # last receive time
    n_recal: int = 0                 # @RECAL lines (recalibration, V1.5)
    breath: dict | None = None       # @BREATH (V3.4): rx_ms, t_ms, interval_ms, depth_um, rate
    # Pulse values of the reference (column hr_bpm); used only without RR (strap/watch without RR intervals)
    ref_rx_ms: np.ndarray = dataclasses.field(default_factory=lambda: np.empty(0))
    ref_bpm: np.ndarray = dataclasses.field(default_factory=lambda: np.empty(0))


# ---------------------------------------------------------------------------------------------------------
# Reading in
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
    """Beat times from ref_hr.csv: lines with the same rx_ms are one notification; its last beat lies at
    rx_ms, the earlier ones summed backwards. Returns (beat times, RR per beat), sorted."""
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
    """@VITAL and @DSP with receive time; points in time at which the history starts over. dsp["state"]: state from the
    first @STATE line after the @DSP (V3.5, same report; before the final review the ESP32 stamped them with
    its own millis(), so the time is not usable for assignment; without @STATE NaN)."""
    vital, dsp, starts, states = [], [], [], []
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
            states.append(math.nan)
        elif tag == "@STATE":
            v = [_num(x) for x in ln.split(",")[1:3]]
            if len(v) == 2 and states and math.isnan(states[-1]):
                states[-1] = v[1]
        elif tag in START_TAGS:
            starts.append(float(t_rx))
    va = np.array(vital, float).reshape(-1, len(VITAL_FIELDS) + 1)
    da = np.array(dsp, float).reshape(-1, len(DSP_FIELDS) + 1)
    vd = {k: va[:, i] for i, k in enumerate(["rx_ms"] + VITAL_FIELDS)}
    dd = {k: da[:, i] for i, k in enumerate(["rx_ms"] + DSP_FIELDS)}
    dd["state"] = np.array(states, float)
    return vd, dd, starts


BREATH_FIELDS = ["t_ms", "tick", "interval_ms", "depth_um", "rate"]


def parse_breath(lines: list[str], rx: np.ndarray) -> dict[str, np.ndarray]:
    """@BREATH,t_ms,tick,intervall_ms,tiefe_um,rate (V3.4) with receive time."""
    rows = []
    for i, ln in enumerate(lines):
        if ln.startswith("@BREATH,"):
            v = [_num(x) for x in ln.split(",")[1:6]]
            if len(v) == 5:
                rows.append([rx[i] if i < rx.size else math.nan] + v)
    a = np.array(rows, float).reshape(-1, 6)
    return {k: a[:, i] for i, k in enumerate(["rx_ms"] + BREATH_FIELDS)}


def load_recording(folder: pathlib.Path, lines: list[str] | None = None) -> Recording:
    """lines: recomputed lines instead of stream.log (--replay); their rx_ms comes from the original via t_ms."""
    folder = pathlib.Path(folder)
    orig = (folder / "stream.log").read_text(errors="replace").splitlines()
    rx = np.array([_num(r[1]) for r in _read_csv(folder / "rxtime.csv")], float)
    vd, dd, starts = parse_lines(orig, rx)
    br = parse_breath(orig, rx)
    if lines is not None:
        vd, dd, _ = parse_lines(lines, np.full(len(lines), math.nan))
        br = parse_breath(lines, np.full(len(lines), math.nan))
        ref_t, ref_rx = _t_to_rx(orig, rx)
        for x in (vd, dd, br):
            x["rx_ms"] = np.interp(x["t_ms"], ref_t, ref_rx) if ref_t.size else x["t_ms"]
    ref = _read_csv(folder / "ref_hr.csv")
    beats, rr = beats_from_ref(np.array([_num(r[0]) for r in ref], float),
                               np.array([_num(r[2]) if len(r) > 2 else math.nan for r in ref], float))
    ref_rx = np.array([_num(r[0]) for r in ref], float)
    ref_bpm = np.array([_num(r[1]) if len(r) > 1 else math.nan for r in ref], float)
    keep = np.isfinite(ref_rx) & np.isfinite(ref_bpm) & (ref_bpm > 0)
    events = [(_num(r[0]), ",".join(r[1:]).strip()) for r in _read_csv(folder / "events.csv")]
    end = float(np.nanmax(rx)) if rx.size else 0.0
    n_recal = sum(1 for ln in orig if ln.startswith("@RECAL,"))
    return Recording(folder, vd, dd, beats, rr, events, starts, end, n_recal, br, ref_rx[keep], ref_bpm[keep])


def _t_to_rx(lines: list[str], rx: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Mapping ESP32 time (t_ms of the @DSP lines) → receive time, for recomputed lines."""
    t, r = [], []
    for i, ln in enumerate(lines):
        if ln.startswith("@DSP,") and i < rx.size:
            t.append(_num(ln.split(",")[1]))
            r.append(rx[i])
    return np.array(t, float), np.array(r, float)


# ---------------------------------------------------------------------------------------------------------
# Reference values
# ---------------------------------------------------------------------------------------------------------

def ref_hr_at(beats_ms: np.ndarray, t_end_ms: float, win_s: float = 15.0, rr_ms: np.ndarray | None = None) -> float:
    """Reference pulse in the window (t_end − win, t_end] like the pulse window of the DSP: 60,000 / mean IBI. With rr_ms
    the intervals of the beats in the window measured by the strap count (independent of the BLE latency of the timestamps),
    otherwise the spacings of the beat times. NaN with fewer than 3 beats."""
    m = (beats_ms > t_end_ms - win_s * 1000.0) & (beats_ms <= t_end_ms)
    if m.sum() < 3:
        return math.nan
    if rr_ms is not None:
        ibi = rr_ms[m][1:]
    else:
        ibi = np.diff(beats_ms[m])
    return 60000.0 / float(np.mean(ibi))


def ref_bpm_at(rx_ms: np.ndarray, bpm: np.ndarray, t_end_ms: float, win_s: float = 15.0, min_n: int = 2) -> float:
    """Reference pulse without beats (pulse values only, e.g. strap without RR or watch): mean of the values in the window (t_end − win, t_end].
    Such devices smooth over a few seconds themselves; the value is coarser than the one from strap RR. NaN with fewer than min_n
    values in the window."""
    m = (rx_ms > t_end_ms - win_s * 1000.0) & (rx_ms <= t_end_ms)
    if m.sum() < min_n:
        return math.nan
    return float(np.mean(bpm[m]))


def ref_ibi_at(beats_ms: np.ndarray, rr_ms: np.ndarray, t_end_ms: float, win_s: float = 15.0) -> tuple[float, float]:
    """Strap in the window (t_end − win, t_end] (V1.6/V3.6): median of the RR and RMSSD of consecutive RR (ms). The first
    RR in the window starts before the window and does not count (like ref_hr_at). NaN with fewer than 3 RR."""
    m = (beats_ms > t_end_ms - win_s * 1000.0) & (beats_ms <= t_end_ms)
    rr = rr_ms[m][1:]
    if rr.size < 3:
        return math.nan, math.nan
    return float(np.median(rr)), float(np.sqrt(np.mean(np.diff(rr) ** 2)))


def br_ref_at(marks_ms: np.ndarray, t_end_ms: float, win_s: float = 15.0, tol: float = 0.05) -> float:
    """Metronome breathing rate at the window end: intervals of the inhale markers in the window, at least the last two.
    NaN if the intervals differ by more than tol (tempo change) or too few markers are present."""
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
    """Sections from <name>_start / <name>_ende; an open section ends at end_ms."""
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
# Key figures
# ---------------------------------------------------------------------------------------------------------

def metrics(est: np.ndarray, ref: np.ndarray, valid: np.ndarray) -> dict:
    """Pulse against reference. coverage_pct = share of valid reports among all; errors only over valid reports with
    reference. Bland-Altman limits = bias ± 1.96 SD."""
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


def count_jumps(hr: np.ndarray, valid: np.ndarray, thr: float = 15.0) -> int:
    """Jumps > thr /min between consecutive valid reports (V3.2); an invalid one interrupts."""
    hr, valid = np.asarray(hr, float), np.asarray(valid, bool)
    both = valid[1:] & valid[:-1]
    return int(np.sum(both & (np.abs(np.diff(hr)) > thr)))


def calibration(conf: np.ndarray, abs_err: np.ndarray, edges=(0, 30, 50, 70, 90, 101)) -> list[tuple]:
    """Per confidence class [lo, hi): (lo, hi, count, share within ±5 /min in %). Empty class: share NaN."""
    conf, abs_err = np.asarray(conf, float), np.asarray(abs_err, float)
    out = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (conf >= lo) & (conf < hi) & np.isfinite(abs_err)
        n = int(m.sum())
        out.append((lo, hi, n, 100.0 * float(np.mean(abs_err[m] <= 5.0)) if n else math.nan))
    return out


def apnea_eval(t_ms: np.ndarray, state: np.ndarray, holds: list[tuple[float, float]], grace_s: float = 10.0) -> dict:
    """Breathing state (br_state per report) against breath-hold sections. Detected = state 2 within the section
    (up to grace_s after it); delay from the section start. False alarm = contiguous run with state 2 that touches no
    section (± grace_s)."""
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
    """For each start (iq3, rezoom, recalibration): seconds until the next @VITAL, even if another
    start lies in between (there was no value for that long). Starts without a following value are missing from the list."""
    out = []
    for s in starts_ms:
        v = vital_rx_ms[vital_rx_ms > s]
        if v.size:
            out.append((float(v[0]) - s) / 1000.0)
    return out


def best_lag_s(t_ms: np.ndarray, est: np.ndarray, ref_fn, max_lag_s: float = 20.0, step_s: float = 0.5) -> float:
    """Delay of the estimate relative to the reference: shift at which MAE(est(t), ref(t − lag)) is smallest."""
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
# Report
# ---------------------------------------------------------------------------------------------------------

def beats_vs_strap(rec: Recording, t: np.ndarray, ok: np.ndarray, win_s: float) -> dict:
    """IBI median (@VITAL ibi_ms) and RMSSD (rmssd_ms, 0 = not computed) per valid report against the strap in the same
    window. Without individual beats the DSP writes 60,000 / pulse as IBI and the pulse as hr_ibi; such reports do not
    count. Returns: number of reports, median |IBI − strap| (ms), number with RMSSD, median |RMSSD − strap| and median
    RMSSD / strap."""
    v = rec.vital
    ibi, rms, hr_ibi, hr = (np.full(t.size, math.nan) for _ in range(4))
    if t.size and v["rx_ms"].size:
        idx = np.searchsorted(t, v["rx_ms"], side="right") - 1
        g = idx >= 0
        for arr, k in ((ibi, "ibi_ms"), (rms, "rmssd_ms"), (hr_ibi, "hr_ibi"), (hr, "hr_fused")):
            arr[idx[g]] = v[k][g]
    fallback = (np.abs(hr_ibi - hr) < 0.05) & (np.abs(ibi - 60000.0 / np.where(hr > 0, hr, math.nan)) < 0.6)
    ref = np.array([ref_ibi_at(rec.beats_ms, rec.rr_ms, x, win_s) for x in t]).reshape(-1, 2)
    m = ok & np.isfinite(ibi) & np.isfinite(ref[:, 0]) & ~fallback
    mr = m & (rms > 0) & np.isfinite(ref[:, 1]) & (ref[:, 1] > 0)
    e_ibi = np.abs(ibi[m] - ref[m, 0])
    return {"n": int(m.sum()), "ibi_mae_ms": float(np.median(e_ibi)) if e_ibi.size else math.nan,
            "n_rmssd": int(mr.sum()),
            "rmssd_mae_ms": float(np.median(np.abs(rms[mr] - ref[mr, 1]))) if mr.any() else math.nan,
            "rmssd_ratio": float(np.median(rms[mr] / ref[mr, 1])) if mr.any() else math.nan}


def evaluate(rec: Recording, win_s: float = 15.0) -> dict:
    """All key figures of a session. Reference time of a report = receipt of its @DSP line (end of the window)."""
    d = rec.dsp
    t = d["rx_ms"]
    ok = d["ok"] > 0
    # Pulse value per report: @VITAL belongs to the preceding @DSP line
    hr = np.full(t.size, math.nan)
    br = np.full(t.size, math.nan)
    if t.size and rec.vital["rx_ms"].size:
        idx = np.searchsorted(t, rec.vital["rx_ms"], side="right") - 1
        good = idx >= 0
        hr[idx[good]] = rec.vital["hr_fused"][good]
        br[idx[good]] = rec.vital["br"][good]
    ok &= np.isfinite(hr)
    rr = rec.rr_ms if rec.rr_ms.size == rec.beats_ms.size else None
    if rec.beats_ms.size >= 3:  # strap with RR: beats
        ref_kind = "rr"

        def ref_fn(x):
            return ref_hr_at(rec.beats_ms, x, win_s, rr)
    elif rec.ref_bpm.size >= 3:  # pulse values only (without RR): no individual beats, no IBI/RMSSD comparison
        ref_kind, rr = "bpm", None

        def ref_fn(x):
            return ref_bpm_at(rec.ref_rx_ms, rec.ref_bpm, x, win_s)
    else:
        ref_kind = None
    have_ref = ref_kind is not None
    ref = np.array([ref_fn(x) for x in t]) if have_ref else np.full(t.size, math.nan)
    res = {"reports": int(t.size), "valid": int(ok.sum()), "have_ref": have_ref, "ref_kind": ref_kind,
           "jumps": count_jumps(hr, ok)}
    n_eff = d["n_eff"][np.isfinite(d["n_eff"])]
    res["n_eff"] = float(np.median(n_eff)) if n_eff.size else None  # V3.1
    res["hr"] = metrics(hr, ref, ok) if have_ref else None
    if have_ref:
        err = np.abs(hr - ref)
        res["calibration"] = calibration(np.where(ok, d["conf"], math.nan), np.where(ok, err, math.nan))
        if np.nanmax(ref) - np.nanmin(ref) >= 15.0:  # delay only if the pulse changes markedly (R4)
            res["lag_s"] = best_lag_s(t[ok], hr[ok], lambda tt: np.array([ref_fn(x) for x in tt]))
    if have_ref and rr is not None:  # V1.6/V3.6 (day A): individual beats against strap RR per report
        res["ibi_ref"] = beats_vs_strap(rec, t, ok, win_s)
    marks = np.array([tm for tm, text in rec.events if text == "ein"], float)
    if marks.size >= 3:
        bref = np.array([br_ref_at(marks, x, win_s) for x in t])
        m = ok & np.isfinite(bref) & np.isfinite(br)
        e = np.abs(br[m] - bref[m])
        res["br"] = {"n": int(m.sum()), "mae": float(np.mean(e)) if e.size else math.nan,
                     "p1": 100.0 * float(np.mean(e <= 1.0)) if e.size else math.nan}
    holds = intervals(rec.events, "anhalten", rec.end_ms)
    b = rec.breath
    if b is not None and b["rx_ms"].size:  # V3.4: rate per breath against the metronome (R3)
        bt = b["rx_ms"]
        be = {"n": int(bt.size), "n_ref": 0, "mae": math.nan, "p1": math.nan,
              "first_s": first_value_delays(rec.starts_ms, bt),
              "in_hold": int(sum(((bt >= a) & (bt <= z)).sum() for a, z in holds))}
        if marks.size >= 3:
            bref = np.array([br_ref_at(marks, x, win_s) for x in bt])
            m = np.isfinite(bref) & np.isfinite(b["rate"])
            e = np.abs(b["rate"][m] - bref[m])
            be.update(n_ref=int(m.sum()), mae=float(np.mean(e)) if e.size else math.nan,
                      p1=100.0 * float(np.mean(e <= 1.0)) if e.size else math.nan)
        res["breath_events"] = be
    if holds:
        if np.isfinite(d["br_state"]).any():
            res["apnea"] = apnea_eval(t, d["br_state"], holds)
        else:
            res["apnea"] = "kein br_state in @DSP (Firmware vor V1.4)"
    moves = intervals(rec.events, "bewegung", rec.end_ms)
    if moves and np.isfinite(d["state"]).any():  # V3.5: motion sections as BEWEGUNG (motion; up to 15 s afterwards in the window)
        in_mot = np.zeros(t.size, bool)
        for a, b in moves:
            in_mot |= (t > a) & (t <= b + win_s * 1000.0)
        st4 = d["state"] == 4
        res["motion_state"] = {"reports_in": int(in_mot.sum()), "motion_in": int((st4 & in_mot).sum()),
                               "motion_out": int((st4 & ~in_mot).sum())}
    res["first_s"] = first_value_delays(rec.starts_ms, rec.vital["rx_ms"])
    # Sensor flags (V1.5): sums of the counters per report; older firmware without the fields → None
    if any(np.isfinite(d[k]).any() for k in ("n_delayed", "n_sat", "n_tickgap")):
        res["sensor"] = {k: int(np.nansum(d[k])) for k in ("n_delayed", "n_sat", "n_tickgap")}
        res["sensor"]["recal"] = rec.n_recal
    return res


def _f(x, fmt="{:.1f}") -> str:
    return "–" if x is None or (isinstance(x, float) and math.isnan(x)) else fmt.format(x)


def format_report(name: str, r: dict) -> str:
    out = io.StringIO()
    w = out.write
    w(f"## {name}\n\n")
    w(f"Reports {r['reports']}, gültig {r['valid']}, Sprünge > 15 /min {r.get('jumps', 0)}")
    if r.get("n_eff") is not None:
        w(f"; effektive Punktzahl (V3.1) {_f(r['n_eff'])}")
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
        if r.get("ref_kind") == "bpm":
            w("| Referenz | nur Pulswerte ohne RR (Gurt ohne RR oder Uhr); „Gurt“ unten = diese Referenz |\n")
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
    if "ibi_ref" in r:
        b = r["ibi_ref"]
        w(f"Einzelschläge gegen Gurt-RR (V1.6/V3.6, je Report im 15-s-Fenster): IBI-Median-Fehler "
          f"{_f(b['ibi_mae_ms'])} ms ({b['n']} Reports); RMSSD-Fehler {_f(b['rmssd_mae_ms'])} ms, RMSSD / Gurt "
          f"{_f(b['rmssd_ratio'], '{:.2f}')} ({b['n_rmssd']} Reports mit RMSSD; experimentell).\n\n")
    if "br" in r:
        b = r["br"]
        w(f"Atmung gegen Metronom: {b['n']} Reports, MAE {_f(b['mae'])} /min, innerhalb ±1 /min {_f(b['p1'])} %.\n\n")
    if "breath_events" in r:
        b = r["breath_events"]
        w(f"Atmung Atemzug für Atemzug (@BREATH): {b['n']} Atemzüge")
        if b["n_ref"]:
            w(f", gegen Metronom {b['n_ref']}: MAE {_f(b['mae'])} /min, innerhalb ±1 /min {_f(b['p1'])} %")
        if b["first_s"]:
            w(f"; erster nach Start {_f(b['first_s'][0])} s")
        if b["in_hold"]:
            w(f"; {b['in_hold']} in Anhalte-Abschnitten")
        w(".\n\n")
    if "apnea" in r:
        a = r["apnea"]
        if isinstance(a, str):
            w(f"Atem anhalten: {a}.\n\n")
        else:
            d = ", ".join(_f(x) for x in a["delays_s"]) or "–"
            w(f"Atem anhalten: {a['detected']} von {a['episodes']} erkannt, Verzug {d} s, Fehlalarme {a['false_alarms']}.\n\n")
    if "motion_state" in r:
        m = r["motion_state"]
        w(f"Zustand BEWEGUNG: {m['motion_in']} von {m['reports_in']} Reports in Bewegungsabschnitten (bis 15 s danach), "
          f"{m['motion_out']} außerhalb.\n\n")
    if "sensor" in r:
        sn = r["sensor"]
        w(f"Sensor-Flags: verspätete Frames {sn['n_delayed']}, gesättigt {sn['n_sat']}, tick-Lücken {sn['n_tickgap']}, "
          f"Neu-Kalibrierungen {sn['recal']}.\n\n")
    return out.getvalue()


def replay_lines(folder: pathlib.Path, header_dir: pathlib.Path) -> list[str]:
    """Recompute stream.log with the header from header_dir (host/dsp_replay); returns: new lines."""
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
