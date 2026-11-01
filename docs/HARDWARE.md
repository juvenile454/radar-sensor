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
- **Ursache `LinkError: recv timeout` beim zweiten Verbinden geklärt und behoben (2026-10-31):**
  `ExplorationClient._update_baudrate` in `acconeer-exptool` schaltet den STM32 nach dem Öffnen von 115 200 auf
  2 000 000 Baud, setzt ihn beim `close()` aber nicht zurück. Beim nächsten `Client.open()` öffnet `ExploreSerialLink`
  wieder mit 115 200 Baud und sendet ein UART-`send_break()`, das über den CH342 (`cdc_acm`) beim ersten Versuch ins
  Timeout läuft. Lösung in `host/a121_vitalaufnahme.py` und `host/a121_livemonitor.py`: `override_baudrate=921600`.

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
**WAKE_UP und MCU_INT hängen am CH342** (2026-10-31 aus den Vektorpfaden des Schaltplan-PDFs nachgezogen;
Platine nicht nachgemessen): Netz WAKE_UP = CH342 DTR0 (Pin 23, Ausgang) + Stecker Pin 3 + STM32 PA11;
Netz MCU_INT = CH342 DSR0 (Pin 22, Eingang) + Stecker Pin 4 + STM32 PB9. Direkte Leitungen, keine
Serienwiderstände, keine Pull-ups/-downs. CH342: VIO = 3V3 (Pegel 3,3 V), VDD5 **und** VBUS-Erkennung am 5V-Netz
→ der CH342 läuft immer mit, auch ohne USB, und treibt WAKE_UP aktiv. Stecker-Pins 1/2 = MISC_GPIO1 (PA4) /
MISC_GPIO0 (PB1); NRESET und BOOT0 liegen nicht am Stecker. Am Board bestätigt: GPIO17 lässt sich gegen die Leitung
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

Konsole (`help`): `info`, `bus`, `probe`, `scan`, `ver`, `cfg [start end]` (mm), `vital [start end]` (mm, Standard 450–850),
`zoom a b [p s t]`, `auto [sek]` (vollautomatischer mobiler Modus: `WIDE_SEARCH 150–2500 mm` → `TARGET_LOCK` → `VITAL_ZOOM` mit Auto-Rezoom),
`dist [n]` (bis 5000), `r <reg>`, `w <reg> <wert>`, `wake 0|1|z`, `uart`, `led 48|38`,
`link [i2c|srv]` (seit 2026-10-31 abends: I2C-Befehle nur bei `link i2c`, Server-Befehle siehe unten).
Register/Kommandos wie Waveshare-Demo (`…/ESP32S3/Arduino/A121_Distance_Detector`): START 0x40, END 0x41,
MAX_STEP_LENGTH 0x42, CLOSE_RANGE_LEAKAGE 0x43, SIGNAL_QUALITY 0x44 (×1000), MAX_PROFILE 0x45,
THRESHOLD_METHOD 0x46 (1 fest/Amplitude, 2 aufgezeichnet, 3 CFAR, 4 fest/Stärke), PEAK_SORTING 0x47 (1 nächstes, 2 stärkstes),
FIXED_AMPLITUDE_THRESHOLD 0x49 (×1000), THRESHOLD_SENSITIVITY 0x4A (×1000), FIXED_STRENGTH_THRESHOLD 0x4C (×1000 dB),
COMMAND 0x100 (1 Konfig+Kalibrierung, 2 Messen, 32/33 UART-Logs an/aus, 34 Konfig loggen, 1381192737 Modul-Reset).

Geprüft und optimiert (2026-10-31):
- **I2C-Takt & Interrupt-Polling:** Bus in `firmware/esp32s3/src/main.cpp` auf **400 kHz** umgestellt; `waitNotBusy()`
  pollt den Hardware-Pin `MCU_INT` (`GPIO18`, LOW = beschäftigt, HIGH = bereit) im Mikrosekundenraster statt fester
  `delay(10)`/`delay(5)`-Wartezeiten. Dadurch sinkt die Messdauer auf ~24,5 ms (**37,1 Hz** Dauer-Abtastrate bei `dist 1800`).
- **Volle Stärke-Auflösung:** `dist` gibt `REG_PEAKn_STRENGTH` jetzt mit voller **0,001-dB-Auflösung** (`%.3f dB`) aus.
- **Konfiguration greift nur einmal nach Reset.** Spätere APPLY-Kommandos werden ohne Fehlermeldung ignoriert
  (Status sofort 0x3FF, Messung unverändert). `vital [start end]` führt den Modul-Reset (`1381192737`) automatisch aus.
- Nach einem Kommando wartet `waitNotBusy()` kurz auf die fallende Flanke von `MCU_INT` (max. 2 ms) und kehrt sofort
  zurück, sobald `MCU_INT` wieder HIGH ist.
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

**Warum die Testperson an Tag 1 nicht erkannt wurde — Ursache geklärt und mit `vital` gelöst (2026-10-31):**
1. **CFAR-Sensitivitätsformel:** In `acconeer/exptool/a121/algo/distance/_processors.py` lautet die Abbildung
   `_sensitivity_to_standard_deviations = 8.0 - 7.0 * sensitivity`. Das Absenken von `0.5` auf `0.2` an Tag 1 hat die
   Schwelle von $4{,}5\,\sigma$ auf $6{,}6\,\sigma$ **erhöht** (unempfindlicher gemacht).
2. **CFAR-Selbstmaskierung durch `PROFILE_3`:** Mit Standard `max_profile = 5` wählt der Detektor ab 300 mm `PROFILE_3`
   (140 mm Halbwertsbreite, 280 mm CFAR-Schutzfenster). Ein ausgedehnter menschlicher Oberkörper + Stuhl/Tisch hebt
   damit seinen eigenen CFAR-Hintergrund an.
3. **`close_range_leakage_cancellation` (`0x43 = 1`):** Sitzt die Person während `cfg` bereits vor dem Sensor,
   kalibriert die Nahbereichs-Leckagekompensation Teile des ruhenden Echos weg.
- **Lösung (`vital [start end]` in `firmware/esp32s3/src/main.cpp`):** Modul-Reset → `0x43 = 0` (Leckage-Kalibrierung aus),
  `0x42 = 1` (Schrittweite 2,5 mm), `0x44 = 25000` (Signalqualität 25,0 dB → hohe HWAAS), `0x45 = 1` (`PROFILE_1`,
  40 mm Halbwertsbreite trennt Vorderbrust ~675 mm sauber von Oberkörper/Schulter ~730 mm), `0x46 = 4` (`FIXED_STRENGTH`),
  `0x47 = 1` (`CLOSEST`), `0x4C = -25000` (−25,0 dB). Kalibrierung dauert nur 102 ms; Trefferquote auf den Brustkorb der Testperson
  bei ~650–740 mm: **3608/3608 Frames (100,0 %)**. Host-Skript: `host/a121_esp_vital.py`.

