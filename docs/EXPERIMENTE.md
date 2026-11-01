# Experimente — A121 ausreizen

Fähigkeiten-Matrix für Phase 1. Werkzeug: Exploration Tool / `acconeer-exptool` (geprüft:
Version 7.18.2 auf PyPI, Python ≥ 3.9, am 2026-10-27). Algorithmus-Module dort:
`distance, presence, smart_presence, breathing, phase_tracking, sparse_iq, vibration, speed,
surface_velocity, hand_motion, touchless_button, tank_level, waste_level, parking, obstacle,
cargo, bilateration`.

Ergebnis-Spalte erst nach echter Messung füllen. Aufnahmen nach `~/Data/radar-sensor/`.

## Grundlagen

| # | Experiment | Modul | Messgröße | Ergebnis |
|---|---|---|---|---|
| G1 | Rohsignal verstehen: Profile 1–5, Schrittweite, HWAAS, Sweep-Rate | sparse_iq | Rauschen, Reichweite, max. Rate | — |
| G2 | Entfernungsgenauigkeit vs. Maßband (0,1–5 m, Metallplatte) | distance | Abweichung mm | Teil 1 (2026-10-30): 1,00 m Backsteinwand, ohne Kappe → Median 1,0016 m, σ 2,9 mm, Spanne 0,995–1,009 m (100/100 Frames). Teil 2: ca. 600 mm (Maß nur ungefähr) → Wand 0,5847 m, σ 1,7 mm, nur 56/100 Frames; zusätzlich Echo bei ~1,60 m (Ursache offen). Teil 3: 570 mm (Maßband) → 0,5589 m (−11 mm), σ 1,2 mm, 100/100 Frames. Echo ~1,58–1,60 m danach nur schwach und wechselnd (deutlich nur bei flacher Abdeckung); Ursache offen. 3 m räumlich nicht möglich. Teil 4 (2026-10-31, über ESP32 + `i2c_distance_detector`, CFAR, Bereich 300–1000 mm, hyperbolische Linse, 570 mm Maßband): Median 575 mm (+5 mm), σ 1,3 mm, 50/50 Messungen, Log `2026-10-31_105545_esp_i2c_wand570.log`. Weitere Abstände/Metallplatte offen. |
| G3 | Maximale Reichweite Mensch / Metall / Wand | distance, sparse_iq | m | — |
| G4 | Zwei Ziele trennen (Abstand verringern) | distance | min. Abstand cm | — |
| G5 | Kleinste messbare Bewegung (Mikrometerschraube/Lautsprecher) | phase_tracking | µm | — |
| G6 | Strahlbreite ohne Linse / Fresnel / hyperbolisch | sparse_iq | Winkel, Reichweite | Echostärke Backsteinwand ~570 mm (2026-10-30, Profil 2, HWAAS 32, je 50 Frames; Detektor je 100 Frames, 100/100 erkannt): nackt 2318 / SNR 33,9 dB / 0,5589 m σ 1,2 mm · flach +0,7 dB / 34,2 dB / 0,5627 m σ 1,6 mm · Fresnel +8,7 dB / 42,9 dB / 0,5643 m σ 0,6 mm · Pyramide +10,0 dB / 44,3 dB / 0,5637 m σ 0,7 mm · hyperbolisch +11,3 dB / 45,2 dB / 0,5585 m σ 0,4 mm. Echobreite (FWHM) überall 78–85 mm. Abstandsversatz je Kappe uneinheitlich (Ursache offen: Position/Mechanik). Kappenwechsel bei „ca. gleicher“ Position; Wiederholung ohne Umbau (hyperbolisch): 8508 → 8495, Detektor 0,5585 → 0,5581 m — Messung selbst sehr stabil. Winkel noch nicht gemessen. Skript: `host/a121_zielmessung.py`. |

## Mensch und Vitalwerte

