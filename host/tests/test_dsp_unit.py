"""Einzeltests für Funktionen im Firmware-DSP-Header (am PC übersetzt; nur Logik, keine Laufzeitprüfung am Gerät).

  .venv/bin/python -m unittest host/tests/test_dsp_unit.py
"""

import math
import pathlib
import subprocess
import sys
import tempfile
import unittest

HOST = pathlib.Path(__file__).resolve().parent.parent
FW = HOST.parent / "firmware" / "esp32s3" / "src"
SHIM = HOST / "dsp_replay"


def run_cpp(body: str, flags: tuple[str, ...] = ()) -> str:
    """Übersetzt ein kleines Programm gegen a121_esp_dsp.h und gibt seine Ausgabe zurück (flags: z. B. -D…)."""
    src = ('#include <Arduino.h>\n#include <vector>\n#include <random>\n#include "a121_esp_dsp.h"\nusing namespace a121_dsp;\nint main() {\n'
           + body + "\nreturn 0; }\n")
    with tempfile.TemporaryDirectory() as d:
        c = pathlib.Path(d) / "t.cpp"
        exe = pathlib.Path(d) / "t"
        c.write_text(src)
        cc = subprocess.run(["g++", "-O1", "-std=c++17", "-Wall", *flags, f"-I{SHIM}", f"-I{FW}", str(c), "-o", str(exe),
                             "-lm"],
                            capture_output=True, text=True)
        if cc.returncode != 0:
            raise AssertionError("Übersetzen fehlgeschlagen:\n" + cc.stderr[-3000:])
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
                 hr_fused_5s_ago=72.0, motion_pct=0.0, sat_pct=0.0, win_frac=1.0)
        q.update(kw)
        init = ", ".join(str(q[k]) + ("f" if isinstance(q[k], float) else "") for k in
                         ("peak_db", "on_harmonic", "hr_spec", "hr_acf", "hr_ibi", "hr_fused", "hr_fused_5s_ago",
                          "motion_pct", "sat_pct", "win_frac"))
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

    def test_saturation_halves_from_one_percent(self):
        """V1.5: gesättigte Frames im Fenster ab 1 % → × 0,5"""
        self.assertAlmostEqual(self.quality(sat_pct=0.5), 100.0, places=2)
        self.assertAlmostEqual(self.quality(sat_pct=2.0), 50.0, places=2)

    def test_short_window_scales(self):
        """V2.4: wachsendes Fenster ab 6 s, Konfidenz × N/900"""
        self.assertAlmostEqual(self.quality(win_frac=0.4), 40.0, places=2)

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

    def test_reference_forgotten_after_echo_lost(self):
        """Abschluss-Review C1: Person geht (kein sauberes Echo, amp < 0), danach ein Rausch-Report mit „Echo" — das ist
        kein Atemanhalten; die Referenz der gegangenen Person gilt nach 3 Reports ohne Echo nicht mehr."""
        st = self.states("amp = (k < 40) ? 2000.0f : (k < 43) ? -1.0f : 30.0f; peak = (k < 40); motion = 0")
        self.assertNotIn(2, st[43:])
        # kurzer Echo-Ausfall (2 Reports) beim Anhalten: Referenz bleibt, Apnoe wird erkannt
        st = self.states("amp = (k < 40) ? 2000.0f : (k < 42) ? -1.0f : 30.0f; peak = (k < 40); motion = 0")
        self.assertEqual(st[42], 2)

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

    def test_sensor_counters_at_end(self):
        out = run_cpp('VitalReport r = {}; r.n_delayed = 3; r.n_sat = 2; r.n_tickgap = 1; char b[256];'
                      'formatDspLine(b, sizeof(b), 1000, true, r, 5); printf("%s", b);')
        self.assertEqual(out.split(",")[10:13], ["3", "2", "1"])

    def test_snap_frame_last(self):
        """Feld 13 = frame_count des Schnappschusses (V2.1): dort rechnet die Nachrechnung den Report."""
        out = run_cpp('VitalReport r = {}; r.frame_count = 1234; char b[256];'
                      'formatDspLine(b, sizeof(b), 1000, true, r, 5); printf("%s", b);')
        self.assertEqual(out.split(",")[13], "1234")

    def test_reacq_fields_at_end(self):
        """V6: win_s und reacq hinten (Feld 15, 16); Feld 14 (n_eff) ist ohne A121_DSP_COMBINE leer."""
        out = run_cpp('VitalReport r{}; r.win_s = 7; r.reacq = 2; char b[256]; '
                      'formatDspLine(b, sizeof b, 1000, true, r, 5); puts(b);', ("-DA121_DSP_REACQ",))
        p = out.strip().split(",")
        self.assertEqual(len(p), 17)
        self.assertEqual(p[14], "")
        self.assertEqual((p[15], p[16]), ("7", "2"))

    def test_reacq_with_combine_has_n_eff(self):
        out = run_cpp('VitalReport r{}; r.n_eff = 1.4f; r.win_s = 7; r.reacq = 1; char b[256]; '
                      'formatDspLine(b, sizeof b, 1000, true, r, 5); puts(b);',
                      ("-DA121_DSP_REACQ", "-DA121_DSP_COMBINE"))
        p = out.strip().split(",")
        self.assertEqual(len(p), 17)
        self.assertEqual((p[14], p[15], p[16]), ("1.4", "7", "1"))

    def test_reacq_constants(self):
        out = run_cpp('printf("%u %u %u", (unsigned)kReacqNone, (unsigned)kReacqShort, (unsigned)kReacqMemory);')
        self.assertEqual(out.split(), ["0", "1", "2"])

    def test_line_unchanged_without_switch(self):
        out = run_cpp('VitalReport r{}; char b[256]; formatDspLine(b, sizeof b, 1000, true, r, 5); puts(b);')
        self.assertEqual(len(out.strip().split(",")), 14)


class FrameFlagsTest(unittest.TestCase):
    """V1.5 (B15, B16): Sensor-Flags je Frame, Zähler je Report (seit dem letzten), tick-Lücken > 25 ms ohne Rezoom."""

    PUSH = """
    static Complex16 pts[kTotal3SubPts];
    static UltimateEspDsp d; d.begin(244, 120, kRoomStep);
    auto push = [&](uint32_t tick, bool sat, bool del) {
        FrameFlags ff; ff.tick = tick; ff.have_tick = true; ff.saturated = sat; ff.delayed = del;
        float um = 0; d.pushFrame(pts, kTotal3SubPts, 20, 0.0f, um, ff);
    };
    VitalReport r;
    """

    def counters(self, body: str) -> list[str]:
        return run_cpp(self.PUSH + body).split()

    def test_counts_since_last_report(self):
        out = self.counters("""
        uint32_t t = 0;
        for (int i = 0; i < 10; i++) { t += (i == 5) ? 60 : 17; push(t, i == 2 || i == 3, i >= 7); }
        d.computeVitalReport(r); printf("%u %u %u ", r.n_delayed, r.n_sat, r.n_tickgap);
        push(t + 17, false, false);
        d.computeVitalReport(r); printf("%u %u %u", r.n_delayed, r.n_sat, r.n_tickgap);""")
        self.assertEqual(out, ["3", "2", "1", "0", "0", "0"])

    def test_no_tick_gap_across_rezoom(self):
        out = self.counters("""
        push(100, false, false); push(117, false, false);
        d.setZoomStartPoint(250);
        push(5000, false, false); push(5017, false, false);
        d.computeVitalReport(r); printf("%u", r.n_tickgap);""")
        self.assertEqual(out, ["0"])



class TimeDomainIbiTest(unittest.TestCase):
    """Einzelschläge (V1.6, B7): Spitzen parabolisch verfeinert, Sperrzeit 0,6 × 60 / Puls, IBI 40–180 /min, IBI
    > 20 % vom Median der 5 vorigen verworfen, RMSSD nur bei ≥ 90 % gültigen erwarteten Schlägen."""

    def ibi(self, beats: str, hr_bpm: float, extra: str = "0.0f") -> dict:
        out = run_cpp(f"""
        static float x[900];
        std::vector<float> tb; {{ {beats} }}
        double mean = 0;
        for (int n = 0; n < 900; n++) {{
            const float t = n / 60.0f; float v = 0;
            for (float b : tb) {{ const float u = (t - b) / 0.05f; v += expf(-0.5f * u * u); }}
            x[n] = v + {extra}; mean += x[n];
        }}
        for (int n = 0; n < 900; n++) x[n] -= (float)(mean / 900);
        IbiResult r = timeDomainIbi(x, 900, 60.0f, {hr_bpm}f);
        printf("%.3f %.3f %.3f %u %u", r.bpm, r.ibi_ms, r.rmssd_ms, r.n_beats, r.n_valid);""")
        k = ("bpm", "ibi_ms", "rmssd_ms", "n_beats", "n_valid")
        return dict(zip(k, (float(v) for v in out.split())))

    def test_rmssd_from_refined_peaks(self):
        # IBI abwechselnd 800 / 850 ms, Schlagzeiten zwischen den Abtastwerten: RMSSD genau 50 ms
        r = self.ibi("float t = 0.31f; for (int k = 0; t < 15.0f; k++) { tb.push_back(t); t += (k % 2) ? 0.85f : 0.80f; }",
                     72.7)
        self.assertAlmostEqual(r["rmssd_ms"], 50.0, delta=4.0)
        self.assertTrue(70.0 <= r["bpm"] <= 75.5, r["bpm"])  # Median der IBI: 800 oder 850 ms

    def test_refractory_follows_pulse(self):
        # Puls 60 mit Nebenspitze 400 ms nach jedem Schlag (70 % Höhe): Sperrzeit 600 ms lässt sie aus
        r = self.ibi("for (float t = 0.4f; t < 15.0f; t += 1.0f) tb.push_back(t);", 60.0,
                     extra="0.0f; for (float b : tb) { const float u = (t - b - 0.4f) / 0.05f; x[n] += 0.7f * expf(-0.5f * u * u); }")
        self.assertAlmostEqual(r["bpm"], 60.0, delta=1.0)

    def test_artifact_rejected_and_no_rmssd(self):
        # ein Schlag 250 ms zu spät: beide IBI (1250, 750) > 20 % daneben; 12 von 14 gültig < 90 % → kein RMSSD
        r = self.ibi("for (int k = 0; k < 15; k++) tb.push_back(0.4f + k + (k == 7 ? 0.25f : 0.0f));", 60.0)
        self.assertEqual(r["n_valid"], 12)
        self.assertEqual(r["rmssd_ms"], 0.0)
        self.assertAlmostEqual(r["bpm"], 60.0, delta=1.0)

    def test_slow_pulse_40(self):
        r = self.ibi("for (float t = 0.5f; t < 15.0f; t += 1.48f) tb.push_back(t);", 40.5)
        self.assertAlmostEqual(r["bpm"], 40.5, delta=1.0)


