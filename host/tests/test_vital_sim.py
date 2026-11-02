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


class ReacqMetricsTest(unittest.TestCase):
    """V6: Wiederfinden nach Bewegung (Zeit bis zum ersten richtigen Report, falsche Reports danach)."""

    def test_first_correct_after_episode(self):
        t = np.arange(50.0, 100.0)
        v = t >= 66
        e = np.where(t >= 68, 1.0, 20.0)
        m = vital_sim.reacq_metrics(t, v, e, [(60.0, 63.0)])
        self.assertEqual(m["wiederfind_s"], 5.0)          # 68 - 63
        self.assertEqual(m["falsch_nach_beweg_n"], 2)     # 66, 67 gültig und > 10 daneben

    def test_never_found_is_nan(self):
        t = np.arange(50.0, 100.0)
        m = vital_sim.reacq_metrics(t, np.zeros_like(t, bool), np.ones_like(t), [(60.0, 63.0)])
        self.assertTrue(math.isnan(m["wiederfind_s"]))
        self.assertEqual(m["falsch_nach_beweg_n"], 0)
        self.assertEqual(m["nie_gefunden_n"], 1)

    def test_one_found_one_never_found(self):
        t = np.arange(0.0, 100.0)
        e = np.where(t <= 50, 1.0, 20.0)   # ab 51 s dauerhaft falsch
        m = vital_sim.reacq_metrics(t, np.ones_like(t, bool), e, [(10.0, 12.0), (60.0, 62.0)])
        self.assertEqual(m["nie_gefunden_n"], 1)
        self.assertEqual(m["wiederfind_s"], 1.0)   # nur die gefundene Episode zählt, endlich
        self.assertEqual(m["falsch_nach_beweg_n"], 30)

    def test_median_over_episodes_and_no_double_count(self):
        t = np.arange(0.0, 100.0)
        v = np.ones_like(t, bool)
        e = np.where(((t > 12) & (t < 17)) | ((t > 32) & (t < 40)), 20.0, 1.0)   # falsch 13–16 und 33–39
        m = vital_sim.reacq_metrics(t, v, e, [(10.0, 12.0), (30.0, 32.0)])
        self.assertEqual(m["wiederfind_s"], (5.0 + 8.0) / 2)   # Median aus 17 - 12 und 40 - 32
        self.assertEqual(m["nie_gefunden_n"], 0)
        self.assertEqual(m["falsch_nach_beweg_n"], 4 + 7)


class SectionMetricsTest(unittest.TestCase):
    def test_share_and_mae(self):
        t = np.arange(0.0, 10.0)
        v = t % 2 == 0
        e = np.full(10, 1.5)
        m = vital_sim.section_metrics(t, v, e, 0.0, 10.0)
        self.assertAlmostEqual(m["abschnitt_gueltig_pct"], 44.4, places=1)  # t = 2,4,6,8 von 1…9
        self.assertAlmostEqual(m["abschnitt_mae"], 1.5)

    def test_no_valid_report_is_nan_mae(self):
        t = np.arange(0.0, 10.0)
        m = vital_sim.section_metrics(t, np.zeros(10, bool), np.ones(10), 2.0, 8.0)
        self.assertEqual(m["abschnitt_gueltig_pct"], 0.0)
        self.assertTrue(math.isnan(m["abschnitt_mae"]))