| # | Experiment | Modul | Messgröße | Ergebnis |
|---|---|---|---|---|
| M1 | Präsenz im Raum, Zonen, ruhig sitzend vs. bewegt | presence, smart_presence | Trefferquote, Reichweite | — |
| M2 | Atmung sitzend/liegend, 0,3–1,5 m | breathing | Atemzüge/min vs. Zählen | Sitzend ~250–350 mm (Sparse-IQ-Phase, 2026-10-30): 20,0–20,5 /min (Spitze-Spitze 0,46–1,5 mm). Sitzend ~675–760 mm über ESP32-I2C (`vital` / `auto`, 37,1–45,5 Hz, 2026-10-31): **20,5–22,5 /min** (Hub **2,42 mm**). Sitzend ~673 mm über A121-USB (`host/a121_ultimate_vital.py`, 3-Subsweep Sparse-IQ @ 60,0 Hz, `2026-10-31_a121_usb_3subsweep_ultimate.npz`): **21,5 /min** (Hub Spitze-Spitze **2,29 mm**, 0 Apnoe-Episoden). Sitzend ~0,58–0,60 m, ESP32 allein über LPUART1 (2026-11-01): Rohdaten **18,3–18,4 /min** (Hub 3,4 mm), `iq3`-Median **20,3 /min**. `iq3` lieferte bis dahin in rund einem Viertel der Reports fälschlich 6,9 /min (Bandrand, Bug 6 in `docs/HARDWARE.md`). Nach der Korrektur live (2026-11-01, Brust ≈ 0,68 m, `2026-11-01_iq3_atmung_live1.log`): **17–24 /min**, auf der Brust alle Werte innerhalb ±2 /min der Zyklenzählung im selben Radarsignal (keine unabhängige Referenz). |
| M3 | Puls sitzend, 40/60/80 cm, mit/ohne Linse | phase_tracking, sparse_iq (eigene Auswertung) | BPM vs. Smartwatch | **Aufnahme A** (`2026-10-30_vital_A_250mm_hyperbolisch_atmen.npz`, ~250 mm, hyperbolisch, 60 s): Mit Phasenstabilitäts-Bin-Auswahl (Bin 55 statt 57), I/Q-Kreisfit + regularisierter DACM-Demodulation, Hilbert-Projektion der momentanen Atmungs-Oberwellen ($m=1\dots 6$) und Beschleunigungs-HPS ($f_0\cdot 2f_0\cdot 3f_0$) in `host/a121_vitalaufnahme.py` wird der stationäre Kandidat **82,5 /min** (HPS **83,0 /min**) zum primären Peak (vorher war die 3. Atmungs-Oberwelle bei 62 /min stärker). Gleitende 15-s-Fenster zeigen: während die Atmung zwischen 18,0 und 22,5 /min schwankt ($4 f_b = 72\dots 90\text{ /min}$), bleibt der 83-/min-Kandidat frequenzstabil. **Aufnahme B** (`2026-10-30_201219_live.npz`, ~20 s atmen / ~17,5 s anhalten / ~20 s atmen): Der scheinbare $\pm 500\text{ }\mu\text{m}$-Ausbruch bei $t=36\text{–}42\text{ s}$ im naiven Amplitudenmaximum (Bin 79, 348 mm) war ein destruktiver Mehrwege-Einbruch ($\min|z|=48$). Auf dem phasenstabilsten Bin 110 (425 mm, CV 0,38) bleibt das Pulsband über den gesamten Anhalte-Abschnitt (28,1–45,6 s) gleichmäßig bei 62–75 µm RMS; Spektrum **76,0 /min**, Beschleunigungs-HPS **76,5 /min** (Nebenpeaks < 4 %), Autokorrelation **75,0 /min** ($r=0{,}75$), 22 Einzelpulse mit $0{,}770 \pm 0{,}072\text{ s}$ (CV 9,4 % → **77,9 /min**) und Oberwellen bis $6 f_0 = 7{,}52\text{ Hz}$ (`host/a121_anhalteanalyse.py`). **Aufnahme C über ESP32-I2C** (`2026-10-31_esp_i2c_brust_hq_45s.npz`, `2026-10-31_esp_i2c_brust_pos2_40s.npz`, `2026-10-31_esp_auto_multiinfo.npz`, `2026-10-31_esp_auto_reconnected.npz`, ~675–762 mm sitzend vorm PC, `vital`/`auto` @ 37,1–45,5 Hz): Gated Scatterer Tracking trennt Vorderbrust und Sekundärziel ohne Zieltausch-Sprünge ($\sigma$ auf 0,85–1,4 mm gesenkt); mit gleitender 7-s-Hann-Hilbert-Atmungsprojektion liefern Weg ($\Delta d$: **79,0 /min**) und Echo-Stärke ($\text{dB}$: **81,5 /min**) in `2026-10-31_esp_auto_reconnected.npz` bei 729 mm einen konsistenten Fusions-Kandidaten bei **80,5 /min** (3:1 vor zweitem Peak 0,33; Autokorrelation **81,8 /min**, Konfidenz 72 %). **Aufnahme D mit 3-Subsweep Sparse-IQ über A121-USB** (`2026-10-31_a121_usb_3subsweep_ultimate.npz`, 45 s @ 60,00 Hz, `host/a121_ultimate_vital.py`, Sub 0 On-Chip-Loopback + Sub 1 2,5-mm-Vital-Zoom `PROFILE_1`, `PRF_19_5_MHz`, `phase_enhancement=True`, `iq_imbalance_compensation=True` + Sub 2 Vollraum-Scan `150–2070 mm`): Auto-Search lokalisiert Brustkorb bei 690 mm (`|IQ|=2308`), kohärentes Multi-Bin-Zentrum bei **672,5 mm** (`|IQ|=1004`, 60,0 dB), Loopback kompensiert **14,6 µm** thermische PLL-Drift. Nach gleitender 7-s-Hilbert-Atmungsprojektion sind die 3. und 5. Atmungs-Oberwelle um >90 % unterdrückt; während normaler Atmung (21,5 /min, 2,29 mm Hub) zeigt sich ein dominanter Herzschlag-Kandidat bei **84,5 /min** (Spektrum/HPS 0,97 vs. zweiter Peak 0,14), Autokorrelation **81,8 /min** ($r=0{,}77$) und im Zeitbereich **28 Einzelschläge in 20 s** mit IBI-Median **716,7 ms** (**83,7 /min**), Variationskoeffizient **CV 9,4 %**, RMSSD **102,0 ms** und Pulswellen-Hub **106,6 µm**. **Aufnahme E, ESP32 allein über LPUART1** (2026-11-01, mobiler Aufbau, sitzend vor dem Sensor; Daten `iq3_person_2026-11-01.log`, `iq3_breathhold_2026-11-01.log`, `person_scan_2026-11-01.npz`, Bild `img_herzschlag_atemhalten_2026-11-01.png`). **Referenz: Puls selbst gezählt, 38 Schläge in 30 s = 76 /min (laut Testperson unsicher).** *Abstand:* Rohdaten-Scan (0,25–2,5 m) zeigt die stärkste atmende Bewegung bei **580 mm** (3,4 mm Hub, 18,4 /min). `iq3` findet das Ziel ohne Vorgabe, Median **601 mm** (10–90 %: 580–610 mm), erster Rezoom nach 3 s. Danach springt der Zoom aber 5–7-mal pro Minute (teils auf 0,71–0,80 m), und jeder Rezoom löscht die Signalhistorie. *Puls bei normaler Atmung (70 s):* `iq3`-Median **81,3 /min** (Spektrum 81,8, Autokorrelation 74,7, Zeitbereich 81,1). Die Einzelwerte streuen aber 53,8–128,2 /min, nur 39 % liegen innerhalb ±8 /min der Referenz, 15 % am Rand des Suchbands. Ursachen: Hier lag die Herzfrequenz ≈ 4 × Atemfrequenz (4 × 18,3 = 73 /min); dazu Rezooms und die DSP-Pausen. In den Rohdaten ist die Komponente bei 75 /min 9–13 dB stärker als die 2. und 3. Atem-Oberwelle, ein Hinweis auf den Herzschlag. *Puls bei angehaltenem Atem (~10 s):* `iq3` liefert **73–81 /min**, die drei Schätzer liegen eng beieinander, Konfidenz 91–99 %. Eigene Auswertung des ESP32-Wegsignals (13 s, Zeitachse über den Frame-Zähler): Spektrum **72,1 /min** (Peak/Median 29), **15 Einzelschläge**, IBI 683–1000 ms, Median 833 ms = **72,0 /min**. Fazit: Der Herzschlag ist im Signal, und bei angehaltenem Atem isoliert der ESP32 ihn. Bei normaler Atmung trennt er ihn noch nicht zuverlässig. Eine Messung, eine Person, Referenz unsicher, also keine Genauigkeitsaussage. Auswertungs-Fallstrick: Die Zeitstempel in `@IQ3` sind ESP32-Verarbeitungszeit. Nach jeder DSP-Pause kommen Frames gebündelt, also für Analysen den Frame-Zähler × 60 Hz nehmen (sinngemäß gilt das auch für `dtS` in `pushFrame()`). **Aufnahme F, ESP32-DSP überarbeitet** (2026-11-01, `firmware/esp32s3/src/a121_esp_dsp.h`). Referenz jeweils selbst gezählt (je 30 s). Daten: `iq3raw_normal_120s_2026-11-01.npz` (Rohdaten, Zählung 76/80, Entwicklung), `iq3raw_motion_120s_2026-11-01.npz` (Rohdaten, Zählung 70/78, Atmung ≈ 20 /min, dazwischen 10 s absichtliche Bewegung), Live-Logs `iq3_live_newdsp_2026-11-01.log` (86/78), `iq3_rezoomtest_2026-11-01.log` (ohne Zählung), `iq3_live_harm_2026-11-01.log` (74/78). *Vorgehen:* Die Rohdaten laufen am PC durch denselben C++-Code wie auf dem ESP32 (Ergebnis bitgleich). Bewertet wird der verfolgte Puls gegen die Zählfenster. Zusätzlich drei Dateien aus dem Entwicklungsdatensatz mit künstlich eingefügter Bewegung (Phasendrehung der Zoom-Punkte: Zappeln mit 63-/min-Anteil, 2-mm-Lagewechsel, Armbewegung). *Änderungen:* (1) alle 24 Zoom-Punkte einzeln, Nullphasen-Bandpass 0,8–3 Hz, Spektren gewichtet gemittelt, Zeitbasis fest 60 Hz; (2) Spitzen am Bandrand zählen nicht (häufigste Fehlspitze war 54 /min = 0,90 Hz, direkt an der Suchgrenze); (3) Bewegungsdämpfung je 1-s-Block, Bezug ist das ruhige Viertel des Fensters; (4) Atemfrequenz aus allen Punkten (vorher oft am Bandrand 6 /min), und ein Sprung auf eine Atem-Oberwelle ($m = 2\dots 6$) gilt erst nach 10 statt 3 Reports. *Ergebnis (Anteil innerhalb ±8 /min, in Klammern ±5 /min):* Entwicklungsdatensatz: Original 32 % (19 %), erste Fassung 84 % (81 %), jetzt **100 % (93 %)**. Neuer Datensatz (an ihm wurde die Oberwellen-Haltezeit entwickelt, also kein unabhängiger Test): Original 42 % (24 %), erste Fassung 42 % (8 %), jetzt **89 % (50 %)**. Künstliche Bewegung: erste Fassung 74–84 %, jetzt 91–100 %. **Live am ESP32, einzige unabhängige Prüfung** (`iq3_live_harm`, 100 s, ein automatischer Rezoom auf 630–690 mm): Median **74,6 /min** bei Zählung 74 und **78,4 /min** bei Zählung 78, alle Reports im Zählfenster innerhalb ±5 /min, keine Sprünge über 15 /min, Rechenzeit 307 ms je Report. Bei 31–39 s sprang die stärkste Spitze auf 55–58 /min (3 × Atmung), der verfolgte Wert blieb bei 78. *Was die Fehler waren:* Im Rezoom-Test hing der Puls bei 60–63 /min. Grund waren Bewegung in den ersten 8 s (Herzband 700–1100 µm statt ≈ 200 µm) und eine Verfolgung, die nicht losließ. Im neuen Datensatz lag bei schneller Atmung die 3. Atem-Oberwelle bei 55–60 /min und überdeckte den Puls zeitweise ganz. *Verworfen:* Haltezeit abbrechen, wenn die Punkte den alten Wert nicht mehr stützen (mehr Sprünge, rhythmische Bewegung wird von allen Punkten gestützt). Bei Oberwelle auf den nächsten Kandidaten ausweichen (ohne Wirkung). Oberwellen herausrechnen (entfernt den Puls mit, wenn er nahe $m \cdot f_{\text{Atem}}$ liegt; live lag er bei 4 × 19,5). *Offen:* Im neuen Datensatz zeigen alle Fassungen im Fenster A stabil ≈ 77 /min bei Zählung 70; ob Zählung oder Messung abweicht, ist ungeklärt. Während deutlicher Bewegung ist der Wert falsch (bis 100 /min), die Konfidenz fällt dann aber auf 0–12 %. Bleibt der Puls dauerhaft auf einer Oberwelle, folgt die Verfolgung erst nach 10 s. Zwei Personen-Datensätze und zwei Live-Läufe einer Person: Das belegt eine Verbesserung, aber keine Genauigkeit. `@DSP` meldet jetzt zusätzlich den Bewegungsanteil (%) und `hr_harm` (Spitze liegt auf einer Atem-Oberwelle; das heißt nicht, dass der Wert falsch ist). **Wichtig:** Alle Ergebnisse sind Rhythmus-Nachweise im Radarsignal; **ohne Referenzmessung (Smartwatch/Pulsoximeter/EKG) keine Aussage über Puls-Genauigkeit.** |
| M4 | Gesten / Wischen / berührungsloser Knopf | hand_motion, touchless_button | Erkennungsrate | — |
| M5 | Referenzdatensatz Puls/Atmung mit EKG-Brustgurt (Plan V0.3 in `docs/AUDIT_VITAL.md`, Ablauf unten) | `iq3 0 raw` + Polar H10 | MAE, ±5-Anteil, Verfügbarkeit, Kalibrierung, Apnoe-Verzug | — (Aufnahmen offen) |