**Hänger beim Einschalten 2026-10-31:** Nach dem Einstecken des ESP32 antwortete der A121
nicht: `scan` leer, MCU_INT und WAKE_UP hart LOW (ESP32 treibt GPIO17 HIGH, liest aber 0; ohne lila Ader ist
GPIO17 einwandfrei), RESET-Taste half nicht. Nach frischem Einschalten (erst per A121-USB, dann wieder über
ESP32-5Vin) lief alles normal, `probe` → alle sechs Leitungen HIGH.
Wahrscheinliche Ursache laut Schaltplan (s. o.): Der CH342 hielt WAKE_UP über DTR0 auf LOW → STM32 schläft, RESET
ändert daran nichts. Passender Mechanismus (Hypothese, nicht reproduziert): Laut CH342-Datenblatt liest der Chip
DTR0 beim Einschalten als Konfigurationseingang; liegt der Pin dann LOW (Pull-down 3–8 kΩ), schaltet er auf
TNOW0 um (RS485-Sendeanzeige, LOW solange nichts gesendet wird) – bis zum nächsten Stromlos-Machen. Auslöser
könnte der ESP32-S3 sein: GPIO17 gibt beim Einschalten ~60 µs aktiv LOW aus (ESP32-S3-Datenblatt v2.2,
Tabelle 2-2), und ESP32 und A121 bekommen über 5Vin gleichzeitig Strom. Die lockere grüne Ader (SCL) zur selben
Zeit ist als Ursache weniger wahrscheinlich, aber nicht ausgeschlossen.
Bis zur Firmware-Änderung am 2026-10-31 trieben `ver`/`cfg` GPIO17 auf HIGH – an diesem Morgen also gegen
den CH342 (ca. 20 min, beide Seiten 3,3-V-Push-Pull). Seitdem treibt die Firmware WAKE_UP nicht mehr; `ver`/`cfg`
melden ein LOW nur noch.
Regel: Nach Umstecken einer Ader oder bei `WAKE_UP ist LOW` den A121 stromlos machen und neu einschalten,
dann `probe` (alle HIGH) und `ver` prüfen, bevor gemessen wird. Hilft das nicht: A121 kurz per USB an den PC
(rote Ader vorher ab).

### Stromsparmodus über ESP32 (Analyse 2026-10-31)

Quellen: Acconeer „I2C Distance Detector User Guide“ (Stand a121-v1.3.0; unsere Firmware ist 1.12.0 – Ablauf
dort als gleich angenommen, nicht geprüft), XM125-Datenblatt v1.2, CH342-Datenblatt 1E, Waveshare-Schaltplan.
- Der Stromsparzustand von `i2c_distance_detector` wird **nur über WAKE_UP** gesteuert, es gibt keinen
  I2C-Befehl dafür. Schlafen: MCU_INT HIGH abwarten → WAKE_UP LOW → MCU_INT geht LOW. Wecken: WAKE_UP HIGH →
  MCU_INT HIGH abwarten → I2C. Optional Register 0x80 „Measure On Wakeup“: misst bei jedem Wecken selbst.
  XM125-Datenblatt (Modul, nicht dieses Board): Ruhestrom Hibernate 17,7 µA, Off 6,9 µA.
- Auf diesem Board treibt der CH342 WAKE_UP (s. Schaltplan-Fakten). Ohne USB-Host steht DTR0 auf inaktiv = HIGH
  → **der A121 ist dauerhaft wach; der ESP32 kann ihn heute nicht schlafen legen.** Am Gerät bestätigt: GPIO17
  lässt sich nicht gegen den CH342 ziehen (`wake 0` → Pegel bleibt 1).
- Braun/grau (MISC_GPIO0/1) spielen für die Standard-Firmware beim Stromsparen keine Rolle; dass sie nicht am
  ESP32 hängen, schränkt heute nichts ein. NRESET liegt nicht am Stecker → hängt der A121 (s. o.), hat der ESP32
  keinen Weg, ihn neu zu starten.
- Grundverbrauch des Boards ohne A121/STM32 (Schätzung, nicht gemessen): PWR-LED über 2,2 kΩ an 3V3 ≈ 0,6 mA,
  CH342 im Suspend 0,09–0,16 mA (Datenblatt), dazu Ruhestrom des LDO RT9193 → grob ~1 mA, also weit mehr als
  der Hibernate-Wert des Moduls.

Wege, damit der ESP32 alles steuert (keiner umgesetzt):

| Weg | Eingriff | Bringt | Nachteile |
|---|---|---|---|
| A Lastschalter im 5V-Draht (P-MOSFET + Transistor, Steuerpin GPIO6/7, LOW = aus – beide Pins geben beim Einschalten ~60 µs LOW aus) | nur ESP32-Seite, A121 unverändert | Board komplett aus (LED/LDO/CH342 auch), Neustart des A121 durch den ESP32, feste Einschaltreihenfolge (ESP32 zuerst) gegen den Hänger | nach jedem Einschalten Boot + `cfg` + Kalibrierung (Dauer ungemessen); vor dem Abschalten alle Leitungen hochohmig (Phantomspeisung) |
| B Leitung DTR0 → WAKE_UP auf dem A121-Board auftrennen | Leiterbahn schneiden (Lage auf der Platine unbekannt, Foto nötig) | echter Hibernate per GPIO17, Hänger-Ursache weg | schwer rückgängig; PC-Seite verliert DTR-Steuerung (vermutlich egal) |
| C eigene STM32-Firmware: Wecken über MISC_GPIO0 (braun) statt PA11 | braun an GPIO6/7; Acconeer-SDK + ARM-Toolchain, STM32 flashen | Hibernate ohne Löten am A121, umkehrbar, passt zu Phase 4 | Aufwand, SDK-Download (Konto/Lizenz), PA11 bleibt vom CH342 getrieben (ignoriert) |
| D ESP32 als USB-Host am A121-USB-C, DTR per CDC-ACM setzen | kein Löten; Konsole wandert auf den UART-Port des ESP32 | WAKE_UP über den CH342 steuerbar | ESP32 muss als Host wach bleiben → fürs Stromsparen sinnlos; zweites Kabel am A121 |

Nächste Prüfungen (offen): Strom im roten Draht mit dem Multimeter messen (heute: dauerhaft wach). Schlaf/Wecken
mit 1.12.0 nachweisen: A121 per USB am PC (rote Ader ab), DTR auf Kanal A (`…-if00`) per pyserial schalten,
der ESP32 liest WAKE_UP/MCU_INT und spricht I2C. Hänger nachstellen: ESP32 hält GPIO17 mit kleinster
Treiberstärke LOW, rote Ader wird eingesteckt → bleibt WAKE_UP nach dem Freigeben LOW, ist die TNOW0-Hypothese
bestätigt.

Tag 1 (2026-10-30): Sensor auf die Testperson gerichtet (Abstand nicht gemessen), Bereich 100–1500 mm:
- CFAR (Standard, auch Sensitivität 0,2): über ~30 Messungen **0 Ziele** (Ursache am 2026-10-31 geklärt, siehe oben).
- Feste Amplitudenschwelle 1,0 / 3,0 / 10,0: 5–10 „Ziele“ pro Messung an wechselnden Abständen (Skalierung ×1000 war
  deutlich unter dem Rauschboden von ~25–80 Einheiten).
- Mit `vital 450 850` (`FIXED_STRENGTH = -25 dB`, `PROFILE_1`, `step_length = 1`, `leakage_cancel = 0`) am 2026-10-31
  vollständig gelöst: 100 % Trefferquote auf der Brust bei 37,1 Hz über ESP32-I2C (`host/a121_esp_vital.py`).

