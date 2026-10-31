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
**WAKE_UP und MCU_INT hängen am CH342** (Schaltplan-Ausschnitt vergrößert gelesen 2026-10-31, Verlauf über
PDF-Textkoordinaten gegengeprüft; Bauteile nicht nachgemessen): WAKE_UP ← DTR0 (Pin 23, Ausgang),
MCU_INT → DSR0 (Pin 22, Eingang); STM32: WAKE_UP = PA11, MCU_INT = PB9. Der CH342 hängt mit VDD5/VBUS am
5V-Netz und treibt WAKE_UP deshalb auch ohne USB aktiv. Am Board bestätigt: GPIO17 lässt sich gegen die Leitung
weder auf HIGH (2026-10-31 früh) noch auf LOW (nach Neustart) ziehen. Folge: Der ESP32 steuert WAKE_UP nicht; die
Bring-up-Firmware liest GPIO17 nur (`wake` treibt nur auf Befehl und gibt bei Widerstand sofort frei).
Schaltplan: https://files.waveshare.com/wiki/A121_Range_Sensor/A121_Range_Sensor_schematic.pdf
(sha256 `38e88c1ef357ba4f1ce77ac87b70f4a609cc197ace7a70b9a56eb02349627440`, nicht im Repo).

## Verdrahtung ESP32-S3 ↔ A121 (gesteckt und geprüft 2026-10-30)

I2C/INT/WAKE wie in den Waveshare-ESP32-S3-Demos (geprüft im Demo-Code 2026-10-30: SDA 15, SCL 16,
WAKEUP 17, BUSY 18, Adresse 0x52, 400 kHz). MCU_INT high = STM32 bereit.

| A121 | ESP32-S3 | Zweck |
|---|---|---|
| 5V | 5Vin | Versorgung aus USB (Nachbau-Board: nur mit geschlossener IN-OUT-Brücke, siehe unten) |
| GND | GND | — |
| RXD | GPIO4 (UART1 TX) | Befehle → STM32 |
| TXD | GPIO5 (UART1 RX) | Daten ← STM32 |
| SDA | GPIO15 | I2C |
| SCL | GPIO16 | I2C |
| WAKE_UP | GPIO17 | Wake-up – vom CH342 (DTR0) getrieben, ESP32 liest nur |
| MCU_INT | GPIO18 | Busy |
| GPIO0/1 | — | nicht verbunden |

Adernfarben des mitgelieferten MX1.25→Dupont-Kabels (am Board geprüft 2026-10-30; Funktion bestätigt:
PWR-LED an, alle Leitungen High, I2C antwortet):

| A121 | Ader | ESP32-S3 (DevKitC-1, linke Leiste J1) |
|---|---|---|
| 5V | rot | 5Vin |
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
   Kanal A, `python -m acconeer.exptool.flash flash XM125 -p /dev/ttyACMx -i vendor/firmware/A121_Firmware/i2c_distance_detector.bin`
   (`ttyACMx` = Ziel von `…-if00`, by-id-Pfad wird nicht akzeptiert). Zurück: gleiche Zeile mit `acc_exploration_server_a121.bin`.
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
   `uart` im ESP32 reicht ihn an USB durch. Auch mit `i2c_distance_detector` nutzbar: Kommando 32
   (`w 0x100 32`) schaltet Logs ein, 34 gibt die Konfiguration aus.
10. **Distanzmessung über I2C** – siehe nächster Abschnitt. **Erledigt 2026-10-31** (Wand 570 mm erkannt).

### I2C-Distanzdetektor am ESP32 (Stand 2026-10-31)

Konsole (`help`): `info`, `bus`, `probe`, `scan`, `ver`, `cfg [start end]` (mm), `dist [n]`,
`r <reg>`, `w <reg> <wert>`, `wake 0|1|z`, `uart`, `led 48|38`.
Register/Kommandos wie Waveshare-Demo (`…/ESP32S3/Arduino/A121_Distance_Detector`): START 0x40, END 0x41,
THRESHOLD_METHOD 0x46 (1 fest/Amplitude, 2 aufgezeichnet, 3 CFAR), PEAK_SORTING 0x47 (1 nächstes, 2 stärkstes),
FIXED_AMPLITUDE_THRESHOLD 0x49 (×1000), THRESHOLD_SENSITIVITY 0x4A (×1000), COMMAND 0x100
(1 Konfig+Kalibrierung, 2 Messen, 32/33 UART-Logs an/aus, 34 Konfig loggen, 1381192737 Modul-Reset).

