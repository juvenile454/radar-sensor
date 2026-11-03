"""ESP32 firmware on the PC (stub headers host/fw_stub/, no substitute for `pio run`, no run on the device):
syntax check of main.cpp; iq3 preset (a121_server_client.h) adjustable via compile switches, default
unchanged (V4); host/noise_floor.py takes the same default values.

  .venv/bin/python -m unittest host/tests/test_firmware_pc.py
"""

import pathlib
import subprocess
import sys
import tempfile
import unittest

HOST = pathlib.Path(__file__).resolve().parent.parent
REPO = HOST.parent
sys.path.insert(0, str(HOST))

import noise_floor as nf  # noqa: E402

STUB = HOST / "fw_stub"
SRC = REPO / "firmware" / "esp32s3" / "src"
HARNESS = r"""
#include "a121_server_client.h"
#include <cstdio>
int main() {
    auto s = a121_srv::ExplorationServerClient::make3SubsweepPreset(200);
    for (int i = 0; i < 2; i++) {
        const auto &u = s.subsweeps[i];
        printf("%d %d %d %d %s\n", (int)u.profile, (int)u.hwaas, (int)u.receiver_gain, (int)u.enable_loopback, u.prf);
    }
    printf("%s\n", a121_srv::iq3PresetTag());
}
"""


class FirmwareSyntaxTest(unittest.TestCase):
    def test_main_cpp(self):
        c = subprocess.run(["g++", "-std=gnu++17", "-fsyntax-only", "-Wall", "-Wextra", "-Wno-unused-parameter",
                            "-DBOARD_HAS_PSRAM", f"-I{STUB}", "main.cpp"], cwd=SRC, capture_output=True, text=True)
        self.assertEqual(c.returncode, 0, c.stderr[-2000:])


class BuildFlagsTest(unittest.TestCase):
    """The replay on the PC, in the test bench and on the phone is compiled with -ffp-contract=off; the firmware must be too,
    otherwise GCC on the ESP32-S3 fuses multiplication and addition into madd.s and the values differ in the last
    bits (the comparison ESP32 ↔ replay would no longer be bit-identical)."""

    def flags(self) -> list[str]:
        ini = (REPO / "firmware" / "esp32s3" / "platformio.ini").read_text().splitlines()
        i = next(k for k, ln in enumerate(ini) if ln.startswith("build_flags"))
        vals = [ini[i].split("=", 1)[1]]
        for ln in ini[i + 1:]:
            if not ln[:1].isspace():
                break
            vals.append(ln)
        return [t for ln in vals for t in ln.split(";", 1)[0].split()]

    def test_fp_contract_off_like_replay(self):
        f = self.flags()
        self.assertIn("-DBOARD_HAS_PSRAM", f)  # reader finds the list
        self.assertIn("-ffp-contract=off", f)
        self.assertFalse([x for x in f if x in ("-ffast-math", "-Ofast") or x.startswith("-ffp-contract=fast")])


class Iq3PresetTest(unittest.TestCase):
    def run_preset(self, *defines: str) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as d:
            src, exe = pathlib.Path(d) / "p.cpp", pathlib.Path(d) / "p"
            src.write_text(HARNESS)
            c = subprocess.run(["g++", "-std=gnu++17", *defines, f"-I{STUB}", f"-I{SRC}", str(src), "-o", str(exe)],
                               capture_output=True, text=True)
            if c.returncode != 0:
                if not defines:
                    self.fail(c.stderr[-2000:])
                return c
            return subprocess.run([str(exe)], capture_output=True, text=True, check=True)

    def test_default_unchanged(self):
        out = self.run_preset().stdout.splitlines()
        self.assertEqual(out[0], "1 8 16 1 19_5_MHz")   # Loopback
        self.assertEqual(out[1], "1 32 16 0 15_6_MHz")  # Zoom
        self.assertEqual(out[2], "p1h32/lp1g16/19_5_MHz")

    def test_switches(self):
        out = self.run_preset("-DA121_IQ3_ZOOM_PROFILE=2", "-DA121_IQ3_ZOOM_HWAAS=64", "-DA121_IQ3_LOOP_PROFILE=4",
                              "-DA121_IQ3_LOOP_GAIN=10", '-DA121_IQ3_LOOP_PRF="15_6_MHz"').stdout.splitlines()
        self.assertEqual(out[0], "4 8 10 1 15_6_MHz")
        self.assertEqual(out[1], "2 64 16 0 15_6_MHz")
        self.assertEqual(out[2], "p2h64/lp4g10/15_6_MHz")

    def test_loopback_profile_2_refused_at_compile_time(self):
        c = self.run_preset("-DA121_IQ3_LOOP_PROFILE=2")
        self.assertNotEqual(c.returncode, 0)
        self.assertIn("Profil 2", c.stderr)

    def test_noise_floor_defaults_match_firmware(self):
        """V4.1 measures with noise_floor.py record; without arguments the same zoom/loopback setting as iq3."""
        lb, zoom = nf.build_config(start_mm=500).subsweeps
        out = self.run_preset().stdout.splitlines()

        def row(u):
            return f"{u.profile.value} {u.hwaas} {u.receiver_gain} {int(u.enable_loopback)} {u.prf.name[4:]}"

        self.assertEqual(row(lb), out[0])
        self.assertEqual(row(zoom), out[1])


if __name__ == "__main__":
    unittest.main()