**Aktuelle Firmware auf dem A121 (Stand 2026-11-01, ~08:40):** **`acc_exploration_server_a121-v1.13.0_lpuart1.bin`**, gehärtet (SHA256 `b29e328d…1757`, Neustart bei jedem Server-UART-Fehler, s. „Härtung 2026-11-01“), ersetzt den Stand vom 2026-10-31 (`5dfefa8f…3827`, aufbewahrt als `…_lpuart1_2026-10-31.bin`). Eigener Build aus dem Acconeer-SDK, s. u., mit Freigabe des Projektinhabers über Kanal A geflasht (100 %). Server nur auf LPUART1 = SDA/SCL-Adern; **USB Kanal A und B antworten nicht als Server** (geprüft). Nach RESET kommt auf Kanal B (921 600 Baud, PA9) „Acconeer Exploration Server“ → Firmware startet, Break-Warteschleife wird verlassen. Der LPUART1-Weg zum ESP32 ist seit 2026-11-01 am Gerät geprüft (s. „ESP32-Test am Gerät 2026-11-01“). Vorher: `acc_exploration_server_a121_usart1_esp32.bin` (bis 23:50). Für `vital`/`auto`/`zoom` (I2C) muss wieder `i2c_distance_detector.bin` (`a121-v1.12.0`, 109 816 Bytes) geflasht werden. Hinweis zum USB-Flashen bei gestecktem (stromlosem) ESP32-Kabel ohne rote Ader: Durch die ESD-Schutzdioden des stromlosen ESP32-S3 an den Signalleitungen verschluckt der CH342 beim Richtungswechsel (`STM32 TX → CH342 RX`) deterministisch das allererste Startbit (`8,68 µs` bei 115 200 8E1), sodass `ACK 0x79` als `0xDE`, `NACK 0x1F` als `0xFC` und `GET_ID 79 01 04 35 79` als `de c0 b0 a3 ff` dekodiert werden, während der Hinweg (`CH342 TX → STM32 RX`) inkl. Hardware-Paritäts- und XOR-Prüfsummen zu 100 % bitexakt bleibt.

### Voller A121 Sparse-IQ & 3-Subsweep Zugriff über ESP32-S3 (`USART1`, 2026-10-31)

In der Werks-Server-Firmware (`acc_exploration_server_a121.bin`) läuft der Exploration Server nur auf `USART2` (`PA2`/`PA3` → ausschließlich CH342 Kanal A USB), während `USART1` (`PA9`/`PA10` → 10-Pin MX1.25-Stecker `TXD`/`RXD` ↔ ESP32-S3 `GPIO5`/`GPIO4`) nur als ungenutzter Debug-Port ohne DMA initialisiert wird.

Um dem ESP32-S3 über das vorhandene 10-Pin-Kabel **100 % Zugriff auf alle A121-Hardwarefunktionen** (rohe komplexe 16-Bit Sparse-IQ-Daten, bis zu 4 Subsweeps pro Frame, On-Chip Loopback, Profile 1–5, PRF 19,5–5,2 MHz, HWAAS 1–511, Phase Enhancement, IQ Imbalance Compensation, Continuous Sweep Mode & Double Buffering, Idle-States) zu geben, stehen ab sofort bereit:

1. **STM32L431 `USART1`-Exploration-Server-Firmware (`firmware/stm32l431/` & `host/build_stm32_usart1_server.py`):**
   - Erzeugt deterministisch `~/Data/radar-sensor/acc_exploration_server_a121_usart1_esp32.bin` (`109 008` Bytes, SHA256 `dd1226c4ac60d73fc6488f6ebd1447302a06d17c19d0312f8a2a209dffda2fc6`, 25 verifizierte Instruktions-/Vektor-Patches).
   - Legt den Exploration Server mit voller DMA-Unterstützung (`DMA1_Channel5` RX / `DMA1_Channel4` TX, `IRQ 14/15/37`, `GPIO_PULLUP` auf `PA9`/`PA10`, `UART_HWCONTROL_NONE`) direkt auf `USART1` (`GPIO5`/`GPIO4` am ESP32-S3) und unterstützt Baudraten von `115 200` bis `2 000 000` Baud sowie Soft-Reset per UART-Break.
