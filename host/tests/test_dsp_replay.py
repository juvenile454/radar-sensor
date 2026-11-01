"""Tests für die Nachrechnung host/dsp_replay (über synthetische iq3-Logs aus host/vital_sim.py, nur Simulation).

  .venv/bin/python -m unittest host/tests/test_dsp_replay.py
"""

import pathlib
import sys
import tempfile
import unittest

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST))

import vital_sim  # noqa: E402


class ReplayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = pathlib.Path(cls.tmp.name)
        base = d / "base"
        vital_sim.make_frames(base, seed=1, T=20.0)
        cls.iq = pathlib.Path(f"{base}.iq")
        cls.zoom_pt = int(np.load(f"{base}_truth.npz")["zoom_start_pt"])
        cls.dir = d

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def replay(self, vital_placeholders: bool) -> list[str]:
        skel = self.dir / f"skel_{vital_placeholders}.log"
        out = self.dir / f"out_{vital_placeholders}.log"
        vital_sim.write_skeleton(self.iq, self.zoom_pt, skel, vital_placeholders=vital_placeholders)
        vital_sim.replay_skeleton(skel, out)
        return out.read_text().splitlines()

    def test_own_vital_lines_without_esp32_vital(self):
        """Hat der ESP32 an einer Stelle kein @VITAL, schreibt die Nachrechnung trotzdem ihr eigenes (Varianten mit
        mehr gültigen Reports sollen in vital_eval.py --replay zählen)."""
        lines = self.replay(vital_placeholders=False)
        ok = [ln for ln in lines if ln.startswith("@DSP,") and ln.split(",")[2] == "1"]
        vital = [ln for ln in lines if ln.startswith("@VITAL,")]
        self.assertGreater(len(ok), 5)
        self.assertEqual(len(vital), len(ok))

    def test_vital_lines_same_with_and_without_placeholders(self):
        """Eigene @VITAL-Zeilen tragen die Zeit der @DSP-Zeile; mit Platzhalter (gleiche Zeit) sind sie identisch."""
        a = [ln for ln in self.replay(True) if ln.startswith("@VITAL,")]
        b = [ln for ln in self.replay(False) if ln.startswith("@VITAL,")]
        self.assertEqual(a, b)

    def test_vital_follows_room_line(self):
        lines = self.replay(vital_placeholders=False)
        i = next(k for k, ln in enumerate(lines) if ln.startswith("@VITAL,"))
        self.assertTrue(lines[i - 1].startswith("@ROOM,"), lines[i - 1][:40])
        self.assertTrue(lines[i - 2].startswith("@DSP,"), lines[i - 2][:40])


if __name__ == "__main__":
    unittest.main()
