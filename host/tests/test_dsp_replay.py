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


class SensorFlagsReplayTest(unittest.TestCase):
    """V1.5: @IQ3 trägt hinten frame_delayed; die Nachrechnung gibt die Zeile gleich wieder aus und zählt wie der
    ESP32. @RECAL (Neu-Kalibrierung, gleiches Zoom-Fenster) beginnt die Historie neu wie ein Rezoom."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = pathlib.Path(cls.tmp.name)
        vital_sim.make_frames(d / "base", seed=1, T=30.0)
        skel = d / "skel.log"
        vital_sim.write_skeleton(pathlib.Path(f"{d / 'base'}.iq"), 244, skel, vital_placeholders=False)
        lines, out = skel.read_text().splitlines(), []
        for ln in lines:
            if ln.startswith("@IQ3,"):
                frame = int(ln.split(",")[7])
                ln += ",1" if frame in (100, 101, 102) else ",0"   # 3 verspätete Frames im 2. Report
            out.append(ln)
            if ln.startswith("@ROOM,") and ln.split(",")[1] == "14983":   # nach dem 15. Report neu kalibriert
                out.append("@RECAL,14990")
        (d / "skel2.log").write_text("\n".join(out) + "\n")
        vital_sim.replay_skeleton(d / "skel2.log", d / "out.log")
        cls.lines = (d / "out.log").read_text().splitlines()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_iq3_keeps_delayed_field(self):
        iq3 = [ln for ln in self.lines if ln.startswith("@IQ3,")]
        self.assertEqual(iq3[100].split(",")[-1], "1")
        self.assertEqual(iq3[99].split(",")[-1], "0")
        self.assertEqual(len(iq3[0].split(",")), 10)

    def test_delayed_counted_in_report(self):
        dsp = [ln.split(",") for ln in self.lines if ln.startswith("@DSP,")]
        self.assertEqual([f[10] for f in dsp[:3]], ["0", "3", "0"])

    def test_recal_restarts_history(self):
        i = next(k for k, ln in enumerate(self.lines) if ln.startswith("@RECAL,"))
        dsp_after = [ln.split(",") for ln in self.lines[i:] if ln.startswith("@DSP,")]
        self.assertEqual([f[2] for f in dsp_after[:7]], ["0"] * 7)   # < 8 s Daten nach dem Neubeginn
        self.assertIn("1", [f[2] for f in dsp_after])


class BaselineHeaderTest(unittest.TestCase):
    """Die Basislinie (V0.6) rechnet Aufnahmen mit dem DSP-Header vom 2026-11-01 (Commit 0e2d836) nach; die heutige
    Nachrechnung muss mit diesem alten Header übersetzen und eigene @VITAL-Zeilen schreiben."""

    def test_replay_with_baseline_header(self):
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            hdr = subprocess.run(["git", "-C", str(HOST.parent), "show", "0e2d836:firmware/esp32s3/src/a121_esp_dsp.h"],
                                 capture_output=True, text=True, check=True).stdout
            (d / "basis").mkdir()
            (d / "basis" / "a121_esp_dsp.h").write_text(hdr)
            vital_sim.make_frames(d / "base", seed=1, T=20.0)
            skel, out = d / "skel.log", d / "out.log"
            vital_sim.write_skeleton(pathlib.Path(f"{d / 'base'}.iq"), 244, skel, vital_placeholders=False)
            vital_sim.replay_skeleton(skel, out, d / "basis" / "a121_esp_dsp.h")
            vital = [ln for ln in out.read_text().splitlines() if ln.startswith("@VITAL,")]
        self.assertGreater(len(vital), 5)


if __name__ == "__main__":
    unittest.main()