2. **ESP32-S3 Dual-Stack-Firmware (`firmware/esp32s3/src/main.cpp`, `a121_server_client.h`, `a121_esp_dsp.h`):**
   - **Umgestellt am 2026-10-31 (Abend), am Gerät geprüft am 2026-11-01:** Der Server-Link läuft über `Serial2` auf den
     SDA/SCL-Adern (`GPIO15` RX ← PB11, `GPIO16` TX → PB10) und braucht die geplante LPUART1-Server-Firmware aus dem SDK
     (`firmware/stm32l431/README.md`). Mit der USART1-Variante spricht der ESP32 nicht mehr.
     Nach dem Start sind GPIO15/16 hochohmig (`link` = frei). `link i2c|srv` legt fest, wer GPIO15/16 nutzt; Server-Befehle schalten selbst auf `srv`, I2C-Befehle sind im `srv`-Modus gesperrt
     (der Server treibt die SDA-Ader push-pull). `uart` empfängt die Debug-Logs nur noch auf GPIO5, GPIO4 bleibt hochohmig.
   - Behält alle bestehenden I2C-Befehle (`vital`, `zoom`, `auto`, `dist`, `cfg`, `ver`, `scan`) bei und ergänzt den Sparse-IQ-Stack:
     - `srv_info`: Verbindet sich auf `GPIO15`/`GPIO16` (schaltet von 115 200 auf 921 600 Baud hoch) und liest `get_system_info` & `get_sensor_info`.
     - `stm_reset`: Sendet ein 15-ms UART-Break auf `GPIO16` (`PB10`). Der Framing-Error setzt den STM32 zurück (USART1-Variante und SDK-Neubau, beide über Kanal B bestätigt). Jeder Server-Connect des ESP32 beginnt seit 2026-11-01 mit diesem Break.
     - `iq3 [sek] [start_mm] [raw]`: Startet die **3-Subsweep Sparse-IQ Session @ 60 Hz** (Subsweep 0 On-Chip-Loopback + Subsweep 1 2,5-mm-Vital-Zoom mit 24 Punkten (Übersetzungsschalter `A121_ZOOM_POINTS=48` für 120 mm vorbereitet, Spektren dann nur für die 16 Punkte mit der stärksten Atmung; Wahl nach Messung V4.4) + Subsweep 2 Raum-Scan 0,30–2,67 m, 80 × 30 mm) und rechnet auf dem ESP32-S3 (PSRAM, `a121_esp_dsp.h`; Reports seit V2.1 im eigenen Task auf Kern 0 aus einem Schnappschuss je 60 Frames, das Lesen auf Kern 1 läuft weiter): Loopback-Phasenderotation je Frame; je Zoom-Punkt Phase um den Kreismittelpunkt der IQ-Werte (Kåsa-Kreisfit) abgewickelt, Bandpass 0,6–5 Hz, Oberwellen-Summe S(f) = P(f) + 0,5 · P(2f) über alle Zoom-Punkte gewichtet; Puls-Kandidaten 40–180 /min mit Abschlag auf Atem-Oberwellen und Oktav-Prüfung, Verfolgung mit Haltezeit (Variante Viterbi über 20 Reports hinter `A121_DSP_VITERBI`, V3.2, Standard aus; Variante „Atem-Oberwellen geführt von der Atemphase entfernen" hinter `A121_DSP_HARM_REMOVE`, V3.3, Standard aus); Autokorrelation und Einzelschläge (IBI, RMSSD experimentell) nach Abzug der Atem-Oberwellen; Konfidenz aus Spitzenhöhe, Oberwelle, Einigkeit der drei Schätzer, Bewegung und Sättigung; Atemfrequenz, Atemzustand (normal / keine Atemspitze / angehalten / Bewegung / flach), Lebenszeichen-Test; Sensor-Flags (verspätete und gesättigte Frames, tick-Lücken), Neu-Kalibrierung bei `calibration_needed` (`@RECAL`) und Rezoom (`@REZOOM`; seit V2.3 bleibt dabei die Puls-Verfolgung, wenn der letzte gültige Wert jünger als 30 s ist). Ausgabe: `@IQ3` je Frame (mit `raw` zusätzlich `@RAW`), `@ROOMPROF` 5/s (Raumprofil), `@DSP`/`@ROOM`/`@VITAL`/`@STATE` 1/s (`@STATE`: Zustand der Messung, V3.5), `@BREATH` je Atemzug (Spitzen des Wegs
0,05–0,8 Hz mit Hysterese, V3.4); Formate im Kopf und am Ende von `a121_esp_dsp.h`, Stand und Plan in `docs/AUDIT_VITAL.md`, Auswertung mit `host/vital_eval.py` und `host/a121_esp_ultimate.py`. (Bis 2026-11-01 stand hier eine Kette mit DACM und Hilbert-Projektion; die gehört zu den PC-Skripten, nicht zur Firmware.)
     - `doppler [sek] [mm] [hz]`: Aktiviert `continuous_sweep_mode=true` + `double_buffering=true` (64 Sweeps/Frame @ bis zu 4 000 Hz) für kohärente Mikro-Doppler-/Vibrationsmessungen.
     - `bridge`: Transparente USB-CDC ↔ Server-UART Bridge (inkl. automatischer Baudraten-Synchronisation), sodass PC-Skripte wie `host/a121_ultimate_vital.py` auch direkt durch den ESP32-S3 streamen können.
     - `sess …` (seit 2026-11-01, `a121_session_console.h`): beliebige Sparse-IQ-Session ohne PC. Die Konfiguration liegt im RAM
       und wird feldweise gesetzt: `fr`, `sr`, `spf`, `csm`, `dbuf`, `fidle`/`sidle`, `subs 1..4`, je Subsweep `range a b` (mm),
       `start`, `n`, `step`, `profile`, `hwaas`, `gain`, `prf`, `tx`, `loop`, `phase`, `iqimb`.
       `sess` zeigt die Konfiguration mit mm-Bereich und erwarteter Datenrate (inkl. ~150 B Kopfzeile pro Frame) und warnt über der
       Link-Grenze. `sess check` schickt sie nur an den Server und zeigt Metadaten bzw. den Fehlergrund aus dem Server-Log.
       `sess run [s] [stat|peak|amp]` streamt und wertet aus: Amplitude über alle Sweeps gemittelt, stärkster Punkt je Subsweep.
       Jede Sekunde `@SESS_STAT`, am Ende `@SESS_END` mit UART-Fehlern und Rückstau. Die Ausgabe ist gedrosselt
       (`peak` ≤ 100/s, `amp` ≤ 10/s), damit der ESP32 nie hinter den Datenstrom fällt. Höchstens 4096 Punkte pro Frame.
     - `gpio [0|1|z] [0|1|z]`: Liest oder schaltet `MISC_GPIO0` (`GPIO6` ↔ `PB1`, braun) und `MISC_GPIO1` (`GPIO7` ↔ `PA4`, grau).
     - **App-Protokoll `proto=1` (seit 2026-11-01, Firmware `2026-11-01-app1`, nur ergänzt, PC-Skripte laufen unverändert):**
       `hello` → `@HELLO,proto=1,fw=…,build=…,link=…,server=…,mac=…,up_ms=…,modes=…` (`up_ms` zeigt, ob der ESP32 neu
       gestartet ist). `@IQ3` und `@DOPPLER` haben hinten Frame-Zähler und Sensor-`tick`. `iq3 [sek] [start_mm] raw`
       schickt je Frame zusätzlich `@RAW,frame,tick,n,<Base64>` (105 komplexe int16, ≈ 35 kB/s statt ≈ 5 kB/s).
       Nach 3 Frame-Timeouts verbindet der ESP32 den Server neu (`@RECONNECT,t_ms,versuch,ok`, danach beginnt `tick`
       von vorn). Strg+R (0x12) während eines Streams startet den Server zum Test absichtlich neu.
     - Port-Öffnen setzt den ESP32 nicht zurück; nur DTR = 0 bei RTS = 1 löst einen Reset aus (am PC geprüft). PC-Skripte
       öffnen deshalb mit den Standardwerten, die App setzt nur DTR und fasst RTS nie an.

**Am Gerät geprüft (2026-10-31, nur PC über CH342 Kanal B, ESP32 nicht angeschlossen):**
- Server `a121-v1.12.0`, `max_baudrate` 2 000 000. exptool mit 921 600 Baud: 16/16 Sessions nach Reset ok, je 100 Frames @ 100 Hz.
- **Jeder Framing-Error auf PA10 setzt den STM32 zurück** (Server danach auf 115 200 Baud, antwortet nach ≲ 1 s).
  Das gilt für einen Break genauso wie für Bytes mit falscher Baudrate. Eine Baudsuche löst also selbst einen Reset aus.
  Ohne Reset bleibt der Server auf der zuletzt gesetzten Baudrate.
- Vor fast jeder Antwort schickt der Server eine Logzeile `{"status":"log",...}`. Clients müssen sie überspringen.
- Die Enums kodiert der Server wie exptool: `"profile":1`, `"prf":"15_6_MHz"`, `"ready"`, freie Rate `0.0`.
  `step_length` muss ein Teiler oder ein Vielfaches von 24 sein, `16` lehnt der Server mit „invalid config“ ab.
- Ein Python-Nachbau der ESP32-Client-Logik (gleiches JSON und Ablauf, inkl. Reset-Pfad) lief am echten Server:
  `iq3`-Preset 300 Frames @ 60,0 Hz, 420 B/Frame. `doppler`-Preset (CSM + Double Buffering, 64×4 @ 2000 Hz)
  60 Frames @ 31 Hz, 1024 B/Frame. Keine Flags (saturated/delayed/calibration_needed).
- Ein früherer Hänger (keine Antwort auf allen Baudraten, kein Reset per Framing-Error) ließ sich nur mit der RESET-Taste
  lösen und trat danach nicht wieder auf. Wahrscheinliche Ursache: s. „Bug 1“ beim ESP32-Test 2026-11-01.

**Offenes Hardware-Risiko vor dem ESP32-Betrieb über USART1:** Laut Schaltplan hängt PA10 (`DEBUG_RX`, Stecker Pin 8)
nicht nur an ESP32 GPIO4, sondern über **R9 = 22 Ω direkt am CH342 TXD1**. Der CH342 läuft immer mit (5V-Netz) und
treibt TXD1 im Ruhezustand HIGH. Dann arbeiten ESP32-TX und CH342-TX gegeneinander (Push-Pull über nur 22 Ω). Der
LOW-Pegel an PA10 wird damit unsicher, und beide Treiber werden stark belastet. Das ist derselbe Mechanismus wie bei
WAKE_UP/DTR0. Nicht nachgemessen. Vor dem Dauerbetrieb klären: R9 auslöten (Kanal B kann dann nur noch mitlesen), oder
den Server statt auf USART1 auf die I2C-Leitungen legen.

