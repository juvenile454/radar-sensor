# Hardware — Boards, Pins, Verdrahtung

Stand 2026-10-27. Pins vor dem ersten Einschalten am echten Board prüfen.

## Vorhandene Teile

| Teil | Details | Zustand |
|---|---|---|
| ESP32-S3-DevKitC-1 (ggf. Klon), Modul WROOM-1 **N16R8** | 16 MB Flash, 8 MB Octal-PSRAM, 2× USB-C (nativ + USB-UART), RGB-LED, BOOT/RST | Stiftleisten gelötet |
| Raspberry Pi Pico (RP2040, Version 2020) | Micro-USB, kein Funk | Stiftleisten fehlen; **zurückgestellt** |
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

### Bring-up am Linux-PC (geprüft 2026-10-30)

- USB-ID `1a86:55d2` „USB Dual_Serial“, Seriennr. `5C75025105`, Treiber `cdc_acm` (kein Zusatztreiber).
- `/dev/serial/by-id/usb-1a86_USB_Dual_Serial_5C75025105-if00` (ttyACM0) = Kanal A, Exploration Server
  antwortet; `-if02` (ttyACM1) = Kanal B (Debug).
- Werksfirmware: Exploration Server, `rss_version a121-v1.12.0`, `hardware_name xm125`, max. 2 000 000 Baud.
- Zugriff braucht Gruppe `dialout` (der Benutzer ist Mitglied; greift nach Neuanmeldung, bis dahin `sg dialout -c …`).
- Python-Umgebung: `.venv` (Python 3.12, `acconeer-exptool[app]` 7.18.2).
- Sparse IQ (Profil 3, 40 Punkte, 8 Sweeps, HWAAS 16): ~117 Frames/s. Distance-Detektor läuft.
- Reproduzierbar: Verbindung direkt nach einer beendeten Session scheitert mit `LinkError: recv timeout`,
  der nächste Versuch läuft (Ursache offen; Verdacht Baudraten-Umschaltung).

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

Aus der Schaltplan-Netzliste (H6, gelesen 2026-10-30, ungeprüft am Board): 1 GPIO1, 2 GPIO0, 3 WAKE_UP,
4 MCU_INT, 5 SCL, 6 SDA, 7 DEBUG_TX, 8 DEBUG_RX, 9 GND, 10 5V — passt zur Tabelle oben (umgekehrt).
Weitere Schaltplan-Fakten: I2C-Pull-ups 4,7 kΩ nach 3,3 V auf dem A121-Board (R13/R14);
USB-VBUS speist das 5V-Netz über Schottky-Diode D3 (MBR230) → 5 V vom Stecker fließt nicht zurück in den PC-USB.

## Verdrahtung ESP32-S3 ↔ A121 (Vorschlag)

I2C/INT/WAKE wie in den Waveshare-ESP32-S3-Demos (geprüft im Demo-Code 2026-10-30: SDA 15, SCL 16,
WAKEUP 17, BUSY 18, Adresse 0x52, 400 kHz). MCU_INT high = STM32 bereit.

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

Adernfarben des mitgelieferten MX1.25→Dupont-Kabels (aus Foto 2026-10-30, TXD/SDA am Board korrigiert;
Multimeter-Bestätigung 5V/GND offen; SCL grün auf dem Foto nicht ganz eindeutig):

| A121 | Ader | ESP32-S3 (DevKitC-1, linke Leiste J1) |
|---|---|---|
| 5V | rot | 5V |
| GND | schwarz | GND |
| RXD | blau | GPIO4 |
| TXD | gelb | GPIO5 |
| SDA | orange | GPIO15 |
| SCL | grün | GPIO16 |
| MCU_INT | weiß | GPIO18 |
| WAKE_UP | lila | GPIO17 |
| GPIO0 | braun | frei, Ende isolieren |
| GPIO1 | grau | frei, Ende isolieren |

### Bring-up-Ablauf (Reihenfolge einhalten)

Firmware: `firmware/esp32s3` (PlatformIO, `pio run`), Konsole über nativen USB-Port, Befehle `help`.
Flashen: `pio run -t upload --upload-port <by-id-Port>`. Danach bleibt der S3 im Download-Modus
(PIO-esptool 4.5 kann am USB-JTAG nicht sauber neu starten) → `esptool --chip esp32s3 -p <Port>
--after watchdog-reset read-mac` (esptool ≥ 4.8, `uv tool install esptool`) oder RST drücken.
Erstes Flashen über der Händler-Demo ging nur per BOOT+RST von Hand (Demo nutzt keinen USB-JTAG-Modus).
Konsole mit DTR/RTS unverändert öffnen, sonst Reset in den Download-Modus.

1. **Kabel prüfen:** Liegt der Box ein MX1.25→Dupont-Kabel bei? Dann zuerst ohne Löten auf die
   ESP32-Stiftleisten stecken. Adernfarben notieren.
2. **5V/GND messen:** A121 per USB am PC, Multimeter am Kabelende: 5V-Ader gegen GND ≈ 4,5–5 V
   (nach Diode D3). Danach USB wieder ab. Erst dann Farben → Signale festschreiben.
3. **ESP32 allein:** nativer USB-Port an den PC, Port identifizieren (`ls -l /dev/serial/by-id`),
   Bring-up-Firmware flashen, `info` (16 MB Flash, 8 MB PSRAM erwartet), `led 48`/`led 38`.
