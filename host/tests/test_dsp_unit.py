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


if __name__ == "__main__":
    unittest.main()