**Entscheidung 2026-10-31 (Abend): Server auf die I2C-Adern, neu gebaut aus dem Acconeer XM125 SDK.**
- Pins geprüft: SDA/SCL am Stecker = **PB11/PB10** (Schaltplan-Netze `I2C2_SDA`/`I2C2_SCL`; `i2c_distance_detector.bin`
  stellt GPIOB-Maske 0x0C00 auf AF4). Die frühere Angabe PB13/PB14 in `firmware/stm32l431/xm125_waveshare_bsp.h` war falsch.
  An den Leitungen hängen nur R13/R14, kein CH342.
- Peripherie: **LPUART1** (AF8: PB11 TX → ESP32 GPIO15, PB10 RX ← GPIO16) mit DMA2 Kanal 6/7. USART3 scheidet praktisch aus:
  Er bräuchte DMA1 Kanal 2/3, und die belegt der Exploration Server schon für SPI1 zum A121.
- Weg: offizielles XM125 SDK **a121-v1.13.0** (vom Projektinhaber geladen) + `firmware/stm32l431/build_server.py`. Das SDK selbst bestätigt
  die Pins (`main.h`: `I2C_SCL` = PB10, `I2C_SDA` = PB11); die LPUART1-Zuordnung (AF8, PB10 RX/PB11 TX) laut ST
  `STM32_open_pin_data`, DMA2 Kanal 6 TX / Kanal 7 RX mit Request 4 laut CubeMX-generiertem L4-Code.
- **Gebaut 2026-10-31 23:40, geflasht ~23:50 (Start per Debug-Log bestätigt, LPUART1-Weg zum ESP32 noch ungetestet):**
  - Kontrolle: SDK unverändert gebaut (`--variant original`) ist **bitgleich** zum vorgebauten `out/acc_exploration_server_a121.bin`
    des SDK (109 168 Bytes) → Compiler, ST-Treiber und SDK passen exakt zu Acconeers eigenem Build.
  - `~/Data/radar-sensor/acc_exploration_server_a121-v1.13.0_lpuart1.bin`, 109 760 Bytes,
    SHA256 `5dfefa8f5c9e8666bbda2b394705ec0352861a4b1344ef56dcfd891502ea3827`. Gebaut mit `-Werror` ohne Warnungen.
  - Am ELF geprüft: Vektoren IRQ 68/69/70 → `DMA2_Channel6/7_IRQHandler`, `LPUART1_IRQHandler`; Server-Sende-/Empfangspfade,
    Baudraten-Umschaltung und Break-Reset (`HAL_UART_ErrorCallback`) nutzen `hlpuart1`; die Break-Abfrage beim Start liest GPIOB Bit 10.
  - In diesem Build: Server nur auf LPUART1 (USB Kanal A antwortet nicht mehr), `printf`-Logs weiter auf USART1/PA9
    (CH342 Kanal B und ESP32 GPIO5 lesen mit), I2C2 wird nach dem SDK-Init wieder abgeschaltet.
- ESP32-Seite umgestellt; am Gerät geprüft am 2026-11-01 (s. u.).
- Zustand am Gerät 2026-10-31, ~23 Uhr: Die geflashte USART1-Variante antwortet auf keiner Baudrate (Kanal A und B), also derselbe
  Hänger wie oben. Vor weiteren Tests RESET-Taste am A121.

**ESP32-Test am Gerät 2026-11-01 (A121 am PC-USB, rote Ader ab, ESP32 am nativen USB `…12:34:56:78:9A:BC`):**
- Ergebnis: Der LPUART1-Server läuft über die SDA/SCL-Adern. Geprüft mit der ESP32-Firmware vom 2026-11-01 (mit den Fixes unten):
  - `srv_info`: `a121-v1.13.0`, `hw` xm125, `max_baudrate` 2 000 000, Sensor verbunden; der ESP32 schaltet selbst auf 921 600 Baud.
  - `stm_reset`: Der Break auf GPIO16 setzt den STM32 zurück (Kanal B: „Acconeer Exploration Server“ + „Waiting for host line
    break“), danach verbindet der ESP32 neu. Ein Connect dauert ≈ 0,4 s.
  - `iq3 10`: 583 Frames in 10 s (58 Hz, Soll 60), keine Sättigung, keine `frame_timeout`, `@REZOOM` läuft.
    In jeder Sekunde, in der `computeVitalReport()` rechnet, entsteht eine Lücke von 190–320 ms zwischen zwei Frames.
    Die Frames gehen dabei nicht verloren, sie stauen sich im 32-KB-UART-Puffer. Das sollte man noch optimieren.
    Die `@VITAL`-Werte sind **nicht bewertet**: Es gab keine Referenzmessung, und es saß niemand gezielt vor dem Sensor.
  - `doppler 5`: 156 Frames in 5 s (31 Hz, 64 Sweeps @ 2000 Hz), keine Warnungen.
  - `bridge` + exptool 7.18.2 am PC (`a121.Client.open(serial_port=…)`, Standard: Umschaltung auf 2 000 000 Baud):
    Setup mit Kalibrierung ok, 90 Frames 16 × 60 @ 30,09 Hz, keine Flags. `+++EXIT` beendet die Bridge.
  - Zwei ESP32-Neustarts hintereinander (esptool-Hard-Reset wie beim Flashen) bei laufendem Server auf 921 600 Baud
    → `srv_info` ging jedes Mal ohne RESET-Taste.
- **Bug 1, behoben (ESP32): Server nach ESP32-Neustart taub.** Der Client versuchte den ersten Handshake auf der zuletzt
  gemerkten Baudrate (nach dem Start 921 600), der frisch gestartete Server hört aber auf 115 200. Die falschen Bytes erzeugen am
  STM32 Noise- bzw. Overrun-Fehler. Im DMA-Modus bricht die ST-HAL dann bei **jedem** UART-Fehler den Empfang ab und schaltet
  die Fehler-Interrupts ab. `HAL_UART_ErrorCallback` des SDK startet aber nur bei einem Framing-Error neu. Folge: Der Server
  bleibt taub, auch ein Break hilft nicht mehr, nur die RESET-Taste. Am Gerät dreimal gesehen (Einstecken am Morgen, zweimal
  nach dem ESP32-Flashen). Die USB-Variante merkt davon nichts, weil exptool immer auf 115 200 beginnt. Wahrscheinlich war das
  auch der frühere Hänger der USART1-Variante, nachgewiesen ist das nicht.
  Fix: `connectAndNegotiateBaud()` schickt jetzt immer zuerst den Break (der Framing-Error löst sicher einen Neustart aus)
  und spricht dann nur 115 200. Dazu stehen GPIO15/16 nach dem Start hochohmig (Link „frei“). I2C startet erst beim ersten
  I2C-Befehl, damit `Wire.begin()` beim Booten keine Flanken auf PB10 legt.
  Die eigentliche Schwachstelle im STM32 ist inzwischen ebenfalls behoben, s. „Härtung 2026-11-01“.
