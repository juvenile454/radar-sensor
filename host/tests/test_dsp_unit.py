"""Einzeltests für Funktionen im Firmware-DSP-Header (am PC übersetzt; nur Logik, keine Laufzeitprüfung am Gerät).

  .venv/bin/python -m unittest host/tests/test_dsp_unit.py
"""

import pathlib
import subprocess
import sys
import tempfile
import unittest

HOST = pathlib.Path(__file__).resolve().parent.parent
FW = HOST.parent / "firmware" / "esp32s3" / "src"
SHIM = HOST / "dsp_replay"


def run_cpp(body: str) -> str:
    """Übersetzt ein kleines Programm gegen a121_esp_dsp.h und gibt seine Ausgabe zurück."""
    src = '#include <Arduino.h>\n#include "a121_esp_dsp.h"\nusing namespace a121_dsp;\nint main() {\n' + body + "\nreturn 0; }\n"
    with tempfile.TemporaryDirectory() as d:
        c = pathlib.Path(d) / "t.cpp"
        exe = pathlib.Path(d) / "t"
        c.write_text(src)
        subprocess.run(["g++", "-O1", "-std=c++17", "-Wall", f"-I{SHIM}", f"-I{FW}", str(c), "-o", str(exe), "-lm"],
                       check=True, capture_output=True, text=True)
        return subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout


def ring_code(points: str, n: int) -> str:
    """C++: Ring mit kHistoryLen Einträgen [I,Q], die ersten n aus dem Ausdruck points(k) → (i, q) gefüllt."""
    return f"""
    static float ring[kHistoryLen * 2];
    const uint16_t N = {n};
    float mi = 0, mq = 0;
    for (uint16_t k = 0; k < N; k++) {{ {points}; ring[2 * k] = pi; ring[2 * k + 1] = pq; mi += pi; mq += pq; }}
    mi /= N; mq /= N;
    float ci = 0, cq = 0;
    """


class CircleCenterTest(unittest.TestCase):
    def test_full_circle(self):
        out = run_cpp(ring_code("float a = 6.2832f * k / N; float pi = 300 + 1000 * cosf(a), pq = -200 + 1000 * sinf(a)", 900)
                      + 'bool ok = circleCenter(ring, 0, N, mi, mq, ci, cq); printf("%d %.2f %.2f", ok, ci, cq);')
        ok, ci, cq = out.split()
        self.assertEqual(ok, "1")
        self.assertAlmostEqual(float(ci), 300.0, delta=1.0)
        self.assertAlmostEqual(float(cq), -200.0, delta=1.0)

    def test_short_arc_with_noise(self):
        # Bogen 2,5 rad Spitze-Spitze (≈ 1 mm Atemhub), Rauschen 1 % des Radius: Mittelpunkt auf 5 % des Radius genau
        body = ring_code("float a = 1.27f * sinf(6.2832f * k / 240.0f); float e1 = 10.0f * sinf(k * 12.9898f), "
                         "e2 = 10.0f * cosf(k * 78.233f); float pi = 1000 * cosf(a) + e1 + 500, pq = 1000 * sinf(a) + e2", 900)
        out = run_cpp(body + 'bool ok = circleCenter(ring, 0, N, mi, mq, ci, cq); printf("%d %.2f %.2f %.2f %.2f", ok, ci, cq, mi, mq);')
        ok, ci, cq, mi, mq = (float(x) for x in out.split())
        self.assertEqual(ok, 1)
        self.assertLess(((ci - 500) ** 2 + cq ** 2) ** 0.5, 50.0)
        self.assertGreater(((mi - 500) ** 2 + mq ** 2) ** 0.5, 100.0)  # der Mittelwert liegt deutlich daneben

    def test_ring_wraps_around(self):
        # Fenster läuft über das Ringende: Start bei kHistoryLen − 100
        body = """
        static float ring[kHistoryLen * 2];
        const uint16_t N = 600, start = kHistoryLen - 100;
        float mi = 0, mq = 0;
        for (uint16_t k = 0; k < N; k++) {
            float a = 6.2832f * k / N; uint16_t j = (start + k) % kHistoryLen;
            ring[2 * j] = -400 + 800 * cosf(a); ring[2 * j + 1] = 100 + 800 * sinf(a); mi += ring[2 * j]; mq += ring[2 * j + 1];
        }
        mi /= N; mq /= N;
        float ci = 0, cq = 0;
        bool ok = circleCenter(ring, start, N, mi, mq, ci, cq); printf("%d %.2f %.2f", ok, ci, cq);"""
        ok, ci, cq = run_cpp(body).split()
        self.assertEqual(ok, "1")
        self.assertAlmostEqual(float(ci), -400.0, delta=1.0)
        self.assertAlmostEqual(float(cq), 100.0, delta=1.0)

    def test_degenerate_falls_back_to_mean(self):
        # alle Punkte auf einer Geraden: kein Kreis bestimmbar → false, Mittelwert zurück
        out = run_cpp(ring_code("float pi = 100 + k, pq = 50 + 2 * k", 300)
                      + 'bool ok = circleCenter(ring, 0, N, mi, mq, ci, cq); printf("%d %.3f %.3f %.3f %.3f", ok, ci, cq, mi, mq);')
        ok, ci, cq, mi, mq = (float(x) for x in out.split())
        self.assertEqual(ok, 0)
        self.assertAlmostEqual(ci, mi, places=3)
        self.assertAlmostEqual(cq, mq, places=3)

    def test_constant_points_fall_back(self):
        out = run_cpp(ring_code("float pi = 7, pq = 9", 100)
                      + 'bool ok = circleCenter(ring, 0, N, mi, mq, ci, cq); printf("%d %.3f %.3f", ok, ci, cq);')
        ok, ci, cq = out.split()
        self.assertEqual(ok, "0")
        self.assertAlmostEqual(float(ci), 7.0, places=3)
        self.assertAlmostEqual(float(cq), 9.0, places=3)


