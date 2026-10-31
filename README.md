# radar-sensor

60-GHz-Radar (Acconeer A121 auf Waveshare-Board) + ESP32-S3 + Android-App am Galaxy S26 Ultra.
Lernprojekt: löten, Mikrocontroller programmieren, Signalverarbeitung (Atmung, Puls, Materialtests).
Aktueller Fokus: den A121 vollständig ausreizen; Pico und SAR-Scanner zurückgestellt.

**Status 2026-10-31 (Abend):**
- **Aktueller Hardware-Aufbau (Mobiler ESP32-S3 ↔ A121 Modus):** A121 mit `i2c_distance_detector.bin` (`v1.12.0`) über das 10-Pin-Kabel am ESP32-S3 (`firmware/esp32s3/src/main.cpp`). 400 kHz I2C + Hardware-Interrupt-Polling (`GPIO18` / `MCU_INT`) erreicht **37,1–45,5 Hz** Dauer-Abtastrate mit **0,001-dB-Stärkeauflösung**.
- **Vollautomatischer Mobiler Modus (`auto [sek]` + `host/a121_esp_auto.py`):** Autonomer Zustandsautomat auf dem ESP32-S3 (`WIDE_SEARCH 150–2500 mm` → `TARGET_LOCK` → `VITAL_ZOOM ±145 mm` mit `PROFILE_1`, `2,5 mm` Schrittweite, `-25 dB`, `CLOSEST` und Auto-Rezoom) liefert gleichzeitig 10 Radar-Metriken für die spätere Android-App (Hauptziel-Abstand, Raum-Echo-Karte, Relativgeschwindigkeit, Stabilitäts-Score, Ziel-Klassifikation, Atmung, Apnoe-Überwachung, Herzschlag-Kandidat + Konfidenz, Technik-Vibration, normierter RCS & Chip-Temperatur).
- **PC-Direktmodus mit 3-Subsweep Sparse-IQ (`host/a121_ultimate_vital.py`):** Mit `acc_exploration_server_a121.bin` über A121-USB-C simultan 3 Hardware-Subsweeps @ 60 Hz: Subsweep 0 (On-Chip-Loopback `enable_loopback=True` zur 1/f-PLL-Phasenkorrektur), Subsweep 1 (2,5-mm-Vital-Zoom, `PROFILE_1`, `PRF_19_5_MHz`, `phase_enhancement=True`, `iq_imbalance_compensation=True`) und Subsweep 2 (Vollraum-Scan `150–2070 mm`, `PROFILE_3`) inkl. Einzel-Herzschlag-Zeitbereichsanalyse (IBI/CV/RMSSD).
- **Signalverarbeitung (`host/`):** Kohärente Multi-Bin-/Phasenstabilitäts-Auswahl, Kåsa-Kreisfit + sanft regularisierte DACM-Phasendemodulation (`eps = 0.01 * P5(|z|²)`), gleitende 7-s-Hann-Hilbert-Projektion auf Basis der Schmalband-Atemgrundphase $\theta_1(t)$ ($m=1\dots 6$, unterdrückt 3./5. Atmungs-Oberwelle um >90 %) und Beschleunigungs-HPS ($f_0\cdot 2f_0\cdot 3f_0$) in `a121_ultimate_vital.py`, `a121_vitalaufnahme.py`, `a121_anhalteanalyse.py`, `a121_esp_vital.py` und `a121_esp_auto.py`.
- **Wichtig:** Alle ermittelten Puls-Werte sind Rhythmus-Kandidaten im Radarsignal; ohne Referenzmessung
  (Smartwatch/Pulsoximeter/EKG) keine Aussage über Puls-Genauigkeit (`docs/EXPERIMENTE.md`).

## Dokumente

- `docs/KONZEPT.md` — Ziele, Realitätscheck, Architektur, Phasen, Einkauf
- `docs/EXPERIMENTE.md` — Fähigkeiten-Matrix (Phase 1), Ergebnisse nur nach Messung
- `docs/HARDWARE.md` — Boards, Pinouts, Verdrahtung, Strom

## Geplante Struktur (Ordner erst anlegen, wenn die Phase beginnt)

| Ordner | Inhalt |
|---|---|
| `host/` | Python: `a121_ultimate_vital.py`, `a121_zielmessung.py`, `a121_vitalaufnahme.py`, `a121_livemonitor.py`, `a121_anhalteanalyse.py`, `a121_esp_vital.py`, `a121_esp_auto.py` (Setup: `uv venv --python 3.12 .venv && uv pip install --python .venv/bin/python -r host/requirements.txt`) |
| `firmware/esp32s3/` | PlatformIO-Projekt ESP32-S3 (angelegt: Bring-up-Konsole mit I2C-Distanzbefehlen; `uv tool install platformio esptool`) |
| `firmware/stm32-a121/` | Eigene STM32-Firmware für das A121-Board (Acconeer-SDK) |
| `android/` | Android-App (Kotlin/Compose) |
| `hardware/` | Verdrahtung, Lochraster-Layout, später KiCad |

Messdaten/Aufnahmen gehören **nicht** ins Projekt, sondern nach `~/Data/radar-sensor/`.
