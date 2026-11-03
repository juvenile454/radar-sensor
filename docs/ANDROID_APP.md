# Android-App — Plan und Stand

Stand 2026-11-01. Ziel: Alles, was A121 und ESP32-S3 liefern, am Galaxy S26 Ultra live sehen, aufzeichnen und
auswerten. Zuerst über USB-C, BLE später als zweiter Transport.

**Stand:** A0–A4 umgesetzt und am Handy geprüft. A5 (optional) läuft am Handy, ist aber nur zum PC bitgleich, nicht
zum ESP32 (siehe Ergebnisse). Nur Debug-Build, nur Sideload auf das eigene Handy.

## Geprüfter Ausgangspunkt (2026-11-01)

- **Handy:** SM-S948B, Android 17 (API 37), One UI 9.0; Features `usb.host`, `bluetooth_le` (per ADB gelesen).
- **USB-OTG (A0):** C-auf-C-Kabel, Handy ist Stromquelle + Host, alle Power-LEDs an. ESP32 erkannt als `303a:1001`
  „USB JTAG/serial debug unit“ (Seriennr. `12:34:56:78:9A:BC`): Interface 0 CDC-Steuerung, 1 CDC-Daten (Bulk, 64 B),
  2 Hersteller (JTAG). Datenfluss seit A3 geprüft.
- **ADB:** drahtlos über Tailscale (`<Tailscale-IP des Handys>`); der Port wechselt nach jedem Ein/Aus, finden mit
  `adb mdns services`. Seriennummer immer live aus `adb devices -l`.
- **ESP32 + A121 am PC:** eigener Exploration Server `a121-v1.13.0`, 2 Mbaud. `iq3 10` → 595 Frames in 10 s
  (59,4 Hz), insgesamt ≈ 4,9 kB/s Text (Log `2026-11-01_app_machbarkeit_iq3_probe.log`).
- **Werkzeuge:** Android SDK Platform 36/37, Build-Tools 37, JDK 17, Gradle 9.7.1; seit A5 NDK `28.2.13676358`
  (Standard von AGP 9.4) und CMake 3.22.1.

## Grundentscheidungen

| Thema | Entscheidung | Grund |
|---|---|---|
| Transport | USB-CDC mit `usb-serial-for-android` 3.11.0 (MIT). Schnittstelle `Transport` (USB, TCP für die Entwicklung, Wiedergabe); BLE später als weitere Umsetzung | Firmware bleibt nutzbar wie am PC, Bandbreite reicht für Rohdaten |
| Protokoll | Bestehendes Zeilenprotokoll `@TAG,feld,…` bleibt. Erweitern nur durch neue Tags oder Felder **hinten** | PC-Skripte lesen Felder nach Position (`@VITAL` mit fester Feldzahl) |
| Auswertung | ESP32 rechnet weiter (DSP dort geprüft). App zeigt, zeichnet auf und wertet nach; Rohdaten optional dazu. Nachrechnen am Handy (A5) nutzt denselben Firmware-Header, keine zweite Fassung | Keine zweite DSP-Fassung pflegen |
| Zeitbasis | Sensor-`tick` + Frame-Zähler; Android-Empfangszeit (`elapsedRealtimeNanos`) nur zusätzlich. Nach ESP32- oder Server-Neustart (Zähler beginnen neu) läuft die Zeitachse der App mit Versatz weiter | Nach DSP-Pausen kommen Frames gebündelt (EXPERIMENTE M3, Aufnahme E) |
| Aufnahme | Ein Ordner je Aufnahme: `stream.log` (Zeilen genau wie vom ESP32, also wie die PC-Logs), `rxtime.csv` (Zeilennummer → Empfangszeit), `meta.json` (Gerät, Quelle, `@HELLO`, Befehle mit Zeilennummer, Start/Ende) | PC-Skripte lesen Handy-Aufnahmen ohne Umbau |
| Ablage | App-Ordner `Android/data/de.timo.radarsensor/files/recordings/`, holen per `adb pull` über Tailscale nach `~/Data/radar-sensor/phone/` | Keine Speicherberechtigung, nichts im Repo |
| App-Gerüst | `android/`, Paket `de.timo.radarsensor`, Kotlin + Compose. AGP 9.4.0 (Kotlin eingebaut), Kotlin 2.4.10, Compose-BOM 2026.08.00 (wie OrbitLens). compileSdk/targetSdk 37, minSdk 34. Nur Debug-Build, Sideload | Zielgerät ist API 37 und live prüfbar; nur eigenes Gerät |
| Plots | Eigene Compose-Canvas-Zeichnungen (Linien, Wasserfall als Bitmap) | 60 Hz flüssig, keine Chart-Bibliothek |
| Hintergrund | Foreground-Service Typ `connectedDevice` + WakeLock; Dienst wartet nach Abziehen auf das Gerät. „Trennen“ beendet alles und schaltet automatisches USB-Verbinden ab. „Bildschirm an lassen“ ist eine Einstellung | Samsung beendet Hintergrund-Apps sonst; Aufnahme bei ausgeschaltetem Bildschirm geprüft (A4) |
| Neuverbinden | Dieselbe Quelle: Zähler/Anzeige bleiben, der zuletzt gestartete Stream (`iq3`, `doppler`, `sess run`) startet neu. Andere Quelle: Anzeige wird geleert | Abziehen startet den ESP32 neu (Handy versorgt ihn) |

