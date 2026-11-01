"""Tests für host/vital_eval.py (Auswertung gegen Referenz).

  .venv/bin/python -m unittest host/tests/test_vital_eval.py
"""

import math
import pathlib
import sys
import tempfile
import unittest

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST))

import vital_eval as ve  # noqa: E402


class RefHrTest(unittest.TestCase):
    def test_ref_hr_constant(self):
        beats = np.arange(0, 60000, 800.0)
        self.assertAlmostEqual(ve.ref_hr_at(beats, 30000.0), 75.0, delta=0.01)

    def test_ref_hr_uses_rr_values_when_given(self):
        # Schlagzeiten verrauscht (BLE-Latenz), RR-Werte exakt 1000 ms → 60 /min
        beats = np.arange(0, 60000, 1000.0) + np.tile([0.0, 90.0], 30)
        rr = np.full(beats.size, 1000.0)
        self.assertAlmostEqual(ve.ref_hr_at(beats, 30000.0, rr_ms=rr), 60.0, delta=0.01)

    def test_ref_hr_too_few_beats_is_nan(self):
        self.assertTrue(math.isnan(ve.ref_hr_at(np.array([1000.0, 2000.0]), 15000.0)))


class BeatsFromCsvTest(unittest.TestCase):
    def test_beats_backwards_from_notification(self):
        # Benachrichtigung bei 5000 ms mit RR 800, 750: Schläge bei 4250 und 5000
        rx = np.array([5000.0, 5000.0])
        rr = np.array([800.0, 750.0])
        beats, rr_out = ve.beats_from_ref(rx, rr)
        np.testing.assert_allclose(beats, [4250.0, 5000.0])
        np.testing.assert_allclose(rr_out, [800.0, 750.0])

    def test_rows_without_rr_are_skipped(self):
        beats, _ = ve.beats_from_ref(np.array([1000.0, 2000.0]), np.array([np.nan, 900.0]))
        np.testing.assert_allclose(beats, [2000.0])


class MetricsTest(unittest.TestCase):
    def test_metrics_basic(self):
        m = ve.metrics(np.array([70.0, 72.0, 80.0]), np.array([70.0, 70.0, 70.0]), np.array([True, True, True]))
        self.assertEqual(m["n"], 3)
        self.assertAlmostEqual(m["mae"], 4.0)
        self.assertAlmostEqual(m["p5"], 66.7, delta=0.05)
        self.assertAlmostEqual(m["bias"], 4.0)
        self.assertAlmostEqual(m["rmse"], math.sqrt((0 + 4 + 100) / 3))
        sd = np.std([0.0, 2.0, 10.0], ddof=1)
        self.assertAlmostEqual(m["loa_low"], 4.0 - 1.96 * sd)
        self.assertAlmostEqual(m["loa_high"], 4.0 + 1.96 * sd)

    def test_coverage(self):
        m = ve.metrics(np.array([70.0, 0.0, 71.0, 69.0]), np.full(4, 70.0), np.array([1, 0, 1, 1], bool))
        self.assertAlmostEqual(m["coverage_pct"], 75.0)
        self.assertEqual(m["n"], 3)

    def test_no_valid_reports(self):
        m = ve.metrics(np.array([70.0]), np.array([70.0]), np.array([False]))
        self.assertEqual(m["n"], 0)
        self.assertTrue(math.isnan(m["mae"]))

    def test_calibration_bins(self):
        conf = np.array([10.0, 40.0, 60.0, 80.0, 95.0, 95.0])
        err = np.array([20.0, 1.0, 6.0, 2.0, 1.0, 7.0])
        cal = ve.calibration(conf, err)
        self.assertEqual([c[2] for c in cal], [1, 1, 1, 1, 2])
        self.assertEqual([c[3] for c in cal], [0.0, 100.0, 0.0, 100.0, 50.0])
        self.assertEqual((cal[0][0], cal[0][1]), (0, 30))

    def test_calibration_empty_bin_is_nan(self):
        cal = ve.calibration(np.array([95.0]), np.array([1.0]))
        self.assertTrue(math.isnan(cal[0][3]))
        self.assertEqual(cal[0][2], 0)


