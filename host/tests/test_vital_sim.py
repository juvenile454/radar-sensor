"""Tests für den Prüfstand host/vital_sim.py (nur Simulation).

  .venv/bin/python -m unittest host/tests/test_vital_sim.py
"""

import math
import re
import pathlib
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST))

import vital_sim  # noqa: E402


def worsen_header(src: str) -> str:
    """Absichtlich verschlechterter Header: Suchband beginnt erst bei 1,25 Hz (75 /min), Puls 72 fällt heraus."""
    return re.sub(r"(kHrFMin\s*=\s*)[0-9.]+f", r"\g<1>1.25f", src, count=1)


class CheckLimitsTest(unittest.TestCase):
    def test_all_within_limits(self):
        res = {"base": {"hr_p5": 97.0, "hr_mae": 0.7}}
        lim = {"base": [("hr_p5", ">=", 95.0), ("hr_mae", "<=", 1.0)]}
        self.assertEqual(vital_sim.check_limits(res, lim), [])

    def test_violation_names_scenario_and_metric(self):
        res = {"base": {"hr_p5": 80.0, "hr_mae": 0.7}}
        lim = {"base": [("hr_p5", ">=", 95.0), ("hr_mae", "<=", 1.0)]}
        msgs = vital_sim.check_limits(res, lim)
        self.assertEqual(len(msgs), 1)
        self.assertIn("base", msgs[0])
        self.assertIn("hr_p5", msgs[0])

    def test_nan_is_a_violation(self):
        res = {"base": {"hr_p5": math.nan}}
        lim = {"base": [("hr_p5", ">=", 0.0)]}
        self.assertEqual(len(vital_sim.check_limits(res, lim)), 1)

    def test_missing_scenario_is_skipped(self):
        lim = {"base": [("hr_p5", ">=", 95.0)]}
        self.assertEqual(vital_sim.check_limits({}, lim), [])


class SceneTest(unittest.TestCase):
    def test_niemand_has_no_echo_in_zoom_window(self):
        """Ohne Person bleiben im Zoom-Fenster (0,61–0,67 m) nur Rauschen und die Ausläufer ferner Echos."""
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "niemand"
            vital_sim.make_frames(p, seed=1, T=2.0, **vital_sim.SCENARIOS["niemand"][1])
            iq = np.fromfile(f"{p}.iq", "<i2").reshape(-1, vital_sim.ZOOM_N + vital_sim.ROOM_N + 1, 2)
            zoom = iq[:, 1:1 + vital_sim.ZOOM_N, :].astype(float)
            amp = np.hypot(zoom[..., 0], zoom[..., 1])
            self.assertLess(float(amp.mean()), 60.0)


class CheckCommandTest(unittest.TestCase):
    """Fertig-Kriterium V0.5: heutiger Header → Exit 0, absichtlich verschlechterter Header → Exit 1."""

    def run_check(self, header: pathlib.Path) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--check", "--header", str(header)],
                              capture_output=True, text=True)

    def test_current_header_passes(self):
        r = self.run_check(vital_sim.DSP_HEADER)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])

    def test_worse_header_fails(self):
        src = vital_sim.DSP_HEADER.read_text()
        worse = worsen_header(src)
        self.assertNotEqual(worse, src)
        with tempfile.TemporaryDirectory() as d:
            h = pathlib.Path(d) / "a121_esp_dsp.h"
            h.write_text(worse)
            r = self.run_check(h)
        self.assertEqual(r.returncode, 1, r.stdout[-3000:] + r.stderr[-2000:])


if __name__ == "__main__":
    unittest.main()
