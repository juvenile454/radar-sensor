"""Tests for host/reacq_eval.py (pulse dropouts after motion on recordings, V6).

  .venv/bin/python -m unittest host/tests/test_reacq_eval.py
"""

import math
import pathlib
import sys
import tempfile
import unittest

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST))

import reacq_eval as re_  # noqa: E402

NAN = math.nan


def _args(ok, motion=None, br=None, state=None):
    n = len(ok)
    t = np.arange(n, dtype=float)
    return (t, np.array(ok, float),
            np.array(motion if motion is not None else [0.0] * n, float),
            np.array(br if br is not None else [0.0] * n, float),
            np.array(state if state is not None else [NAN] * n, float))


class DropoutsTest(unittest.TestCase):
    def test_dropout_after_valid_only(self):
        # leading invalid reports do not count (never valid yet); then a dropout of 3 reports until the next valid one
        d = re_.dropouts(*_args([0, 0, 1, 1, 0, 0, 0, 1]))
        self.assertEqual(len(d), 1)
        self.assertEqual(d[0].start_s, 4.0)
        self.assertAlmostEqual(d[0].dur_s, 3.0)
        self.assertEqual(d[0].cause, "rest")

    def test_cause_motion(self):
        d = re_.dropouts(*_args([1, 0, 0, 0, 1], motion=[0, 0, 45, 0, 0]))
        self.assertEqual(d[0].cause, "bewegung")
        d = re_.dropouts(*_args([1, 0, 0, 0, 1], state=[2, 2, 4, 2, 2]))
        self.assertEqual(d[0].cause, "bewegung")
        # motion only outside the dropout does not count
        d = re_.dropouts(*_args([1, 0, 0, 1, 1], motion=[90, 0, 0, 90, 0]))
        self.assertEqual(d[0].cause, "rest")

    def test_cause_motion_from_win_s_zero(self):
        # Ruling R10 (V6): a report without a pulse window (win_s == 0, motion map) turns the dropout into motion;
        # a missing field (NaN, recording without A121_DSP_REACQ) and win_s > 0 do not
        ok = [1, 0, 0, 0, 1]
        d = re_.dropouts(*_args(ok, br=[0, 1, 1, 1, 0]), win_s=np.array([15, 0, 6, 7, 15], float))
        self.assertEqual(d[0].cause, "bewegung")
        d = re_.dropouts(*_args(ok, br=[0, 1, 1, 1, 0]), win_s=np.array([15, 6, 7, 8, 9], float))
        self.assertEqual(d[0].cause, "keine_atemspitze")
        d = re_.dropouts(*_args(ok), win_s=np.full(5, NAN))
        self.assertEqual(d[0].cause, "rest")
        d = re_.dropouts(*_args(ok, motion=[0, 0, 0, 0, 0]))   # without win_s as before
        self.assertEqual(d[0].cause, "rest")
        # win_s == 0 only outside the dropout does not count
        d = re_.dropouts(*_args(ok), win_s=np.array([0, 6, 6, 6, 0], float))
        self.assertEqual(d[0].cause, "rest")

    def test_rezoom_start_is_not_motion(self):
        # win_s == 0 in the first 6 s after a rezoom (snap_frame < 360) is warm-up, not motion
        ok = [1, 0, 0, 0, 1]
        w = np.array([15, 0, 0, 6, 7], float)
        d = re_.dropouts(*_args(ok), win_s=w, frames=np.array([900, 60, 300, 360, 420], float))
        self.assertEqual(d[0].cause, "rest")
        d = re_.dropouts(*_args(ok), win_s=w, frames=np.array([900, 960, 1020, 1080, 1140], float))
        self.assertEqual(d[0].cause, "bewegung")

    def test_ohne_karte_ignores_win_s(self):
        # Ruling R16 (--ohne-karte): base rule as without the switch, win_s == 0 does not make a motion dropout;
        # motion_pct > 30 and state 4 still count
        ok = [1, 0, 0, 0, 1]
        w = np.array([15, 0, 0, 6, 7], float)
        d = re_.dropouts(*_args(ok, br=[0, 1, 1, 1, 0]), win_s=w, ohne_karte=True)
        self.assertEqual(d[0].cause, "keine_atemspitze")
        self.assertEqual(re_.dropouts(*_args(ok, br=[0, 1, 1, 1, 0]), win_s=w)[0].cause, "bewegung")
        d = re_.dropouts(*_args(ok, motion=[0, 0, 45, 0, 0]), win_s=w, ohne_karte=True)
        self.assertEqual(d[0].cause, "bewegung")
        d = re_.dropouts(*_args(ok, state=[2, 2, 4, 2, 2]), win_s=w, ohne_karte=True)
        self.assertEqual(d[0].cause, "bewegung")

    def test_cause_no_breath_peak(self):
        d = re_.dropouts(*_args([1, 0, 0, 0, 1], br=[0, 1, 1, 0, 0]))
        self.assertEqual(d[0].cause, "keine_atemspitze")
        # motion takes precedence
        d = re_.dropouts(*_args([1, 0, 0, 0, 1], br=[0, 1, 1, 1, 0], motion=[0, 0, 31, 0, 0]))
        self.assertEqual(d[0].cause, "bewegung")
        # br_state == 1 only in the minority → rest
        d = re_.dropouts(*_args([1, 0, 0, 0, 0, 1], br=[0, 1, 0, 0, 0, 0]))
        self.assertEqual(d[0].cause, "rest")

    def test_no_person_not_counted(self):
        d = re_.dropouts(*_args([1, 0, 0, 0, 1, 0, 0, 1], state=[2, 0, 0, 2, 2, 0, 2, 2]))
        # first run (reports 1..3): 2 of 3 in state 0 → no person; second (5, 6): 1 of 2 = 50 % → counts
        self.assertEqual([x.start_s for x in d], [5.0])

    def test_no_person_counted_on_request(self):
        d = re_.dropouts(*_args([1, 0, 0, 0, 1], state=[2, 0, 0, 2, 2]), keine_person_zaehlen=True)
        self.assertEqual(len(d), 1)

    def test_open_run_at_end(self):
        d = re_.dropouts(*_args([1, 1, 0, 0]))
        self.assertEqual(len(d), 1)
        self.assertEqual(d[0].start_s, 2.0)
        self.assertAlmostEqual(d[0].dur_s, 1.0)  # up to the last report