class SnapshotTest(unittest.TestCase):
    """V2.1 (B12): Report aus einem Schnappschuss, während weiter Frames ankommen. Bis zu kSnapMaxLag = 180 Frames (3 s) später
    gerechnet muss er gleich sein wie sofort gerechnet (kHistoryLen 1080 = 15-s-Fenster + 3 s)."""

    BODY = """
    static Complex16 pts[kTotal3SubPts];
    auto fill = [&](uint32_t k, float extra) {
        const float t = k / 60.0f;
        const float ph = 2.0f * sinf(6.2832f * 0.25f * t) + 0.05f * sinf(6.2832f * 1.2f * t) + extra;
        pts[0].real = 2000; pts[0].imag = 0;
        for (int b = 0; b < kZoomPoints; b++) {
            const float a = ph * (1.0f - 0.02f * b) + 0.1f * b;
            pts[1 + b].real = (int16_t)(1500 * cosf(a) + 3 * sinf(k * 1.7f + b));
            pts[1 + b].imag = (int16_t)(1500 * sinf(a) + 3 * cosf(k * 2.3f + b));
        }
        for (int r = 0; r < kRoomPoints; r++) {
            pts[1 + kZoomPoints + r].real = (int16_t)((r == 20 ? 900 : 60) * cosf(0.3f * r + (r == 20 ? ph : 0)));
            pts[1 + kZoomPoints + r].imag = (int16_t)((r == 20 ? 900 : 60) * sinf(0.3f * r + (r == 20 ? ph : 0)));
        }
    };
    static UltimateEspDsp a, b;
    a.begin(244, 120, kRoomStep); b.begin(244, 120, kRoomStep);
    float um;
    for (uint32_t k = 0; k < 960; k++) { fill(k, 0); a.pushFrame(pts, kTotal3SubPts, 20, 0, um); b.pushFrame(pts, kTotal3SubPts, 20, 0, um); }
    VitalReport ra, rb;
    a.computeVitalReport(ra);
    static ReportSnapshot s; b.takeSnapshot(s);
    for (uint32_t k = 960; k < 960 + LATER; k++) { fill(k, 1.5f); b.pushFrame(pts, kTotal3SubPts, 25, 0, um); }
    b.computeVitalReport(s, rb);
    char la[256], lb[256], va[256], vb[256];
    formatDspLine(la, sizeof(la), 1, true, ra, 0); formatDspLine(lb, sizeof(lb), 1, true, rb, 0);
    formatVitalLine(va, sizeof(va), 1, ra); formatVitalLine(vb, sizeof(vb), 1, rb);
    printf("%s\\n%s\\n%s\\n%s\\n", la, lb, va, vb);
    """

    def lines(self, later: int) -> list[str]:
        return run_cpp(self.BODY.replace("LATER", str(later))).splitlines()

    def test_snapshot_report_equals_immediate(self):
        la, lb, va, vb = self.lines(180)   # kSnapMaxLag
        self.assertEqual(la, lb)
        self.assertEqual(va, vb)
        self.assertEqual(la.split(",")[13], "960")

    def test_counter_deltas_with_snapshot_every_frame(self):
        """Nachrechnung: Schnappschuss nach jedem Frame, gerechnet wird nur jeder 60.; die Zähler sind Differenzen
        zwischen zwei gerechneten Schnappschüssen."""
        out = run_cpp("""
        static Complex16 pts[kTotal3SubPts];
        static UltimateEspDsp d; d.begin(244, 120, kRoomStep);
        static ReportSnapshot s; VitalReport r; float um;
        for (int k = 0; k < 120; k++) {
            FrameFlags ff; ff.delayed = (k == 10 || k == 20 || k == 70); ff.have_tick = true; ff.tick = 17 * k;
            d.pushFrame(pts, kTotal3SubPts, 20, 0, um, ff);
            d.takeSnapshot(s);
            if (k == 59 || k == 119) { d.computeVitalReport(s, r); printf("%u ", r.n_delayed); }
        }""")
        self.assertEqual(out.split(), ["2", "1"])


class FastAtan2Test(unittest.TestCase):
    """V2.2 (B13): Polynom statt atan2f beim Abwickeln (21 600 Aufrufe je Report); Fehler ≤ 1 · 10⁻⁴ rad."""

    def test_grid_against_atan2(self):
        out = run_cpp("""
        double worst = 0;
        for (int i = -200; i <= 200; i++)
            for (int j = -200; j <= 200; j++) {
                const float y = i * 7.3f, x = j * 5.1f + 0.37f;
                const double e = fabs((double)fastAtan2(y, x) - atan2((double)y, (double)x));
                if (e > worst) worst = e;
            }
        const float tiny[][2] = {{1e-6f, 1.0f}, {1.0f, 1e-6f}, {-1.0f, -1e-6f}, {0.0f, -1.0f}, {-1e-6f, -1.0f}};
        for (auto &t : tiny) {
            const double e = fabs((double)fastAtan2(t[0], t[1]) - atan2((double)t[0], (double)t[1]));
            if (e > worst) worst = e;
        }
        printf("%.3g %.3f", worst, fastAtan2(0.0f, 0.0f));""")
        worst, zero = out.split()
        self.assertLessEqual(float(worst), 1e-4)
        self.assertEqual(float(zero), 0.0)


class RealFftTest(unittest.TestCase):
    """V2.2: reelle FFT (1024 Punkte, nullaufgefüllt) in a121_fft.h; Leistung je Bin wie numpy."""

    def test_power_matches_numpy(self):
        import numpy as np
        out = run_cpp("""
        static a121_fft::RealFft<1024> fft;
        static float x[225], p[513];
        for (int n = 0; n < 225; n++) x[n] = 3.0f * sinf(0.37f * n) + 0.5f * cosf(1.91f * n + 0.2f) + 0.01f * (n % 7);
        fft.power(x, 225, p, 513);
        for (int k = 0; k < 513; k++) printf("%.9g\\n", p[k]);""")
        got = np.array([float(v) for v in out.split()])
        n = np.arange(225)
        x = (3.0 * np.sin(0.37 * n) + 0.5 * np.cos(1.91 * n + 0.2) + 0.01 * (n % 7)).astype(np.float32).astype(float)
        want = np.abs(np.fft.rfft(x, 1024)) ** 2
        self.assertEqual(got.size, 513)
        self.assertLess(np.max(np.abs(got - want)) / want.max(), 2e-6)

    def test_fewer_output_bins(self):
        out = run_cpp("""
        static a121_fft::RealFft<1024> fft;
        static float x[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, p[4] = {-1, -1, -1, -1};
        fft.power(x, 10, p, 3);
        printf("%.6f %.6f %.6f %.1f", p[0], p[1], p[2], p[3]);""")
        self.assertEqual(out.split(), ["1.000000", "1.000000", "1.000000", "-1.0"])

    def test_complex_fft_matches_numpy(self):
        """V3.3: komplexe FFT in place (vorwärts e^{-j}, rückwärts e^{+j} mit 1/N) für das analytische Signal."""
        import numpy as np
        out = run_cpp("""
        static a121_fft::ComplexFft<256> f;
        static float re[256], im[256];
        for (int n = 0; n < 256; n++) { re[n] = sinf(0.21f * n) + 0.1f * (n % 5); im[n] = 0.3f * cosf(0.9f * n); }
        f.transform(re, im, false);
        for (int k = 0; k < 256; k++) printf("%.7g %.7g\\n", re[k], im[k]);
        f.transform(re, im, true);
        for (int k = 0; k < 256; k++) printf("%.7g %.7g\\n", re[k], im[k]);""")
        v = np.array([[float(a) for a in ln.split()] for ln in out.splitlines()])
        n = np.arange(256)
        x = (np.sin(0.21 * n) + 0.1 * (n % 5)).astype(np.float32) + 1j * (0.3 * np.cos(0.9 * n)).astype(np.float32)
        want = np.fft.fft(x)
        got = v[:256, 0] + 1j * v[:256, 1]
        self.assertLess(np.max(np.abs(got - want)) / np.max(np.abs(want)), 2e-6)
        back = v[256:, 0] + 1j * v[256:, 1]
        self.assertLess(np.max(np.abs(back - x)), 1e-5)


class LinearTrendTest(unittest.TestCase):
    """V2.2: Trend in float mit geschlossenem Σt² (vorher double, auf dem ESP32-S3 in Software)."""

    def test_large_offset_ramp(self):
        import numpy as np
        out = run_cpp("""
        static float x[900];
        for (int n = 0; n < 900; n++) x[n] = 8000.0f + 3.1f * n + 40.0f * sinf(0.21f * n);
        removeLinearTrend(x, 900);
        for (int n = 0; n < 900; n++) printf("%.6g\\n", x[n]);""")
        got = np.array([float(v) for v in out.split()])
        n = np.arange(900)
        x = (8000.0 + 3.1 * n + 40.0 * np.sin(0.21 * n)).astype(np.float32).astype(float)
        want = x - np.polyval(np.polyfit(n, x, 1), n)
        self.assertLess(np.max(np.abs(got - want)), 0.01)   # µm


class RefinedPeakTest(unittest.TestCase):
    """V2.2: Spektrum im FFT-Raster (0,0146 Hz), Spitze parabolisch verfeinert: ein Puls zwischen zwei Rasterpunkten
    wird auf ±0,1 /min getroffen (vorher Raster 0,6 /min ohne Verfeinerung)."""

    def test_pulse_between_bins(self):
        out = run_cpp("""
        static Complex16 pts[kTotal3SubPts];
        static UltimateEspDsp d; d.begin(244, 120, kRoomStep);
        float um; VitalReport r = {};
        for (uint32_t k = 0; k < 900; k++) {
            const float t = k / 60.0f;
            const float ph = 2.0f * sinf(6.2832f * 0.23f * t) + 0.08f * sinf(6.2832f * 1.2067f * t);
            pts[0].real = 2000; pts[0].imag = 0;
            for (int b = 0; b < kZoomPoints; b++) {
                const float a = ph * (1.0f - 0.02f * b) + 0.1f * b;
                pts[1 + b].real = (int16_t)(1500 * cosf(a) + 3 * sinf(k * 1.7f + b));
                pts[1 + b].imag = (int16_t)(1500 * sinf(a) + 3 * cosf(k * 2.3f + b));
            }
            for (int q = 0; q < kRoomPoints; q++) {
                pts[1 + kZoomPoints + q].real = (int16_t)((q == 20 ? 900 : 60) * cosf(0.3f * q + (q == 20 ? ph : 0)));
                pts[1 + kZoomPoints + q].imag = (int16_t)((q == 20 ? 900 : 60) * sinf(0.3f * q + (q == 20 ? ph : 0)));
            }
            d.pushFrame(pts, kTotal3SubPts, 20, 0, um);
        }
        d.computeVitalReport(r);
        printf("%d %.3f", r.valid, r.hr_spec_bpm);""")
        valid, bpm = out.split()
        self.assertEqual(valid, "1")
        self.assertAlmostEqual(float(bpm), 1.2067 * 60, delta=0.1)