## Daten → Ansichten

Tabs: Übersicht, Signal, Raum, Vital, Doppler, Experten, Diagnose, Konsole, Aufnahmen.

| Quelle (Befehl) | Zeilen | Rate | Ansicht |
|---|---|---|---|
| `iq3` | `@IQ3` (Zeit, Temperatur, Loopback-Amplitude, Weg µm, Zoom-Amplitude, Sättigung, Frame, tick, verspätet) | 60 Hz | Signal: Weg-Kurve, Amplituden, Sättigung, verspätete Frames |
| `iq3 … raw` | zusätzlich `@RAW` (105 komplexe int16 je Frame, Base64; mit `A121_ZOOM_POINTS=48` 129) | 60 Hz | nur Aufnahme (Nachrechnen, PC) |
| `iq3` | `@ROOMPROF` (80 Amplituden + 80 Bewegungswerte, 0,30–2,67 m) | 5 Hz | Raum: Profil + Wasserfall |
| `iq3` | `@ROOM` (Rauschen, Ziele: Abstand, Stärke, Bewegung, bewegt) | 1 Hz | Raum: Zielmarker; Übersicht: Hauptziel |
| `iq3` | `@VITAL` (Atmung, Puls fused/Spektrum/AKF/IBI, IBI-Median, RMSSD, Konfidenz, Atemtiefe, Herz-Amplitude, SNR, Loopback-Phasenrauschen, Zielabstand, Anzahl Ziele) | 1 Hz | Übersicht/Vital; Puls **immer als „Kandidat“** mit Konfidenz |
| `iq3` | `@DSP` (gültig, Punkt, Konfidenz, Rechenzeit, Bewegungsanteil, Oberwelle, Punkt-Einigkeit, Atemzustand 0–4, je Report verspätete / gesättigte Frames und tick-Lücken, Frame-Stand des Schnappschusses; mit Schalter `A121_DSP_COMBINE` zusätzlich die effektive Punktzahl, V3.1) | 1 Hz | Vital + Diagnose, Atemzustand im Vital-Tab, „angehalten" als Ereignis |
| `iq3` | `@STATE,t_ms,zustand,seit_ms` (0 keine Person, 1 Suche, 2 Erfassung, 3 gültig, 4 Bewegung, 5 Apnoe; V3.5) | 1 Hz | Gütezustand in Übersicht und Vital-Tab (V5.1): Zustandstext („Erfassung läuft x s", „gültig seit x s"), Puls grau bei Konfidenz < 50 %, Bewegung oder ohne gültige Messung |
| `iq3` | `@BREATH,t_ms,tick,intervall_ms,tiefe_um,rate` je Atemzug (Abstand zur vorigen Spitze, Tiefe Spitze − Tal, Rate = 60 / Median der letzten 4 Abstände; nur in Erfassung/gültig/Apnoe; V3.4) | je Atemzug | Atmung Atemzug für Atemzug im Vital-Tab (V5.1, Punkte „je Atemzug" im Atem-Diagramm, letzter Atemzug als Zeile) |
| `iq3` | `@IQ3_START`, `@REZOOM`, `@RECAL` (Neu-Kalibrierung nach `calibration_needed`), `@RECONNECT`, `@WARN` (u. a. `dsp_busy`: Report ausgefallen, `dsp_late`: Report zu spät, ungültig), `FEHLER…`, `@END` | Ereignis | Ereignisleiste, Diagnose |
| `doppler` | `@DOPPLER_START`, `@DOPPLER` (mit Frame, tick) | ~31 Hz | Doppler |
| `sess …` | `@SESS_META`, `@SESS_SUB`, `@PEAK`, `@AMP`, `@SESS_STAT`, `@SESS_END` | je Session | Experten |
| `help`, `info`, `bus`, `srv_info`, `hello`, … | Freitext / `@HELLO` | — | Konsole (jeder Befehl frei sendbar, Schnellbefehle, Server-Neustart-Test Strg+R) |
| `bridge` | Rohprotokoll des Exploration Servers | bis ≈ 195 kB/s | nicht umgesetzt |
| Modus B (`dist`, `vital`, `auto`) | `@F`, `@Z`, … | — | später; braucht anderes A121-Flash |

Diagnose: Datenrate, empfangene Menge, Frameverlust (Lücken im Frame-Zähler), Neuverbindungen und Frame-Timeouts,
unlesbare Zeilen, DSP-Rechenzeit, Temperatur, Zeilen je Tag, Ereignisse.

## Firmware-Erweiterungen (ESP32, nur ergänzend) — umgesetzt in `2026-11-01-app1`

- **F1** `hello` → `@HELLO,proto=1,fw=…,build=…,link=srv|i2c|frei,server=…,mac=…,up_ms=…,modes=…`; seit V4 hinten
  `iq3=p1h32/lp1g16/19_5_MHz` (iq3-Preset: Zoom-Profil/HWAAS, Loopback-Profil/Gain/PRF).
- **F2** `@IQ3` und `@DOPPLER` hinten um Frame-Zähler und Sensor-`tick` ergänzt.
- **F3** `iq3 [sek] [start_mm] raw` → je Frame `@RAW,frame,tick,n,<Base64>` (105 Punkte = 420 B), App misst
  ≈ 41 kB/s gesamt statt ≈ 5 kB/s.
- **F4** Nach 3 Frame-Timeouts verbindet der ESP32 den Server neu (`@RECONNECT,t_ms,versuch,ok`); Strg+R (0x12)
  während eines Streams startet den Server zum Test absichtlich neu.
- **Reset-Falle geklärt:** Nur DTR = 0 bei RTS = 1 setzt den ESP32 zurück (am PC mit `up_ms` geprüft). Die App setzt
  nur DTR und fasst RTS nie an; erneutes Verbinden am Handy ohne Reset geprüft. Die App liest immer, auch ohne
  Aufnahme (TX-Timeout 500 ms würde sonst die DSP-Schleife bremsen).
- PC-Skripte laufen unverändert (`--mode verify-cpp` und `--mode plot` mit neuen Logs geprüft); `tick_time()` in
  `host/a121_esp_ultimate.py` kommt mit `@RECONNECT` und neu beginnendem `tick` zurecht.

## Phasen

| Phase | Inhalt | ESP32 steckt an | Fertig, wenn (geprüft) | Stand |
|---|---|---|---|---|
| A0 | USB-Grundtest | Handy | Strom + Erkennung | ✅ 2026-11-01 |
| A1 | F1–F4; Testaufnahmen aller Modi nach `~/Data/radar-sensor/app-fixtures/` | PC | Geflasht, Live-Läufe ok, PC-Skripte unverändert lauffähig | ✅ geflasht und live geprüft |
| A2 | App-Gerüst; Protokoll-Modul in reinem Kotlin mit JVM-Tests; Wiedergabe von Aufnahmen; Entwicklungs-Weiterleitung `host/esp_tcp_bridge.py` (ESP32 am PC → TCP nur auf der Tailscale-IP → Debug-App) | PC | Tests grün; App installiert; Wiedergabe und Live über Tailscale zeigen Plots (Screenshot) | ✅ 28 JVM-Tests grün; Wiedergabe + TCP live per Screenshot |
| A3 | Live über USB: Gerät erkennen, Berechtigung, DTR/RTS ohne Reset, Start/Stop, Konsole, Wiederverbinden nach Abziehen | Handy | 10 min `iq3` ohne Frameverlust laut Zähler; Abziehen/Einstecken fängt sich; kein ungewollter Neustart | ✅ (31,7 min ohne Frameverlust, siehe A4) |
| A4 | Alle Ansichten; Aufnahme im Hintergrunddienst; Export | Handy | 30-min-Aufnahme bei ausgeschaltetem Bildschirm vollständig, per `adb pull` geholt, `host/a121_esp_ultimate.py --mode plot` wertet sie aus | ✅ |
| A5 (optional) | DSP-Header per NDK in die App; Rohdaten am Handy neu durchrechnen | PC + Handy | Ergebnis bitgleich zu ESP32/PC | ⚠️ bitgleich zum PC, nicht zum ESP32 |

Später: BLE-Transport, Puls-Referenz (sobald ein Gerät da ist), Ideen I1–I5 aus `CONCEPT.md`.

## Ergebnisse (2026-11-01)

Puls-Werte in allen Aufnahmen sind Rhythmus-Kandidaten; keine Referenz vorhanden.

- **A1:** Fixtures in `app-fixtures/`: `2026-11-01_iq3raw_20s` (1195 Frames), `2026-11-01_iq3_30s_reconnecttest`,
  `2026-11-01_doppler_8s`, `2026-11-01_txt_*` (help, info, bus, srv_info, sess, sess run 5 stat/peak/amp, hello).
- **A2 (TCP über Tailscale):** Aufnahme in der App `phone/2026-11-01_161356_tcp_iq3raw_test`: 2786 Frames, 0 Lücken,
  `@RAW` passt zu `@IQ3` (|IQ| ≤ 0,05 Abweichung = Rundung); verify-cpp und plot laufen damit.
- **A3 (USB):** Berechtigung per Dialog, danach „Immer mit Radar Sensor öffnen“ gesetzt. Alle Tabs live, dazu
  `doppler 8` und `sess run 5 amp` über die Konsole. Abziehtest (`phone/2026-11-01_163341_usb_abziehtest`, 2× ab/an
  während der Aufnahme): Der ESP32 startet neu (`up_ms=732`), die App verbindet selbst neu und startet `iq3` wieder.
  17 042 Frames in 3 Streams, 0 Lücken innerhalb der Streams. Pause letzter Frame → neuer Stream 34,9 s (mit
  Berechtigungsdialog) bzw. 8,2 s (ohne Dialog; enthält die Zeit ohne Kabel, nicht getrennt gemessen).
- **A4 (30 min, Bildschirm aus):** `phone/2026-11-01_164043_usb_iq3raw_bildschirmaus`, `iq3 0 raw` über USB,
  1902,7 s, 113 289 Frames, 59,64 Hz nach tick, **0 Lücken, 0 Neuverbindungen**, ≈ 41 kB/s, `stream.log` 77,6 MB.
  Bildschirm während der Kontrollen `Dozing`. Größter tick-Abstand 440 ms: alle 38 Abstände > 100 ms fallen auf
  `@REZOOM`. Größte Empfangspause 337 ms (DSP-Report am ESP32: im Mittel 270 ms, max. 323 ms). `esp_record.py --check`
  OK, `--mode plot` wertet sie aus (9440 Raumprofile, 1899 Reports, 1632 `@VITAL`; Bild nicht von Hand begutachtet).
  Akku 83 → 80 % (16:33–17:12, abgelesen, nicht gemessen; der ESP32 hängt am Handy, es lädt nicht).
- **A5 (DSP am Handy):** `libradardsp.so` (arm64) aus `host/dsp_replay/` + `firmware/esp32s3/src/a121_esp_dsp.h`.
  Für die 30-min-Aufnahme: Handy 28,4 s, PC 8 s. **Handy und PC liefern bytegleiche Ergebnisse** (`dsp_phone.log`,
  md5 gleich). Gegen die Live-Ausgabe des ESP32 sind 51 715 von 128 197 Zeilen gleich:

  | Zeile | gleich | größte Abweichung |
  |---|---|---|
  | `@IQ3` | 37 318 / 113 289 | Weg 0,21 µm (aufsummierte Phase) |
  | `@ROOMPROF` | 9413 / 9440 | 1 in der letzten Stelle |
  | `@DSP` | 1877 / 1899 | 0,1 Konfidenz (letzte Stelle) |
  | `@ROOM`, `@REZOOM` | 1899 / 1899, 38 / 38 | — |
  | `@VITAL` | 1170 / 1632 | Atemtiefe in 358 Zeilen (76 mehr als 1 Stelle, max. 1,6 µm); IBI/RMSSD in 1 Zeile (IBI 16,7 ms) |

  Vermutete Ursache: unterschiedliche Float-Mathebibliotheken (newlib am ESP32, glibc/bionic). Eine Variante mit
  zusammengefassten Multiply-Add-Befehlen (FMA) ändert daran fast nichts. Der Shim rechnet `atan2f`, `cosf`, `powf`,
  `log10f` über double; damit sind PC und Handy gleich, gegen den ESP32 so gut wie die Float-Funktionen.
  Seit dem Abschluss-Review des Vital-Plans übersetzt auch die Firmware mit `-ffp-contract=off` (`platformio.ini`;
  vorher durfte GCC am ESP32-S3 zu `madd.s` zusammenfassen, die Nachrechnung nicht).
  **Am Gerät geprüft 2026-11-02** (Firmware `2026-11-02-vital1`, `iq3 60 raw` am PC über `esp_record.py`, 60 s,
  `app-fixtures/2026-11-02_vital1_iq3raw_60s`): 3569 Frames, 0 Lücken, 59,5 Hz. `replay-cpp` gegen die Live-Ausgabe:
  `@DSP` 59/59, `@VITAL` 17/17, `@STATE` 59/59, `@BREATH` 5/5, `@ROOM` 59/59, `@ROOMPROF` 297/297, `@REZOOM` 5/5
  gleich; `@IQ3` 3531/3569 (Rest 1 in der letzten Stelle). `compute_us` je Report Median 91 ms, max. 155 ms (vorher
  270–320 ms in der Leseschleife; jetzt im eigenen Task auf Kern 0). Nur 60 s und ohne Referenz; 30-min-Lauf offen.
  Gleicher Test über die App am Handy (USB, Aufnahme per Debug-Befehl, `phone/2026-11-02_171254_livetest_vital1`):
  3594 Frames, 0 Lücken, 59,9 Hz; Nachrechnung `@DSP`/`@STATE`/`@BREATH`/`@ROOM` gleich, `@VITAL` 50/51 (1 in der
  letzten Stelle); `compute_us` Median 152 ms, max. 155 ms (51 statt 17 `@VITAL`, vermutlich Person vor dem Sensor).

## Bedienung und Werkzeuge

- **Bauen + testen:** `cd android && ./gradlew :app:testDebugUnitTest :app:assembleDebug` (JVM-Tests lesen die
  Fixtures aus `RADAR_FIXTURES` oder `~/Data/radar-sensor/app-fixtures`).
- **Installieren (nur mit Freigabe):** `adb -s <serial> install -r app/build/outputs/apk/debug/app-debug.apk`.
- **Debug-Steuerung ohne Bildschirm** (nur Debug-Build, nur Shell wegen Berechtigung `DUMP`):
  `adb shell am broadcast -a de.timo.radarsensor.DEBUG -n de.timo.radarsensor/.debug.DebugReceiver --es cmd <befehl>`
  mit `status`, `usb`, `disconnect`, `send --es line "'iq3 0 raw'"`, `stop`, `rec_start --es name <name>`, `rec_stop`,
  `dsp_replay --es name <aufnahmeordner>`; EKG-Gurt (V0.1) `ref_scan`, `ref_connect --es addr <mac>`, `ref_disconnect`,
  `ref_status`; Marker und Metronom (V0.2) `mark --es text anhalten_start`, `metro --ei rate 15` (6/10/15/20, 0 = aus);
  Handy-DSP (V5.2) `phone_dsp --ez on true|false`, `phone_dsp_status` (Puls, Zustand, CPU-Anteil, ESP32-Wert daneben).
  Antwort steht im `data="…"` der Broadcast-Ausgabe.
- **EKG-Gurt und Marker (V0.1/V0.2, Vital-Tab):** „Gurt suchen" findet Geräte mit Heart Rate Service (z. B. Polar
  H10), Antippen verbindet; reißt die Verbindung ab, verbindet Android von selbst wieder. Jede Benachrichtigung geht in
  die laufende Aufnahme (`ref_hr.csv`: `rx_ms,hr_bpm,rr_ms,contact`, eine Zeile je RR-Wert) und als grüne Kurve „Gurt"
  ins Pulsdiagramm. Knöpfe „Atem anhalten", „Bewegung", „Zählfenster" schreiben Start/Ende nach `events.csv`
  (`anhalten_start`/`_ende`, `bewegung_…`, `zaehlen_…`, so liest `host/vital_eval.py` sie); das Metronom (6/10/15/20
  /min) piept zum Einatmen und schreibt je Ton `ein`. Beide Dateien entstehen erst mit dem ersten Wert, Zeitachse wie
  `rxtime.csv`. Berechtigungen Bluetooth (Suche ohne Standort) fragt die App beim Start.
