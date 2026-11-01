"""V5.3: alte PC-Skripte mit eigener Vital-DSP sind im Kopfkommentar als Archiv gekennzeichnet; neue Auswertungen
laufen über host/vital_eval.py und Varianten des Firmware-Headers.

  .venv/bin/python -m unittest host/tests/test_archive.py
"""

import ast
import pathlib
import unittest

HOST = pathlib.Path(__file__).resolve().parent.parent
README = HOST.parent / "README.md"

# eigene DSP-Kette (Kåsa/DACM/Hilbert-Projektion/HPS) bzw. deren Nutzer (a121_livemonitor importiert a121_vitalaufnahme)
ARCHIVE = ["a121_ultimate_vital.py", "a121_vitalaufnahme.py", "a121_anhalteanalyse.py", "a121_esp_vital.py",
           "a121_esp_auto.py", "a121_livemonitor.py"]
# gepflegt: rechnen mit dem Firmware-Header oder sind Werkzeuge ohne eigene Vital-DSP
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
