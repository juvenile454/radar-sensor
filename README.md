# radar-sensor

60-GHz-Radar (Acconeer A121 auf Waveshare-Board) + ESP32-S3 + Android-App am Galaxy S26 Ultra.
Lernprojekt: löten, Mikrocontroller programmieren, Signalverarbeitung (Atmung, Puls, Materialtests).
Aktueller Fokus: den A121 vollständig ausreizen; Pico und SAR-Scanner zurückgestellt.

**Status 2026-10-30:** A121 läuft am Linux-PC (Exploration Server ab Werk), erste Referenzmessungen
in `docs/EXPERIMENTE.md`. Noch keine eigene Firmware, keine App.

## Dokumente

- `docs/KONZEPT.md` — Ziele, Realitätscheck, Architektur, Phasen, Einkauf
- `docs/EXPERIMENTE.md` — Fähigkeiten-Matrix (Phase 1), Ergebnisse nur nach Messung
- `docs/HARDWARE.md` — Boards, Pinouts, Verdrahtung, Strom

## Geplante Struktur (Ordner erst anlegen, wenn die Phase beginnt)

| Ordner | Inhalt |
|---|---|
| `host/` | Python: Exploration Tool, Aufnahme-/Analyse-Skripte (angelegt; Setup: `uv venv --python 3.12 .venv && uv pip install --python .venv/bin/python -r host/requirements.txt`) |
| `firmware/esp32s3/` | PlatformIO-Projekt ESP32-S3 (angelegt: Bring-up-Konsole; `pio` via `uv tool install platformio`) |
| `firmware/stm32-a121/` | Eigene STM32-Firmware für das A121-Board (Acconeer-SDK) |
| `android/` | Android-App (Kotlin/Compose) |
| `hardware/` | Verdrahtung, Lochraster-Layout, später KiCad |

Messdaten/Aufnahmen gehören **nicht** ins Projekt, sondern nach `~/Data/radar-sensor/`.
