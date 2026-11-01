# Konzept — 60-GHz-Radar am Android-Handy

Stand 2026-11-01. Status: **Phase 0 erledigt; Phase 1 und 2 weit ausgebaut (3-Subsweep Sparse-IQ mit On-Chip-Loopback @ 60 Hz und gleitende 7-s-Hilbert-Atmungsprojektion laufen auf PC und ESP32; Vitalwerte ohne Referenzmessung, Smartwatch/Oximeter noch offen). Phase 3 & 4: Der ESP32-S3 spricht über die SDA/SCL-Adern (`GPIO15`/`GPIO16`) mit dem vollen Exploration Server (eigener Build aus dem Acconeer-SDK v1.13.0 auf LPUART1, `firmware/stm32l431/`). Am Gerät geprüft am 2026-11-01 mit dem A121 am PC-USB: `srv_info`, `stm_reset`, `iq3` (58 Hz), `doppler` (31 Hz) und `bridge` mit exptool (2 Mbaud). Server gegen UART-Fehler gehärtet (Neustart statt taub). Mobil (A121 über die rote 5V-Ader) ebenso geprüft; Server-Link 2 Mbaud, netto ≈ 195 kB/s. Offen: Betrieb am Handy, automatisches Neuverbinden während einer Messung und die Rechenpausen der On-Device-DSP (s. `docs/HARDWARE.md`).**
Fokus: **den A121 vollständig ausreizen** — mit ESP32-S3, ohne Pico.

## Ziel

Lern- und Bastelprojekt: Radarsensor + Mikrocontroller selbst verdrahten/löten und
programmieren, Daten per USB-C live auf dem Galaxy S26 Ultra anzeigen. Erst alles
herausholen, was der A121 allein kann; Erweiterungen danach.

## Realitätscheck

| Ziel | Erwartung | Begründung |
|---|---|---|
| Entfernung, Präsenz, Bewegung | ✅ | Fertige Detektoren in der A121-Firmware |
| Atemfrequenz | ✅ | Acconeer-Referenz-App `breathing` |
| Puls | ⚠️ experimentell | Herzschlag bewegt die Brust nur ~0,1–0,5 mm, Atmung mehrere mm. Phasenauswertung der Rohdaten (Sparse IQ) nötig; nur ruhig sitzend, ~30–80 cm, Sensor auf Brust, ideal mit hyperbolischer Linse. Eigener Algorithmus, Validierung gegen Smartwatch. |
| Menschliche Umrisse live | ❌ | A121 = 1 Sende-/1 Empfangskanal → nur Entfernung, keine Richtung. Selbst 3–4 Rx-Kanäle liefern nur grobe Punkte. |
| Bild (statisch) | ✅ später | Sensor auf Linearachse → synthetische Apertur (SAR). Nur unbewegte Szenen; zurückgestellt (Mechanik). |
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
  Linse bündelt → besser für Puls; ohne Linse breiter Strahl → Raumabdeckung.
- Kein SMD-Löten nötig; Lötkolben reicht.

Verworfen/zurückgestellt: 24-GHz-Module (grobe Auflösung, kaum Rohdaten), HLK-LD6002
(fertige Pulswerte, keine Rohdaten), Infineon BGT60TR13C / TI IWR6843 (Winkel möglich, teurer).

## Architektur

```
A121-Board ──MX1.25: UART + I2C──► ESP32-S3 ──USB-C (native, CDC)──► Galaxy S26 Ultra (App)
     │
     └─USB-C (CH342) ► Linux-PC: Exploration Tool, Aufnahmen, Algorithmen
```

| Teil | Aufgabe |
|---|---|
| STM32 auf A121-Board | Zuerst Waveshare-/Acconeer-Firmware; später eigene „IQ-Streamer“-Firmware (C, Acconeer-SDK, kostenloses Konto nötig), binär über den Stecker-UART |
| ESP32-S3-DevKitC-1 N16R8 (Nachbau, IN-OUT-Brücke geschlossen) | Einziger Hauptcontroller: Sensor steuern, Frames puffern/prüfen, an Handy weiterreichen; später Vorverarbeitung, BLE |
| Android-App | Kotlin/Compose, `usb-serial-for-android`, Live-Plots: Entfernung, Amplitude/Phase, Atmung, Puls |
| Linux-PC (Python) | Exploration Tool (`acconeer-exptool`), Aufnahmen, Algorithmus-Entwicklung, bevor etwas aufs Handy portiert wird |

Datenrate grob: 40 Messpunkte × 4 Byte × 100 Hz ≈ 16 kB/s — passt in 921 600 Baud (~92 kB/s).

## Phasen und Abschlusskriterien

| Phase | Inhalt | Fertig, wenn |
|---|---|---|
| 0 PC-Bring-up | Board per USB an Linux, Exploration Tool, erste Live-Daten | Board verbunden, Sparse IQ + Distance live |
| 1 Fähigkeiten-Matrix | Alle Algorithmen und Grenzen systematisch testen (`docs/EXPERIMENTE.md`) | Jede Zeile mit Messwert oder begründet „geht nicht“ |
| 2 Puls offline | Sparse-IQ-/Phasen-Aufnahmen (sitzend, 40/60/80 cm, mit/ohne Linse) + Smartwatch-Referenz; Python-Algorithmus | Abweichung zur Uhr gemessen und ehrlich dokumentiert |
| 3 ESP32-Kette | ESP32-S3 ↔ A121 verlöten (Lochraster); I2C-Detektoren auf ESP32; Android-App-Gerüst; Direkttest A121-USB am Handy | App zeigt Live-Werte vom ESP32 (Entfernung, Präsenz, Atmung) |
| 4 Rohdaten zum Handy | Eigene STM32-IQ-Streamer-Firmware → ESP32-Bridge → App; Puls-Algorithmus nach Kotlin portiert | App zeigt Rohdaten-Plots + Atmung (+ Puls, falls Phase 2 trägt) |

## Zurückgestellt

- **Pico:** vorerst nicht im Projekt (kein Bedarf für den A121-Fokus).
- **SAR-Scanner:** Linearachse + Schrittmotor + Bildrekonstruktion. Erst nach Phase 4;
  Motorsteuerung dann über ESP32-S3 oder Pico entscheiden. Motor nie aus dem Handy versorgen.
- BLE statt Kabel, Akku, Gehäuse, eigene Platine (KiCad), zweiter Sensor (Bilateration).

## Einkauf

| Was | Wann | Preis |
|---|---|---|
| Waveshare A121 Range Sensor Box | geliefert, in Betrieb seit 2026-10-30 | 48,99 € |
| USB-C↔USB-C-Datenkabel (Handy↔ESP32) | jetzt, falls nicht vorhanden | — |
| Testmaterial: Sperrholz, Gipskartonrest, Pappe, Glasplatte, Metallblech, Maßband | Phase 1 | meist vorhanden |
| Lochrasterplatine, Buchsenleisten 2,54 mm | Phase 3, für die feste Verdrahtung | wenige € |
| Multimeter | vorhanden (Benning MM 3, ohne Autorange) | — |

## Grenzen und Regeln

- Funkmodule unverändert betreiben (keine Antennen-/Leistungsänderung).
- „Durch Wände schauen“ nur in eigenen Räumen oder mit Einwilligung.
- Kein Medizinprodukt; Pulswerte sind Experimentdaten.