class RezoomTrackingTest(unittest.TestCase):
    """V2.3 Schritt 2 (B11): Ein Rezoom beginnt nur die Historie neu; die Puls-Verfolgung bleibt, wenn der letzte
    gültige Report weniger als 30 s zurückliegt. Erster Report danach: naher Kandidat geglättet (0,7 · alt + 0,3 · neu)
    statt Neustart auf dem rohen Wert."""

    GEN = """
    static Complex16 pts[kTotal3SubPts];
    static UltimateEspDsp d; d.begin(244, 120, kRoomStep);
    float um; VitalReport r = {};
    uint32_t k = 0;
    auto push = [&](uint32_t n, float f_hr) {
        for (uint32_t e = k + n; k < e; k++) {
            const float t = k / 60.0f;
            const float ph = 2.0f * sinf(6.2832f * 0.23f * t) + 0.08f * sinf(6.2832f * f_hr * t);
            pts[0].real = 2000; pts[0].imag = 0;
            for (int b = 0; b < kZoomPoints; b++) {
                const float a = ph * (1.0f - 0.02f * b) + 0.1f * b;
                pts[1 + b].real = (int16_t)(1500 * cosf(a) + 3 * sinf(k * 1.7f + b));
                pts[1 + b].imag = (int16_t)(1500 * sinf(a) + 3 * cosf(k * 2.3f + b));
            }
            for (int q = 0; q < kRoomPoints; q++) {
                pts[1 + kZoomPoints + q].real = (int16_t)((q == 20 ? 900 : 60) * cosf(0.3f * q + (q == 20 ? ph : 0)));
                pts[1 + kZoomPoints + q].imag = (int16_t)((q == 20 ? 900 : 60) * sinf(0.3f * q + (q == 20 ? ph : 0)));
            }
            d.pushFrame(pts, kTotal3SubPts, 20, 0, um);
        }
    };
    push(900, 1.2f);
    d.computeVitalReport(r);
    printf("%d %.2f ", r.valid, r.hr_fused_bpm);
    push(GAP, 1.2f);
    d.setZoomStartPoint(244);
    push(480, 80.0f / 60.0f);
    d.computeVitalReport(r);
    printf("%d %.2f", r.valid, r.hr_fused_bpm);
    """

    def run_gap(self, gap: int) -> list[float]:
        return [float(v) for v in run_cpp(self.GEN.replace("GAP", str(gap))).split()]

    def test_recent_valid_keeps_tracking(self):
        v0, hr0, v1, hr1 = self.run_gap(0)
        self.assertEqual((v0, v1), (1, 1))
        self.assertAlmostEqual(hr0, 72.0, delta=0.5)
        self.assertAlmostEqual(hr1, 0.7 * hr0 + 0.3 * 80.0, delta=0.6)

    def test_old_valid_restarts_tracking(self):
        v0, hr0, v1, hr1 = self.run_gap(1860)   # 31 s ohne Report
        self.assertEqual((v0, v1), (1, 1))
        self.assertAlmostEqual(hr1, 80.0, delta=0.6)


class ZoomWidthTest(unittest.TestCase):
    """V2.3 Schritt 3: Zoom-Breite per A121_ZOOM_POINTS (24 Standard, 48 vorbereitet bis Messung V4.4); bei mehr als
    24 Punkten Spektren nur für die kSpecPoints = 16 Punkte mit der stärksten Atmung."""

    def test_default_24(self):
        self.assertEqual(run_cpp('printf("%u %u %u", kZoomPoints, kTotal3SubPts, kSpecPoints);').split(),
                         ["24", "105", "24"])

    def test_switch_48(self):
        out = run_cpp('printf("%u %u %u", kZoomPoints, kTotal3SubPts, kSpecPoints);', ("-DA121_ZOOM_POINTS=48",))
        self.assertEqual(out.split(), ["48", "129", "16"])

    def test_select_strongest(self):
        out = run_cpp("""
        const float v[8] = {5, 1, 9, 3, 9, 0, 7, 2};
        bool sel[8];
        selectStrongest(v, 8, 3, sel);
        for (bool b : sel) printf("%d", b);
        selectStrongest(v, 8, 8, sel);
        printf(" ");
        for (bool b : sel) printf("%d", b);""")
        self.assertEqual(out, "00101010 11111111")   # 9, 9, 7; bei k = n alle


class FastStartTest(unittest.TestCase):
    """V2.4 (B14): erster Report mit Vitalwerten ab 6 s (wachsendes Fenster); beim Start nach 2 s auf ein Ziel außerhalb
    des Zoom-Fensters zoomen (vorher 3 Reports), nach einem Rezoom weiter erst nach 3 Reports."""

    GEN = RezoomTrackingTest.GEN.split("    push(900, 1.2f);")[0]

    def test_valid_from_6_s(self):
        out = run_cpp(self.GEN + """
        push(359, 1.2f); d.computeVitalReport(r); printf("%d ", r.valid);
        push(1, 1.2f); d.computeVitalReport(r); printf("%d %.1f %.1f", r.valid, r.hr_fused_bpm, r.confidence_pct);""")
        v0, v1, hr, conf = out.split()
        self.assertEqual((v0, v1), ("0", "1"))
        self.assertAlmostEqual(float(hr), 72.0, delta=1.0)
        self.assertLessEqual(float(conf), 40.0 + 1e-6)

    def test_start_rezoom_after_2_s(self):
        # Person im Raum-Profil bei 0,9 m (Punkt 20), Zoom-Fenster 610–670 mm
        out = run_cpp(self.GEN + """
        uint32_t first = 0;
        for (int i = 0; i < 6 && !first; i++) { push(60, 1.2f); d.computeVitalReport(r); if (r.needs_rezoom) first = d.frameCount(); }
        d.setZoomStartPoint(244);
        uint32_t again = 0;
        for (int i = 0; i < 6 && !again; i++) { push(60, 1.2f); d.computeVitalReport(r); if (r.needs_rezoom) again = d.frameCount(); }
        printf("%u %u", first, again);""")
        self.assertEqual(out.split(), ["120", "180"])

    def test_provisional_not_on_low_breath_harmonic(self):
        # Atmung 15 /min mit kräftiger 3. Oberwelle (45 /min), Puls 96 schwach: Im 6-s-Fenster ist 3 × Atmung die
        # stärkste Spitze, und AKF/Einzelschläge desselben Signals bestätigen sie. Vorläufig darf ein Wert auf einer
        # Atem-Oberwelle m ≤ 3 nicht gelten (V3.1, Prüfstand base mit A121_DSP_COMBINE: 46 statt 71 /min gültig).
        gen = self.GEN.replace("const float ph = 2.0f * sinf(6.2832f * 0.23f * t) + 0.08f * sinf(6.2832f * f_hr * t);",
                               "const float ph = 2.0f * sinf(6.2832f * 0.25f * t) + 0.3f * sinf(6.2832f * 0.75f * t + 0.5f)"
                               " + 0.02f * sinf(6.2832f * f_hr * t);")
        self.assertNotEqual(gen, self.GEN)
        out = run_cpp(gen + """
        push(360, 1.6f); d.computeVitalReport(r);
        printf("%d %.1f %d", r.valid, r.hr_spec_bpm, (int)r.hr_harm);""")
        valid, spec, harm = out.split()
        self.assertAlmostEqual(float(spec), 45.0, delta=2.0)   # die Oberwelle ist die stärkste Spitze …
        self.assertEqual(harm, "1")
        self.assertEqual(valid, "0")                            # … und gilt vorläufig nicht


class WindowLengthTest(unittest.TestCase):
    """V2.5: Pulsfenster per A121_HR_WIN (Frames, Standard 900 = 15 s) für den Vergleich 10 / 12 / 15 s an Tag A;
    die Historie wächst mit (Fenster + kSnapMaxLag)."""

    def test_default_15_s(self):
        self.assertEqual(run_cpp('printf("%u %u %u", kHrWin, kHistoryLen, kSnapMaxLag);').split(), ["900", "1080", "180"])

    def test_switch_10_s(self):
        out = run_cpp('printf("%u %u %u", kHrWin, kHistoryLen, kSnapMaxLag);', ("-DA121_HR_WIN=600",))
        self.assertEqual(out.split(), ["600", "780", "180"])


class MeasurementStateTest(unittest.TestCase):
    """V3.5: Zustand der Messung (0 KEINE_PERSON, 1 SUCHE, 2 ERFASSUNG, 3 GÜLTIG, 4 BEWEGUNG, 5 APNOE) und Zeile
    @STATE,t_ms,zustand,seit_ms."""

    def state(self, person=1, echo=1, breath=1, valid=0, motion=0.0, br_state=0) -> int:
        return int(run_cpp(f"printf(\"%u\", measurementState({person}, {echo}, {breath}, {valid}, {motion}f, {br_state}));"))

    def test_priorities(self):
        self.assertEqual(self.state(person=0, echo=0, breath=0), 0)
        self.assertEqual(self.state(person=0, echo=1, breath=0), 0)   # ruhendes Echo ohne Atmung
        self.assertEqual(self.state(person=1, echo=0, breath=0), 1)   # Person im Raum, kein Echo im Zoom
        self.assertEqual(self.state(person=1, echo=1, breath=0), 2)   # Echo im Zoom, gerade keine Atemspitze
        self.assertEqual(self.state(), 2)                              # Echo und Atmung, noch kein gültiger Wert
        self.assertEqual(self.state(valid=1), 3)
        self.assertEqual(self.state(valid=1, motion=40.0), 4)
        self.assertEqual(self.state(valid=0, br_state=3), 4)
        self.assertEqual(self.state(person=1, echo=0, breath=0, motion=60.0), 1)   # Bewegung nur mit Echo im Zoom
        self.assertEqual(self.state(valid=1, br_state=2), 5)
        self.assertEqual(self.state(person=0, echo=1, breath=0, br_state=2), 5)   # angehalten: keine Atemspitze
        self.assertEqual(self.state(person=0, echo=1, breath=0, motion=60.0), 0)   # ruhendes Echo, Rauschen im Herzband

    def test_state_line(self):
        out = run_cpp('VitalReport r = {}; r.state = 3; r.state_since_ms = 4500; char b[64];'
                      'formatStateLine(b, sizeof(b), 12000, r); printf("%s", b);')
        self.assertEqual(out, "@STATE,12000,3,4500")

    def test_report_sequence(self):
        """Ziel im Raum außerhalb des Zoom-Fensters: nach 2 Reports mit Ziel SUCHE (davor KEINE_PERSON); mit sauberem,
        atmendem Echo im Zoom ab 6 s ERFASSUNG bzw. GÜLTIG; seit_ms zählt ab dem Wechsel."""
        out = run_cpp(RezoomTrackingTest.GEN.split("    push(900, 1.2f);")[0] + """
        for (int i = 0; i < 12; i++) { push(60, 1.2f); d.computeVitalReport(r); printf("%u:%lu ", r.state, (unsigned long)r.state_since_ms); }""")
        seq = [tuple(int(v) for v in x.split(":")) for x in out.split()]
        self.assertEqual(seq[0], (0, 0))
        self.assertEqual(seq[1], (1, 0))
        self.assertEqual(seq[-1][0], 3)
        first_valid = next(i for i, (st, _) in enumerate(seq) if st == 3)
        self.assertEqual(seq[-1][1], (len(seq) - 1 - first_valid) * 1000)


