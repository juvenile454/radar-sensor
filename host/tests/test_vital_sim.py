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
    """Absichtlich verschlechterter Header: Suchband beginnt erst bei Bin 85 (1,25 Hz, 75 /min), Puls 72 fällt heraus."""
    return re.sub(r"(kHrK0\s*=\s*)[0-9]+U", r"\g<1>85U", src, count=1)


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


class ConfStatsTest(unittest.TestCase):
    """Konfidenz falscher (> 10 /min) und richtiger (≤ 5 /min) Reports; ohne falsche Reports ist nichts falsch markiert
    worden, konf_falsch = 0 (vorher NaN und damit ein Verstoß gegen die Grenze, Szenario tachy6)."""

    def test_no_wrong_reports_is_zero(self):
        st = vital_sim.conf_stats(np.array([80.0, 90.0]), np.array([1.0, 2.0]))
        self.assertEqual(st["konf_falsch"], 0.0)
        self.assertEqual(st["n_falsch"], 0)
        self.assertEqual(st["konf_richtig"], 85.0)

    def test_median_of_wrong_reports(self):
        st = vital_sim.conf_stats(np.array([10.0, 30.0, 90.0, 50.0]), np.array([20.0, 15.0, 1.0, 12.0]))
        self.assertEqual(st["konf_falsch"], 30.0)
        self.assertEqual(st["n_falsch"], 3)


class EarlyValueTest(unittest.TestCase):
    """Abschluss-Review I1: erst_s zählt den ersten gültigen Report, auch wenn er falsch ist. Dazu: erster richtiger
    Report (≤ 5 /min) und falsche Reports (> 10 /min) in den ersten 15 s."""

    def test_metrics(self):
        t = np.array([5.0, 6.0, 7.0, 8.0, 9.0, 16.0, 17.0])
        valid = np.array([False, True, True, True, True, True, True])
        err = np.array([np.nan, 30.0, 6.0, 2.0, 1.0, 20.0, 1.0])
        m = vital_sim.early_metrics(t, valid, err)
        self.assertEqual(m["erst_ok_s"], 8.0)
        self.assertEqual(m["frueh_falsch_n"], 1)   # 6 s; 16 s liegt nach den ersten 15 s

    def test_never_correct(self):
        m = vital_sim.early_metrics(np.array([6.0]), np.array([True]), np.array([40.0]))
        self.assertTrue(math.isnan(m["erst_ok_s"]))
        self.assertEqual(m["frueh_falsch_n"], 1)

    def test_limits_pin_known_early_errors(self):
        lim = {(s, k): (op, x) for s, xs in vital_sim.LIMITS.items() for k, op, x in xs}
        self.assertEqual(lim[("tief", "frueh_falsch_n")], ("<=", 1))   # 40,3 statt 71 nach 6 s (3. Atem-Oberwelle)
        self.assertEqual(lim[("base", "frueh_falsch_n")], ("<=", 0))
        self.assertIn(("tief", "erst_ok_s"), lim)


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


class LeaveBenchTest(unittest.TestCase):
    """Abschluss-Review C1: Person geht — danach weder gültiger Puls noch Zustand Apnoe (Prüfen Nr. 1 und 4)."""

    def test_no_apnea_after_leaving(self):
        r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--check", "--scen", "weg"],
                           capture_output=True, text=True)
        self.assertNotIn("VERLETZT weg: weg_apnoe_n", r.stdout)
        self.assertNotIn("VERLETZT weg: weg_gueltig_n", r.stdout)
        self.assertEqual(r.returncode, 0, r.stdout[-1500:])
        self.assertIn(("weg", "weg_apnoe_n", "<=", 0), [(s, *x) for s, xs in vital_sim.LIMITS.items() for x in xs])


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


class ZoomWidthBenchTest(unittest.TestCase):
    """V2.3 Schritt 3: Prüfstand mit 48 Zoom-Punkten (Frames und Header mit A121_ZOOM_POINTS=48) in den Grenzwerten."""

    def test_bench_48_points(self):
        r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--check", "--zoom", "48",
                            "--scen", "base", "flach", "leer"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
        self.assertIn("48 Zoom-Punkte", r.stdout)


class WindowLengthBenchTest(unittest.TestCase):
    """V2.5: Prüfstand mit kürzerem Pulsfenster (Header mit A121_HR_WIN übersetzt)."""

    def test_bench_10_s_window(self):
        r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--hr-win", "600", "--scen", "step"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
        self.assertIn("Pulsfenster 10 s", r.stdout)
        self.assertIn("Reaktion", r.stdout)


class DefineOptionTest(unittest.TestCase):
    """V3: A/B-Varianten im Prüfstand über --define NAME[=WERT] (Header mit -D übersetzt)."""

    def test_define_reaches_header(self):
        r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--define", "A121_HR_WIN=720",
                            "--scen", "base"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
        self.assertIn("Pulsfenster 12 s", r.stdout)
        self.assertIn("Schalter: A121_HR_WIN=720", r.stdout)


class StateBenchTest(unittest.TestCase):
    """V3.5: Prüfstand meldet den Zustand BEWEGUNG im Szenario beweg (50–60 s), sonst nicht."""

    def test_motion_state(self):
        r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--check", "--scen", "beweg", "base"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
        self.assertIn("Zustand BEWEGUNG", r.stdout)
        self.assertIn(("beweg", "zustand_beweg_n", ">=", 5.0), [(k, *x) for k, v in vital_sim.LIMITS.items() for x in v])


