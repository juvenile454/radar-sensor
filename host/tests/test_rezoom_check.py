"""Tests für host/rezoom_check.py (V2.3 Schritt 1) an synthetischen iq3-raw-Logs (nur Simulation).

  .venv/bin/python -m unittest host/tests/test_rezoom_check.py
"""

import base64
import cmath
import pathlib
import sys
import unittest

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST))

import rezoom_check  # noqa: E402

Z, R = 24, 80


def frame_line(i: int, lb: complex, zoom: list[complex], room: list[complex]) -> str:
    v = np.array([lb] + zoom + room)
    iq = np.stack([np.round(v.real), np.round(v.imag)], axis=1).astype("<i2")
    return f"@RAW,{i},{17 * i},{v.size},{base64.b64encode(iq.tobytes()).decode()}"


def scene(pt: int) -> complex:
    """Festes Echo je Entfernung (2,5-mm-Punkt): Amplitude und Phase hängen nur von der Entfernung ab."""
    return (800 + 5 * (pt % 7)) * cmath.exp(1j * 0.37 * pt)


def make_log(rot_after: float, lb_rot_after: float, new_start: int = 208) -> list[str]:
    """20 Frames bei Zoom-Start 200, Rezoom auf new_start, 20 Frames danach. Nach dem Neustart sind alle Echos um
    rot_after gedreht, der Loopback um lb_rot_after."""
    lines = [f"@IQ3_START,{200 * 25 // 10},{(200 + Z) * 25 // 10},60.0,1"]
    room = [500 * cmath.exp(1j * 0.11 * r) for r in range(R)]
    for i in range(20):
        lines.append(frame_line(i, 1000, [scene(200 + b) for b in range(Z)], room))
    lines.append(f"@REZOOM,{new_start * 25 // 10},{(new_start + Z) * 25 // 10}")
    rot, lrot = cmath.exp(1j * rot_after), cmath.exp(1j * lb_rot_after)
    for i in range(20, 40):
        lines.append(frame_line(i, 1000 * lrot, [scene(new_start + b) * rot for b in range(Z)], [x * rot for x in room]))
    lines.append("@END")
    return lines


class RezoomCheckTest(unittest.TestCase):
    def test_continuous_phase(self):
        res = rezoom_check.analyze(make_log(0.0, 0.0))
        self.assertEqual(len(res), 1)
        r = res[0]
        self.assertEqual(r["overlap"], 16)   # 208..223
        self.assertLess(abs(r["zoom_jump_rad"]), 0.01)
        self.assertLess(r["zoom_spread_rad"], 0.01)
        self.assertAlmostEqual(r["zoom_amp_ratio"], 1.0, delta=0.01)
        self.assertLess(abs(r["room_jump_rad"]), 0.01)

    def test_common_rotation_removed_by_loopback(self):
        r = rezoom_check.analyze(make_log(0.7, 0.7))[0]
        self.assertAlmostEqual(r["zoom_jump_rad"], 0.7, delta=0.01)
        self.assertLess(abs(r["zoom_jump_derot_rad"]), 0.01)
        self.assertLess(r["zoom_spread_rad"], 0.01)       # gemeinsame Drehung: Punkte einig
        self.assertAlmostEqual(r["room_jump_rad"], 0.7, delta=0.01)
        self.assertLess(abs(r["room_jump_derot_rad"]), 0.01)

    def test_no_overlap(self):
        r = rezoom_check.analyze(make_log(0.0, 0.0, new_start=240))[0]
        self.assertEqual(r["overlap"], 0)
        self.assertTrue(np.isnan(r["zoom_jump_rad"]))

    def test_summary_counts_continuous(self):
        res = rezoom_check.analyze(make_log(0.0, 0.0)) + rezoom_check.analyze(make_log(0.7, 0.0))
        text = rezoom_check.summary(res)
        self.assertIn("2 Rezooms", text)
        self.assertIn("stetig (|Sprung| < 0,1 rad): 1 von 2", text)


if __name__ == "__main__":
    unittest.main()