class BreathDetectorTest(unittest.TestCase):
    """V3.4: Atmung Atemzug für Atemzug. Bandpass 0,05–0,8 Hz auf dem Weg (µm), Spitzen und Täler mit Hysterese; je
    Spitze ein Ereignis mit Abstand zur vorigen Spitze (ms) und Tiefe (Spitze − voriges Tal, µm); Rate = 60 / Median
    der letzten 4 Abstände; nur Abstände im Band 4–42 /min."""

    @staticmethod
    def events(signal: str, seconds: float) -> list[tuple[float, ...]]:
        """signal: C-Ausdruck in t (s, double) für den Weg in µm. Rückgabe je Ereignis (Frame, Abstand_ms, Tiefe_um, Rate)."""
        out = run_cpp(f"""
        BreathDetector bd; BreathEvent ev;
        for (uint32_t k = 0; k < (uint32_t)({seconds} * 60); k++) {{
            const double t = k / 60.0;
            if (bd.push((float)({signal}), ev))
                printf("%lu %lu %.1f %.3f\\n", (unsigned long)ev.frame, (unsigned long)ev.interval_ms, ev.depth_um, ev.rate_cpm);
        }}""")
        return [tuple(float(x) for x in ln.split()) for ln in out.splitlines()]

    @staticmethod
    def hp_gain(f_hz: float) -> float:
        """Verstärkung des Hochpasses 1. Ordnung bei 0,05 Hz (die Tiefe ist bei langsamer Atmung entsprechend kleiner)."""
        return f_hz / math.hypot(f_hz, 0.05)

    def assert_steady(self, ev, period_s: float, per_min: float, start_tol: float = 0.02, iv_tol_ms: float = 17.0):
        """Die ersten zwei Ereignisse (Filter schwingt ein) auf start_tol genau, danach Abstand ±iv_tol_ms, Rate ±0,1."""
        for i, (_, iv, _, rate) in enumerate(ev):
            if i < 2:
                self.assertAlmostEqual(iv, period_s * 1000.0, delta=start_tol * period_s * 1000.0)
                self.assertAlmostEqual(rate, per_min, delta=start_tol * per_min)
            else:
                self.assertAlmostEqual(iv, period_s * 1000.0, delta=iv_tol_ms)
                self.assertAlmostEqual(rate, per_min, delta=0.1)

    def check_rhythm(self, per_min: float, seconds: float):
        period = 60.0 / per_min
        ev = self.events(f"1250.0 * sin(6.283185307 * {per_min / 60.0} * t)", seconds)
        self.assertGreaterEqual(len(ev), int(seconds / period) - 2)
        self.assert_steady(ev, period, per_min)
        for _, _, depth, _ in ev[2:]:
            self.assertAlmostEqual(depth, 2500.0 * self.hp_gain(per_min / 60.0), delta=0.03 * 2500.0)
        # erster Wert nach 2 Atemzügen: nicht vor der 2. Spitze, spätestens 2,5 s nach ihr
        self.assertGreater(ev[0][0] / 60.0, 1.25 * period)
        self.assertLess(ev[0][0] / 60.0, 1.25 * period + 2.5)

    def test_12_per_min(self):
        self.check_rhythm(12.0, 60.0)

    def test_6_per_min(self):
        self.check_rhythm(6.0, 120.0)

    def test_non_sinusoidal_breath(self):
        # Form wie im Prüfstand (2./3. Oberwelle), 15 /min, 2,5 mm
        ph = "(6.283185307 * 0.25 * t)"
        ev = self.events(f"2500.0 / 2.3 * (sin({ph}) + 0.15 * sin(2 * {ph} + 0.5) + 0.05 * sin(3 * {ph} + 1.0))", 60.0)
        self.assertGreaterEqual(len(ev), 12)
        self.assert_steady(ev, 4.0, 15.0)

    def test_band_limits(self):
        self.assertEqual(self.events("1000.0 * sin(6.283185307 * 0.75 * t)", 60.0), [])    # 45 /min: zu schnell
        self.assertEqual(self.events("1250.0 * sin(6.283185307 * 0.05 * t)", 120.0), [])   # 3 /min: zu langsam

    def test_pulse_ripple_alone_no_breath(self):
        self.assertEqual(self.events("50.0 * sin(6.283185307 * 1.2 * t)", 60.0), [])        # Puls 100 µm, keine Atmung

    def test_shallow_after_deep(self):
        """Die Schwelle folgt der Tiefe: Nach 30 s mit 4 mm gehen 0,6-mm-Atemzüge nach höchstens 15 s wieder durch."""
        ev = self.events("(t < 30 ? 2000.0 : 300.0) * sin(6.283185307 * 0.25 * t)", 90.0)
        late = [e for e in ev if e[0] > 45 * 60]
        self.assertGreaterEqual(len(late), 10)
        for _, iv, depth, _ in late:
            self.assertAlmostEqual(iv, 4000.0, delta=17.0)
            self.assertAlmostEqual(depth, 600.0 * self.hp_gain(0.25), delta=30.0)

    def test_pause_restarts_rate(self):
        """20 s ohne Atmung: kein Abstand über 15 s, danach neuer Wert nach 2 Atemzügen aus den neuen Abständen."""
        ev = self.events("(t > 30 && t < 50 ? 0.0 : 1250.0) * sin(6.283185307 * 0.25 * t)", 90.0)
        self.assert_steady(ev, 4.0, 15.0)
        self.assertTrue(any(e[0] > 55 * 60 for e in ev))
        # angehalten nach dem Einatmen (Weg bleibt oben): ebenso kein Atemzug in der Pause
        w = "6.283185307 * 0.25"
        ev = self.events(f"(t < 29 ? 1250.0 * sin({w} * t) : t < 49 ? 1250.0 : 1250.0 * sin({w} * (t - 20)))", 90.0)
        self.assert_steady(ev, 4.0, 15.0)
        self.assertFalse(any(33 * 60 < e[0] < 51 * 60 for e in ev))
        self.assertTrue(any(e[0] > 62 * 60 for e in ev))

    def test_breath_line(self):
        out = run_cpp('BreathEvent ev; ev.interval_ms = 5000; ev.depth_um = 2499.6f; ev.rate_cpm = 12.04f; char b[80];'
                      'formatBreathLine(b, sizeof(b), 1234, 5678, ev); printf("%s", b);')
        self.assertEqual(out, "@BREATH,1234,5678,5000,2500,12.0")

    def test_push_side_of_dsp(self):
        """Im DSP: Weg des Telemetrie-Strahls, Ereignis nach pushFrame() abholen; Neubeginn (Rezoom) startet neu."""
        out = run_cpp(RezoomTrackingTest.GEN.split("    push(900, 1.2f);")[0].replace(
            "d.pushFrame(pts, kTotal3SubPts, 20, 0, um);",
            "d.pushFrame(pts, kTotal3SubPts, 20, 0, um); BreathEvent ev; "
            "if (d.takeBreathEvent(ev)) printf(\"%lu %lu %.3f\\n\", (unsigned long)k, (unsigned long)ev.interval_ms, ev.rate_cpm);") + """
        d.setBreathGate(true);
        push(1800, 1.2f);
        printf("R\\n");
        d.setZoomStartPoint(244);
        push(1800, 1.2f);""")
        before, after = out.split("R\n")
        # Puls-Restwelligkeit auf der flachen Atemspitze: Abstände ±100 ms, Rate (Median aus 4) ±0,2 /min
        ev = [[float(x) for x in ln.split()] for ln in before.splitlines()]
        self.assertGreaterEqual(len(ev), 4)
        for i, (_, iv, rate) in enumerate(ev):
            self.assertAlmostEqual(iv, 1000.0 / 0.23, delta=200.0 if i < 2 else 100.0)
            if i >= 3:
                self.assertAlmostEqual(rate, 0.23 * 60.0, delta=0.2)
        ev2 = [[float(x) for x in ln.split()] for ln in after.splitlines()]
        self.assertGreaterEqual(len(ev2), 4)
        self.assertGreater(ev2[0][0] - 1800, 1.0 / 0.23 * 60)   # nicht vor dem 2. Atemzug nach dem Neubeginn


    def test_gate_from_state(self):
        """Ausgabe nur in ERFASSUNG, GÜLTIG und APNOE (ohne Person ist der Weg Rauschen, in BEWEGUNG Körperbewegung)."""
        out = run_cpp('for (uint8_t st = 0; st <= 5; st++) printf("%d", breathGateOpen(st));')
        self.assertEqual(out, "001101")

    def test_gate_closed_drops_events(self):
        """Tor zu (Start): erkannt wird weiter, aber kein Ereignis; nach dem Öffnen gleich mit dem bisherigen Verlauf."""
        out = run_cpp(RezoomTrackingTest.GEN.split("    push(900, 1.2f);")[0].replace(
            "d.pushFrame(pts, kTotal3SubPts, 20, 0, um);",
            "d.pushFrame(pts, kTotal3SubPts, 20, 0, um); BreathEvent ev; "
            "if (d.takeBreathEvent(ev)) printf(\"%lu %lu\\n\", (unsigned long)k, (unsigned long)ev.interval_ms);") + """
        push(1800, 1.2f);
        printf("A\\n");
        d.setBreathGate(true);
        push(600, 1.2f);""")
        closed, opened = out.split("A\n")
        self.assertEqual(closed, "")
        ev = [[float(x) for x in ln.split()] for ln in opened.splitlines()]
        self.assertGreaterEqual(len(ev), 2)
        self.assertLess(ev[0][0] - 1800, 1.0 / 0.23 * 60 + 60)   # spätestens ein Atemzug nach dem Öffnen


