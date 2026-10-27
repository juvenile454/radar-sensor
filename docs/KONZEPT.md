# Konzept — 60-GHz-Radar am Android-Handy

Stand 2026-10-27. Status: **Konzept, nichts bestellt, nichts gebaut.**

## Ziel

Lern- und Bastelprojekt: Radarsensor + Mikrocontroller selbst verdrahten/löten und
programmieren, Daten per USB-C live auf dem Galaxy S26 Ultra anzeigen. Experimente:
Vitalwerte (Atmung, Puls), Entfernung/Bewegung, Materialdurchdringung, Bildgebung.

## Realitätscheck

| Ziel | Erwartung | Begründung |
|---|---|---|
| Entfernung, Präsenz, Bewegung | ✅ | Fertige Detektoren in der A121-Firmware |
| Atemfrequenz | ✅ | Acconeer-Referenz-App `ref_app_breathing` |
| Puls | ⚠️ experimentell | Herzschlag bewegt die Brust nur ~0,1–0,5 mm, Atmung mehrere mm. Phasenauswertung der Rohdaten (Sparse IQ) nötig; nur ruhig sitzend, ~30–80 cm, Sensor auf Brust, ideal mit hyperbolischer Linse. Eigener Algorithmus, Validierung gegen Smartwatch. |
| Menschliche Umrisse live | ❌ | A121 = 1 Sende-/1 Empfangskanal → nur Entfernung, keine Richtung. Selbst 3–4 Rx-Kanäle liefern nur grobe Punkte. |
| Bild (statisch) | ✅ über Umweg | Sensor auf Linearachse, kohärente Aufnahmen → synthetische Apertur (SAR). Nur unbewegte Szenen. |
| Durch Holz/Gipskarton | ⚠️ | Dünnes, trockenes Holz, Gipskarton, Kunststoff, Stoff: teilweise. Ziegel, Beton, Fliesen, Metall, nasses Material: praktisch nein. |
| Android über USB-C | ✅ | Handy als USB-Host, Gerät als serielle Schnittstelle (CDC-ACM), App mit `usb-serial-for-android` |

Physik-Merkzettel: Entfernungsauflösung ≈ c/(2·Bandbreite). 24-GHz-ISM (250 MHz) ≈ 60 cm,
60-GHz-Sensoren wenige cm. Winkel/Richtung nur mit mehreren Antennen oder bewegtem Sensor (SAR).

## Entscheidung Sensor

**Waveshare A121 Range Sensor Box** (Amazon `B0GJBZSY7P`, 48,99 € am 2026-10-27).
Standardversion ohne Halter/Linsen: `B0GJBQ7RQK`, 40,99 €.

- Acconeer A121 (60 GHz, Pulsed Coherent Radar) + STM32L431CBT6 — elektrisch wie XM125,
  im Exploration Tool als „XM125“ flashbar.
- USB-C über CH342 (2 serielle Kanäle), MX1.25-10-Pin-Stecker (UART, I2C, INT, WAKE), 5 V / max. 80 mA.
- Box-Version: Halter + hyperbolische Linse, Fresnel-Linse, flache Abdeckung.
  Linse bündelt → besser für Puls; ohne Linse breiter Strahl → besser für SAR.
- Kein SMD-Löten nötig; Lötkolben reicht für alles im Projekt.

Verworfen/zurückgestellt: 24-GHz-Module (grobe Auflösung, kaum Rohdaten), HLK-LD6002
(fertige Pulswerte, keine Rohdaten), Infineon BGT60TR13C / TI IWR6843 (Winkel möglich, teurer;
Option für später).

## Architektur

```
A121-Board ──MX1.25: UART + I2C──► ESP32-S3 ──USB-C (native, CDC)──► Galaxy S26 Ultra (App)
     │                                  │
     └─USB-C (CH342) ► Linux-PC          └─(Phase 4) Pico ──► Schrittmotor (Scanner)
       Exploration Tool / Aufnahmen
```

| Teil | Aufgabe |
|---|---|
| STM32 auf A121-Board | Zuerst Waveshare-/Acconeer-Firmware; später eigene „IQ-Streamer“-Firmware (C, Acconeer-SDK, kostenloses Konto nötig), binär über den Stecker-UART |
| ESP32-S3-DevKitC-1 N16R8 | Hauptcontroller: Sensor steuern, Frames puffern/prüfen, an Handy weiterreichen; später Vorverarbeitung, BLE |
| Raspberry Pi Pico (RP2040) | Optional SWD-Debug-Probe für den STM32; Phase 4 Schrittmotorsteuerung (PIO) |
| Android-App | Kotlin/Compose, `usb-serial-for-android`, Live-Plots: Entfernung, Amplitude/Phase, Atmung, Puls |
| Linux-PC (Python) | Exploration Tool, Aufnahmen, Algorithmus-Entwicklung (Puls, SAR) bevor etwas aufs Handy portiert wird |

Datenrate grob: 40 Messpunkte × 4 Byte × 100 Hz ≈ 16 kB/s — passt in 921 600 Baud (~92 kB/s).

## Phasen und Abschlusskriterien

| Phase | Inhalt | Fertig, wenn |
|---|---|---|
| 0 PC-Bring-up | Board per USB an Linux, Exploration Tool, Atmungs-App | Live-Daten im Tool; Atemfrequenz plausibel |
| 1 Puls offline | Sparse-IQ-Aufnahmen (sitzend, 40/60/80 cm, mit/ohne Linse) + Smartwatch-Referenz; Python-Algorithmus | Abweichung zur Uhr gemessen und ehrlich dokumentiert |
| 2 Hardware-Grundlage | Pico-Stiftleisten löten; ESP32-S3 ↔ A121 auf Lochraster; I2C-Atmungsdemo auf ESP32; Direkttest A121-USB am Handy | Atemwerte vom ESP32 am PC; Handy erkennt Gerät |
| 3 Eigene Kette | STM32-IQ-Streamer → ESP32-Bridge → Android-App live | App zeigt Live-Daten + Atmung (+ Puls, falls Phase 1 trägt) |
| 4 Scanner | Linearachse, Schrittmotor über Pico, SAR-Rekonstruktion in Python | Bild eines bekannten Testobjekts (z. B. Metallstab hinter Gipskarton) |
| Optional | BLE statt Kabel, Akku, Gehäuse, eigene Platine (KiCad) | — |

## Einkauf

| Was | Wann | Preis |
|---|---|---|
| Waveshare A121 Range Sensor Box | jetzt | 48,99 € |
| USB-C↔USB-C-Datenkabel (Handy↔ESP32), Micro-USB-Kabel (Pico) | jetzt, falls nicht vorhanden | — |
| Lochrasterplatine, Stift-/Buchsenleisten 2,54 mm, Dupont-Kabel | Phase 2 | wenige € |
| Multimeter | Phase 2, falls nicht vorhanden | — |
| NEMA17, TMC2209, Linearachse, 12-V-Netzteil, Endschalter | Phase 4 | grob 40–80 €, ungeprüft |

## Grenzen und Regeln

- Funkmodule unverändert betreiben (keine Antennen-/Leistungsänderung).
- „Durch Wände schauen“ nur in eigenen Räumen oder mit Einwilligung.
- Kein Medizinprodukt; Pulswerte sind Experimentdaten.
- Schrittmotor nie aus dem Handy versorgen.