### M5 Referenzdatensatz

Ziel: Genauigkeit des Pulses (gegen EKG-Brustgurt) und der Atmung (gegen Metronom) messen und jede DSP-Änderung
daran prüfen. Ohne diesen Datensatz gibt es keine Aussage über Puls-Genauigkeit (`AGENTS.md`).

**Aufbau:** ESP32-S3 + A121 wie in T1, Handy-App nimmt `iq3 0 raw` auf (Rohdaten, damit jede Header-Fassung
nachgerechnet werden kann). Brustgurt Polar H10 (BLE Heart Rate Service) über die App koppeln (Referenz-Karte im
Vital-Tab oder `adb shell am broadcast … ref_scan` / `ref_connect --es addr <mac>`, `docs/ANDROID_APP.md`); die App
schreibt `ref_hr.csv` (rx_ms, Puls, RR-Intervalle) und `events.csv` (Marker) in denselben Aufnahme-Ordner.
Gurt vor Beginn anfeuchten, 2 min tragen, bis die Polar-App einen stabilen Wert zeigt.

**Ablage:** `~/Data/radar-sensor/ref/<datum>_<tag>_<sitzung>_<person>/` (z. B. `2026-11-08_A_R1_p1`).
Personen nur pseudonym (`p1`, `p2`), weitere Personen nur mit Einwilligung.

