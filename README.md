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
  (Smartwatch/Pulsoximeter/EKG) keine Aussage über Puls-Genauigkeit (`docs/EXPERIMENTS.md`).

**Status 2026-11-01:** Android-App (`android/`, Debug-Build) liest den ESP32-S3 per USB-C am Galaxy S26 Ultra:
Live-Ansichten, Konsole, Aufnahme im PC-Format im Hintergrund (31,7 min bei ausgeschaltetem Bildschirm ohne
Frameverlust), Neuverbinden nach Abziehen, DSP-Nachrechnung per NDK. ESP32-Firmware `2026-11-01-app1` (Protokoll
`proto=1`, nur ergänzt). Stand und Ergebnisse: `docs/ANDROID_APP.md`.

**Audit Vitalerkennung (ab 2026-11-01, `docs/VITAL_AUDIT.md`):** Die ESP32-Kette für `iq3` steht in
`docs/HARDWARE.md` (Befehl `iq3`) und im Kopf von `firmware/esp32s3/src/a121_esp_dsp.h`. Die Verfahren der Zeile
„Signalverarbeitung (`host/`)" oben (DACM, Hilbert-Projektion, HPS) gehören zu den PC-Skripten, nicht zur Firmware;
diese Skripte sind seit V5.3 Archiv.
Prüfstand `host/vital_sim.py bench --check` (nur Simulation), Auswertung gegen Brustgurt `host/vital_eval.py`.
Software-Teile des Plans V0–V5 umgesetzt und am PC geprüft, **nicht am Gerät** (Firmware nicht geflasht, App nicht
gebaut); offen sind Gurt-Aufnahmen Tag A/B, die Messungen V4 und alle Gerätetests (Abschnitt 6 im Audit).

## Dokumente

- `docs/CONCEPT.md` — Ziele, Realitätscheck, Architektur, Phasen, Einkauf
- `docs/EXPERIMENTS.md` — Fähigkeiten-Matrix (Phase 1), Ergebnisse nur nach Messung
- `docs/HARDWARE.md` — Boards, Pinouts, Verdrahtung, Strom
- `docs/ANDROID_APP.md` — Android-App: Entscheidungen, Datenquellen, Phasen A0–A5 mit Ergebnissen, Bedienung
- `docs/VITAL_AUDIT.md` — Audit der Vitalerkennung (Befunde, Prüfstand, Umsetzungsplan V0–V5)

## Geplante Struktur (Ordner erst anlegen, wenn die Phase beginnt)

| Ordner | Inhalt |
|---|---|
| `host/` | Python: `a121_esp_ultimate.py` (iq3-Aufnahme, verify-cpp, plot, replay-cpp), `vital_eval.py` (Auswertung gegen Gurt, Metronom, Marker; Header-Varianten mit `--replay`), `vital_sim.py` (synthetischer Prüfstand für die Vital-DSP), `esp_record.py` (Aufnahme/Prüfung im App-Format), `esp_tcp_bridge.py` (ESP32 am PC → App über Tailscale), `rezoom_check.py`, `noise_floor.py` (Weg-Rauschen statisch, V4.1; Aufnahme über `bridge`), `a121_zielmessung.py`; `dsp_replay/` (C++-Nachrechnung und Live-Pfad für PC und App), `fw_stub/` (Stub-Header: Firmware am PC übersetzen, `host/tests/test_firmware_pc.py`; kein Ersatz für `pio run`). Archiv (V5.3, eigene ältere DSP-Ketten, nicht mehr gepflegt; neue Auswertungen nur über `vital_eval.py` und den Firmware-Header): `a121_ultimate_vital.py`, `a121_vitalaufnahme.py`, `a121_anhalteanalyse.py`, `a121_livemonitor.py`, `a121_esp_vital.py`, `a121_esp_auto.py` (Setup: `uv venv --python 3.12 .venv && uv pip install --python .venv/bin/python -r host/requirements.txt`) |
| `firmware/esp32s3/` | PlatformIO-Projekt ESP32-S3 (angelegt: Bring-up-Konsole mit I2C-Distanzbefehlen; `uv tool install platformio esptool`) |
| `firmware/stm32-a121/` | Eigene STM32-Firmware für das A121-Board (Acconeer-SDK) |
| `android/` | Android-App (Kotlin/Compose, angelegt; `./gradlew :app:testDebugUnitTest :app:assembleDebug`) |
| `hardware/` | Verdrahtung, Lochraster-Layout, später KiCad |

Messdaten/Aufnahmen gehören **nicht** ins Projekt, sondern nach `~/Data/radar-sensor/`.

## Lizenz

Eigener Code: MIT (siehe `LICENSE`). Acconeer-SDK, Acconeer- und Waveshare-Firmware sowie deren Demos sind nicht
Teil des Repos und unterliegen den Lizenzen der Hersteller; die Skripte laden oder enthalten sie nicht.