class BreathBenchTest(unittest.TestCase):
    """V3.4: Prüfstand wertet @BREATH aus (Rate gegen die wahre Atemfrequenz, erster Wert in Atemzügen); Szenario
    takt6 = Metronom 6 /min (R3, unter dem Spektralband)."""

    def test_breath_metrics(self):
        r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--check", "--scen", "takt6", "base",
                            "niemand"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
        self.assertIn("Atemzug für Atemzug", r.stdout)
        self.assertIn("takt6", vital_sim.SCENARIOS)
        lim = [(k, *x) for k, v in vital_sim.LIMITS.items() for x in v]
        self.assertIn(("takt6", "atemzug_mae", "<=", 1.0), lim)
        self.assertIn(("niemand", "atemzug_n", "<=", 0), lim)
        self.assertIn(("base", "atemzug_erst_n", "<=", 2.5), lim)


class ViterbiBenchTest(unittest.TestCase):
    """V3.2 (Schalter A121_DSP_VITERBI): Variante hält die Grenzwerte, auch mit 12-s-Fenster (V2.5 „mit der Verfolgung
    aus V3.2"); nur Simulation, Wahl an Tag A."""

    def test_variant_within_limits(self):
        for extra in ([], ["--hr-win", "720"]):
            r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--check", "--define",
                                "A121_DSP_VITERBI", "--scen", "brady", "beweg", "step", *extra],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
            self.assertIn("A121_DSP_VITERBI", r.stdout)


class HarmRemoveBenchTest(unittest.TestCase):
    """V3.3 (Schalter A121_DSP_HARM_REMOVE): Variante hält die Grenzwerte, auch mit Viterbi; brady prüft die Regel
    „nur nach sicherem Report" (ohne sie MAE 3,7 > Grenze 3,7)."""

    def test_variant_within_limits(self):
        for extra in ([], ["--define", "A121_DSP_VITERBI"]):
            r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--check", "--define",
                                "A121_DSP_HARM_REMOVE", "--scen", "brady", "harm4", "base", "step", *extra],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])


class CombineBenchTest(unittest.TestCase):
    """V3.1 (Schalter A121_DSP_COMBINE): ein kombinierter Kanal statt Einzelspektren, NUR SIMULATION. Abnahme laut Plan
    „MAE ≤ heute bei ≤ 50 % Rechenzeit". Bekannte Abweichung, festgehalten: Der kombinierte Kanal ist atemlastig, im
    6-s-Fenster liegt die stärkste Spitze bei step und tief auf einer niedrigen Atem-Oberwelle (verworfen), der erste
    Wert kommt dort nach 7 statt 6 s (bei step ist er auch der erste richtige, daher zusätzlich erst_ok_s; tief hat dafür
    ohnehin 7 s als Grenze)."""

    SCEN = ["--scen", "base", "brady", "step", "tief"]
    DEFINE = "A121_DSP_COMBINE"

    def bench(self, *extra: str) -> tuple[list[str], float]:
        r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--check", *self.SCEN, *extra],
                           capture_output=True, text=True)
        viol = sorted(ln.strip() for ln in r.stdout.splitlines() if "VERLETZT" in ln)
        ms = [float(ln.split(":")[1].split("ms")[0]) for ln in r.stdout.splitlines()
              if ln.startswith("Rechenzeit je Report am PC")]
        self.assertEqual(len(ms), 1, r.stdout[-3000:] + r.stderr[-2000:])
        return viol, ms[0]

    def test_variant_limits_and_compute(self):
        viol_std, ms_std = self.bench()
        viol, ms = self.bench("--define", self.DEFINE)
        self.assertEqual(viol_std, [])
        self.assertEqual(viol, ["VERLETZT step: erst_ok_s = 7.0, Grenze <= 6.0", "VERLETZT step: erst_s = 7.0, Grenze <= 6.0",
                                "VERLETZT tief: erst_s = 7.0, Grenze <= 6.0"])
        self.assertLessEqual(ms, 0.5 * ms_std)


class IbiTemplateBenchTest(unittest.TestCase):
    """V3.6 (Schalter A121_DSP_IBI_TEMPLATE): Einzelschläge mit Vorlage, NUR SIMULATION. Mehr Reports mit RMSSD (mehr
    Schläge bestehen die Artefakt-Regeln), RMSSD gemessen/wahr in den Grenzen. Bekannt: tief verwirft den 6-s-Wert
    (40,3 statt 71 /min, den der Standard über die zirkuläre IBI-Bestätigung gelten lässt), erster Wert nach 7 s."""

    SCEN = ["--scen", "base", "step", "tief", "tachy"]
    DEFINE = "A121_DSP_IBI_TEMPLATE"

    def bench(self, *extra: str) -> tuple[list[str], dict[str, int]]:
        import re
        r = subprocess.run([sys.executable, str(HOST / "vital_sim.py"), "bench", "--check", *self.SCEN, *extra],
                           capture_output=True, text=True)
        viol = sorted(ln.strip() for ln in r.stdout.splitlines() if "VERLETZT" in ln)
        share = {}
        for ln in r.stdout.splitlines():
            m = re.match(r"^(base|step|tief|tachy)\s.*ms \(\s*(\d+) %\)", ln)
            if m:
                share[m.group(1)] = int(m.group(2))
        self.assertEqual(len(share), 4, r.stdout[-3000:] + r.stderr[-2000:])
        return viol, share

    def test_variant_rmssd_share_and_limits(self):
        viol_std, share_std = self.bench()
        viol, share = self.bench("--define", self.DEFINE)
        self.assertEqual(viol_std, [])
        self.assertEqual(viol, ["VERLETZT tief: erst_s = 7.0, Grenze <= 6.0"])
        for k in share:
            self.assertGreater(share[k], share_std[k] + 20, (k, share, share_std))


if __name__ == "__main__":
    unittest.main()