class MotionModelTest(unittest.TestCase):
    """V6: Bewegungsarten von make_frames (Verschiebung, Stöße, mehrere Episoden), NUR SIMULATION."""

    def frames(self, **kw):
        """Zoom-Phase (relativ zum ruhigen Anfang) am stärksten Punkt, ohne Atmung und Herzschlag."""
        kw = dict(T=100.0, breath_pp_mm=0.0, heart_pp_um=0.0) | kw
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "m"
            vital_sim.make_frames(p, seed=3, **kw)
            iq = np.fromfile(f"{p}.iq", "<i2").reshape(-1, vital_sim.ZOOM_N + vital_sim.ROOM_N + 1, 2).astype(float)
        z = (iq[:, 1:1 + vital_sim.ZOOM_N, 0] + 1j * iq[:, 1:1 + vital_sim.ZOOM_N, 1])
        k = int(np.argmax(np.abs(z[:int(10 * vital_sim.FS)]).mean(axis=0)))
        z = z[:, k]
        t = np.arange(len(z)) / vital_sim.FS
        return t, z, z[:int(10 * vital_sim.FS)].mean()

    def test_shift_moves_chest_echo(self):
        t, z, z0 = self.frames(shift=(60.0, 2.0, 8.0, 0.7))
        ph = np.unwrap(np.angle(z))
        jump = abs(ph[(t > 66) & (t < 74)].mean() - ph[(t > 50) & (t < 58)].mean())
        self.assertGreater(jump, 15.0)    # 4π · 8 mm / λ ≈ 20 rad
        self.assertLess(jump, 25.0)
        self.assertLess(np.abs(z[(t > 66) & (t < 74)]).mean(), np.abs(z[(t > 50) & (t < 58)]).mean())

    def test_jolts_only_inside_window(self):
        t, z, z0 = self.frames(jolts=(60.0, 80.0, 2.0))
        dev = np.angle(z * np.conj(z0))
        inside = np.sqrt(np.mean(dev[(t > 60) & (t < 80)] ** 2))
        outside = np.sqrt(np.mean(dev[(t < 59.5) | (t > 80.5)] ** 2))
        self.assertGreater(inside, 0.3)
        self.assertLess(outside, 0.1 * inside)

    def test_motion_list(self):
        eps = [(20.0, 22.0, 4.0), (40.0, 42.0, 4.0), (60.0, 62.0, 4.0)]
        t, z, z0 = self.frames(motion=eps)
        dev = np.abs(np.angle(z * np.conj(z0)))
        inside = np.zeros(len(t), bool)
        for a, b, _ in eps:
            inside |= (t > a) & (t < b)
            self.assertGreater(np.sqrt(np.mean(dev[(t > a) & (t < b)] ** 2)), 0.5, (a, b))
        self.assertLess(np.sqrt(np.mean(dev[~inside] ** 2)), 0.1 * np.sqrt(np.mean(dev[inside] ** 2)))

    def test_single_tuple_equals_list_of_one(self):
        _, z1, _ = self.frames(motion=(20.0, 22.0, 4.0))
        _, z2, _ = self.frames(motion=[(20.0, 22.0, 4.0)])
        self.assertTrue(np.array_equal(z1, z2))

    def test_new_scenarios_defined(self):
        for nm in ("kurz", "zappel", "lang", "lage", "beweg_hr", "beweg_weg", "tippen"):
            self.assertIn(nm, vital_sim.SCENARIOS)
        self.assertEqual(vital_sim.SCENARIOS["lang"][1]["T"], 150.0)
        self.assertEqual(vital_sim.SCENARIOS["lage"][1]["shift"], (60, 2, 8.0, 0.7))
        self.assertEqual(vital_sim.SCENARIOS["tippen"][1]["jolts"], (60, 120, 0.3))
        self.assertEqual(vital_sim.SCENARIOS["beweg_weg"][1]["leave"], 60)
        # bestehende Szenarien behalten ihre Seeds (Index in SCENARIOS), neue stehen hinten
        self.assertEqual(list(vital_sim.SCENARIOS)[13:15], ["weg", "kurz"])

    def test_episodes_cover_all_motion_kinds(self):
        S = vital_sim.SCENARIOS
        self.assertEqual(vital_sim.episodes_of(S["beweg"][1]), [(50, 60)])
        self.assertEqual(vital_sim.episodes_of(S["zappel"][1]), [(40, 42), (60, 62), (80, 82)])
        self.assertEqual(vital_sim.episodes_of(S["lage"][1]), [(60, 62)])
        self.assertEqual(vital_sim.episodes_of(S["tippen"][1]), [(60, 120)])
        self.assertEqual(vital_sim.episodes_of(S["base"][1]), [])


class ReacqLimitsTest(unittest.TestCase):
    """V6: LIMITS_REACQ gilt nur mit dem Schalter A121_DSP_REACQ; die neuen Szenarien haben Grenzen ohne Schalter."""

    def test_limits_reacq_values(self):
        lim = {(s, k): (op, x) for s, xs in vital_sim.LIMITS_REACQ.items() for k, op, x in xs}
        for s in ("kurz", "zappel", "lage"):
            self.assertEqual(lim[(s, "wiederfind_s")], ("<=", 8.0))
        self.assertEqual(lim[("lang", "wiederfind_s")], ("<=", 10.0))
        for s in ("kurz", "zappel", "lang", "lage", "beweg_hr", "beweg_weg", "tippen"):
            self.assertEqual(lim[(s, "falsch_nach_beweg_n")], ("<=", 0))
            self.assertEqual(lim[(s, "nie_gefunden_n")], ("<=", 0))
        self.assertEqual(lim[("beweg_weg", "weg_gueltig_n")], ("<=", 0))
        self.assertEqual(lim[("tippen", "abschnitt_gueltig_pct")], (">=", 80.0))
        self.assertEqual(lim[("tippen", "abschnitt_mae")], ("<=", 2.0))

    def test_active_limits_switch(self):
        old = list(vital_sim.DEFINES)
        self.addCleanup(lambda: vital_sim.DEFINES.__setitem__(slice(None), old))
        vital_sim.DEFINES[:] = []
        self.assertEqual(vital_sim.active_limits(), vital_sim.LIMITS)
        vital_sim.DEFINES[:] = ["A121_DSP_VITERBI"]
        self.assertEqual(vital_sim.active_limits(), vital_sim.LIMITS)
        want = {k: vital_sim.LIMITS.get(k, []) + vital_sim.LIMITS_REACQ.get(k, [])
                for k in {**vital_sim.LIMITS, **vital_sim.LIMITS_REACQ}}
        for define in ("A121_DSP_REACQ", "A121_DSP_REACQ=1"):
            vital_sim.DEFINES[:] = [define]
            got = vital_sim.active_limits()
            self.assertEqual(got, want, define)
            self.assertIn(("wiederfind_s", "<=", 8.0), got["kurz"])
            self.assertEqual(got["base"], vital_sim.LIMITS["base"])
        self.assertNotIn(("wiederfind_s", "<=", 8.0), vital_sim.LIMITS["kurz"])   # LIMITS selbst bleibt unverändert

    def test_new_scenarios_have_baseline_limits(self):
        for s in ("kurz", "zappel", "lang", "lage", "beweg_hr", "beweg_weg", "tippen"):
            names = [k for k, _, _ in vital_sim.LIMITS[s]]
            self.assertIn("falsch_nach_beweg_n", names)
            self.assertIn("zustand_beweg_fehl", names)
            self.assertIn("atemzug_mae", names)
            self.assertNotIn("wiederfind_s", names)    # ohne Schalter keine Wiederfind-Grenze


if __name__ == "__main__":
    unittest.main()