class BandpassTest(unittest.TestCase):
    def test_coefficients_match_scipy(self):
        """kBpSos = scipy.signal.butter(4, [0.6, 5.0], 'band', fs=60, output='sos') (V1.2, Pulsband 40–180 /min)."""
        import re
        import numpy as np
        from scipy import signal
        txt = (FW / "a121_esp_dsp.h").read_text()
        block = txt[txt.index("kBpSos[4][6]"):]
        block = block[:block.index("};")]
        nums = [float(x) for x in re.findall(r"-?\d+\.\d+(?:e-?\d+)?", block)]
        sos = signal.butter(4, [0.6, 5.0], "band", fs=60, output="sos")
        np.testing.assert_allclose(np.array(nums).reshape(4, 6), sos, rtol=1e-6, atol=1e-12)

    def test_search_band_40_to_180(self):
        out = run_cpp('printf("%.3f %.3f", (kHrFMin + kHrFStep * kHrEdgeBins) * 60, '
                      '(kHrFMin + kHrFStep * (kHrNumF - 1 - kHrEdgeBins)) * 60);')
        lo, hi = (float(x) for x in out.split())
        self.assertAlmostEqual(lo, 40.2, delta=0.5)
        self.assertAlmostEqual(hi, 180.0, delta=0.5)


class SignalQualityTest(unittest.TestCase):
    """Konfidenz aus Spitzenhöhe, Oberwelle, Einigkeit Spektrum/AKF/IBI, Bewegung und Sprung (V1.3)."""

    def quality(self, **kw) -> float:
        q = dict(peak_db=15.0, on_harmonic="false", hr_spec=72.0, hr_acf=72.0, hr_ibi=72.0, hr_fused=72.0,
                 hr_fused_5s_ago=72.0, motion_pct=0.0)
        q.update(kw)
        init = ", ".join(str(q[k]) + ("f" if isinstance(q[k], float) else "") for k in
                         ("peak_db", "on_harmonic", "hr_spec", "hr_acf", "hr_ibi", "hr_fused", "hr_fused_5s_ago",
                          "motion_pct"))
        return float(run_cpp(f"QualityInputs q = {{{init}}}; printf(\"%.3f\", signalQuality(q));"))

    def test_clear_peak_full_agreement(self):
        self.assertAlmostEqual(self.quality(), 100.0, places=2)

    def test_peak_height_scales(self):
        self.assertAlmostEqual(self.quality(peak_db=3.0), 0.0, places=2)
        self.assertAlmostEqual(self.quality(peak_db=9.0), 50.0, places=2)
        self.assertAlmostEqual(self.quality(peak_db=30.0), 100.0, places=2)

    def test_harmonic_halves(self):
        self.assertAlmostEqual(self.quality(on_harmonic="true"), 50.0, places=2)

    def test_agreement_two_and_one(self):
        self.assertAlmostEqual(self.quality(hr_ibi=90.0), 70.0, places=2)
        self.assertAlmostEqual(self.quality(hr_ibi=90.0, hr_acf=50.0), 50.0, places=2)

    def test_motion_and_jump(self):
        self.assertAlmostEqual(self.quality(motion_pct=50.0), 50.0, places=2)
        self.assertAlmostEqual(self.quality(hr_fused_5s_ago=60.0), 70.0, places=2)


