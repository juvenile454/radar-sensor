"""Tests for the replay host/dsp_replay (via synthetic iq3 logs from host/vital_sim.py, simulation only).

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
        """If the ESP32 has no @VITAL at some point, the replay still writes its own (variants with
        more valid reports are supposed to count in vital_eval.py --replay)."""
        lines = self.replay(vital_placeholders=False)
        ok = [ln for ln in lines if ln.startswith("@DSP,") and ln.split(",")[2] == "1"]
        vital = [ln for ln in lines if ln.startswith("@VITAL,")]
        self.assertGreater(len(ok), 5)
        self.assertEqual(len(vital), len(ok))

    def test_vital_lines_same_with_and_without_placeholders(self):
        """Own @VITAL lines carry the time of the @DSP line; with a placeholder (same time) they are identical."""
        a = [ln for ln in self.replay(True) if ln.startswith("@VITAL,")]
        b = [ln for ln in self.replay(False) if ln.startswith("@VITAL,")]
        self.assertEqual(a, b)

    def test_vital_follows_room_line(self):
        lines = self.replay(vital_placeholders=False)
        i = next(k for k, ln in enumerate(lines) if ln.startswith("@VITAL,"))
        self.assertTrue(lines[i - 1].startswith("@ROOM,"), lines[i - 1][:40])
        self.assertTrue(lines[i - 2].startswith("@DSP,"), lines[i - 2][:40])


class SensorFlagsReplayTest(unittest.TestCase):
    """V1.5: @IQ3 carries frame_delayed at the end; the replay writes the line out unchanged and counts like the
    ESP32. @RECAL (recalibration, same zoom window) restarts the history like a rezoom."""

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
                ln += ",1" if frame in (100, 101, 102) else ",0"   # 3 delayed frames in the 2nd report
            out.append(ln)
            if ln.startswith("@ROOM,") and ln.split(",")[1] == "14983":   # recalibrated after the 15th report
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
        self.assertEqual([f[2] for f in dsp_after[:5]], ["0"] * 5)   # < 6 s of data after the restart (V2.4)
        self.assertIn("1", [f[2] for f in dsp_after])


class BaselineHeaderTest(unittest.TestCase):
    """The baseline (V0.6) replays recordings with the DSP header from 2026-11-01 (commit 26c9bff); today's
    replay must compile with this old header and write its own @VITAL lines."""

    def test_replay_with_baseline_header(self):
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            hdr = subprocess.run(["git", "-C", str(HOST.parent), "show", "26c9bff:firmware/esp32s3/src/a121_esp_dsp.h"],
                                 capture_output=True, text=True, check=True).stdout
            (d / "basis").mkdir()
            (d / "basis" / "a121_esp_dsp.h").write_text(hdr)
            vital_sim.make_frames(d / "base", seed=1, T=20.0)
            skel, out = d / "skel.log", d / "out.log"
            vital_sim.write_skeleton(pathlib.Path(f"{d / 'base'}.iq"), 244, skel, vital_placeholders=False)
            vital_sim.replay_skeleton(skel, out, d / "basis" / "a121_esp_dsp.h")
            vital = [ln for ln in out.read_text().splitlines() if ln.startswith("@VITAL,")]
        self.assertGreater(len(vital), 5)


class SnapshotReplayTest(unittest.TestCase):
    """V2.1: The ESP32 computes the report on core 0 from a snapshot; @DSP therefore arrives a few frames later
    and carries the frame count of the snapshot in field 13. The replay computes at that count and arrives at
    the same values as without delay. If pushFrame is more than kSnapMaxLag (180) frames ahead, the window is partly
    overwritten: report invalid, no @VITAL (ESP32 and replay alike)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = pathlib.Path(cls.tmp.name)
        vital_sim.make_frames(cls.dir / "base", seed=1, T=30.0)
        cls.skel = cls.dir / "skel.log"
        vital_sim.write_skeleton(pathlib.Path(f"{cls.dir / 'base'}.iq"), 244, cls.skel, vital_placeholders=False)
        cls.sync = cls.run_log(cls.skel.read_text().splitlines(), "sync")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @classmethod
    def run_log(cls, lines: list[str], name: str) -> list[str]:
        src, out = cls.dir / f"{name}.log", cls.dir / f"{name}_out.log"
        src.write_text("\n".join(lines) + "\n")
        vital_sim.replay_skeleton(src, out)
        return out.read_text().splitlines()

    def delayed(self, lag: int) -> list[str]:
        """@DSP/@ROOM each lag frames later (after its @RAW), @DSP with field 13 = frames up to the snapshot;
        whatever would come after the last frame goes before @END."""
        out, held, frames = [], [], 0
        for ln in self.skel.read_text().splitlines():
            if ln.startswith("@DSP,"):
                held.append((frames + lag, [ln + ",0,0,0,0,0," + str(frames)]))   # fields 8–12, 13
                continue
            if ln.startswith("@ROOM,"):
                held[-1][1].append(ln)
                continue
            if ln.startswith("@END"):
                break
            out.append(ln)
            if ln.startswith("@RAW,"):
                frames += 1
                if held and held[0][0] == frames:
                    out += held.pop(0)[1]
        return out + [x for _, g in held for x in g] + ["@END"]

    @staticmethod
    def tail(lines: list[str], tag: str) -> list[list[str]]:
        return [ln.split(",")[2:] for ln in lines if ln.startswith(tag)]

    def test_lag_gives_same_values(self):
        lagged = self.run_log(self.delayed(180), "lag180")
        self.assertGreater(len(self.tail(self.sync, "@VITAL,")), 10)
        self.assertEqual(self.tail(lagged, "@VITAL,"), self.tail(self.sync, "@VITAL,"))
        self.assertEqual(self.tail(lagged, "@DSP,"), self.tail(self.sync, "@DSP,"))

    def test_stale_snapshot_invalid(self):
        lagged = self.run_log(self.delayed(181), "lag181")
        dsp = self.tail(lagged, "@DSP,")
        self.assertEqual(len(dsp), len(self.tail(self.sync, "@DSP,")))
        late = sum(1 for f in dsp if int(f[11]) + 181 > 1800)   # they stand before @END, delay there < 181
        self.assertEqual(late, 4)
        self.assertEqual({f[0] for f in dsp[:-late]}, {"0"})
        self.assertEqual(self.tail(lagged, "@VITAL,"), self.tail(self.sync, "@VITAL,")[-late:])