- **Handy-DSP live (V5.2, Vital-Tab):** „Handy-DSP ein" rechnet jeden empfangenen Frame eines `iq3 … raw`-Streams
  mit demselben Firmware-Header am Handy nach (Bibliothek `libradardsp_live.so`, `host/dsp_replay/dsp_live.h`), aber
  mit den schwereren Varianten (`A121_DSP_HEAVY` = Viterbi-Verfolgung V3.2 + Abzug der Atem-Oberwellen V3.3). Er
  rechnet an denselben Stellen wie der ESP32 (an dessen `@DSP`-Zeilen, aus demselben Schnappschuss) und folgt
  Rezoom/Neu-Kalibrierung; die Werte erscheinen als gelbe Kurve „Handy" neben „fused" im Pulsdiagramm, dazu
  Konfidenz, Zustand, „Handy − ESP32" und „Handy − Gurt" (60 s) und der CPU-Anteil an einem Kern (Thread-CPU-Zeit je
  Sekunde). Einschalten mitten im Stream geht (Start- und letzte Rezoom-Zeile werden nachgereicht, erster Wert nach
  ≈ 6 s). Eigener Rechen-Thread, der Empfang wartet nicht. Standard aus (Akku). Nicht in der Aufnahme: dieselben
  Werte rechnet der PC aus `stream.log` nach (`vital_eval.py --replay` mit einer Header-Kopie, vorn
  `#define A121_DSP_HEAVY 1`, wie in `EXPERIMENTS.md`); Handy = PC war in A5 bytegleich, für diese Variante nicht geprüft.