class SummaryTest(unittest.TestCase):
    def test_summary_share_without_no_person(self):
        ok = np.array([1, 1, 0, 0, 0, 0], float)
        state = np.array([2, 2, 2, 0, 0, 0], float)
        t = np.arange(6, dtype=float)
        s = re_.summary(t, ok, np.full(6, 70.0), state, [])
        # without the three state-0 reports: 2 of 3 valid
        self.assertAlmostEqual(s["anteil_gueltig_pct"], 100.0 * 2 / 3)

    def test_summary_nan_state_counts_as_person(self):
        s = re_.summary(np.arange(4.0), np.array([1, 1, 0, 0.]), np.full(4, 70.0), np.full(4, NAN), [])
        self.assertAlmostEqual(s["anteil_gueltig_pct"], 50.0)

    def test_summary_per_cause_and_jumps(self):
        drops = [re_.Dropout(1.0, 10.0, "bewegung"), re_.Dropout(30.0, 30.0, "bewegung"),
                 re_.Dropout(80.0, 4.0, "rest")]
        hr = np.array([70, 71, 90, 91, 60.])
        s = re_.summary(np.arange(5.0), np.ones(5), hr, np.full(5, NAN), drops)
        self.assertEqual(s["ursachen"]["bewegung"], {"n": 2, "median_s": 20.0, "summe_s": 40.0})
        self.assertEqual(s["ursachen"]["rest"]["n"], 1)
        self.assertEqual(s["ursachen"]["keine_atemspitze"]["n"], 0)
        self.assertTrue(math.isnan(s["ursachen"]["keine_atemspitze"]["median_s"]))
        self.assertEqual(s["ursachen"]["keine_atemspitze"]["summe_s"], 0.0)
        self.assertEqual(s["spruenge"], 2)  # 71 → 90 and 91 → 60
        self.assertEqual(s["summe_s"], 44.0)  # dropout time over all causes


class DefineHeaderTest(unittest.TestCase):
    def test_define_header_copy(self):
        with tempfile.TemporaryDirectory() as src, tempfile.TemporaryDirectory() as dst:
            (pathlib.Path(src) / "a121_esp_dsp.h").write_text("#pragma once\nint x;\n")
            out = re_.header_with_defines(pathlib.Path(src), ["A121_DSP_REACQ", "FOO=5"], pathlib.Path(dst))
            text = (out / "a121_esp_dsp.h").read_text()
            self.assertTrue(text.startswith("#define A121_DSP_REACQ\n#define FOO 5\n"))
            self.assertTrue(text.endswith("#pragma once\nint x;\n"))


if __name__ == "__main__":
    unittest.main()