class ZoomWidthReplayTest(unittest.TestCase):
    """V2.3 step 3: If the point count in @RAW does not match the header (24-point recording, header with 48), the
    replay does not compute with misassigned points but reports it."""

    def test_mismatch_reported(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            vital_sim.make_frames(d / "base", seed=1, T=12.0)
            skel, out = d / "skel.log", d / "out.log"
            vital_sim.write_skeleton(pathlib.Path(f"{d / 'base'}.iq"), 244, skel, vital_placeholders=False)
            rep = vital_sim.replay_skeleton(skel, out, defines=("-DA121_ZOOM_POINTS=48",))
            lines = out.read_text().splitlines()
        self.assertEqual([ln for ln in lines if ln.startswith("@VITAL,")], [])
        self.assertIn("Punktzahl", rep)
        self.assertIn("105", rep)


class StateReplayTest(unittest.TestCase):
    """V3.5: The replay writes one @STATE line per report (its own if the ESP32 had none; otherwise
    compared)."""

    def test_state_line_per_report(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            vital_sim.make_frames(d / "base", seed=1, T=12.0)
            skel, out = d / "skel.log", d / "out.log"
            vital_sim.write_skeleton(pathlib.Path(f"{d / 'base'}.iq"), 244, skel, vital_placeholders=False)
            vital_sim.replay_skeleton(skel, out)
            lines = out.read_text().splitlines()
            dsp = [i for i, ln in enumerate(lines) if ln.startswith("@DSP,")]
            st = [ln for ln in lines if ln.startswith("@STATE,")]
            self.assertEqual(len(st), len(dsp))
            self.assertEqual(st[0].split(",")[1], lines[dsp[0]].split(",")[1])   # time of the @DSP line
            # with @STATE from the ESP32 (same content) no deviation in this line
            skel2, out2 = d / "skel2.log", d / "out2.log"
            src = skel.read_text().splitlines()
            withst, k = [], 0
            for ln in src:
                withst.append(ln)
                if ln.startswith("@ROOM,"):
                    withst.append(st[k])
                    k += 1
            skel2.write_text("\n".join(withst) + "\n")
            rep = vital_sim.replay_skeleton(skel2, out2)
        self.assertRegex(rep, r"@STATE\s+verglichen\s+12, gleich\s+12")


class BreathReplayTest(unittest.TestCase):
    """V3.4: The replay writes one @BREATH line per breath directly after the frame's @IQ3 (its own if the
    ESP32 had none; otherwise compared)."""

    def test_breath_lines(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            vital_sim.make_frames(d / "base", seed=1, T=30.0)       # breathing 15 /min
            skel, out = d / "skel.log", d / "out.log"
            vital_sim.write_skeleton(pathlib.Path(f"{d / 'base'}.iq"), 244, skel, vital_placeholders=False)
            vital_sim.replay_skeleton(skel, out)
            lines = out.read_text().splitlines()
            idx = [i for i, ln in enumerate(lines) if ln.startswith("@BREATH,")]
            self.assertGreaterEqual(len(idx), 4)
            for i in idx:
                iq3 = lines[i - 1].split(",")
                self.assertEqual(iq3[0], "@IQ3")
                b = lines[i].split(",")
                self.assertEqual(len(b), 6)
                self.assertEqual((b[1], b[2]), (iq3[1], iq3[8]))     # time and tick of the frame
                self.assertAlmostEqual(float(b[3]), 4000.0, delta=200.0)
                self.assertAlmostEqual(float(b[5]), 15.0, delta=1.0)
            # with @BREATH from the ESP32 (same content, directly after @RAW) no deviation
            src = skel.read_text().splitlines()
            by_frame = {lines[i - 1].split(",")[7]: lines[i] for i in idx}
            withb = []
            for ln in src:
                withb.append(ln)
                if ln.startswith("@RAW,") and ln.split(",")[1] in by_frame:
                    withb.append(by_frame[ln.split(",")[1]])
            skel2, out2 = d / "skel2.log", d / "out2.log"
            skel2.write_text("\n".join(withb) + "\n")
            rep = vital_sim.replay_skeleton(skel2, out2)
        self.assertRegex(rep, rf"@BREATH\s+verglichen\s+{len(idx)}, gleich\s+{len(idx)}")


if __name__ == "__main__":
    unittest.main()
