"""V5.3: old PC scripts with their own vital DSP are marked as archive in their header comment; new evaluations
run through host/vital_eval.py and variants of the firmware header.

  .venv/bin/python -m unittest host/tests/test_archive.py
"""

import ast
import pathlib
import unittest

HOST = pathlib.Path(__file__).resolve().parent.parent
README = HOST.parent / "README.md"

# own DSP chain (Kåsa/DACM/Hilbert projection/HPS) or its users (a121_livemonitor imports a121_vitalaufnahme)
ARCHIVE = ["a121_ultimate_vital.py", "a121_vitalaufnahme.py", "a121_anhalteanalyse.py", "a121_esp_vital.py",
           "a121_esp_auto.py", "a121_livemonitor.py"]
# maintained: compute with the firmware header or are tools without their own vital DSP
CURRENT = ["vital_eval.py", "vital_sim.py", "a121_esp_ultimate.py", "esp_record.py", "esp_tcp_bridge.py",
           "rezoom_check.py", "a121_zielmessung.py"]


def doc(name: str) -> str:
    return ast.get_docstring(ast.parse((HOST / name).read_text())) or ""


class ArchiveMarkTest(unittest.TestCase):
    def test_archive_scripts_marked_first(self):
        for name in ARCHIVE:
            with self.subTest(name):
                d = doc(name)
                self.assertTrue(d.startswith("ARCHIV"), d.splitlines()[0] if d else "(kein Kopfkommentar)")
                self.assertIn("vital_eval.py", d)
                self.assertIn("a121_esp_dsp.h", d)

    def test_current_scripts_not_marked(self):
        for name in CURRENT:
            with self.subTest(name):
                self.assertFalse(doc(name).startswith("ARCHIV"))

    def test_readme_names_archive(self):
        row = next(ln for ln in README.read_text().splitlines() if ln.startswith("| `host/` |"))
        self.assertIn("Archiv", row)
        tail = row[row.index("Archiv"):]
        for name in ARCHIVE:
            self.assertIn(name.removesuffix(".py"), tail, name)


if __name__ == "__main__":
    unittest.main()