class BreathRefTest(unittest.TestCase):
    def test_metronome_rate(self):
        marks = np.arange(0, 120000, 4000.0)  # 15 /min
        self.assertAlmostEqual(ve.br_ref_at(marks, 60000.0), 15.0, delta=0.01)

    def test_rate_change_in_window_is_nan(self):
        marks = np.concatenate([np.arange(0, 50000, 4000.0), np.arange(50000, 120000, 6000.0)])
        self.assertTrue(math.isnan(ve.br_ref_at(marks, 56000.0)))

    def test_slow_metronome_uses_last_two_marks(self):
        marks = np.arange(0, 120000, 10000.0)  # 6 /min: nur 1–2 Marken im 15-s-Fenster
        self.assertAlmostEqual(ve.br_ref_at(marks, 65000.0), 6.0, delta=0.01)


class EventsTest(unittest.TestCase):
    def test_intervals_from_start_end_marks(self):
        ev = [(1000.0, "anhalten_start"), (21000.0, "anhalten_ende"), (40000.0, "anhalten_start")]
        self.assertEqual(ve.intervals(ev, "anhalten", end_ms=50000.0), [(1000.0, 21000.0), (40000.0, 50000.0)])

    def test_apnea_delay_and_false_alarms(self):
        t = np.arange(0, 100000, 1000.0)
        state = np.zeros(t.size, int)
        state[(t >= 30000) & (t < 52000)] = 2   # erkannt 10 s nach Beginn (20000)
        state[(t >= 80000) & (t < 83000)] = 2   # Fehlalarm
        r = ve.apnea_eval(t, state, [(20000.0, 45000.0)])
        self.assertEqual(r["detected"], 1)
        self.assertEqual(r["episodes"], 1)
        self.assertAlmostEqual(r["delays_s"][0], 10.0)
        self.assertEqual(r["false_alarms"], 1)


class TimingTest(unittest.TestCase):
    def test_first_value_delays(self):
        starts = [0.0, 30000.0]
        vital_rx = np.array([8000.0, 9000.0, 29000.0, 41000.0])
        self.assertEqual(ve.first_value_delays(starts, vital_rx), [8.0, 11.0])

    def test_start_without_value(self):
        self.assertEqual(ve.first_value_delays([0.0, 10000.0], np.array([12000.0])), [12.0, 2.0])

    def test_best_lag(self):
        t = np.arange(20000, 200000, 1000.0)
        ref = lambda tt: 70.0 + 20.0 * np.clip((tt - 60000.0) / 20000.0, 0, 1)  # noqa: E731
        est = ref(t - 5000.0)
        self.assertAlmostEqual(ve.best_lag_s(t, est, ref), 5.0, delta=0.5)


class LoadRecordingTest(unittest.TestCase):
    def test_load_minimal_folder(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d)
            (p / "stream.log").write_text(
                "@IQ3_START,610,670,60.0,1\n"
                "@DSP,1000,1,5,88.0,300000,0,0\n"
                "@VITAL,1001,15.0,72.0,72.0,71.5,73.0,830.0,40.0,88,2400.0,90.0,12.3,1.20,640,4\n"
                "@REZOOM,700,760\n"
                "@DSP,2000,0,0,0.0,300000,0,0\n"
                "@END\n")
            (p / "rxtime.csv").write_text("line,rx_ms\n0,10.0\n1,1020.0\n2,1021.0\n3,1500.0\n4,2010.0\n5,2100.0\n")
            (p / "ref_hr.csv").write_text("rx_ms,hr_bpm,rr_ms,contact\n1000.0,72,800.0,1\n1000.0,72,830.0,1\n"
                                          "1500.0,72,,1\n")
            (p / "events.csv").write_text("rx_ms,text\n500.0,ein\n")
            rec = ve.load_recording(p)
        self.assertEqual(rec.vital["hr_fused"].tolist(), [72.0])
        self.assertEqual(rec.vital["rx_ms"].tolist(), [1021.0])
        self.assertEqual(rec.dsp["ok"].tolist(), [1.0, 0.0])
        self.assertEqual(rec.dsp["rx_ms"].tolist(), [1020.0, 2010.0])
        np.testing.assert_allclose(rec.beats_ms, [170.0, 1000.0])
        self.assertEqual(rec.events, [(500.0, "ein")])
        self.assertEqual(rec.starts_ms, [10.0, 1500.0])

    def test_dsp_extra_fields_missing_are_nan(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d)
            (p / "stream.log").write_text("@IQ3_START,610,670,60.0,1\n@DSP,1000,1,5,88.0,300000,0,0\n")
            (p / "rxtime.csv").write_text("line,rx_ms\n0,0.0\n1,1000.0\n")
            rec = ve.load_recording(p)
        self.assertTrue(np.isnan(rec.dsp["br_state"][0]))
        self.assertEqual(rec.beats_ms.size, 0)