**Trennung:** Tag A = Entwicklung (darf beliebig oft ausgewertet werden), Tag B = Test (nur zur Abnahme einer Phase
auswerten). Beide Tage mit denselben Sitzungen, an verschiedenen Tagen aufgenommen.

| Sitzung | Inhalt | Dauer | Marker | prüft |
|---|---|---|---|---|
| R1 | ruhig sitzend, normal atmen, Brust 0,6 m vor dem Sensor | 5 min | — | Grundgenauigkeit |
| R2 | 3 × 20 s Atem anhalten, dazwischen je 40 s normal | 3 min | „Atem anhalten" Start/Ende | Apnoe-Erkennung, Puls ohne Atmung |
| R3 | Metronom 6 / 10 / 15 / 20 /min, mit jedem Ton einatmen | je 2 min | Metronom (setzt „ein" je Ton) | Atmung, Oberwellen-Kollision |
| R4 | 1 min Kniebeugen (ohne Sensor), dann sofort sitzend erholen | 6 min | „Zählfenster" Start mit dem Hinsetzen | Pulsbereich 110 → 70, Verzug |
| R5 | je 20 s: Arm heben, zurücklehnen, husten, sprechen, dazwischen ruhig | 3 min | „Bewegung" Start/Ende | Bewegung, Zustand, Konfidenz |
| R6 | Abstand 0,4 / 0,6 / 0,8 / 1,0 m (Maßband) | je 3 min | — | Reichweite |
| R7 | ohne Linse / hyperbolische Linse, 0,6 m | je 3 min | — | Linse |
| R8 | liegend, Sensor ≈ 0,5 m über der Brust | 5 min | — | Haltung |

Ablauf je Sitzung: Gurt verbunden (Puls in der App sichtbar) → Aufnahme starten → `iq3 0 raw` → Sitzung → Stop.
Danach prüfen und auswerten:

```bash
.venv/bin/python host/esp_record.py --check ~/Data/radar-sensor/ref/<ordner>
```

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag A>_* --md ~/Data/radar-sensor/ref/bericht_A.md
```

Eine Header-Variante gegen dieselben Daten (vor dem Flashen):

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag A>_* --replay <ordner mit a121_esp_dsp.h>
```

**Fertig, wenn:** beide Tage aufgenommen, `esp_record.py --check` ohne Lücken, `vital_eval.py` liefert für jede
Sitzung einen Bericht.

**Basislinie (V0.6):** DSP-Stand der Firmware `2026-11-01-app1` auf Tag A und Tag B; Tabelle je Sitzung mit Puls-MAE,
Anteil ±5, Verfügbarkeit, Kalibrierung (Konfidenz ≥ 70 % → Anteil ±5) hier eintragen. Weil die Aufnahmen `@RAW`
enthalten, geht das auch mit neuerer Firmware: den alten Header nachrechnen lassen.

```bash
mkdir -p /tmp/dsp_basis && git show 0e2d836:firmware/esp32s3/src/a121_esp_dsp.h > /tmp/dsp_basis/a121_esp_dsp.h
```

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag>_* --replay /tmp/dsp_basis
```

**Rezoom-Stetigkeit (V2.3 Schritt 1, offen):** Bleibt die Phase an gleichen Entfernungen über den Session-Neustart
beim Rezoom stetig (roh und mit Loopback-Derotation)? An T1 (31,7 min, 38 Rezooms, `@RAW`) prüfen; Ergebnis hier
eintragen. Nur wenn die Sprünge klein sind (oder eine gemeinsame Drehung, Streuung klein), lohnt Schritt 4
(überlappende Zoom-Punkte übernehmen, Lücke füllen).

```bash
.venv/bin/python host/rezoom_check.py ~/Data/radar-sensor/<T1-ordner>
```

**Fensterlänge (V2.5, offen):** 10 / 12 / 15 s an Tag A (R4: Verzug, bis der Wert 50 % eines Pulsabfalls
nachvollzogen hat; Ziel ≤ 7 s) vergleichen; kürzestes Fenster ohne MAE-Verlust > 0,5 /min wählen. Variante = Header
mit vorangestelltem `#define A121_HR_WIN 600` (bzw. 720) in einem eigenen Ordner:

```bash
mkdir -p /tmp/dsp_win10 && { echo '#define A121_HR_WIN 600'; cat firmware/esp32s3/src/a121_esp_dsp.h; } > /tmp/dsp_win10/a121_esp_dsp.h
```

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag A>_R4_* --replay /tmp/dsp_win10
```

Prüfstand vorab (nur Simulation, `vital_sim.py bench --hr-win 600|720|900`): Reaktion im Szenario `step` 7 / 8 / 9 s;
mit 10 s verletzt `beweg` (MAE 1,5), `anhalt` (Apnoe nicht erkannt, ±5-Anteil 96 %) und `leer` (1 gültiger Report)
die Grenzwerte, mit 12 s nur `beweg` knapp (MAE 1,4 statt ≤ 1,3), 15 s hält alle. Standard bleibt 15 s bis Tag A.
Mit der Viterbi-Verfolgung (V3.2, `--define A121_DSP_VITERBI`) hält auch 12 s alle Grenzwerte (Reaktion 8 s), 10 s
weiter nicht (`beweg`, `anhalt`, `leer`). Für Tag A: 12 s + Viterbi als eigene Variante mitrechnen.

**Viterbi-Verfolgung (V3.2, offen):** Variante hinter `#define A121_DSP_VITERBI` (Standard aus): Viterbi über die
letzten 20 Reports, Zustände 40–180 /min, Übergang Gauß σ = 3 /min je s plus 2 % „Sprung irgendwohin", Emission aus
den Kandidaten des Reports (Gauß je Kandidat, Gewicht (Wert / bester)³, Grundpegel 10 %), Konfidenz höchstens die
Wahrscheinlichkeitsmasse ±5 /min. Abnahme an Tag A (alle Sitzungen): weniger Sprünge > 15 /min zwischen
aufeinanderfolgenden gültigen Reports (`vital_eval.py` schreibt sie in die Kopfzeile je Sitzung), Anteil ±5 /min nicht
schlechter. Prüfstand vorab (nur Simulation): Sprünge gleich (3 / 3), `brady` MAE 2,8 → 1,5 /min (±5: 95 → 97 %),
`beweg` 0,8 → 0,6, sonst gleich; falsche Reports (> 10 /min) 9 → 5; Rechenzeit am PC +0,2 ms.

```bash
mkdir -p /tmp/dsp_vit && { echo '#define A121_DSP_VITERBI 1'; cat firmware/esp32s3/src/a121_esp_dsp.h; } > /tmp/dsp_vit/a121_esp_dsp.h
```

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag A>_* --replay /tmp/dsp_vit
```

**Atem-Oberwellen geführt entfernen (V3.3, offen):** Variante hinter `#define A121_DSP_HARM_REMOVE` (Standard aus): je
Zoom-Punkt vor der Puls-FFT (15 Hz) die Oberwellen m = 1..8 der Atmung abziehen, Phase = Winkel des analytischen
Signals der Atem-Grundwelle im Weg des Punkts (FFT, Band 0,2–1,8 × Atemfrequenz des vorigen Reports). m bleibt, wenn
Puls oder 2 × Puls in m × [kleinste, größte Atemfrequenz im Fenster] ± 0,1 Hz liegt; nur nach einem gültigen Report mit
Konfidenz ≥ 50 % (sonst schonte die Regel einen falschen Puls und löschte den echten). Abnahme an Tag A: R3 (Metronom)
besser, R1 nicht schlechter. Rechnung vorab (nur Simulation): bei ±23 % Ratenschwankung bleiben 12 % der Oberwellen
(feste Frequenz: 100 %), bei gleichmäßiger Atmung ≈ 10 % (feste Frequenz 24 %); dem Puls nimmt sie < 13 %. Prüfstand:
Puls-Kennzahlen wie ohne Variante (Oberwellen sind dort schwach), Konfidenz-Median richtiger Reports 82 → 100 %,
falscher unverändert 31 %; Rechenzeit am PC +0,7 ms. Als Variante: `#define A121_DSP_HARM_REMOVE 1` voranstellen wie
oben.

**Zoom-Punkte kombinieren (V3.1, offen):** Variante hinter `#define A121_DSP_COMBINE` (Standard aus): die 12 Punkte mit
der stärksten Bewegung um ihre Kreismittelpunkte verschieben (Punkte ohne sauberes Echo bleiben draußen), 1.
Hauptkomponente ihrer IQ-Werte (richtet die Punkte phasengleich aus und gewichtet sie mit der Amplitude, bei gleichem
Rauschen je Punkt = Maximum Ratio Combining), dann **ein** Kanal demodulieren statt 24 Einzelspektren. `@DSP` trägt
dann als 15. Feld die effektive Punktzahl N_eff = (Σλ)² / Σλ² der Kovarianz (≈ 1: alle Punkte sehen dasselbe Echo, mit
Person oder ruhendem Echo; ≈ 12: nur Rauschen); `vital_eval.py` meldet ihren Median je Sitzung. Abnahme an Tag A:
Puls-MAE ≤ ohne Variante bei ≤ 50 % Rechenzeit (ESP32 live, `compute_us` in `@DSP`). Prüfstand vorab (nur Simulation):
Rechenzeit am PC 1,15 → 0,48 ms (42 %); Puls-MAE `base` 0,36 → 0,29, `brady` 2,81 → 1,51, `tief` 0,60 → 0,34, `step`
0,74 → 0,77, `anhalt` 0,41 → 0,51 (beim Anhalten 4 gültige Reports mehr, Fehler 2–4 /min), sonst gleich; Konfidenz-Median
richtiger Reports 82 → 94 %, falscher 31 → 25 %. Erster Wert bei `step` und `tief` nach 7 statt 6 s: Der kombinierte
Kanal ist atemlastig (die Hauptkomponente maximiert die Leistung, und die trägt die Atmung), im 6-s-Fenster liegt die
stärkste Spitze dort auf einer niedrigen Atem-Oberwelle. Seit V3.1 gilt ein vorläufiger Wert (V2.4) nie auf einer
Atem-Oberwelle m ≤ 3, auch ohne Schalter (dort im Prüfstand ohne Wirkung): AKF und Einzelschläge desselben Signals
bestätigten sie mit (`base` mit Variante: 46 statt 71 /min gültig). Raum-Punkte als zweiter Kanal sind nicht umgesetzt
(der DSP hält für sie keine Historie). Als Variante: `#define A121_DSP_COMBINE 1` voranstellen wie oben.

**Einzelschläge mit Vorlage (V3.6, offen):** Variante hinter `#define A121_DSP_IBI_TEMPLATE` (Standard aus): Herzsignal
des klarsten Punkts im Band 0,6–8 Hz (statt 0,6–5 Hz), Atem-Oberwellen wie bisher abgezogen; Vorlage = Mittel der
Abschnitte ±0,3 × Schlagabstand um die ersten Spitzen (ohne Gleichanteil), Korrelationsfilter mit der Vorlage, dessen
Spitzen parabolisch verfeinert, dann dieselben Artefakt-Regeln wie V1.6. Abnahme an Tag A (alle Sitzungen mit Gurt):
IBI-Fehler gegen Gurt-RR halbiert gegenüber V1.6. Werkzeug: `vital_eval.py` schreibt je Sitzung „Einzelschläge gegen
Gurt-RR" (je gültigem Report IBI-Median und RMSSD gegen die Gurt-RR im selben 15-s-Fenster; Reports, deren IBI nur der
Rückfallwert 60 000 / Puls ist, zählen nicht); mit `--replay` und dem Schalter wie oben gegen den Standard vergleichen.
Rechnung vorab (nur Simulation, Fehler je Schlag gegen die wahren Schläge, 6 Fenster): in weißem Rauschen Median-Faktor
0,81 (V1.6 liegt dort schon nahe der Cramér-Rao-Grenze, ≈ 7 ms je Schlag bei σ = 0,15 × Spitze-Spitze; besser geht es
mit keinem Verfahren), bei Atem-Resten im Herzband 0,58 (0,44–0,82) — halbiert also nicht überall. Prüfstand: Anteil der
Reports mit RMSSD (≥ 90 % gültige Schläge) z. B. `base` 17 → 64 %, `tachy` 3 → 75 %, `tief` 20 → 85 %; RMSSD
gemessen / wahr näher an 1 (`tachy` 1,30 → 0,98); Puls-MAE `brady` 2,8 → 3,2, `tief` 0,6 → 0,3. `tief` verwirft dabei
den vorläufigen 6-s-Wert 40,3 /min (wahr 71), den der Standard gelten lässt: Dessen IBI-Bestätigung rechnet mit der
Sperrzeit des Kandidaten (0,6 × 60 / 40 s), überspringt Schläge und bestätigt so den falschen Wert (offener Fehler im
Standard, AUDIT_VITAL-Ledger).

