"""Tests für die Prüfung in host/esp_record.py (an synthetischen Ordnern, ohne ESP32).

  .venv/bin/python -m unittest host/tests/test_esp_record.py
"""

import base64
import pathlib
import sys
import tempfile
import unittest

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST))

import esp_record  # noqa: E402


def write_folder(d: pathlib.Path, zoom_points: int) -> None:
    n = 1 + zoom_points + 80
    rng = np.random.default_rng(1)
    lines = [f"@IQ3_START,610,{610 + zoom_points * 25 // 10},60.0,1"]
    for i in range(30):
        v = rng.integers(-2000, 2000, size=(n, 2)).astype("<i2")
        lb = float(np.hypot(*v[0].astype(float)))
        c = 1 + zoom_points // 2
        za = float(np.hypot(*v[c].astype(float)))
        lines.append(f"@IQ3,{17 * i},20,{lb:.1f},0.00,{za:.1f},0,{i},{17 * i},0")
        lines.append(f"@RAW,{i},{17 * i},{n},{base64.b64encode(v.tobytes()).decode()}")
    (d / "stream.log").write_text("\n".join(lines) + "\n")


class CheckZoomWidthTest(unittest.TestCase):
    """V2.3 Schritt 3: Die Prüfung nimmt die Zoom-Breite aus den @RAW-Zeilen (24 oder 48 Punkte)."""

    def test_24_points_ok(self):
        with tempfile.TemporaryDirectory() as d:
            write_folder(pathlib.Path(d), 24)
            self.assertEqual(esp_record.check(pathlib.Path(d), None), 0)

    def test_48_points_ok(self):
        with tempfile.TemporaryDirectory() as d:
            write_folder(pathlib.Path(d), 48)
            self.assertEqual(esp_record.check(pathlib.Path(d), None), 0)


if __name__ == "__main__":
    unittest.main()