class CardiacAcfTest(unittest.TestCase):
    """Autokorrelation des Herzsignals: nur echte lokale Maxima im Bereich 40–180 /min (V1.2)."""

    def acf(self, expr: str) -> tuple[float, float]:
        out = run_cpp(f"""
        static float x[900];
        for (int n = 0; n < 900; n++) {{ const float t = n / 60.0f; x[n] = {expr}; }}
        float bpm = -1, peak = -1;
        cardiacAcf(x, 900, 60.0f, bpm, peak);
        printf("%.3f %.3f", bpm, peak);""")
        bpm, peak = (float(v) for v in out.split())
        return bpm, peak

    def test_no_peak_at_lower_lag_edge(self):
        # 37 /min liegt unter dem Suchbereich: Die AKF fällt am kleinsten Abstand nur ab, das ist keine Spitze
        _, peak = self.acf("cosf(6.2832f * 0.62f * t)")
        self.assertEqual(peak, 0.0)

    def test_fundamental_with_strong_second_harmonic(self):
        bpm, peak = self.acf("0.6f * cosf(6.2832f * 0.8333f * t) + cosf(6.2832f * 1.6667f * t)")
        self.assertGreater(peak, 0.5)
        self.assertAlmostEqual(bpm, 50.0, delta=1.5)


class RemoveBreathHarmonicsTest(unittest.TestCase):
    """Atem-Oberwellen m · f_Atem (m = 1..8) aus dem Herzsignal abziehen, bevor AKF und Einzelschläge rechnen; m nahe
    am Puls (und an 2 × Puls) bleiben (wie V3.3, ±0,1 Hz)."""

    def run_case(self, expr: str, fb: float, keep: float, ref: str) -> float:
        out = run_cpp(f"""
        static float x[900];
        for (int n = 0; n < 900; n++) {{ const float t = n / 60.0f; x[n] = {expr}; }}
        removeBreathHarmonics(x, 900, 60.0f, {fb}f, {keep}f);
        double e = 0;
        for (int n = 0; n < 900; n++) {{ const float t = n / 60.0f; const double d = x[n] - ({ref}); e += d * d; }}
        printf("%.4f", sqrt(e / 900));""")
        return float(out)

    def test_removes_breath_harmonics_keeps_pulse(self):
        rms = self.run_case("700 * cosf(6.2832f * 0.75f * t + 0.3f) + 300 * cosf(6.2832f * 1.0f * t - 1.0f) "
                            "+ 30 * cosf(6.2832f * 1.2f * t)", 0.25, 1.2, "30 * cosf(6.2832f * 1.2f * t)")
        self.assertLess(rms, 1.5)  # < 5 % der Puls-Amplitude, Atem-Oberwellen 10–20 × größer

    def test_harmonic_on_pulse_stays(self):
        # m = 4 liegt auf dem Puls und bleibt; die übrigen m nehmen ihm höchstens 2 % (Rest-Leckage über das Fenster)
        rms = self.run_case("30 * cosf(6.2832f * 1.0f * t)", 0.25, 1.0, "30 * cosf(6.2832f * 1.0f * t)")
        self.assertLess(rms, 0.6)

    def test_no_breath_no_change(self):
        rms = self.run_case("200 * cosf(6.2832f * 0.75f * t)", 0.0, 1.2, "200 * cosf(6.2832f * 0.75f * t)")
        self.assertLess(rms, 0.01)