**Atmung Atemzug für Atemzug (V3.4, offen):** Abnahme an R3 (Tag A): in allen Takten 6 / 10 / 15 / 20 /min die Rate
jeder `@BREATH`-Zeile (60 / Median der letzten 4 Abstände) innerhalb ±1 /min vom Metronom, erster Wert nach dem Start
nach 2 Atemzügen. `vital_eval.py` schreibt dazu je Sitzung „Atmung Atemzug für Atemzug" (gegen die Marker „ein",
Atemzüge in Anhalte-Abschnitten mitgezählt; R2 prüft, dass beim Anhalten keine kommen). Prüfstand vorab (nur
Simulation): MAE 0,0–0,15 /min, alle innerhalb ±1 /min, erster Wert nach 2,0 Atemzügen (auch `takt6`, 6 /min, wo die
spektrale Atmung mit Band ab 7 /min 8 /min daneben liegt); Ausnahmen: `flach` (0,6 mm) erst nach 4 Atemzügen (der
Telemetrie-Strahl zentriert mit einem gleitenden Mittelwert und schwingt bei kleinem Bogen erst nach ≈ 9 s ein),
`anhalt` 2 Atemzüge direkt nach dem Anhalten 17–19 statt 16 /min; ohne Person keine Zeile (Tor: nur in ERFASSUNG,
GÜLTIG, APNOE).

