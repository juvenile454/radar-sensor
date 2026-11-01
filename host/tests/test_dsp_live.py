"""Tests für den Live-Pfad der Handy-DSP (V5.2): host/dsp_replay/dsp_live.h über die PC-Fassung dsp_live_main.cpp,
gespeist mit synthetischen iq3-Logs aus host/vital_sim.py (nur Simulation, keine Laufzeitprüfung am Handy).

  .venv/bin/python -m unittest host/tests/test_dsp_live.py
"""

import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST))

import vital_sim  # noqa: E402

LIVE_TAGS = ("@DSP,", "@VITAL,", "@STATE,", "@BREATH,")


class LiveDspTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = pathlib.Path(cls.tmp.name)
        base = d / "base"
        vital_sim.make_frames(base, seed=1, T=30.0)
        iq = pathlib.Path(f"{base}.iq")
        zoom_pt = int(np.load(f"{base}_truth.npz")["zoom_start_pt"])
        cls.skel = d / "skel.log"
        vital_sim.write_skeleton(iq, zoom_pt, cls.skel, vital_placeholders=False)
        cls.replayed = d / "replayed.log"
        vital_sim.replay_skeleton(cls.skel, cls.replayed)
        cls.dir = d
        cls.cache = {}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def live(self, *defines: str, log: pathlib.Path | None = None) -> tuple[list[str], str]:
        """Skelett (oder log) Zeile für Zeile durch die Live-Fassung; Rückgabe (Handy-Zeilen, Bericht auf stderr)."""
        log = log or self.skel
        if (defines, log) in self.cache:
            return self.cache[(defines, log)]
        exe = self.dir / ("live_" + "_".join(d.lstrip("-") for d in defines))
        if not exe.exists():
            subprocess.run(["g++", "-O2", "-std=c++17", "-ffp-contract=off", f"-DA121_ZOOM_POINTS={vital_sim.ZOOM_N}",
                            *defines, f"-I{vital_sim.REPLAY_DIR}", f"-I{vital_sim.DSP_HEADER.parent}",
                            str(vital_sim.REPLAY_DIR / "dsp_live_main.cpp"), "-o", str(exe), "-lm"], check=True)
        r = subprocess.run([str(exe), str(log)], capture_output=True, text=True, check=True)
        self.cache[(defines, log)] = (r.stdout.splitlines(), r.stderr)
        return self.cache[(defines, log)]

    def esp_like(self) -> list[str]:
        """Skelett mit den @DSP-Zeilen der Nachrechnung: Feld 13 = Frame-Stand des Schnappschusses wie beim ESP32."""
        dsp = iter(ln for ln in self.replayed.read_text().splitlines() if ln.startswith("@DSP,"))
        return [next(dsp) if ln.startswith("@DSP,") else ln for ln in self.skel.read_text().splitlines()]

    def test_started_mid_stream(self):
        """Handy-DSP erst nach 10 s eingeschaltet (PhoneDsp schickt das gemerkte @IQ3_START vorweg): Der Frame-Stand
        in @DSP ist der des ESP32 und liegt vor dem eigenen; trotzdem gültige Werte nach der Einschwingzeit."""
        lines = self.esp_like()
        self.assertTrue(lines[0].startswith("@IQ3_START,"))
        cut = lines.index(next(ln for ln in lines if ln.startswith("@RAW,600,")))
        mid = self.dir / "mid.log"
        mid.write_text("\n".join([lines[0]] + lines[cut:]) + "\n")
        got, _ = self.live(log=mid)
        dsp = [ln for ln in got if ln.startswith("@DSP,")]
        self.assertGreater(len(dsp), 15)
        self.assertGreater(sum(ln.startswith("@VITAL,") for ln in got), 5)

    def test_esp_snapshot_field_from_start(self):
        """Von Anfang an mit Feld 13: dieselben Zeilen wie die Nachrechnung (Schnappschuss gefunden, nicht veraltet)."""
        whole = self.dir / "esp_like.log"
        whole.write_text("\n".join(self.esp_like()) + "\n")
        want = [ln for ln in self.replayed.read_text().splitlines() if ln.startswith(LIVE_TAGS)]
        got, _ = self.live(log=whole)
        self.assertEqual(got, want)

    def test_same_lines_as_replay(self):
        """Ohne Variante liefert der Live-Pfad genau die neu gerechneten Zeilen der Nachrechnung (nur Handy-Tags)."""
        want = [ln for ln in self.replayed.read_text().splitlines() if ln.startswith(LIVE_TAGS)]
        got, _ = self.live()
        self.assertGreater(sum(ln.startswith("@VITAL,") for ln in want), 5)
        self.assertEqual(got, want)

    def test_only_phone_tags(self):
        got, _ = self.live()
        self.assertTrue(got)
        self.assertTrue(all(ln.startswith(LIVE_TAGS) for ln in got), [ln[:12] for ln in got if not ln.startswith(LIVE_TAGS)][:3])

    def test_heavy_is_viterbi_and_harm_remove(self):
        heavy, _ = self.live("-DA121_DSP_HEAVY")
        both, _ = self.live("-DA121_DSP_VITERBI", "-DA121_DSP_HARM_REMOVE")
        self.assertEqual(heavy, both)

    def test_heavy_differs_from_default(self):
        heavy, _ = self.live("-DA121_DSP_HEAVY")
        plain, _ = self.live()
        hv = [ln for ln in heavy if ln.startswith("@VITAL,")]
        self.assertGreater(len(hv), 5)
        self.assertNotEqual(hv, [ln for ln in plain if ln.startswith("@VITAL,")])

    def test_cpu_share_reported(self):
        """Rechenzeit je Datensekunde (am PC; die Abnahme ≤ 20 % eines Kerns gilt am Handy und bleibt offen)."""
        _, rep = self.live("-DA121_DSP_HEAVY")
        m = re.search(r"Rechenzeit ([\d.]+) ms für ([\d.]+) s Daten \(([\d.]+) % eines Kerns\)", rep)
        self.assertIsNotNone(m, rep)
        self.assertAlmostEqual(float(m.group(2)), 30.0, delta=0.5)
        self.assertLess(float(m.group(3)), 20.0)


if __name__ == "__main__":
    unittest.main()