class ViterbiTest(unittest.TestCase):
    """V3.2 (Schalter A121_DSP_VITERBI): Verfolgung als Viterbi über die letzten 20 Reports. Zustände 40–180 /min
    (1 /min), Übergang Gauß σ = 3 /min je Report plus kleiner Anteil „Sprung irgendwohin"; Emission aus den
    Kandidaten des Reports (Gauß je Kandidat, Gewicht = Kandidatenwert, mit Grundpegel); Konfidenz = Masse ±5 /min."""

    FLAGS = ("-DA121_DSP_VITERBI",)

    def run_vit(self, body: str) -> list[list[float]]:
        out = run_cpp("ViterbiTracker v; float mass; float b[3], c[3];\n" + body, self.FLAGS)
        return [[float(x) for x in ln.split()] for ln in out.splitlines()]

    def test_follows_slow_change_against_strong_distractor(self):
        # Puls 70 → 85 in 30 Reports; eine Fremdspitze springt zwischen 110 und 150 und ist jedes 2. Mal stärker
        rows = self.run_vit("""
        for (int t = 0; t < 30; t++) {
            b[0] = 70.0f + 0.5f * t; c[0] = 1.0f;
            b[1] = (t % 2) ? 110.0f : 150.0f; c[1] = (t % 2) ? 1.4f : 0.6f;
            printf("%.2f %.3f\\n", v.update(b, c, 2, mass), mass);
        }""")
        for t, (bpm, _) in enumerate(rows[3:], 3):
            self.assertAlmostEqual(bpm, 70.0 + 0.5 * t, delta=1.5)

    def test_single_outlier_ignored_persistent_change_followed(self):
        rows = self.run_vit("""
        for (int t = 0; t < 25; t++) {
            const bool out1 = (t == 10), moved = (t >= 15);
            b[0] = (out1 || moved) ? 110.0f : 72.0f; c[0] = 1.0f;
            printf("%.2f %.3f\\n", v.update(b, c, 1, mass), mass);
        }""")
        self.assertAlmostEqual(rows[10][0], 72.0, delta=1.5)    # ein Report mit nur 110: Verfolgung bleibt
        self.assertAlmostEqual(rows[24][0], 110.0, delta=1.5)   # dauerhaft 110: nach wenigen Reports gewechselt
        switch = next(t for t in range(15, 25) if abs(rows[t][0] - 110.0) < 1.5)
        self.assertLessEqual(switch - 15, 4)

    def test_mass(self):
        rows = self.run_vit("""
        for (int t = 0; t < 20; t++) { b[0] = 72.0f; c[0] = 1.0f; v.update(b, c, 1, mass); }
        printf("%.3f\\n", mass);
        v.reset();
        for (int t = 0; t < 20; t++) { b[0] = 60.0f; c[0] = 1.0f; b[1] = 120.0f; c[1] = 1.0f; v.update(b, c, 2, mass); }
        printf("%.3f\\n", mass);""")
        self.assertGreater(rows[0][0], 0.8)
        self.assertLess(rows[1][0], 0.65)

    def test_window_forgets_after_20_reports(self):
        """Nach einem Neubeginn (reset) zählt nur, was danach kam; ohne reset höchstens die letzten 20 Reports."""
        rows = self.run_vit("""
        for (int t = 0; t < 60; t++) { b[0] = 72.0f; c[0] = 1.0f; v.update(b, c, 1, mass); }
        for (int t = 0; t < 25; t++) { b[0] = 72.0f; c[0] = 0.5f; b[1] = 130.0f; c[1] = 1.0f;
                                       printf("%.2f\\n", v.update(b, c, 2, mass)); }""")
        self.assertAlmostEqual(rows[-1][0], 130.0, delta=1.5)

    def test_dsp_with_switch(self):
        out = run_cpp(RezoomTrackingTest.GEN.split("    push(900, 1.2f);")[0] + """
        for (int i = 0; i < 20; i++) { push(60, 1.2f); d.computeVitalReport(r); }
        printf("%d %.2f %.1f", r.valid, r.hr_fused_bpm, r.confidence_pct);""", self.FLAGS)
        valid, hr, conf = out.split()
        self.assertEqual(valid, "1")
        self.assertAlmostEqual(float(hr), 72.0, delta=1.0)
        self.assertGreater(float(conf), 50.0)


class GuidedHarmonicsTest(unittest.TestCase):
    """V3.3 (Schalter A121_DSP_HARM_REMOVE): Atem-Oberwellen geführt von der Atemphase entfernen. Phase = Winkel des
    analytischen Signals der Atem-Grundwelle im Weg, dann Fit von cos(mφ), sin(mφ) für m = 1..8 und abziehen. m bleibt,
    wenn Puls oder 2 × Puls in m × [kleinste, größte Atemfrequenz im Fenster] ± 0,1 Hz liegt: Bei schwankender Rate
    verschmiert die m-te Oberwelle um m × die Schwankung und nähme sonst dem Puls einen Teil weg."""

    FLAGS = ("-DA121_DSP_HARM_REMOVE",)
    HARM = "300.0f * cosf(3.0f * phi + 0.4f) + 150.0f * cosf(4.0f * phi - 1.1f) + 80.0f * cosf(5.0f * phi + 2.0f)"

    def residual(self, fn: str, keep: float, harm: str, pulse: str, beta: float = 0.9) -> float:
        """Atmung 0,27 Hz, Phase mit beta · sin(2π · 0,07 Hz · t) moduliert (0,9 rad: Rate ±23 %), Oberwellen harm(phi),
        Puls pulse(t); 15 Hz, 15 s; f_Atem des vorigen Reports = 0,27 Hz. fn = guided | fixed. Rückgabe:
        Effektivwert(Ergebnis − Puls), Hann-gewichtet."""
        call = ("removeBreathHarmonicsGuided(x, d, N, 15.0f, 0.27f, KEEP, cf, wk);" if fn == "guided"
                else "removeBreathHarmonics(x, N, 15.0f, 0.27f, KEEP);").replace("KEEP", f"{keep}f")
        out = run_cpp(f"""
        const uint16_t N = 225; static float x[N], d[N], wk[2 * kGuideFft];
        static a121_fft::ComplexFft<kGuideFft> cf;
        for (int n = 0; n < N; n++) {{
            const float t = n / 15.0f;
            const float phi = 6.2832f * 0.27f * t + {beta}f * sinf(6.2832f * 0.07f * t);
            d[n] = 1200.0f * sinf(phi);
            x[n] = {harm} + {pulse};
        }}
        {call}
        double e = 0, c = 0;   /* Hann-gewichtet wie vor der Puls-FFT: die Ränder zählen kaum */
        for (int n = 0; n < N; n++) {{ const float t = n / 15.0f; const double r = x[n] - ({pulse});
            const double w = 0.5 * (1 - cos(6.283185307 * n / (N - 1))); e += w * r * r; c += w; }}
        printf("%.4f", sqrt(e / c));""", self.FLAGS)
        return float(out)

    def test_varying_rate_guided_beats_fixed(self):
        # Rate ±23 %, Oberwellen m = 3..5 (10 / 5 / 2,7 × Puls), Puls 1,9 Hz abseits ihrer Streuung
        pulse = "30.0f * cosf(6.2832f * 1.9f * t)"
        g = self.residual("guided", 1.9, self.HARM, pulse)
        f = self.residual("fixed", 1.9, self.HARM, pulse)
        self.assertLess(g, 0.3 * f)

    def test_varying_rate_keeps_pulse(self):
        # nur Puls: Die Basen, deren Streuung den Puls überdeckt, bleiben weg (1,25 Hz liegt in 4 × und 5 × Atmung);
        # der Rest-Fit nimmt dem Puls (30 µm) weniger als 13 %. Ohne diese Regel: 12 µm (40 %).
        for hz in (1.25, 1.9):
            self.assertLess(self.residual("guided", hz, "0.0f", f"30.0f * cosf(6.2832f * {hz}f * t)"), 4.0)

    def test_steady_rate_removes_harmonics(self):
        # Metronom (Rate ±3 %): Oberwellen m = 3..5 (zusammen 243 µm effektiv) um ≥ 88 % kleiner, besser als mit fester
        # Frequenz (die Phasengenauigkeit aus 15 s, ≈ 0,03 rad, begrenzt); Puls 1,6 Hz bleibt (m = 6 liegt am Puls)
        pulse = "30.0f * cosf(6.2832f * 1.6f * t)"
        g = self.residual("guided", 1.6, self.HARM, pulse, beta=0.1)
        self.assertLess(g, 0.12 * 243.0)
        self.assertLess(g, 0.6 * self.residual("fixed", 1.6, self.HARM, pulse, beta=0.1))

    def test_harmonic_on_pulse_stays(self):
        self.assertLess(self.residual("guided", 1.08, "0.0f", "30.0f * cosf(6.2832f * 1.08f * t)", beta=0.1), 3.0)

    def test_no_breath_no_change(self):
        out = run_cpp("""
        const uint16_t N = 225; static float x[N], d[N], wk[2 * kGuideFft]; static a121_fft::ComplexFft<kGuideFft> cf;
        for (int n = 0; n < N; n++) { x[n] = 30.0f * cosf(6.2832f * 1.25f * n / 15.0f); d[n] = 0; }
        removeBreathHarmonicsGuided(x, d, N, 15.0f, 0.0f, 1.25f, cf, wk);
        double e = 0; for (int n = 0; n < N; n++) { const double r = x[n] - 30.0f * cosf(6.2832f * 1.25f * n / 15.0f); e += r * r; }
        printf("%.6f", sqrt(e / N));""", self.FLAGS)
        self.assertLess(float(out), 1e-6)

    def test_dsp_with_switch(self):
        out = run_cpp(RezoomTrackingTest.GEN.split("    push(900, 1.2f);")[0] + """
        for (int i = 0; i < 20; i++) { push(60, 1.2f); d.computeVitalReport(r); }
        printf("%d %.2f", r.valid, r.hr_fused_bpm);""", self.FLAGS)
        valid, hr = out.split()
        self.assertEqual(valid, "1")
        self.assertAlmostEqual(float(hr), 72.0, delta=1.0)