4. **5V-Pin des ESP32 messen**, während er nur über den nativen Port versorgt wird (Klon-Boards).
5. **Verdrahten – alles stromlos:** Tabelle oben. RXD/TXD kreuzen (A121-RXD ← GPIO4, A121-TXD → GPIO5).
6. **Einschalten nur über den ESP32-USB.** Rote LED am A121 muss leuchten. `scan`: mit Werksfirmware
   (Exploration Server) wird **nichts** unter 0x52 erwartet.
7. **STM32 auf I2C-Firmware flashen** (nur mit Freigabe des Projektinhabers; umkehrbar): A121 per USB an den PC,
   Kanal A, `python -m acconeer.exptool.flash flash XM125 -p <if00-Port> -i vendor/firmware/A121_Firmware/i2c_distance_detector.bin`
   (BOOT-Taste laut Wiki). Zurück: gleiche Zeile mit `acc_exploration_server_a121.bin`.
   Solange I2C-Firmware drauf ist, gehen PC-Messungen mit dem Exploration Tool nicht.
   **Erledigt 2026-10-30:** `i2c_distance_detector.bin` (sha256 `e659242d…44e4f`) mit Freigabe des Projektinhabers geflasht,
   100 % ohne Fehler. DFU-Modus nur von Hand: BOOT halten, RESET tippen, BOOT loslassen
   (Tool erwartet Port als `/dev/ttyACMx`, nicht den by-id-Pfad). Danach RESET für Start.
8. **I2C prüfen:** `scan` → 0x52, `ver` → VERSION/APPLICATION_ID lesbar.
   **Erledigt 2026-10-30:** A121 nur über ESP32-5Vin versorgt, alle Leitungen High (`probe`),
   `scan` → genau ein Gerät 0x52; `ver`: VERSION 0x00010C00 (= 1.12.0), APPLICATION_ID 1,
   DETECTOR_STATUS 0x00000007, PROTOCOL_STATUS 0. Vor dem Flashen war SCL low und MCU_INT low (Werksfirmware,
   vermutlich Phantomspeisung über GPIO4) – mit I2C-Firmware und TX hochohmig behoben.
9. **UART (optional):** mit einer `example_*`-UART-Firmware gibt der STM32 Text auf dem Debug-UART aus;
   `uart` im ESP32 reicht ihn an USB durch.

Downloads (nicht im Repo, liegen in `vendor/`, gitignored), geholt 2026-10-30:
- `A121_Firmware.zip` sha256 `fea5bcd0b7d39149c0cc02bf82303abd5cd6d92f209643ba426b18db58da356a`
  (https://files.waveshare.com/wiki/A121_Range_Sensor/A121_Firmware.zip)
- `A121_Range_Sensor_Demo.zip` sha256 `481b587bf8e388d36846ab44a4603a2a2cd60a3a35245d674eaaaf5b1f66e144`
  (https://files.waveshare.com/wiki/A121_Range_Sensor/A121_Range_Sensor_Demo.zip)

ESP32-S3-Pins meiden: GPIO19/20 (nativer USB), GPIO35–37 (Octal-PSRAM), GPIO43/44 (UART0 zur
USB-UART-Bridge), Strapping-Pins 0/3/45/46. RGB-LED je nach Board-Revision GPIO48 oder 38.

## Stromversorgung am Handy

- Handy → nativer USB-C-Port des ESP32-S3 → 5V-Pin → A121-Board.
- Bedarf grob: A121 ≤ 80 mA + ESP32-S3 ~100–250 mA (ohne Funk weniger) — für Handy-OTG unkritisch.
- Bei Klon-Boards prüfen, ob der 5V-Pin auch bei Versorgung über den nativen Port Spannung führt (Multimeter).
- **Geprüft 2026-10-30 (Board des Projektinhabers):** Pin beschriftet „5Vin“; an beiden USB-Ports nur 1,2–1,3 V
  (Leckspannung) statt 5 V → reiner Eingang, vermutlich über Diode. A121 bleibt so ohne Strom (PWR-LED aus).
  Board ist ein DevKitC-1-Nachbau (kein Espressif-Logo, Modul „S3-N16R8“, Foto 2026-10-30) mit **offener
  Lötbrücke „IN-OUT“** unten links neben der RGB-LED. Geschlossen überbrückt sie laut Community-Quellen
  (YD-ESP32-S3-Doku) eine Diode, dann liegt USB-VBUS auf 5Vin – danach nie gleichzeitig externe 5 V und USB.
  Alternative: Original Espressif ESP32-S3-DevKitC-1-N16R8 (5V0 = USB-VBUS über Diode, ~4,7 V).
  **IN-OUT am 2026-10-30 geschlossen (selbst gelötet):** 5Vin–GND = 4,74 V bei USB-Versorgung.
  Regeln seitdem: nie USB + externe 5 V gleichzeitig; bei gesteckter roter Ader A121 nicht zusätzlich per USB-C.
  Übergangsweise am PC: A121 per eigenem USB-C versorgen, rote Ader ab, Signale + GND bleiben.
- Motoren (späterer Scanner) immer separat versorgen; nur GND gemeinsam.

## Pico (zurückgestellt)

Nicht Teil des aktuellen A121-Fokus. Mögliche spätere Rollen: SWD-Debug-Probe für den STM32
(„debugprobe“-Firmware an SWDIO/SWCLK/GND-Pads) oder Schrittmotorsteuerung für den SAR-Scanner.
Flashen des STM32 geht ohne Pico über USB (BOOT-Taste, Kanal A).