— offen (braucht Gurt und Aufnahmen)

| Sitzung | Tag | Puls-MAE | ±5 /min | Verfügbarkeit | Konfidenz ≥ 70 %: ±5 | Atmung-MAE | Apnoe erkannt / Verzug |
|---|---|---|---|---|---|---|---|
| — | — | — | — | — | — | — | — |

### V4 Sensor-Konfiguration (Messplan, offen)

Plan V4.1–V4.4 in `docs/AUDIT_VITAL.md` (Experimente G1, G5, M3). Daten nach `~/Data/radar-sensor/v4/`,
nicht ins Repo. Vor jeder Aufnahme den ESP32 eindeutig bestimmen (`ls -l /dev/serial/by-id`, genau ein
`usb-Espressif…-if00`); die Werkzeuge brechen ab, wenn es nicht genau einer ist. Sensor nur in seinen zulässigen
Einstellungen betreiben (Profile, HWAAS, PRF, Gain); exptool prüft jede Konfiguration vor dem Setup.

**V4.1 Weg-Rauschen statisch.** Metallplatte 0,6 m vor dem Sensor (Maßband), Raum leer, nichts bewegt sich; ESP32
und Sensor 5 min eingeschaltet (Temperatur). `sess run` liefert nur Amplituden, keine Phase; deshalb nimmt
`host/noise_floor.py record` über die ESP32-Brücke `bridge` mit exptool auf (Loopback + 24 Zoom-Punkte ab
`--start-mm`, je 60 s; ohne Angaben wie das `iq3`-Preset: Zoom Profil 1, HWAAS 32, Gain 16, 15,6 MHz; Loopback
Profil 1, HWAAS 8, Gain 16, 19,5 MHz). Zuerst die Lage des Echos mit `iq3 10` oder `sess run 5 peak` bestimmen und
`--start-mm` so wählen, dass es in der Mitte der 60 mm liegt (Platte bei 600 mm → `--start-mm 570`).