- **Bug 2, behoben (ESP32): exptool-`setup_session` lief in der Bridge in einen Timeout.** HWCDC (USB des ESP32, Arduino 2.0.17)
  hat einen 256-Byte-Empfangspuffer und verwirft alles darüber ohne Meldung. Die Bridge schrieb byteweise auf eine UART ohne
  Sendepuffer (nur das 128-Byte-FIFO). Der exptool-Setup-Befehl ist 464 Bytes lang, mehr als 384, und so ging das Zeilenende
  verloren. Nachweis: In 64-Byte-Stücken gesendet, nahm der Server denselben Befehl an. Fix: `Serial.setRxBufferSize(4096)`,
  `setTxBufferSize(4096)` für die Server-UART, und die Bridge reicht blockweise weiter.

**Härtung 2026-11-01 (STM32, mit Freigabe des Projektinhabers geflasht ~08:40):** 7. Edit in `build_server.py`. `HAL_UART_ErrorCallback`
startet jetzt bei **jedem** Fehler der Server-UART neu (`ErrorCode != HAL_UART_ERROR_NONE` statt nur Framing-Error).
- Image `acc_exploration_server_a121-v1.13.0_lpuart1.bin`, 109 760 Bytes, SHA256
  `b29e328d52eeeafc97e28e89051cb8ee74d527fb6a17a706b39ef1488cbc1757`. Die Linker-Warnung „LOAD segment with RWX permissions“
  kommt auch beim bitgleichen `original`-Build, ist also nicht neu.
- Am ELF geprüft: LTO hat den Callback an allen drei Aufrufstellen eingebettet (`UART_DMAAbortOnError`, `UART_DMAError`,
  `HAL_UART_IRQHandler`). Überall steht jetzt `huart == &hlpuart1` und `ErrorCode (+0x88) != 0`, danach folgt `NVIC_SystemReset`.
- Am Gerät geprüft:
  - Normalbetrieb wie vorher: `srv_info`; `iq3 10` mit 575 Frames (57 Hz, ohne Sättigung); `doppler 5` mit 31 Hz; exptool über
    `bridge` mit 90 Frames bei 30,03 Hz. Auf Kanal B kamen nur die erwarteten Neustarts, je einer pro Connect durch den Break.
    Während der Messungen startete der STM32 kein einziges Mal neu.
  - Absichtlicher Baudfehler: Über die Bridge wurde der Server auf 115 200 gestellt, die Bridge blieb auf 921 600 (Trick:
    `"baudrate" : 115200` mit Leerzeichen erkennt die Bridge nicht). Danach kamen dreimal `get_system_info` mit der falschen Rate.
    Ergebnis: Der STM32 startete nach 0,1 s neu (Kanal B: „Acconeer Exploration Server“ ohne „Waiting for host line break“, also
    durch den Fehler und nicht durch einen Break) und war danach wieder erreichbar. Mit der alten Firmware blieb er in genau
    diesem Fall dreimal taub.
  - Von außen nicht zu sehen ist, welcher Fehler den Neustart ausgelöst hat (Framing oder Noise).
- Nebenwirkung: Eine echte Störung mitten in einer Messung startet den STM32 jetzt neu, statt ihn taub zu machen. Die laufende
  Session ist dann weg. Der ESP32-Client merkt das an `frame_timeout` und muss neu verbinden; ein automatisches Neuverbinden
  während `iq3`/`doppler` gibt es noch nicht.

**Mobiler Aufbau am Gerät 2026-11-01 (~09 Uhr): A121 nur über die rote Ader vom ESP32-5Vin, kein A121-USB**
(am PC nur `…12:34:56:78:9A:BC` sichtbar, der CH342 meldet sich ohne USB nicht an). STM32-Debug-Log über die gelbe Ader (`uart`, GPIO5).
- `probe`/`bus`: alle Leitungen HIGH, MCU_INT LOW (Server-Firmware nutzt ihn nicht). `srv_info` und `stm_reset` funktionieren.
  Das Debug-Log zeigt den Neustart („Acconeer Exploration Server“, „Waiting for host line break“).
- ESP32-eigene Server-UART seit 2026-11-01 auf **2 Mbaud** (`SRV_BAUD`, vorher 921 600; die Debug-UART bleibt bei 921 600):
  - `iq3 30`: 1783 Frames in 30 s (59,4 Hz), ohne Sättigung. Weiter 24 Lücken von bis zu 354 ms, je eine pro DSP-Report
    (Rechenzeit auf dem ESP32, nicht der Link). Sensortemperatur stieg von 14 auf 19 °C.
  - `doppler 10`: 31,2 Hz (64 Sweeps @ 2 kHz). `doppler 10 500 4000`: 62,3 Hz (64 Sweeps @ 4 kHz = 16 ms pro Frame, also lückenlos).
- **Durchsatz über die Bridge** (exptool 7.18.2, 2 Mbaud; Sparse IQ, Profil 3, HWAAS 1, je 4 s pro Stufe; Skript lag im Scratchpad, nicht im Repo):
  | angefordert | erreicht | `frame_delayed` |
  |---|---|---|
  | 16×60 @ 30 Hz = 115 kB/s | 30,0 Hz = 115 kB/s | 0/121 |
  | 32×100 @ 10 Hz = 128 kB/s | 10,0 Hz = 128 kB/s | 0/41 |
  | 32×100 @ 20–60 Hz = 256–768 kB/s | 15,2 Hz ≈ 194–195 kB/s | 60/61 |
  Die Grenze ist die 2-Mbaud-UART (200 kB/s brutto). Darüber bremst der Server und kennzeichnet die Frames als `frame_delayed`,
  wie vorgesehen. Zwei Läufe hintereinander mit je 5 061 804 Bytes zum PC: kein UART-Fehler (Overflow, Puffer, Framing,
  Parity), kein unvollständiger USB-Write, höchstens 129 Bytes standen im ESP32-Puffer an.
- **Bug 3, behoben (ESP32): Bei Überlast gingen in der Bridge Daten verloren** (exptool brach mit „Cannot decode header“ ab).
  Die Zähler (`@BRIDGE_STATS` nach `+++EXIT`) zeigten einen unvollständigen USB-Write und keine UART-Fehler. HWCDC hat
  standardmäßig nur einen 256-Byte-Sendepuffer und verwirft Daten, wenn der PC 100 ms lang nicht abholt.
  Fix: `Serial.setTxBufferSize(16384)`, `setTxTimeoutMs(500)`, und die Bridge sendet den Rest nach einem unvollständigen Write erneut.
- Nicht geprüft: Stromaufnahme im roten Draht, Dauerbetrieb über Stunden, Betrieb am Handy statt am PC.

**`sess` am Gerät 2026-11-01 (mobiler Aufbau):**
- Standard (40 Punkte, 10 Hz): 10,00 Hz. Zwei Subsweeps 0,3–1,5 m (Profil 2, 10 mm) + 1,5–7,0 m (Profil 5, 30 mm, PRF 6,5 MHz):
  20,00 Hz, stabile Reflexion bei ≈ 650 mm. Großer Frame 4 × 1000 Punkte (16 kB) bei 5 Hz: sauber.
- Überlast 32 × 100 bei 30 Hz (384 kB/s): Der ESP32 nimmt 15,2 Hz auf (≈ 195 kB/s, Link-Grenze), der Server meldet `frame_delayed`.
  Kein UART-Fehler, kein Rückstau.
- Fehlerfälle: `step 16` → „Invalid step_length“, Start 12,5 m → „Start point or range too far“. Mehr als 4096 Punkte
  lehnt der ESP32 vor dem Setup ab. CSM ohne `sr` → „Invalid settings for continuous_sweep_mode“.