class CombinePointsTest(unittest.TestCase):
    """V3.1 (Schalter A121_DSP_COMBINE): Zoom-Punkte vor der Demodulation kombinieren. Je Punkt die IQ-Werte um den
    Kreismittelpunkt verschieben (u = z − c), dann 1. Hauptkomponente der K × K-Kovarianz C = Σ u uᴴ: Sie richtet die
    Punkte phasengleich aus und gewichtet sie mit der Amplitude (bei gleichem Rauschen je Punkt = Maximum Ratio
    Combining). Ein Kanal y = Σ conj(v) u wird demoduliert. N_eff = (Σλ)² / Σλ² = (tr C)² / ‖C‖²: ≈ 1, wenn alle Punkte
    dieselbe Bewegung sehen, ≈ K bei reinem Rauschen."""

    FLAGS = ("-DA121_DSP_COMBINE",)

    def combine(self, sig: str, K: int = 8, noise: float = 40.0) -> dict:
        """K Ringe: Punkt j = Mittelpunkt (300 j, −200 j) + r_j · e^{i(phi_j(t) + 0,7 j)} + Rauschen (σ je Komponente);
        sig(j, t) setzt r und phi. Rückgabe: N_eff, dominant, Phasenfehler (Effektivwert, rad) von y und vom besten
        Einzelpunkt (gegen die wahre Phase des Punkts mit der größten Amplitude)."""
        out = run_cpp(f"""
        const uint8_t K = {K}; const uint16_t N = 900;
        static float ring[K][kHistoryLen * 2]; static float y[2 * N]; static float tru[K][N];
        float ci[K], cq[K], r_of[K]; const float *rings[K];
        uint32_t s = 7; auto uni = [&] {{ s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f; }};
        auto gauss = [&] {{ float a = 0; for (int i = 0; i < 12; i++) a += uni(); return a - 6.0f; }};
        for (int j = 0; j < K; j++) {{
            ci[j] = 300.0f * j; cq[j] = -200.0f * j; rings[j] = ring[j];
            for (int n = 0; n < N; n++) {{
                const float t = n / 60.0f; float r = 0, phi = 0;
                {sig}
                r_of[j] = r; tru[j][n] = phi;
                ring[j][2 * n]     = ci[j] + r * cosf(phi + 0.7f * j) + {noise}f * gauss();
                ring[j][2 * n + 1] = cq[j] + r * sinf(phi + 0.7f * j) + {noise}f * gauss();
            }}
        }}
        uint8_t dom = 255;
        const float neff = combinePrincipal(rings, ci, cq, K, 0, N, y, dom);
        int jb = 0; for (int j = 1; j < K; j++) if (r_of[j] > r_of[jb]) jb = j;
        auto err = [&](const float *iq, float c_i, float c_q) {{   /* Phase abwickeln, Versatz entfernen, Effektivwert */
            static double e[N]; double prev = 0, acc = 0, m = 0;
            for (int n = 0; n < N; n++) {{
                const double p = atan2(iq[2 * n + 1] - c_q, iq[2 * n] - c_i);
                double d = p - prev; if (n == 0) acc = p; else {{ while (d > M_PI) d -= 2 * M_PI; while (d < -M_PI) d += 2 * M_PI; acc += d; }}
                prev = p; e[n] = acc - tru[jb][n]; m += e[n];
            }}
            m /= N; double v = 0; for (int n = 0; n < N; n++) v += (e[n] - m) * (e[n] - m); return sqrt(v / N);
        }};
        printf("%.4f %d %.5f %.5f", neff, dom, err(y, 0, 0), err(ring[jb], ci[jb], cq[jb]));""", self.FLAGS)
        n, d, ec, es = out.split()
        return {"n_eff": float(n), "dom": int(d), "err_comb": float(ec), "err_single": float(es)}

    COMMON = "r = 400.0f + 100.0f * j; phi = 2.0f * sinf(6.2832f * 0.23f * t) + 0.08f * sinf(6.2832f * 1.2f * t);"

    def test_aligned_sum_beats_best_point(self):
        # 8 Punkte mit Amplitude 400 … 1100, gleiches Rauschen: SNR der Summe ≈ 4 × der beste Punkt → Fehler ≈ 0,5 ×
        r = self.combine(self.COMMON)
        self.assertLess(r["err_comb"], 0.6 * r["err_single"])

    def test_dominant_is_strongest_point(self):
        self.assertEqual(self.combine(self.COMMON)["dom"], 7)

    def test_n_eff_one_for_common_motion(self):
        self.assertLess(self.combine(self.COMMON, noise=0.0)["n_eff"], 1.05)

    def test_n_eff_near_k_for_noise(self):
        self.assertGreater(self.combine("r = 0; phi = 0;", noise=40.0)["n_eff"], 6.5)

    def test_n_eff_two_for_two_movers(self):
        # Punkte 0–3 sehen Bewegung A (0,23 Hz), 4–7 Bewegung B (0,31 Hz), gleiche Amplitude
        r = self.combine("r = 800.0f; phi = (j < 4) ? 2.0f * sinf(6.2832f * 0.23f * t) "
                         ": 2.0f * sinf(6.2832f * 0.31f * t + 1.0f);", noise=0.0)
        self.assertGreater(r["n_eff"], 1.7)
        self.assertLess(r["n_eff"], 2.3)

    def test_dsp_with_switch(self):
        out = run_cpp(RezoomTrackingTest.GEN.split("    push(900, 1.2f);")[0] + """
        for (int i = 0; i < 20; i++) { push(60, 1.2f); d.computeVitalReport(r); }
        char b[256]; formatDspLine(b, sizeof(b), 1000, true, r, 5);
        printf("%d %.2f %.2f %s", r.valid, r.hr_fused_bpm, r.n_eff, b);""", self.FLAGS)
        valid, hr, neff, line = out.split()
        self.assertEqual(valid, "1")
        self.assertAlmostEqual(float(hr), 72.0, delta=1.0)
        self.assertGreaterEqual(float(neff), 1.0)
        self.assertLess(float(neff), 3.0)
        f = line.split(",")
        self.assertEqual(len(f), 15)                      # n_eff hinten an @DSP, nur mit Schalter
        self.assertAlmostEqual(float(f[14]), float(neff), delta=0.06)

    def test_default_line_unchanged(self):
        out = run_cpp('VitalReport r = {}; char b[256]; formatDspLine(b, sizeof(b), 1000, true, r, 5); printf("%s", b);')
        self.assertEqual(len(out.split(",")), 14)


class MotionMapTest(unittest.TestCase):
    """V6: Bewegungskarte aus dem Block-RMS der stärksten Punkte (motionMapFromRms)."""
    FLAGS = ("-DA121_DSP_REACQ",)

    def run_map(self, setup: str, n_pts: int = 3, nb: int = 15) -> tuple[int, int, int]:
        out = run_cpp(f"""
        float rms[{n_pts} * {nb}];
        for (int i = 0; i < {n_pts} * {nb}; i++) rms[i] = 1.0f;
        {setup}
        const MotionMap m = motionMapFromRms(rms, {n_pts}, {nb});
        printf("%u %u %u", (unsigned)m.mask, (unsigned)m.n_blocks, (unsigned)m.clean_tail_blocks);""", self.FLAGS)
        mask, n, tail = (int(v) for v in out.split())
        return mask, n, tail

    def test_quiet_has_full_tail(self):
        self.assertEqual(self.run_map(""), (0, 15, 15))

    def test_motion_needs_two_votes(self):
        # Block 8 nur bei Punkt 0 × 10: eine Stimme, nicht bewegt
        self.assertEqual(self.run_map("rms[0 * 15 + 8] = 10.0f;"), (0, 15, 15))
        # bei Punkt 0 und 1: bewegt, 6 saubere Blöcke danach
        self.assertEqual(self.run_map("rms[0 * 15 + 8] = 10.0f; rms[1 * 15 + 8] = 10.0f;"), (1 << 8, 15, 6))

    def test_tail_after_last_motion(self):
        setup = "for (int p = 0; p < 2; p++) { rms[p * 15 + 3] = 10.0f; rms[p * 15 + 11] = 10.0f; }"
        self.assertEqual(self.run_map(setup), ((1 << 3) | (1 << 11), 15, 3))

    def test_motion_in_last_block_has_empty_tail(self):
        self.assertEqual(self.run_map("rms[14] = 10.0f; rms[15 + 14] = 10.0f;"), (1 << 14, 15, 0))

    def test_empty_and_single_point(self):
        # nb = 0 und nb = 2 (unter 3 Blöcken keine Aussage wie in gateMotion): leere Karte; ein Punkt: Stimmen werden auf
        # die Punktzahl begrenzt, damit er allein entscheiden kann
        out = run_cpp("""
        float rms[15]; for (int i = 0; i < 15; i++) rms[i] = 1.0f;
        const MotionMap a = motionMapFromRms(rms, 3, 0);
        rms[4] = 10.0f;
        const MotionMap b = motionMapFromRms(rms, 1, 15);
        float two[2] = {1.0f, 10.0f};
        const MotionMap c = motionMapFromRms(two, 1, 2);
        printf("%u %u %u %u %u %u %u %u %u", (unsigned)a.mask, (unsigned)a.n_blocks, (unsigned)a.clean_tail_blocks,
               (unsigned)b.mask, (unsigned)b.n_blocks, (unsigned)b.clean_tail_blocks,
               (unsigned)c.mask, (unsigned)c.n_blocks, (unsigned)c.clean_tail_blocks);""", self.FLAGS)
        self.assertEqual(out.split(), ["0", "0", "0", str(1 << 4), "15", "10", "0", "2", "2"])

    def test_mask_fits_all_blocks(self):
        self.assertEqual(run_cpp('printf("%d", (int)(kMaxBlocks <= 32U));', self.FLAGS), "1")