| Block | Aufnahmen | Werte |
|---|---|---|
| Zoom | 24 | `--profile` 1 / 2 / 3 × `--hwaas` 16 / 32 / 64 / 128 × `--rate` 30 / 60 |
| Loopback | 5 | bei Zoom Profil 1, HWAAS 32, 60 Hz: `--loop-profile 4 --loop-prf 15.6`, `--loop-profile 5 --loop-prf 15.6`, `--loop-prf 15.6`, `--loop-gain 10`, und die Grundeinstellung noch einmal am Ende (Drift) |
| Firmware-Pfad | 1 | `iq3 60 570 raw` mit `host/esp_record.py` (dieselbe Einstellung wie Zoom 1/32/60, aber über die Firmware) |
| Neustart | — | Phase vor/nach Session-Neustart: `host/rezoom_check.py` (V2.3 Schritt 1) |

Exptool-Regeln (offline geprüft, `host/tests/test_noise_floor.py`): 19,5 MHz nur mit Profil 1; Loopback nicht mit
Profil 2; die PRF eines Subsweeps darf nicht kleiner sein als die des folgenden — Loopback mit 15,6 MHz geht also nur,
weil der Zoom 15,6 MHz hat. Ob der Sensor 60 Hz bei HWAAS 128 schafft, zeigt erst die Messung: `record` zählt Frames
mit Flags (gesättigt, verspätet, Kalibrierung); eine Einstellung mit Flags gilt als nicht erreichbar.

```bash
sg dialout -c ".venv/bin/python host/noise_floor.py record --out ~/Data/radar-sensor/v4/v41/p1_h32_60.npz --start-mm 570 --profile 1 --hwaas 32 --rate 60"
```

```bash
.venv/bin/python host/noise_floor.py eval ~/Data/radar-sensor/v4/v41/*.npz ~/Data/radar-sensor/v4/v41/iq3_* > ~/Data/radar-sensor/v4/v41/bericht.txt
```

Auswertung je Aufnahme: Weg-RMS im Band 0,6–5 Hz (wie das Pulsband der DSP), Median der starken Zoom-Punkte und bester
Punkt, je für Derotation `aus`, `roh` (wie die Firmware) und `tiefpass` (Loopback-Phase mit 0,8 Hz geglättet, wie
früher am PC). Entscheidung: Rauschboden je Einstellung (kleinster Median ohne Flags); Derotation `roh` bleibt, wenn
sie den Median gegen `aus` senkt; `tiefpass` kommt in die Firmware, wenn er mindestens 10 % unter `roh` liegt;
Loopback-Einstellung wie die beste der fünf.