- **Continuous Sweep Mode, 1 Punkt, Profil 1, HWAAS 1** (`max_sweep_rate` laut Metadaten 24 673 Hz):
  | sweep_rate | spf | erreicht | delayed |
  |---|---|---|---|
  | 2 000 Hz, ohne `dbuf` | 64 | 30,36 Hz | alle |
  | 2 000–21 000 Hz, mit `dbuf` | 64 | 31,2 … 323,8 Hz (Soll −0,1 … −1,4 %) | 0 |
  | 22 000–24 000 Hz, mit `dbuf` | 64 / 256 | effektiv ≈ 21,7–23,6 kHz Sweeps | alle |
  Folgen: CSM braucht Double Buffering, und lückenlos geht es bis **21 kHz Sweep-Rate**, unabhängig von der Frame-Größe.
  Der Engpass liegt im Sensor/STM32-Pfad, nicht im Link (≈ 150 kB/s) und nicht im ESP32 (Rückstau 0). Bei 5 Punkten
  begrenzt der Link: 137 Hz × (1280 B + ~150 B Kopfzeile) ≈ 196 kB/s.
- **Bug 4, behoben (ESP32): Der Client las jedes Byte der Frame-Kopfzeile einzeln aus dem UART-Treiber.** Bei 375 Frames/s
  lief der 32-KB-Puffer über (4873 FIFO-Overflows, Rate brach auf 12 Hz ein). Fix: 1-KB-Zwischenpuffer im Client, gefüllt in
  Blöcken. Danach 369 Frames/s mit Rückstau 0. `iq3` (58,5 Hz), `doppler` (31,2 Hz) und Bridge (≈ 195 kB/s) danach erneut geprüft.

**`iq3` gibt den Raum aus, Zielsuche korrigiert (2026-11-01, mobiler Aufbau):**
- Neue Zeilen: `@ROOMPROF` alle 12 Frames (5/s) mit geglätteter Amplitude und Bewegung aller 80 Raum-Punkte (0,30–2,67 m,
  30 mm) und `@ROOM` je Report (1/s) mit Rauschpegel und bis zu 8 Zielen (Entfernung, Amplitude, Bewegung, bewegt ja/nein).
  Zeitachse ist der Sensor-`tick` (ms), der auch über Rechenpausen lückenlos läuft. Vorher wurde nur die Anzahl der Ziele ausgegeben.
- **Bug 5, behoben (ESP32-DSP): Die Zielsuche im Raum-Scan sah fast nur Rauschen.** Die festen Schwellen (Wert > 120, Bewegung > 12)
  lagen unter dem Rauschen (Amplitude ≈ 55, Bewegung ≈ 55), und es zählten nur die 4 nächsten Spitzen. Die Liste war deshalb immer
  mit Rausch-Spitzen bis ≈ 1,2 m gefüllt, und eine Rausch-Spitze im Zoom-Fenster galt als „bewegtes Ziel in der Nähe“ und
  verhinderte den Rezoom. Jetzt: Rauschpegel = unteres Viertel der 80 Punkte, Ziel ab 2 × Rauschen, „bewegt“ ab 3 ×, die 8 stärksten,
  Entfernung parabolisch verfeinert, Rezoom auf das stärkste bewegte Ziel.
- PC-Prüfung (`host/a121_esp_ultimate.py --mode verify-cpp`, derselbe Header für den PC übersetzt, `iq3raw_normal_120s` und
  `iq3raw_motion_120s`): verfolgter Puls in allen 242 Reports wie vorher (der Puls-/Atemteil ist nicht geändert). Zoom-Fenster
  richtig (570–630 mm): alt und neu kein Rezoom. Fenster absichtlich falsch gesetzt: alt bei 400 mm (beide Aufnahmen) und 1000 mm
  (`motion`) nie ein Rezoom; neu bei 400, 750, 1000, 1500 und 2250 mm nach 3–4 s eine Empfehlung auf die Person
  (Fensterstart 562–688 mm, Person bei 0,60–0,67 m).
- Am Gerät (`iq3 30 500`, Person sitzend bei ≈ 0,7 m, `2026-11-01_iq3_raum_live1.log/.png`): 1794 Frames in 30 s (59,8 Hz), keine
  Sättigung; 149 Raumprofile im 200-ms-Takt, eine Lücke von 307 ms beim Rezoom. Rezoom nach 3 s von 500–560 auf 632–692 mm,
  danach keiner mehr. Rechenzeit je Report weiter ≈ 306 ms.
- Grenzen: Schwellen an zwei Aufnahmen und einem Live-Lauf derselben Person im selben Raum gesehen. Schwache feste Echos
  (2–2,6 × Rauschen, z. B. bei 0,42 m und 1,0–1,3 m) erscheinen und verschwinden an der Schwelle.

**Bug 6, behoben (ESP32-DSP): Die Atemfrequenz klebte oft am unteren Rand des Suchbands (6,9 /min)**, in `iq3raw_normal` bei
30 von 113 Reports, in `iq3raw_motion` bei 35 von 115, live bei 10 von 18. Zwei Ursachen, an den Aufnahmen einzeln belegt
(Python-Nachbau, der bis auf 0,1 /min mit dem C++-Code übereinstimmt):
- Die Spektren der 24 Zoom-Punkte wurden ungewichtet summiert. Schwache Punkte (|z| 120–300) hatten 140–240 Phasensprünge in
  15 s und ein Schein-Wegsignal von 8–11 mm Spitze-Spitze (Abwickel-Fehler). Ihr Tieftonanteil überdeckte die Atmung der
  starken Punkte, die z. B. bei t = 75 s sauber 17 /min zeigten.
- Gesucht wurde das größte Bin im Bereich. Bei Lagewechseln (Stufen von 2–5 mm im Wegsignal) steigt die Leistung zu tiefen
  Frequenzen weiter an, das größte Bin ist dann der Rand, obwohl dort keine Spitze liegt.
- Fix: Gewicht Amplitude² je Punkt (umgekehrtes Phasenrauschen) und nur echte lokale Maxima; ohne Spitze `br` = 0.
  Einzeln getestet: nur Gewicht 30→15 bzw. 35→34 Randwerte, nur lokale Maxima 0/0 (2 Reports ohne Wert); beides 0/0.
- PC-Prüfung: Randwerte 30→0 und 35→0, keine Reports ohne Wert. Verfolgter Puls in allen 228 Reports unverändert, nur `hr_harm`
  ist öfter gesetzt (30→40, 41→43), weil die Oberwellen-Prüfung jetzt die richtige Atemfrequenz bekommt. Gegen die gezählten
  Atemzyklen der starken Punkte: `normal` 60–75 s 17,6 gezählt / 17 /min, 80–95 s 19,8 / 21; `motion` 0–20 s 20,7 / 20–21.
- Am Gerät (`iq3 60 500`, Person sitzend, `2026-11-01_iq3_atmung_live1.log/.png`): 40 Atemwerte, keiner am Rand, 9,7–24,3 /min.
  Vergleich mit den Nulldurchgängen des `@IQ3`-Wegsignals im selben 15-s-Fenster: 29 von 40 innerhalb ±3 /min, bei 31–56 s
  (Zoom 650–710 mm, auf der Brust) alle 26 innerhalb ±2 /min. Abweichungen bei 12–22 s (7 Reports; dort schwanken auch die
  gezählten Zyklen zwischen 11 und 19 /min) und bei 56,5 s. Rechenzeit je Report Median 291 ms (vorher ≈ 306 ms).
