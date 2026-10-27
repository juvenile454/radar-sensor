# Radar_sensor

60-GHz-Radar (Acconeer A121 auf Waveshare-Board) + ESP32-S3 + Android-App am Galaxy S26 Ultra.
Lernprojekt: löten, Mikrocontroller programmieren, Signalverarbeitung (Atmung, Puls, SAR-Bildgebung).

**Status 2026-10-27:** Konzeptphase. Nichts bestellt, keine Firmware, keine App.

## Dokumente

- `docs/KONZEPT.md` — Ziele, Realitätscheck, Architektur, Phasen, Einkauf
- `docs/HARDWARE.md` — Boards, Pinouts, Verdrahtung, Strom

## Geplante Struktur (Ordner erst anlegen, wenn die Phase beginnt)

| Ordner | Inhalt |
|---|---|
| `host/` | Python: Exploration Tool, Aufnahme-/Analyse-Skripte |
| `firmware/esp32s3/` | PlatformIO-Projekt ESP32-S3 |
| `firmware/stm32-a121/` | Eigene STM32-Firmware für das A121-Board (Acconeer-SDK) |
| `firmware/pico/` | Pico: Debug-Probe-Notizen, Schrittmotorsteuerung |
| `android/` | Android-App (Kotlin/Compose) |
| `hardware/` | Verdrahtung, Lochraster-Layout, später KiCad |

Messdaten/Aufnahmen gehören **nicht** ins Projekt, sondern nach `~/Data/radar-sensor/`.