**V4.2 Person mit Gurt.** Die 3 besten Einstellungen aus V4.1 mit 60 Hz (die DSP rechnet nur mit 60 Hz) je als
Firmware-Variante übersetzen und je R1 (5 min, M5) aufnehmen. Zoom-Profil/HWAAS und Loopback-Profil/Gain/PRF sind
Übersetzungsschalter in `firmware/esp32s3/src/a121_server_client.h` (`A121_IQ3_ZOOM_PROFILE`, `A121_IQ3_ZOOM_HWAAS`,
`A121_IQ3_LOOP_PROFILE`, `A121_IQ3_LOOP_GAIN`, `A121_IQ3_LOOP_PRF`; Standard 1 / 32 / 1 / 16 / 19,5 MHz), z. B. in
`platformio.ini` als `build_flags = -DA121_IQ3_ZOOM_HWAAS=64`. `@HELLO` meldet die Einstellung als `iq3=p1h64/lp1g16/19_5_MHz`
(steht in `meta.json` der Aufnahme). Flashen nur nach eindeutiger Port-Bestimmung. Auswertung mit `vital_eval.py`
wie M5; Entscheidung: Puls-MAE und Verfügbarkeit in R1, bei Gleichstand der kleinere Rauschboden. Ergebnis wird das
neue Standard-Preset. 30 Hz nur, wenn V4.1 dort deutlich weniger Rauschen zeigt — dafür müsste die DSP (Filter,
Fenster) für 30 Hz neu ausgelegt werden (eigener Plan).

**V4.3 Linse und Abstand.** Wie R6/R7 in M5 (ohne / hyperbolische Linse × 0,4 / 0,6 / 0,8 / 1,0 m, je 3 min mit Gurt),
mit dem Preset aus V4.2. Entscheidung: empfohlener Aufbau = kleinster Puls-MAE bei Verfügbarkeit ≥ 80 %; Reichweite =
größter Abstand mit Puls-MAE ≤ 5 /min.

**V4.4 Zoom-Breite.** Firmware mit `A121_ZOOM_POINTS=48` gegen 24 (Standard), je R1 und R5 an Tag A; auswerten mit
`vital_eval.py` (für die Nachrechnung mit 48 Punkten eine Header-Kopie mit `#define A121_ZOOM_POINTS 48`). Entscheidung
für V2.3 Schritt 3: 48 nur, wenn R5 (Bewegung) mehr gültige Werte bei gleichem Puls-MAE in R1 hat.

— offen (braucht Metallplatte, Gurt, Aufnahmen; Werkzeuge nur mit Simulation geprüft)

| Messung | Ergebnis |
|---|---|
| V4.1 Rauschboden | — |
| V4.2 Preset | — |
| V4.3 Aufbau / Reichweite | — |
| V4.4 Zoom-Breite | — |

## Materialien und Umgebung

| # | Experiment | Modul | Messgröße | Ergebnis |
|---|---|---|---|---|
| U1 | Dämpfung: Pappe, Sperrholz, Gipskarton, Kunststoff, Glas, Stoff | sparse_iq | Amplitude vor/hinter Material (dB) | — |
| U2 | Metallgegenstand / Person hinter Gipskarton bzw. Holztür | distance, presence | erkennbar ja/nein, Abstand | — |
| U3 | Vibrationsfrequenz: Lautsprecher, Ventilator, Waschmaschine | vibration | Hz vs. Sollwert | — |
| U4 | Geschwindigkeit: Ventilatorblatt, rollender Ball, Pendel | speed | m/s | — |
| U5 | Wasser-Fließgeschwindigkeit (Wasserhahn, Gartenschlauch) | surface_velocity | m/s plausibel? | — |
| U6 | Füllstand: Eimer/Glas Wasser, Mülleimer | tank_level, waste_level | mm vs. Lineal | — |

## Übertragung zum Handy (Android-App)

Aufbau: ESP32-S3 (`2026-11-01-app1`) per C-auf-C am Galaxy S26 Ultra, A121 über die rote 5V-Ader, App per USB-CDC.
Details und Werkzeuge: `docs/ANDROID_APP.md`. Aufnahmen unter `~/Data/radar-sensor/phone/`.

| # | Experiment | Messgröße | Ergebnis |
|---|---|---|---|
| T1 | Dauerlauf `iq3 0 raw` über USB, Aufnahme im Hintergrund, Bildschirm aus | Frameverlust, Rate, Pausen | 2026-11-01, 31,7 min: 113 289 Frames, 59,64 Hz nach tick, **0 Lücken**, 0 Neuverbindungen, ≈ 41 kB/s. Größter tick-Abstand 440 ms (alle 38 Abstände > 100 ms bei `@REZOOM`), größte Empfangspause 337 ms (DSP-Report ESP32 Ø 270 ms, max. 323 ms). Akku 83 → 80 % in 39 min (abgelesen) |
| T2 | Kabel abziehen/einstecken während `iq3` | Wiederanlauf | 2026-11-01, 2×: ESP32 startet neu (Handy-Strom), App verbindet selbst und startet `iq3` neu; 0 Lücken innerhalb der 3 Streams. Pause bis zum neuen Stream 34,9 s (mit Berechtigungsdialog) bzw. 8,2 s (ohne; inkl. Zeit ohne Kabel) |
| T3 | ESP32-DSP am Handy nachrechnen (NDK, Rohdaten aus T1) | Gleichheit Handy / PC / ESP32 | 2026-11-01: Handy = PC bytegleich; gegen ESP32 51 715 / 128 197 Zeilen gleich, Rest meist letzte Stelle, Weg ≤ 0,21 µm, Atemtiefe ≤ 1,6 µm, IBI in 1 / 1632 `@VITAL` um 16,7 ms. Rechenzeit Handy 28,4 s für 31,7 min |

## Nicht möglich mit einem Sensor

- `bilateration` braucht zwei Sensoren.
- Winkel/Richtung und Umrisse: siehe `KONZEPT.md`.