class SensorCountersTest(unittest.TestCase):
    def test_sums_and_recal(self):
        """V1.5: Summe der Sensor-Zähler aus @DSP und Zahl der Neu-Kalibrierungen (@RECAL) je Aufnahme."""
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d)
            (p / "stream.log").write_text(
                "@IQ3_START,610,670,60.0,1\n"
                "@DSP,1000,0,5,0.0,300000,0,0,0,1,2,0,1\n"
                "@WARN,calibration_needed\n@RECAL,1500\n"
                "@DSP,2000,0,5,0.0,300000,0,0,0,1,1,2,0\n")
            (p / "rxtime.csv").write_text("line,rx_ms\n0,0\n1,1000\n2,1400\n3,1500\n4,2000\n")
            r = ve.evaluate(ve.load_recording(p))
        self.assertEqual(r["sensor"], {"n_delayed": 3, "n_sat": 2, "n_tickgap": 1, "recal": 1})
        self.assertIn("Neu-Kalibrierungen 1", ve.format_report("x", r))


class SyntheticRecordingTest(unittest.TestCase):
    """Fertig-Kriterium V0.4 auf einer synthetischen Aufnahme (vital_sim.py record): Bericht entsteht, und --replay mit
    dem unveränderten Header liefert dieselben Zahlen wie die „Live"-Zeilen. Nur Simulation."""

    @classmethod
    def setUpClass(cls):
        import vital_sim
        cls.tmp = tempfile.TemporaryDirectory()
        cls.folder = pathlib.Path(cls.tmp.name) / "sim_anhalt"
        vital_sim.record_scenario("anhalt", cls.folder)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_recording_has_app_files(self):
        for f in ("stream.log", "rxtime.csv", "ref_hr.csv", "events.csv", "meta.json"):
            self.assertTrue((self.folder / f).is_file(), f)

    def test_report_against_reference(self):
        r = ve.evaluate(ve.load_recording(self.folder))
        self.assertGreater(r["hr"]["n"], 80)
        self.assertLess(r["hr"]["mae"], 5.0)
        self.assertGreater(r["br"]["n"], 50)
        self.assertEqual(r["apnea"]["detected"], 1)  # @DSP br_state (V1.4): Anhalten erkannt, ≤ 12 s, kein Fehlalarm
        self.assertLessEqual(r["apnea"]["delays_s"][0], 12.0)
        self.assertEqual(r["apnea"]["false_alarms"], 0)
        self.assertIn("Puls", ve.format_report("sim", r))

    def test_replay_unchanged_header_gives_same_numbers(self):
        live = ve.evaluate(ve.load_recording(self.folder))
        lines = ve.replay_lines(self.folder, HOST.parent / "firmware" / "esp32s3" / "src")
        again = ve.evaluate(ve.load_recording(self.folder, lines))
        self.assertEqual(again["valid"], live["valid"])
        for k in ("n", "mae", "p5", "coverage_pct"):
            self.assertAlmostEqual(again["hr"][k], live["hr"][k], places=6, msg=k)
        self.assertAlmostEqual(again["br"]["mae"], live["br"]["mae"], places=6)


class MotionStateEvalTest(unittest.TestCase):
    def test_motion_markers_vs_state(self):
        """V3.5 (Abnahme R5): Anteil der Reports mit Zustand BEWEGUNG in den Bewegungsabschnitten (bis 15 s danach) und
        Reports mit BEWEGUNG außerhalb."""
        lines, rx = ["@IQ3_START,610,670,60.0,1"], [0]
        for k in range(1, 41):  # 40 Reports, Bewegung 10–20 s laut Marker
            st = 4 if 12 <= k <= 24 else (4 if k == 35 else 3)
            lines += [f"@DSP,{k * 1000},1,5,80.0,1000,0,0,0,0,0,0,0", f"@STATE,{k * 1000},{st},0"]
            rx += [k * 1000, k * 1000]
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d)
            (p / "stream.log").write_text("\n".join(lines) + "\n")
            (p / "rxtime.csv").write_text("line,rx_ms\n" + "".join(f"{i},{t}\n" for i, t in enumerate(rx)))
            (p / "events.csv").write_text("rx_ms,text\n10000,bewegung_start\n20000,bewegung_ende\n")
            r = ve.evaluate(ve.load_recording(p))
        m = r["motion_state"]
        self.assertEqual(m["reports_in"], 25)            # 11..35 s (Ende + 15 s)
        self.assertEqual(m["motion_in"], 14)             # 12..24 und 35
        self.assertEqual(m["motion_out"], 0)
        self.assertIn("Zustand BEWEGUNG", ve.format_report("x", r))


