# Hardware — Boards, Pins, Verdrahtung

Stand 2026-10-27. Pins vor dem ersten Einschalten am echten Board prüfen.

## Vorhandene Teile

| Teil | Details | Zustand |
|---|---|---|
| ESP32-S3-DevKitC-1 (ggf. Klon), Modul WROOM-1 **N16R8** | 16 MB Flash, 8 MB Octal-PSRAM, 2× USB-C (nativ + USB-UART), RGB-LED, BOOT/RST | Stiftleisten gelötet |
| Raspberry Pi Pico (RP2040, Version 2020) | Micro-USB, kein Funk | **Stiftleisten fehlen** → erste Lötaufgabe |
| Galaxy S26 Ultra | USB-C, USB-Host (OTG) | — |
| Werkzeug | Lötkolben (kein Heißluft/Reflow) | — |

Nicht verwenden: ESP32-S3-CAM aus `esp32-laser-triangulation` (gehört dem anderen Projekt).

## Waveshare A121 Range Sensor

Quelle: Waveshare-Wiki „A121 Range Sensor“, Produktseite, Schaltplan-PDF (gelesen 2026-10-27).

- Versorgung 5 V, max. 80 mA; I/O 3,3 V.
- USB-C → CH342 (USB to 2CH TTL):
  - Kanal A: STM32-Hauptuart — Exploration Tool und Flashen (BOOT-Taste).
  - Kanal B: Debug-UART — Ausgabe der UART-Firmwares; **dieselben Leitungen liegen am Stecker (TXD/RXD)**.
  - Folgerung (laut Schaltplan, ungetestet): Exploration-Server-Rohdaten nur über USB; für Rohdaten zum ESP32 braucht es eigene Firmware auf dem Debug-UART.
- UART-Firmware: 921 600 Baud. I2C-Firmware: Adresse 0x52 (per Lötbrücke 0x51/0x53), 100/400 kHz.
- SWDIO/SWCLK-Pads, BOOT- und RESET-Taste vorhanden.

### MX1.25-10-Pin-Stecker (laut Pinout-Grafik, Reihenfolge von oben)

| # | Signal | Funktion |
|---|---|---|
| 1 | 5V | Versorgung |
| 2 | GND | Masse |
| 3 | RXD | UART RX (in den STM32) |
| 4 | TXD | UART TX (aus dem STM32) |
| 5 | SDA | I2C Daten |
| 6 | SCL | I2C Takt |
| 7 | MCU_INT | I2C-Busy-Signal |
| 8 | WAKE_UP | I2C-Wake-up |
| 9 | GPIO0 | frei |
| 10 | GPIO1 | frei |

Pin-Nummern sind auf der Grafik nicht aufgedruckt; Schaltplan nummeriert umgekehrt (5V = 10).
Vor dem Löten mit Multimeter 5V/GND am Kabel bestätigen.

## Verdrahtung ESP32-S3 ↔ A121 (Vorschlag)

I2C/INT/WAKE wie in den Waveshare-ESP32-S3-Demos, damit diese unverändert laufen.

| A121 | ESP32-S3 | Zweck |
|---|---|---|
| 5V | 5V | Versorgung aus USB |
| GND | GND | — |
| RXD | GPIO4 (UART1 TX) | Befehle → STM32 |
| TXD | GPIO5 (UART1 RX) | Daten ← STM32 |
| SDA | GPIO15 | I2C |
| SCL | GPIO16 | I2C |
| WAKE_UP | GPIO17 | Wake-up |
| MCU_INT | GPIO18 | Busy |
| GPIO0/1 | — | nicht verbunden |

ESP32-S3-Pins meiden: GPIO19/20 (nativer USB), GPIO35–37 (Octal-PSRAM), GPIO43/44 (UART0 zur
USB-UART-Bridge), Strapping-Pins 0/3/45/46. RGB-LED je nach Board-Revision GPIO48 oder 38.

## Stromversorgung am Handy

- Handy → nativer USB-C-Port des ESP32-S3 → 5V-Pin → A121-Board.
- Bedarf grob: A121 ≤ 80 mA + ESP32-S3 ~100–250 mA (ohne Funk weniger) — für Handy-OTG unkritisch.
- Bei Klon-Boards prüfen, ob der 5V-Pin auch bei Versorgung über den nativen Port Spannung führt (Multimeter).
- Motoren (Phase 4) immer separat versorgen; nur GND gemeinsam.

## Pico

- Erst Stiftleisten löten.
- Rolle A (optional): SWD-Debug-Probe (Raspberry-Pi-„debugprobe“-Firmware) für den STM32 → Drähte an SWDIO/SWCLK/GND-Pads.
- Rolle B (Phase 4): Schrittmotor-Takt über PIO, Endschalter, Befehle vom ESP32 oder PC.