Geprüft:
- `cfg` → DETECTOR_STATUS 0x3FF (alle OK-Bits), Kalibrierung 0,5–0,7 s; Messung ~25 ms; Temperatur 12–14 °C.
- **Konfiguration greift nur einmal nach Reset.** Spätere APPLY-Kommandos werden ohne Fehlermeldung ignoriert
  (Status sofort 0x3FF, Messung unverändert). Ablauf daher immer: `w 0x100 1381192737` → Register setzen → `cfg`.
- Nach einem Kommando ≥ 10 ms warten, bevor DETECTOR_STATUS gelesen wird (sonst alter Status, BUSY noch nicht gesetzt).
- ESP32-Reset setzt den A121 nicht zurück (Register bleiben).
- Standardwerte nach Modul-Reset: Profil max. 5, CFAR, Sensitivität 0,5, Signalqualität 15, Leckage-Kompensation an,
  Sortierung „stärkstes“.

**Wandtest 2026-10-31 (gelöst):** A121 nur über ESP32-5Vin versorgt, Backsteinwand 570 mm (Maßband),
hyperbolische Linse, Ablauf Modul-Reset → `cfg 300 1000` (CFAR, sonst Standard) → `dist 50`:
**50/50 Messungen mit genau einem Ziel, Median 575 mm, σ 1,3 mm, Spanne 572–578 mm**, Stärke 2,3–3,1 dB,
Messung ~18 ms. (+5 mm zum Maßband, +16 mm zur PC-Referenz 0,5585 m – andere Detektor-Konfiguration,
nicht weiter bewertet.) Log: `~/Data/radar-sensor/2026-10-31_105545_esp_i2c_wand570.log`.
Detektor-Konfiguration laut Debug-UART (`w 0x100 34`): 2 Subsweeps, Profil 1 (HWAAS 2) + Profil 3 (HWAAS 1),
Startpunkt 68 bzw. 120. Versorgung über ESP32 und I2C-Kette funktionieren damit für ein starkes Ziel.

**Hänger beim Einschalten 2026-10-31:** Nach dem Einstecken des ESP32 antwortete der A121
nicht: `scan` leer, MCU_INT und WAKE_UP hart LOW (ESP32 treibt GPIO17 HIGH, liest aber 0; ohne lila Ader ist
GPIO17 einwandfrei), RESET-Taste half nicht. Nach frischem Einschalten (erst per A121-USB, dann wieder über
ESP32-5Vin) lief alles normal, `probe` → alle sechs Leitungen HIGH.
Wahrscheinliche Ursache laut Schaltplan (s. o.): Der CH342 hielt WAKE_UP über DTR0 auf LOW → STM32 schläft, RESET
ändert daran nichts. Warum DTR0 nach diesem einen Einschalten LOW war, ist offen. Die lockere grüne Ader (SCL)
zur selben Zeit ist als Ursache weniger wahrscheinlich, aber nicht ausgeschlossen.
Bis zur Firmware-Änderung am 2026-10-31 trieben `ver`/`cfg` GPIO17 auf HIGH – an diesem Morgen also gegen
den CH342 (ca. 20 min, beide Seiten 3,3-V-Push-Pull). Seitdem treibt die Firmware WAKE_UP nicht mehr; `ver`/`cfg`
melden ein LOW nur noch.
Regel: Nach Umstecken einer Ader oder bei `WAKE_UP ist LOW` den A121 stromlos machen und neu einschalten,
dann `probe` (alle HIGH) und `ver` prüfen, bevor gemessen wird. Hilft das nicht: A121 kurz per USB an den PC
(rote Ader vorher ab).

Tag 1 (2026-10-30): Sensor auf die Testperson gerichtet (Abstand nicht gemessen), Bereich 100–1500 mm:
- CFAR (Standard, auch Sensitivität 0,2): über ~30 Messungen **0 Ziele**.
- Feste Amplitudenschwelle 1,0 / 3,0 / 10,0: 5–10 „Ziele“ pro Messung an wechselnden Abständen; Stärke steigt
  stetig mit der Entfernung (−49 dB bei ~0,13 m … −20 dB bei ~1,1 m), Schwelle 3 vs. 10 ohne Unterschied →
  sieht nach Rauschen aus, kein stabiles Echo.
- Am PC (Exploration Server) fand derselbe Sensor die Wand sofort. Der Wandtest 2026-10-31 (oben) zeigt:
  Kette und Versorgung über ESP32 funktionieren; offen bleibt nur, warum die Testperson als Ziel nicht erkannt wurde.

**Achtung:** Der A121 hat zurzeit `i2c_distance_detector` drauf → Exploration Tool, `host/a121_*.py` und
Live-Monitor funktionieren erst nach Zurückflashen (`acc_exploration_server_a121.bin`, Schritt 7).

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
