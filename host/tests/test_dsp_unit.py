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


def run_cpp(body: str, flags: tuple[str, ...] = ()) -> str:
    """Übersetzt ein kleines Programm gegen a121_esp_dsp.h und gibt seine Ausgabe zurück (flags: z. B. -D…)."""
    src = ('#include <Arduino.h>\n#include <vector>\n#include "a121_esp_dsp.h"\nusing namespace a121_dsp;\nint main() {\n'
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


if __name__ == "__main__":
    unittest.main()