- Grenzen: keine Referenz, die Zyklenzählung stammt aus demselben Radarsignal (nur Plausibilität). Bei deutlicher Bewegung
  (`motion` 46–58 s) gibt es trotzdem einen Atemwert (23–26 /min), der dort nicht belastbar ist.

**Bug 7, behoben (ESP32-DSP): Der Rezoom wählte bei zwei nahen Echos das falsche und blieb dort hängen.** Im Lauf
`2026-11-01_iq3_atmung_live1` ging der erste Rezoom auf 745–805 mm, die Brust lag bei ≈ 0,66 m; das Fenster blieb 19 s dort.
- Ursache 1, Momentaufnahme: Es gab zwei bewegte Echos, die Brust bei 0,66 m (Amplitude schwankend 440–1220, Bewegung 600–960)
  und ein zweites bei 0,78 m (Amplitude gleichmäßig ≈ 950, Bewegung 200–570). Entschieden wurde in einem einzigen Report:
  0,774 m mit 1096 + 8 × 596 = 5864 gegen 0,656 m mit 529 + 8 × 660 = 5809, weil die Brust gerade einen Amplitudeneinbruch hatte.
- Ursache 2, Nähe-Regel: Jedes bewegte Echo am Fenster verhinderte den Rezoom, also hielt das zweite Echo das Fenster fest.
- Das falsche Fenster war schlechter: Pulskonfidenz Median 78 % gegen 99 % auf der Brust, Puls-Spitze/Median 9,2 gegen 15,8 dB
  (verschiedene Zeitabschnitte desselben Laufs).
- Kriterien an den 5-Hz-Raumprofilen von vier Logs verglichen (Brust gewählt / Wahl bei t = 3 s im Fehlerlauf): Momentaufnahme
  heute 92 % / falsch; über 3 s geglättet „Amplitude + 8 × Bewegung“ 99–100 % / richtig; geglättet nur Bewegung ebenso; stärkstes
  Echo 85–88 % / falsch (das zweite Echo war im Mittel stärker, bewegte sich aber weniger). Glättung 1–8 s: überall richtig.
- Fix: Das Zoom-Ziel kommt aus einem zusätzlich geglätteten Raumprofil (τ ≈ 3 s), und nur dieses Ziel hält das Fenster (±60 mm).
  Simulation auf den Logs: Start auf dem zweiten Echo (745 mm) heute bis 26 s gefangen, neu nach 3 s auf der Brust; in allen
  anderen Fällen gleiche Rezooms wie vorher, kein Hin- und Herspringen.
- PC-Prüfung (`iq3raw_normal`/`motion`): Vitalwerte bitgleich. Fenster richtig: kein Rezoom. Fenster bei 400–2250 mm: Brust nach 3 s
  gefunden, Fensterstart 580–635 mm (vorher 562–688 mm).
- Am Gerät (`iq3 45 745`, Person sitzend, `2026-11-01_iq3_rezoom_live1.log/.png`): ein Rezoom nach 3 s auf 635–695 mm, danach keiner,
  obwohl bei 16, 25 und 43 s bewegte Echos bei 0,74–0,79 m auftauchten. 59,9 Hz, Atmung 11,5–24,8 /min ohne Randwert,
  Pulskonfidenz Median 94 %, Rechenzeit Median 319 ms. Das zweite Echo war diesmal schwächer als im Fehlerlauf; der harte Fall
  ist nur durch die Simulation auf dem Log belegt.

Downloads (nicht im Repo, liegen in `vendor/`, gitignored), geholt 2026-10-30:
- `A121_Firmware.zip` sha256 `fea5bcd0b7d39149c0cc02bf82303abd5cd6d92f209643ba426b18db58da356a`
  (https://files.waveshare.com/wiki/A121_Range_Sensor/A121_Firmware.zip)
- `A121_Range_Sensor_Demo.zip` sha256 `481b587bf8e388d36846ab44a4603a2a2cd60a3a35245d674eaaaf5b1f66e144`
  (https://files.waveshare.com/wiki/A121_Range_Sensor/A121_Range_Sensor_Demo.zip)

Für den eigenen Server-Build, geholt 2026-10-31 (Versionen laut `doc/XM125 Software User Guide.pdf` des SDK):
- `acconeer_xm125_a121-v1_13_0.zip` sha256 `0f4e74b1d3af18583b650c9f27865838a8bf11e93387889775689940664aa3f4`
  (Acconeer Developer Site, mit dem Konto des Projektinhabers geladen) → `vendor/acconeer_xm125_a121-v1_13_0.zip`, entpackt nach
  `vendor/acconeer_sdk/a121-v1.13.0/xm125/`. Lizenz der Acconeer-Teile: BSD-3-Clause (`LICENSES/license_acconeer.txt`).
- STM32CubeL4 v1.18.1, nur `Drivers/` → `vendor/stm32cube/STM32Cube_FW_L4_V1.18.1/Drivers/`, per Git auf die Commits des
  Tags v1.18.1: `STMicroelectronics/stm32l4xx-hal-driver` @ `41027c61f69401afe4937db0ebd5d0cdbffbea0d` (BSD-3),
  `STMicroelectronics/cmsis-device-l4` @ `a2530753e86dd326a75467d28feb92e2ba7d0df2` (Apache-2.0),
  `Drivers/CMSIS/Include` aus `STMicroelectronics/STM32CubeL4` @ `d4c40b36185bd0d75e33c94ebc8337b634bbcc94` (Tag v1.18.1).
- Arm GNU Toolchain 13.3.Rel1 `arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz`
  sha256 `95c011cee430e64dd6087c75c800f04b9c49832cc1000127a92a97f9c8d83af4` (stimmt mit ARMs `.sha256asc` überein,
  https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/) → `~/.local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/`.

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
- **Geprüft 2026-11-01 am Galaxy S26 Ultra:** nativer USB-Port des ESP32 per C-auf-C-Kabel am Handy → Handy ist
  Stromquelle + Host, alle Power-LEDs (ESP32 und A121) an, ESP32 als `303a:1001` (CDC-ACM + JTAG) erkannt. Die
  USB-C-Buchse des Nachbaus braucht also keinen OTG-Adapter. Datenfluss mit der App geprüft: 31,7 min `iq3 … raw`
  über USB ohne Frameverlust (`docs/ANDROID_APP.md`). Abziehen startet den ESP32 neu (er hängt am Handy-Strom); das
  Handy lädt währenddessen nicht, Verbrauch nicht gemessen.
- Nie beide USB-Ports des ESP32 gleichzeitig anschließen (z. B. Handy + PC): Mit geschlossener IN-OUT-Brücke könnten
  zwei 5-V-Quellen verbunden sein (nicht gemessen).
- Motoren (späterer Scanner) immer separat versorgen; nur GND gemeinsam.

## Pico (zurückgestellt)

Nicht Teil des aktuellen A121-Fokus. Mögliche spätere Rollen: SWD-Debug-Probe für den STM32
(„debugprobe“-Firmware an SWDIO/SWCLK/GND-Pads) oder Schrittmotorsteuerung für den SAR-Scanner.
Flashen des STM32 geht ohne Pico über USB (BOOT-Taste, Kanal A).