- **Aufnahmen holen:** `adb pull /sdcard/Android/data/de.timo.radarsensor/files/recordings/<ordner>
  ~/Data/radar-sensor/phone/`. Schreiben in den App-Ordner (z. B. Fixtures für die Wiedergabe) nur über
  `run-as de.timo.radarsensor`.
- **PC:** `host/esp_record.py` (Aufnahme im App-Format, `--check` prüft Lücken/Ticks/`@RAW`, `--npz` für verify-cpp),
  `host/esp_tcp_bridge.py` (ESP32 am PC → App über Tailscale, Port 7777), `host/a121_esp_ultimate.py --mode
  replay-cpp --log <stream.log>` (gleiche Nachrechnung wie „DSP neu“ in der App).

## Offen

- BLE-Transport; Akkuverbrauch am Handy messen.
- Gütezustand (V5.1): Logik (Puls grau, Zustandstext, Abweichung Radar − Gurt über 60 s, Paare ±2 s) und Parser
  `@STATE`/`@BREATH` am PC getestet; Anzeige (Pages.kt) nicht übersetzt. Abnahme „Screenshot je Zustand aus einer
  Wiedergabe" offen (Handy oder Emulator).
- EKG-Gurt, Marker, Metronom (V0.1/V0.2): Parser, Recorder-Dateien, Metronom-Takt und Referenzkurve am PC getestet;
  BLE-Client und Metronom gegen die Android-Klassen (API 34) übersetzt; Controller, Debug-Befehle und Oberfläche
  nicht übersetzt (kein Android-SDK in der Cloud-Umgebung). Offen am Handy (nur mit Freigabe): 10 min `iq3 0 raw` mit
  Gurt — `ref_hr.csv` vorhanden, Summe der RR = Aufnahmedauer ±2 %, Puls = Polar-App ±1 /min; Abstand der
  Metronom-Marker = Sollwert ±20 ms.