class SegmentPeriodicTest(unittest.TestCase):
    """V6 Variante Periodizität: 3-s-Ausschnitt des Wegsignals (60 Hz → 15 Hz, Hann) auf eine Spitze im Herzband prüfen.

    Spitze/Mittel der Bins außerhalb der Hauptkeule: Sinus (mit 10 % Rauschen) über 400, weißes Rauschen im Mittel 5
    (bis 39 in 500 Seeds), unregelmäßige Stöße bis 167. Mit kPeriodK = 5 gilt also gut ein Drittel der Rausch-Ausschnitte
    als periodisch (nur ≈ 9 unabhängige Bins in 3 s). Getrennt wird erst bei großem k; dort prüfen die Tests mit klarem
    Abstand, bei k = 5 nur den Sinus und je einen festen Seed für Rauschen und Stöße."""
    FLAGS = ("-DA121_DSP_REACQ", "-DA121_DSP_REACQ_PERIOD")

    def periodic(self, signal: str, n_seeds: int, k: str = "kPeriodK") -> list[int]:
        """C++: signal füllt h[180] (60 Hz) je seed; Ausschnitt um die Mitte, dann segmentPeriodic mit Schwelle k."""
        out = run_cpp(f"""
        static a121_fft::RealFft<kFftN> fft;
        static float h[180], seg[kPeriodSegN], spec[kPeriodK1 + 1];
        for (int seed = 0; seed < {n_seeds}; seed++) {{
            {signal}
            const uint16_t n = periodSegment(h, 180, 90, seg);
            printf("%d ", (int)segmentPeriodic(seg, n, fft, spec, {k}));
        }}""", self.FLAGS)
        return [int(v) for v in out.split()]

    SINE = ("std::mt19937 g(seed + 1); std::normal_distribution<float> nd(0.0f, 0.1f);"
            "for (int i = 0; i < 180; i++) h[i] = sinf(6.2832f * {f} * i / 60.0f) + nd(g);")
    NOISE = ("std::mt19937 g(seed + 100); std::normal_distribution<float> nd(0.0f, 1.0f);"
             "for (int i = 0; i < 180; i++) h[i] = nd(g);")
    IMPULSES = """std::mt19937 g(seed + 7); std::uniform_int_distribution<int> gap(24, 60);
            for (int i = 0; i < 180; i++) h[i] = 0.0f;
            for (int t = gap(g) / 2; t < 180; t += gap(g))
                for (int j = 0; j < 12 && t + j < 180; j++) h[t + j] += 0.5f * (1.0f - cosf(6.2832f * j / 12.0f));"""

    def test_sine_is_periodic(self):
        # 1,2 Hz (72 /min); ebenso 0,9 und 2,4 Hz; auch bei k = 100
        for f in ("1.2f", "0.9f", "2.4f"):
            sig = self.SINE.replace("{f}", f)
            self.assertEqual(self.periodic(sig, 5), [1] * 5, f)
            self.assertEqual(self.periodic(sig, 5, "100.0f"), [1] * 5, f)

    def test_noise_is_not_periodic(self):
        # Seed 100: bei k = 5 nicht periodisch; 40 feste Seeds bei k = 100 keiner
        self.assertEqual(self.periodic(self.NOISE, 1), [0])
        self.assertEqual(self.periodic(self.NOISE, 40, "100.0f"), [0] * 40)

    def test_impulses_irregular_not_periodic(self):
        # Stöße (Hann-förmig, 0,2 s) in zufälligen Abständen 0,4–1,0 s: Seed 7 bei k = 5 nicht periodisch;
        # 20 feste Seeds bei k = 500 keiner
        self.assertEqual(self.periodic(self.IMPULSES, 1), [0])
        self.assertEqual(self.periodic(self.IMPULSES, 20, "500.0f"), [0] * 20)

    def test_silence_and_short_segment_not_periodic(self):
        self.assertEqual(self.periodic("for (int i = 0; i < 180; i++) h[i] = 0.0f;", 1), [0])

    def test_period_without_reacq_is_compile_error(self):
        with self.assertRaises(AssertionError) as e:
            run_cpp('printf("x");', ("-DA121_DSP_REACQ_PERIOD",))
        self.assertIn("nur zusammen mit A121_DSP_REACQ", str(e.exception))

    def test_segment_is_shifted_at_the_edges(self):
        # Mitte nahe am Rand: Ausschnitt bleibt 3 s lang (45 Werte), N unter 3 s: ganzes Signal
        out = run_cpp("""
        static float h[400], seg[kPeriodSegN];
        for (int i = 0; i < 400; i++) h[i] = (float)i;
        printf("%u %u %u %u", periodSegment(h, 400, 10, seg), periodSegment(h, 400, 395, seg),
               periodSegment(h, 400, 200, seg), periodSegment(h, 100, 50, seg));""", self.FLAGS)
        self.assertEqual(out.split(), ["45", "45", "45", "25"])


class ReacqMemoryTest(unittest.TestCase):
    """V6: Kurzzeit-Gedächtnis des Pulses (ReacqMemory): gilt 60 s, erlischt mit dem Echo oder wenn das Ziel die Zone verlässt."""
    FLAGS = ("-DA121_DSP_REACQ",)

    def run_mem(self, body: str) -> list[str]:
        return run_cpp("ReacqMemory m;\n" + body, self.FLAGS).split()

    def test_empty_is_inactive(self):
        out = self.run_mem('printf("%d %g %g", (int)m.active(100U, true, true), m.hr(), m.ibi_ms());')
        self.assertEqual(out, ["0", "0", "0"])

    def test_store_and_active(self):
        out = self.run_mem("""
        m.store(72.0f, 833.0f, 1000U);
        printf("%d %g %g", (int)m.active(1000U + 600U, true, true), m.hr(), m.ibi_ms());""")
        self.assertEqual(out, ["1", "72", "833"])

    def test_expires_after_60_s(self):
        out = self.run_mem("""
        const uint32_t t0 = 1000U;
        m.store(72.0f, 833.0f, t0);
        const int before = m.active(t0 + 60U * 60U - 1U, true, true);
        const int at = m.active(t0 + 60U * 60U, true, true);
        printf("%d %d %g %g", before, at, m.hr(), m.ibi_ms());""")
        self.assertEqual(out, ["1", "0", "0", "0"])

    def test_expiry_survives_frame_counter_wrap(self):
        out = self.run_mem("""
        const uint32_t t0 = 0xFFFFFF00U;
        m.store(72.0f, 833.0f, t0);
        const int before = m.active(t0 + 3599U, true, true);
        const int at = m.active(t0 + 3600U, true, true);
        printf("%d %d", before, at);""")
        self.assertEqual(out, ["1", "0"])

    def test_store_again_refreshes(self):
        out = self.run_mem("""
        m.store(72.0f, 833.0f, 0U);
        m.store(80.0f, 750.0f, 3000U);
        const int a = m.active(3000U + 3599U, true, true);
        printf("%d %g %g", a, m.hr(), m.ibi_ms());""")
        self.assertEqual(out, ["1", "80", "750"])

    def test_cleared_when_echo_lost(self):
        out = self.run_mem("""
        m.store(72.0f, 833.0f, 0U);
        const int lost = m.active(10U, false, true);
        const int back = m.active(20U, true, true);
        printf("%d %d %g", lost, back, m.hr());""")
        self.assertEqual(out, ["0", "0", "0"])

    def test_cleared_when_target_leaves(self):
        out = self.run_mem("""
        m.store(72.0f, 833.0f, 0U);
        const int left = m.active(10U, true, false);
        const int back = m.active(20U, true, true);
        printf("%d %d %g", left, back, m.hr());""")
        self.assertEqual(out, ["0", "0", "0"])

    def test_clear(self):
        out = self.run_mem("""
        m.store(72.0f, 833.0f, 0U);
        m.clear();
        printf("%d %g %g", (int)m.active(1U, true, true), m.hr(), m.ibi_ms());""")
        self.assertEqual(out, ["0", "0", "0"])

    def test_constants(self):
        out = run_cpp('printf("%g %g %g", kMemS, kMemBandBpm, kMemMinConf);', self.FLAGS).split()
        self.assertEqual(out, ["60", "15", "50"])


class PreferMemoryTest(unittest.TestCase):
    """V6: preferMemoryCandidate wählt den stärksten FFT-Kandidaten nahe dem gemerkten Puls (±15 /min, ≥ 50 % des besten)."""
    FLAGS = ("-DA121_DSP_REACQ",)

    def pick(self, bpm: list[float], score: list[float], mem: float) -> int:
        b = ", ".join(f"{float(v)!r}f" for v in bpm)
        s = ", ".join(f"{float(v)!r}f" for v in score)
        out = run_cpp(f"""
        const float b[] = {{{b}}};
        const float s[] = {{{s}}};
        printf("%d", (int)preferMemoryCandidate(b, s, {len(bpm)}, {float(mem)!r}f));""", self.FLAGS)
        return int(out)

    def test_prefers_near_candidate(self):
        self.assertEqual(self.pick([140, 74, 50], [1.0, 0.6, 0.3], 72), 1)

    def test_no_preference_outside_band(self):
        self.assertEqual(self.pick([90, 120], [1.0, 0.8], 72), -1)

    def test_weak_near_candidate_ignored(self):
        self.assertEqual(self.pick([140, 74], [1.0, 0.4], 72), -1)

    def test_strongest_of_several_near(self):
        self.assertEqual(self.pick([140, 80, 66], [1.0, 0.6, 0.7], 72), 2)

    def test_best_candidate_itself_near(self):
        self.assertEqual(self.pick([74, 140], [1.0, 0.9], 72), 0)

    def test_band_edge_inclusive(self):
        self.assertEqual(self.pick([140, 87], [1.0, 0.8], 72), 1)
        self.assertEqual(self.pick([140, 57], [1.0, 0.8], 72), 1)
        self.assertEqual(self.pick([140, 87.5], [1.0, 0.8], 72), -1)

    def test_score_edge_inclusive(self):
        self.assertEqual(self.pick([140, 74], [1.0, 0.5], 72), 1)
        self.assertEqual(self.pick([140, 74], [1.0, 0.49], 72), -1)

    def test_no_candidates_or_no_memory(self):
        out = run_cpp("""
        const float b[] = {74.0f}; const float s[] = {1.0f};
        printf("%d %d %d", (int)preferMemoryCandidate(b, s, 0, 72.0f), (int)preferMemoryCandidate(b, s, 1, 0.0f),
               (int)preferMemoryCandidate(b, s, 1, -5.0f));""", self.FLAGS)
        self.assertEqual(out.split(), ["-1", "-1", "-1"])

    def test_nan_is_rejected(self):
        # NaN als Frequenz oder Wert eines Kandidaten darf nicht gewählt werden; ohne positiven besten Wert keine Wahl
        out = run_cpp("""
        const float b1[] = {140.0f, NAN}; const float s1[] = {1.0f, 0.8f};
        const float b2[] = {140.0f, 74.0f}; const float s2[] = {1.0f, NAN};
        const float b3[] = {74.0f, 70.0f}; const float s3[] = {NAN, 0.8f};
        const float b4[] = {74.0f}; const float s4[] = {0.0f};
        printf("%d %d %d %d", (int)preferMemoryCandidate(b1, s1, 2, 72.0f), (int)preferMemoryCandidate(b2, s2, 2, 72.0f),
               (int)preferMemoryCandidate(b3, s3, 2, 72.0f), (int)preferMemoryCandidate(b4, s4, 1, 72.0f));""", self.FLAGS)
        self.assertEqual(out.split(), ["-1", "-1", "-1", "-1"])