class BreathAmplitudeTest(unittest.TestCase):
    """Atem-Amplitude je Report: Spitze-Spitze des 1-s-Mittels über die letzten 10 s, Trend entfernt (V1.4)."""

    def amp(self, expr: str) -> float:
        return float(run_cpp(f"""
        static float x[900];
        for (int n = 0; n < 900; n++) {{ const float t = n / 60.0f; x[n] = {expr}; }}
        printf("%.3f", breathAmplitude(x, 900, 60.0f));"""))

    def test_breathing_gives_about_peak_to_peak(self):
        for rate in (0.1, 0.25, 0.4):  # 6, 15, 24 /min
            a = self.amp(f"1000 * sinf(6.2832f * {rate}f * t)")
            self.assertGreater(a, 1000.0, rate)
            self.assertLess(a, 2000.0, rate)

    def test_held_breath_with_heartbeat_and_drift(self):
        # angehalten: Lage driftet 40 µm/s, Herzschlag 100 µm Spitze-Spitze bei 76 /min
        a = self.amp("40.0f * t + 50 * sinf(6.2832f * 1.267f * t)")
        self.assertLess(a, 40.0)


class BreathStateTest(unittest.TestCase):
    """Atemzustand (V1.4): 0 normal, 1 keine Atemspitze, 2 angehalten (< 10 % des 60-s-Medians, 10 s lang),
    3 Bewegung (> 50 %), 4 flach (< 70 % des Medians, 10 s lang)."""

    def states(self, steps: str) -> list[int]:
        """steps: C++-Ausdrücke für (amp, peak, motion) je Sekunde k; Ausgabe Zustand je Sekunde."""
        out = run_cpp(f"""
        BreathStateTracker bs; bs.reset();
        for (int k = 0; k < 120; k++) {{
            float amp, motion; bool peak; {steps};
            printf("%u ", (unsigned)bs.update(amp, peak, motion));
        }}""")
        return [int(v) for v in out.split()]

    def test_apnea_after_ten_seconds_without_breath(self):
        # 40 s Atmung, ab k = 40 nichts mehr; das 10-s-Fenster ist ab k = 50 frei von Atmung (Amplitude 30)
        st = self.states("amp = (k < 50) ? 2000.0f : 30.0f; peak = true; motion = 0")
        self.assertNotIn(2, st[:50])
        self.assertEqual(st[50], 2)   # 10 s nach dem letzten Atemzug
        self.assertEqual(set(st[50:]), {2})   # bleibt, auch wenn das Anhalten länger als 30 s dauert

    def test_breath_returns(self):
        st = self.states("amp = (k >= 50 && k < 65) ? 30.0f : 2000.0f; peak = true; motion = 0")
        self.assertEqual(st[60], 2)
        self.assertEqual(st[65], 0)

    def test_shallow(self):
        st = self.states("amp = (k < 40) ? 2000.0f : 1000.0f; peak = true; motion = 0")
        self.assertEqual(st[39], 0)
        self.assertEqual(st[40], 4)

    def test_motion_and_no_peak(self):
        st = self.states("amp = 2000.0f; peak = (k < 30); motion = (k >= 60) ? 70.0f : 0.0f")
        self.assertEqual(st[20], 0)
        self.assertEqual(st[40], 1)
        self.assertEqual(st[70], 3)

    def test_no_apnea_before_reference(self):
        st = self.states("amp = 30.0f; peak = true; motion = 0")
        self.assertNotIn(2, st)

    def test_unknown_amplitude_is_no_peak(self):
        st = self.states("amp = (k < 30) ? 2000.0f : -1.0f; peak = true; motion = 0")
        self.assertEqual(st[40], 1)


class DspLineTest(unittest.TestCase):
    def test_br_state_after_point_agree(self):
        out = run_cpp('VitalReport r = {}; r.point_agree_pct = 55; r.br_state = 2; char b[256];'
                      'formatDspLine(b, sizeof(b), 1000, true, r, 5); printf("%s", b);')
        f = out.split(",")
        self.assertEqual(f[0], "@DSP")
        self.assertEqual(f[8], "55")
        self.assertEqual(f[9], "2")


if __name__ == "__main__":
    unittest.main()