class BreathEventsEvalTest(unittest.TestCase):
    def test_breath_vs_metronome(self):
        """V3.4 (Abnahme R3): Rate je @BREATH gegen das Metronom (Einatem-Marker „ein"), Anteil ±1 /min, erster Wert
        nach dem Start; Atemzüge in Anhalte-Abschnitten werden gezählt."""
        lines, rx = ["@IQ3_START,610,670,60.0,1"], [0]
        for k in range(2, 12):  # Metronom 10 /min (Marker alle 6 s), @BREATH ab 12,5 s, ein Ausreißer 12,5 /min
            t = k * 6000 + 500
            lines.append(f"@BREATH,{t},{t},6000,2400,{12.5 if k == 7 else 10.0}")
            rx.append(t)
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d)
            (p / "stream.log").write_text("\n".join(lines) + "\n")
            (p / "rxtime.csv").write_text("line,rx_ms\n" + "".join(f"{i},{t}\n" for i, t in enumerate(rx)))
            (p / "events.csv").write_text("rx_ms,text\n" + "".join(f"{k * 6000},ein\n" for k in range(13))
                                          + "40000,anhalten_start\n50000,anhalten_ende\n")
            r = ve.evaluate(ve.load_recording(p))
        b = r["breath_events"]
        self.assertEqual(b["n"], 10)
        self.assertEqual(b["n_ref"], 10)
        self.assertAlmostEqual(b["mae"], 0.25, places=6)
        self.assertAlmostEqual(b["p1"], 90.0, places=6)
        self.assertEqual(b["first_s"], [12.5])
        self.assertEqual(b["in_hold"], 2)                  # 42,5 s und 48,5 s
        self.assertIn("Atemzug für Atemzug", ve.format_report("x", r))

    def test_old_firmware_without_breath(self):
        lines, rx = ["@IQ3_START,610,670,60.0,1", "@DSP,1000,1,5,80.0,1000,0,0,0,0,0,0,0"], [0, 1000]
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d)
            (p / "stream.log").write_text("\n".join(lines) + "\n")
            (p / "rxtime.csv").write_text("line,rx_ms\n" + "".join(f"{i},{t}\n" for i, t in enumerate(rx)))
            r = ve.evaluate(ve.load_recording(p))
        self.assertNotIn("breath_events", r)


class JumpCountTest(unittest.TestCase):
    def test_jumps_between_consecutive_valid_reports(self):
        """V3.2 (Abnahme): Sprünge > 15 /min zwischen aufeinanderfolgenden gültigen Reports; ein ungültiger Report
        unterbricht die Folge."""
        hr = np.array([70, 72, 90, 91, 60, 61, 95, 96], float)
        ok = np.array([1, 1, 1, 1, 1, 0, 1, 1], bool)
        self.assertEqual(ve.count_jumps(hr, ok), 2)          # 72→90, 91→60; 61 ungültig, 60→95 zählt nicht
        self.assertEqual(ve.count_jumps(hr, ok, thr=30.0), 1)


class EffectivePointsTest(unittest.TestCase):
    def test_n_eff_median_and_line(self):
        """V3.1: @DSP mit Schalter A121_DSP_COMBINE trägt als 15. Feld die effektive Punktzahl; die Auswertung meldet
        ihren Median über die Reports, ohne das Feld (Standard) nichts."""
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d)
            (p / "stream.log").write_text(
                "@IQ3_START,610,670,60.0,1\n"
                "@DSP,1000,0,5,0.0,300000,0,0,0,1,0,0,0,60,1.2\n"
                "@DSP,2000,0,5,0.0,300000,0,0,0,1,0,0,0,120,1.6\n"
                "@DSP,3000,0,5,0.0,300000,0,0,0,1,0,0,0,180,3.0\n")
            (p / "rxtime.csv").write_text("line,rx_ms\n0,0\n1,1000\n2,2000\n3,3000\n")
            r = ve.evaluate(ve.load_recording(p))
            (p / "stream.log").write_text("@IQ3_START,610,670,60.0,1\n@DSP,1000,0,5,0.0,300000,0,0,0,1,0,0,0,60\n")
            r0 = ve.evaluate(ve.load_recording(p))
        self.assertAlmostEqual(r["n_eff"], 1.6)
        self.assertIn("effektive Punktzahl (V3.1) 1.6", ve.format_report("x", r))
        self.assertIsNone(r0["n_eff"])
        self.assertNotIn("effektive Punktzahl", ve.format_report("x", r0))


if __name__ == "__main__":
    unittest.main()