class TemplateIbiTest(unittest.TestCase):
    """V3.6 (Schalter A121_DSP_IBI_TEMPLATE): Einzelschläge mit Vorlage. Herzsignal im Band 0,6–8 Hz (statt 0,6–5 Hz),
    Vorlage = Mittel der Abschnitte ±0,3 × Schlagabstand um die ersten Spitzen (wie V1.6 gefunden), Korrelationsfilter
    mit der Vorlage, Spitzen der Korrelation parabolisch verfeinert, danach dieselben Artefakt-Regeln wie V1.6.
    Abnahme laut Plan: IBI-Fehler gegen Gurt-RR halbiert gegenüber V1.6 (hier gegen die wahren Schläge, NUR SIMULATION)."""

    FLAGS = ("-DA121_DSP_IBI_TEMPLATE",)

    BEAT = "expf(-0.5f * u * u) - 0.45f * expf(-0.5f * w * w)"

    def run_beats(self, noise: float, hrv_ms: float = 40.0, seed: int = 3, hr: float = 72.0, beat: str = BEAT,
                  extra: str = "0.0f") -> dict:
        """Herzwelle wie host/vital_sim.py (zwei Gauß-Anteile je Schlag, Spitze-Spitze 1) + weißes Rauschen (σ = noise),
        60 Hz, 15 s. V1.6: Band 0,6–5 Hz + timeDomainIbi; V3.6: Band 0,6–8 Hz + templateIbi. Rückgabe je Verfahren:
        Median |IBI-Fehler| (ms) über aufeinanderfolgende getroffene Schläge, Zahl der Treffer, bpm."""
        out = run_cpp(f"""
        const uint16_t N = 900; const float fs = 60.0f;
        static float raw[N], ext[N + 2 * kFiltPad], x5[N], x8[N], work[N];
        std::vector<float> tb;
        uint32_t s = {seed}u; auto uni = [&] {{ s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0f; }};
        auto gauss = [&] {{ float a = 0; for (int i = 0; i < 12; i++) a += uni(); return a - 6.0f; }};
        for (float t = 0.3f; t < 15.5f; t += 60.0f / {hr}f + {hrv_ms}f / 1000.0f * gauss()) tb.push_back(t);
        float lo = 1e9f, hi = -1e9f;
        for (int n = 0; n < N; n++) {{
            const float t = n / fs; float v = 0;
            for (size_t k = 0; k < tb.size(); k++) {{ const float b = tb[k];
                const float u = (t - b) / 0.06f, w = (t - b - 0.18f) / 0.09f; v += {beat}; }}
            raw[n] = v; lo = fminf(lo, v); hi = fmaxf(hi, v);
        }}
        for (int n = 0; n < N; n++) {{ const float t = n / fs; raw[n] = raw[n] / (hi - lo) + {noise}f * gauss() + {extra}; }}
        sosFiltFiltSos(raw, N, ext, kBpSos);  memcpy(x5, ext + kFiltPad, N * sizeof(float));
        sosFiltFiltSos(raw, N, ext, kBp8Sos); memcpy(x8, ext + kFiltPad, N * sizeof(float));
        float t5[kIbiMaxPk], t8[kIbiMaxPk];
        const IbiResult a = timeDomainIbi(x5, N, fs, {hr}f, t5);
        const IbiResult b = templateIbi(x8, N, fs, {hr}f, work, t8);
        auto err = [&](const float *td, uint8_t nd) {{   /* Treffer: nächste Spitze ±150 ms nach Abzug des Versatzes */
            std::vector<float> off;
            for (float bt : tb) {{ if (bt < 0.5f || bt > 14.5f) continue; float best = 1e9f;
                for (int k = 0; k < nd; k++) if (fabsf(td[k] / fs - bt) < fabsf(best)) best = td[k] / fs - bt;
                if (fabsf(best) < 0.15f) off.push_back(best); }}
            if (off.size() < 3) return std::make_pair(-1.0f, 0);
            std::nth_element(off.begin(), off.begin() + off.size() / 2, off.end()); const float o = off[off.size() / 2];
            std::vector<float> e; float prev_d = -1, prev_t = -1;
            for (float bt : tb) {{ if (bt < 0.5f || bt > 14.5f) {{ prev_t = -1; continue; }} float best = 1e9f;
                for (int k = 0; k < nd; k++) if (fabsf(td[k] / fs - o - bt) < fabsf(best - bt)) best = td[k] / fs - o;
                if (fabsf(best - bt) > 0.15f) {{ prev_t = -1; continue; }}
                if (prev_t >= 0) e.push_back(fabsf((best - prev_d) - (bt - prev_t)) * 1000.0f);
                prev_d = best; prev_t = bt; }}
            if (e.empty()) return std::make_pair(-1.0f, 0);
            std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end());
            return std::make_pair(e[e.size() / 2], (int)e.size());
        }};
        const auto ea = err(t5, a.n_beats), eb = err(t8, b.n_beats);
        printf("%.3f %d %.2f %.3f %d %.2f", ea.first, ea.second, a.bpm, eb.first, eb.second, b.bpm);""", self.FLAGS)
        v = out.split()
        return {"v16": float(v[0]), "n16": int(v[1]), "bpm16": float(v[2]),
                "v36": float(v[3]), "n36": int(v[4]), "bpm36": float(v[5])}

    def test_coefficients_match_scipy(self):
        """kBp8Sos = scipy.signal.butter(4, [0.6, 8.0], 'band', fs=60, output='sos')."""
        import re
        import numpy as np
        from scipy import signal
        txt = (FW / "a121_esp_dsp.h").read_text()
        block = txt[txt.index("kBp8Sos[4][6]"):]
        block = block[:block.index("};")]
        nums = [float(x) for x in re.findall(r"-?\d+\.\d+(?:e-?\d+)?", block)]
        sos = signal.butter(4, [0.6, 8.0], "band", fs=60, output="sos")
        np.testing.assert_allclose(np.array(nums).reshape(4, 6), sos, rtol=1e-6, atol=1e-12)

    SEEDS = (3, 11, 29, 41, 57, 73)
    REST = (dict(noise=0.05, extra="0.3f * sinf(6.2832f * 1.05f * t + 0.4f)"),   # Atem-Rest nahe am Puls
            dict(noise=0.05, extra="0.25f * sinf(6.2832f * 0.81f * t)",
                 beat="(1.0f + 0.35f * sinf(6.2832f * 0.27f * b)) * (expf(-0.5f * u * u) - 0.45f * expf(-0.5f * w * w))"))

    def ratios(self, **kw) -> list[float]:
        out = []
        for sd in self.SEEDS:
            r = self.run_beats(seed=sd, **kw)
            self.assertGreater(r["n36"], 10, r)
            out.append(r["v36"] / r["v16"])
        return out

    def test_white_noise_not_worse(self):
        # weißes Rauschen σ = 0,15 × Spitze-Spitze: V1.6 liegt schon nahe der Cramér-Rao-Grenze (≈ 7 ms je Schlag),
        # die Vorlage gewinnt dort wenig (Median-Faktor gemessen 0,81, ein Fenster 1,24)
        import statistics
        self.assertLessEqual(statistics.median(self.ratios(noise=0.15)), 0.9)

    def test_breath_residual_error_reduced(self):
        # Atem-Rest im Herzband (0,81 / 1,05 Hz, wie ihn removeBreathHarmonics am Puls stehen lässt), Schlaghöhe ±35 %:
        # die mittelwertfreie Vorlage unterdrückt ihn; Median-Faktor gemessen 0,58 (Plan: 0,5, an Tag A gegen Gurt-RR)
        import statistics
        r = [x for kw in self.REST for x in self.ratios(**kw)]
        self.assertLessEqual(statistics.median(r), 0.65)

    def test_clean_signal_same_rate(self):
        r = self.run_beats(0.0)
        self.assertAlmostEqual(r["bpm36"], r["bpm16"], delta=1.0)
        self.assertLess(r["v36"], 5.0)

    def test_slow_and_fast_pulse(self):
        for hr in (45.0, 130.0):
            r = self.run_beats(0.1, hr=hr, hrv_ms=20.0)
            self.assertAlmostEqual(r["bpm36"], hr, delta=3.0, msg=r)

    def test_dsp_with_switch(self):
        # mit Schalter laufen die Einzelschläge über das Band 0,6–8 Hz und die Vorlage: Puls gleich, IBI-Werte anders
        body = RezoomTrackingTest.GEN.split("    push(900, 1.2f);")[0].replace(
            "+ 0.08f * sinf(6.2832f * f_hr * t);",
            "+ 0.08f * (expf(-8.0f * (1.0f - cosf(6.2832f * f_hr * t))) + 0.4f * sinf(6.2832f * 2.0f * f_hr * t + 0.7f));") + """
        for (int i = 0; i < 20; i++) { push(60, 1.2f); d.computeVitalReport(r); }
        printf("%d %.2f %.2f %.4f", r.valid, r.hr_fused_bpm, r.hr_ibi_bpm, r.ibi_median_ms);"""
        self.assertIn("expf(-8.0f", body)
        v1, hr1, ibi1, ms1 = run_cpp(body, self.FLAGS).split()
        v0, hr0, ibi0, ms0 = run_cpp(body).split()
        self.assertEqual((v1, v0), ("1", "1"))
        self.assertAlmostEqual(float(hr1), 72.0, delta=1.0)
        self.assertAlmostEqual(float(ibi1), 72.0, delta=2.0)
        self.assertNotEqual(ms1, ms0)

    def test_default_ibi_unchanged_with_times(self):
        # Standard: timeDomainIbi mit Zeitausgabe liefert dieselben Werte wie ohne
        out = run_cpp("""
        static float x[900]; float tt[kIbiMaxPk];
        for (int n = 0; n < 900; n++) { const float t = n / 60.0f; x[n] = sinf(6.2832f * 1.2f * t) + 0.3f * sinf(6.2832f * 2.4f * t); }
        const IbiResult a = timeDomainIbi(x, 900, 60.0f, 72.0f), b = timeDomainIbi(x, 900, 60.0f, 72.0f, tt);
        printf("%d %d %.3f", a.bpm == b.bpm && a.rmssd_ms == b.rmssd_ms && a.n_valid == b.n_valid, (int)b.n_beats, tt[1] - tt[0]);""")
        same, nb, d = out.split()
        self.assertEqual(same, "1")
        self.assertGreater(int(nb), 10)
        self.assertAlmostEqual(float(d), 50.0, delta=0.5)


if __name__ == "__main__":
    unittest.main()
