"""Tests for host/noise_floor.py (V4.1: static displacement noise), with synthetic data only.

  .venv/bin/python -m unittest host/tests/test_noise_floor.py
"""

import io
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

import numpy as np

HOST = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HOST))

import noise_floor as nf  # noqa: E402
import vital_sim  # noqa: E402

FS, N, P = 60.0, 60 * 60, 8  # 60 s, 8 zoom points


def plate(rng, sigma_rad=0.002, jitter_um=0.0, loop_sigma_rad=0.0005):
    """Static target: fixed IQ values per point, white phase noise, common phase jitter (also in the loopback)."""
    t = np.arange(N) / FS
    jit = jitter_um / nf.RAD_TO_UM * np.sin(2 * np.pi * 1.3 * t + 0.4)  # 1.3 Hz: in the middle of the pulse band
    base = 1500.0 * np.exp(1j * rng.uniform(-np.pi, np.pi, P))
    frames = base[None, :] * np.exp(1j * (jit[:, None] + rng.normal(0, sigma_rad, (N, P))))
    loop = 900.0 * np.exp(1j * (0.3 + jit + rng.normal(0, loop_sigma_rad, N)))
    return frames.astype(np.complex64), loop.astype(np.complex64)


class NoiseFloorTest(unittest.TestCase):
    def test_white_phase_noise_in_band(self):
        """White phase noise σ: share in the 0.6–5 Hz band ≈ σ · λ/4π · √(4.4 / 30)."""
        rng = np.random.default_rng(1)
        frames, _ = plate(rng, sigma_rad=0.002)
        rms = nf.noise_floor(frames, FS, None, "aus")
        want = 0.002 * nf.RAD_TO_UM * np.sqrt(4.4 / 30.0)
        self.assertEqual(rms.shape, (P,))
        np.testing.assert_allclose(rms, want, rtol=0.15)

    def test_derotation_removes_common_jitter(self):
        rng = np.random.default_rng(2)
        frames, loop = plate(rng, sigma_rad=0.002, jitter_um=5.0)
        off = np.median(nf.noise_floor(frames, FS, loop, "aus"))
        raw = np.median(nf.noise_floor(frames, FS, loop, "roh"))
        self.assertGreater(off, 3.0)  # jitter 5 µm peak → ≈ 3.5 µm RMS
        self.assertLess(raw, 0.5)

    def test_lowpass_avoids_loopback_noise(self):
        """Noisy loopback, no fast jitter: raw derotation injects its noise, the lowpass does not."""
        rng = np.random.default_rng(3)
        frames, loop = plate(rng, sigma_rad=0.001, loop_sigma_rad=0.004)
        off = np.median(nf.noise_floor(frames, FS, loop, "aus"))
        raw = np.median(nf.noise_floor(frames, FS, loop, "roh"))
        low = np.median(nf.noise_floor(frames, FS, loop, "tiefpass"))
        self.assertGreater(raw, 2.5 * off)
        self.assertLess(low, 1.2 * off)

    def test_modes_without_loopback(self):
        rng = np.random.default_rng(4)
        frames, _ = plate(rng)
        with self.assertRaises(ValueError):
            nf.noise_floor(frames, FS, None, "roh")
        with self.assertRaises(ValueError):
            nf.noise_floor(frames, FS, None, "falsch")

    def test_load_raw_stream_log(self):
        """iq3 … raw: point 0 loopback, then zoom points; room points are dropped."""
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            vital_sim.make_frames(d / "b", seed=1, T=5.0)
            zoom = int(np.load(f"{d}/b_truth.npz")["zoom_start_pt"])
            vital_sim.write_skeleton(pathlib.Path(f"{d}/b.iq"), zoom, d / "stream.log")
            rec = nf.load(d / "stream.log")
        self.assertEqual(rec.frames.shape, (300, vital_sim.ZOOM_N))
        self.assertEqual(rec.loop.shape, (300,))
        self.assertEqual(rec.fs, 60.0)

    def test_npz_roundtrip_and_report(self):
        rng = np.random.default_rng(5)
        frames, loop = plate(rng, jitter_um=5.0)
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "p1_h32_60hz.npz"
            nf.save(p, nf.Recording(frames, loop, FS, {"profile": 1, "hwaas": 32}))
            rec = nf.load(p)
            np.testing.assert_array_equal(rec.frames, frames)
            self.assertEqual(rec.config["hwaas"], 32)
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(nf.main(["eval", str(p)]), 0)
        text = out.getvalue()
        self.assertIn("p1_h32_60hz", text)
        for mode in ("aus", "roh", "tiefpass"):
            self.assertRegex(text, rf"{mode}\s+[\d.]+ µm")


class RecordConfigTest(unittest.TestCase):
    """Recording configuration for exptool via `bridge` (without a device: only the validation by exptool itself)."""

    def test_default_like_iq3_zoom(self):
        cfg = nf.build_config(start_mm=600, points=24, profile=1, hwaas=32, gain=16, rate=60.0)
        cfg.validate()
        lb, zoom = cfg.subsweeps
        self.assertTrue(lb.enable_loopback)
        self.assertEqual(zoom.start_point, 240)
        self.assertEqual(zoom.num_points, 24)
        self.assertEqual(zoom.hwaas, 32)
        self.assertEqual(zoom.prf.name, "PRF_15_6_MHz")
        self.assertEqual((lb.hwaas, lb.receiver_gain, lb.prf.name), (8, 16, "PRF_19_5_MHz"))
        self.assertEqual(cfg.frame_rate, 60.0)

    def test_profile_3_with_slow_loopback_prf(self):
        cfg = nf.build_config(start_mm=600, profile=3, hwaas=64, rate=30.0, loop_prf="15.6")
        cfg.validate()
        self.assertEqual(cfg.subsweeps[1].prf.name, "PRF_15_6_MHz")
        self.assertEqual(cfg.subsweeps[0].prf.name, "PRF_15_6_MHz")

    def test_loopback_profile_2_rejected(self):
        with self.assertRaises(Exception) as e:
            nf.build_config(start_mm=600, prf="15.6", loop_profile=2, loop_prf="15.6").validate()
        self.assertIn("Profile 2", str(e.exception))

    def test_loopback_prf_not_below_zoom(self):
        """Loopback 15.6 MHz only works if the zoom (subsweep after it) has at most 15.6 MHz too."""
        with self.assertRaises(Exception) as e:
            nf.build_config(start_mm=600, profile=1, prf="19.5", loop_prf="15.6").validate()
        self.assertIn("prf", str(e.exception))
        nf.build_config(start_mm=600, profile=1, prf="15.6", loop_prf="15.6").validate()

    def test_without_loopback(self):
        cfg = nf.build_config(start_mm=600, loop=False)
        cfg.validate()
        self.assertEqual(len(cfg.subsweeps), 1)


if __name__ == "__main__":
    unittest.main()