- Handy-DSP live (V5.2): Live-Pfad am PC gleich der Nachrechnung (`host/tests/test_dsp_live.py`, Rechenzeit am PC
  0,29 % eines Kerns für 120 s simulierte Daten mit `A121_DSP_HEAVY`); JNI-Bibliothek am PC gegen die JDK-`jni.h`
  übersetzt und aus Kotlin getestet (Zuführung, Neubeginn, CPU-Anteil). Nicht übersetzt: NDK-Build, Controller,
  Debug-Befehle, Oberfläche. Offen am Handy (nur mit Freigabe): 30 min `iq3 0 raw` live mit „Handy-DSP ein",
  CPU-Anteil ≤ 20 % eines Kerns (`phone_dsp_status`), Handy- und ESP32-Wert nebeneinander (Screenshot). Schätzung aus
  A5 (Handy ≈ 3,5 × langsamer als der PC): ≈ 1 % — nicht gemessen.
- Knopf „DSP neu“ unter Aufnahmen nur per Debug-Befehl geprüft, nicht angesehen (Handy war gesperrt).
- Doppler-Tab: Achsenbeschriftung der Amplitude endet unter dem Maximum (Linien selbst werden nicht abgeschnitten).

## Risiken und Regeln

- **Nie beide USB-Ports des ESP32 gleichzeitig** (IN-OUT-Brücke geschlossen, zwei 5-V-Quellen evtl. verbunden).
- Am Handy lädt das Handy nicht; Verbrauch ungemessen (grob 1,5–2 W Last).
- Abziehen startet den ESP32 neu; laufende Streams beginnen mit neuem Frame-Zähler und tick.
- Kabelbewegung erzeugt Messartefakte (später Handy-Beschleunigungssensor, Idee I4).
- Puls bleibt ein Rhythmus-Kandidat; keine Genauigkeitsaussage ohne Referenzmessung.
- Build/Tests am PC sind keine Laufzeitprüfung am Handy; beides getrennt ausweisen.

## Freigaben

- **Testphase (Projektinhaber, 2026-11-01):** Radar-Debug-App installieren/aktualisieren, starten/beenden, Logs lesen,
  Screenshots, USB-Zugriffsabfrage per ADB bestätigen. Andere Apps und Einstellungen nicht anfassen.
- Flashen des ESP32 nur nach Identifikation über `/dev/serial/by-id`.
