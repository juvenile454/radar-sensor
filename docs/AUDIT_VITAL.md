# Audit Vitalerkennung — Befunde und Umsetzungsplan

Stand 2026-11-01 (Umsetzung der Software-Teile: Abschnitt 6, „Stand der Umsetzung“). Ziel: bessere Vitalerkennung
(Atmung, Puls), genauere Daten, schnellere Erkennung, belastbare Auswertung. Geprüft wurden Doku, Firmware (ESP32-DSP,
Server-Client, `iq3`-Schleife), PC-Skripte, C++-Nachrechnung und App. Dazu Herstellerdoku (Acconeer), Fachliteratur
und ein synthetischer Prüfstand (`host/vital_sim.py`).

**Wichtig:** Im Audit lagen keine Messdaten vor (die liegen unter `~/Data/radar-sensor/`). Zahlen sind deshalb
mit ihrer Quelle markiert: **[Code]** gelesen, **[Doku]** aus `docs/`, **[Sim]** aus dem Prüfstand (Modell, keine
Messung), **[Hersteller]** Acconeer-Doku, **[Lit]** Fachliteratur. Es gibt weiterhin keine Aussage über Puls-Genauigkeit;
dafür fehlt die Referenzmessung (Befund B1).

## Kurzfassung

1. **Ohne Referenzgerät lässt sich nichts verbessern, nur verändern.** Alle Puls-Optimierungen wurden gegen selbst
   gezählten Puls und am selben Datensatz entwickelt und geprüft. Erster Schritt: EKG-Brustgurt (z. B. Polar H10, liefert
   Schlag-Intervalle über BLE) in die App, versionierte Auswertung, getrennte Entwicklungs- und Testdaten.
2. **Die ESP32-DSP zentriert die IQ-Werte am Fenster-Mittelwert statt per Kreisfit** (`binDisplacement`). Bei einem
   Atemhub von 2,4–3,4 mm, wie gemessen, liegt der Mittelwert 27–40 % des Kreisradius neben dem Mittelpunkt. Das erzeugt
   Atem-Oberwellen im Pulsband. [Sim] Herzband-Fehler bei 3 mm Hub 38 µm statt 14 µm, bei 0,5 mm 88 µm statt 3 µm
   (Herzsignal 100 µm); Puls = 4 × Atmung: 0 % statt 91 % der Reports innerhalb ±5 /min. Die PC-Skripte haben den
   Kreisfit, die Firmware hat ihn nicht (HARDWARE.md beschreibt ihn für `iq3` trotzdem).
3. **Die Konfidenz sagt nichts über die Richtigkeit.** Sie zählt, wie viele der 24 Zoom-Punkte dieselbe Spitze sehen;
   die Punkte liegen 2,5 mm auseinander und sehen fast dasselbe Signal. [Sim] 98–100 % Konfidenz bei 50 /min Fehler.
   Ohne atmende Person (nur ruhendes Echo) meldet die DSP [Sim] in 113 von 120 Reports einen gültigen Puls (Median 63 /min),
   Konfidenz-Median 50 %, bis 87 %.
4. **Puls unter 55 /min ist nicht erfassbar** (Suchband 53–138 /min, Randbins gesperrt). [Sim] Puls 50 → Ausgabe 100
   (2. Oberwelle) mit 98 % Konfidenz.
5. **Verfügbarkeit:** Jeder Rezoom löscht 15 s Historie; danach 8 s ohne Puls. In der 30-min-Aufnahme (T1) hatten
   267 von 1899 Reports (14 %) kein `@VITAL` bei 38 Rezooms [Doku].
6. **Geschwindigkeit:** Die DSP rechnet 270–320 ms je Sekunde in der Leseschleife [Doku]; Frames stauen sich, die
   Ausgabe kommt gebündelt. Puls-Reaktion auf eine Änderung ≈ 10–11 s [Sim], erste Werte nach 8 s.
7. **Datenqualität:** `calibration_needed` und `frame_delayed` werden in `iq3` ignoriert [Code] (nur `sess` warnt);
   bei 30 min Dauerlauf mit Temperaturanstieg ist das ein Risiko. Atem anhalten wird nicht erkannt: [Sim] 28 /min statt
   „keine Atmung". RMSSD ist [Sim] systematisch 40–230 % zu hoch.

Umsetzung in Phasen: **V0** Messgrundlage (Referenz, Auswertung), **V1** gezielte DSP-Korrekturen, **V2** Geschwindigkeit
und Verfügbarkeit, **V3** bessere Algorithmen, **V4** Sensor-Konfiguration vermessen, **V5** App/Handy-DSP. Details in
Abschnitt 6.

---

## 1. Grundlagen

### 1.1 Messprinzip und Größen

| Größe | Wert | Quelle |
|---|---|---|
| Wellenlänge λ bei 60,5 GHz | 4,955 mm | [Code] `kWavelengthM` |
| Weg je Radiant Phase (λ/4π) | 394 µm/rad | [Code] `kRadToUm` |
| Voller Phasenumlauf | λ/2 = 2,48 mm Weg | Physik |
| Atemhub Brust (sitzend, 0,6–0,7 m) | 2,3–3,4 mm Spitze-Spitze → 0,9–1,4 Umläufe | [Doku] M2, M3 |
| Herzanteil im Wegsignal | 62–107 µm (RMS bzw. Hub) → 0,16–0,27 rad | [Doku] M3 B, D |
| Atemfrequenz gemessen | 17–24 /min (0,28–0,40 Hz) | [Doku] M2 |
| Puls (selbst gezählt) | 70–86 /min (1,17–1,43 Hz) | [Doku] M3 E, F |
| Brustabstand | 0,58–0,76 m | [Doku] |
| Echo-Amplitude Brust / Raumrauschen | 1000–3300 / ≈ 55 (Rohwerte) | [Doku] Bug 5, M3 D |

Folgen:
- Die Atmung bewegt die Brust um **10–30-mal** mehr als der Herzschlag. Jede Verzerrung der Atmung um wenige Prozent
  landet in der Größe des Herzsignals.
- Ein Atemzug dreht die IQ-Werte eines Punkts fast einmal um den Kreis. Die Phase muss also um den **Kreismittelpunkt**
  (das feste Echo im selben Punkt) gemessen werden, nicht um den Ursprung oder einen Mittelwert (Befund B2).

### 1.2 Frequenzen: Atem-Oberwellen im Pulsband

Ein Atemzug ist nicht sinusförmig (Einatmen schneller als Ausatmen, Pause). Seine Oberwellen m × Atemfrequenz fallen ins
Pulsband:

| Atmung /min | Oberwellen im Bereich 40–180 /min |
|---|---|
| 12 | 48, 60, 72, 84, 96 |
| 15 | 45, 60, 75, 90, 105, 120 |
| 18 | 54, 72, 90, 108, 126, 144 |
| 20 | 40, 60, 80, 100, 120, 140, 160 |

Bei 18–20 /min Atmung liegt alle 18–20 /min eine Oberwelle; ein Puls von 72–80 trifft fast immer eine davon (M3 E/F:
„Puls ≈ 4 × Atmung"). Rein über das Spektrum ist das bei exakter Übereinstimmung nicht trennbar; helfen nur geringere
Verzerrung (B2), die 2. Herz-Oberwelle, Verfolgung über die Zeit und Zeitbereichsmerkmale (Abschnitt 6, V3).

### 1.3 Fensterlänge, Auflösung und Verzug

| Fenster | Hann-Hauptkeule (±) | −3-dB-Breite | Verzug (≈ halbes Fenster) |
|---|---|---|---|
| 8 s | ±15 /min | 10,8 /min | 4 s |
| 10 s | ±12 /min | 8,6 /min | 5 s |
| 15 s (heute) | ±8 /min | 5,8 /min | 7,5 s |
| 20 s | ±6 /min | 4,3 /min | 10 s |
| 30 s | ±4 /min | 2,9 /min | 15 s |

Heute: 15-s-Fenster + Glättung 0,7/0,3 + Haltezeit 3 (bzw. 10) Reports → Reaktion auf eine Pulsänderung ≈ 10–11 s [Sim].
Für die Atmung enthält ein 15-s-Fenster bei 15 /min nur 3,75 Atemzüge; für langsame Atmung (< 8 /min) ist es zu kurz.

### 1.4 Sensor-Konfiguration `iq3` [Code]

`make3SubsweepPreset()` in `firmware/esp32s3/src/a121_server_client.h`, 60 Hz, 1 Sweep je Frame, Verstärkung 16:

| Subsweep | Punkte | Raster | Profil | HWAAS | PRF | Phase Enh. | Zweck |
|---|---|---|---|---|---|---|---|
| 0 | 1 (Start 0) | — | 1 | 8 | 19,5 MHz | aus | Loopback, Phasenreferenz |
| 1 | 24 ab Zoom-Start | 2,5 mm (60 mm) | 1 | 32 | 15,6 MHz | an | Vital-Zoom |
| 2 | 80 ab 0,30 m | 30 mm (bis 2,67 m) | 3 | 16 | 15,6 MHz | an | Raum, Zielsuche |

Datenrate 105 Punkte × 4 B × 60 Hz ≈ 25 kB/s Nutzdaten, mit ≈ 150 B Kopfzeile je Frame ≈ 34 kB/s; Link-Grenze
≈ 195 kB/s [Doku] → **gut 5-fache Reserve** für ein breiteres Zoom-Fenster oder mehr Punkte (Abschnitt 6, V2.3/V4).

Profil 1 hat 43 mm Halbwertsbreite [Hersteller, Datenblatt]. Bei 2,5 mm Raster überlappen benachbarte Punkte fast
vollständig: Die 24 Zoom-Punkte (60 mm) enthalten nur etwa 1,4 unabhängige Entfernungszellen. Das ist für Befund B4
wichtig.

### 1.5 Herstellerangaben (Acconeer) und was sie für `iq3` bedeuten

Quellen: Sphinx-Quellen von docs.acconeer.com und Code in `acconeer/acconeer-python-exploration` (Tag v7.18.2 = SDK
1.13.0), A121-Datenblatt v1.3. docs.acconeer.com selbst war aus der Audit-Umgebung nicht erreichbar.
Abkürzungen: `GH:` = https://github.com/acconeer/acconeer-python-exploration/blob/v7.18.2/

| Thema | Herstellerangabe [Hersteller] | Folge für das Projekt |
|---|---|---|
| Phase Enhancement | Wird bei der Sensor-Kalibrierung aus einer Loopback-Messung bestimmt und korrigiert das Phasenmuster **über die Entfernung** („phase coherent data in the distance domain"); keine Aussage zu Stabilität von Frame zu Frame (GH: `docs/radar_data_and_control/a121/in_depth_topics/loopback.rst`, `subsweep_config.py`) | Richtig für den Zoom (Punkte kohärent kombinierbar). Nach jeder Neu-Kalibrierung (jeder Session-Start, also jeder Rezoom) kann die Phase springen → Abschnitt neu beginnen oder über Loopback angleichen (V2.3) |
| Loopback je Frame | Acconeer nutzt genau diese Derotation in der Vibration-App („low frequency enhancement", `frame * exp(-1j*angle(loopback))`, GH: `src/acconeer/exptool/a121/algo/vibration/_processor.py`) gegen Phasenjitter, dort mit **gleicher PRF** für Loopback und Messung; der Distanzdetektor kombiniert Phase Enhancement und Loopback | Technik ist legitim, aber nicht kostenlos: das Rauschen der Loopback-Messung geht mit ein. Heute 19,5 MHz Loopback gegen 15,6 MHz Zoom (bei Acconeer nie so). Loopback mit Profil 2 verboten |
| Profile | Halbwertsbreite P1 43 mm, P2 80 mm, P3 138 mm, P4 203 mm, P5 346 mm (Datenblatt Tab. 10); P3 hat je HWAAS ≈ 8 dB mehr Signal als P1, je Messzeit ≈ 10 dB | Profil 1 nur nötig, wenn 43 mm Auflösung den Puls verbessern (Brust von Schulter trennen). Profil 3 am Zoom ist eine ernsthafte Alternative (V4.1/V4.2) |
| HWAAS | SNR +3 dB je Verdopplung, Messzeit wächst linear | Zoom heute ≈ 1,5 ms, ganzer Frame ≈ 3–3,6 ms (Zeitmodell aus `timing.rst`): Bei 60 Hz (16,7 ms) ist Platz für HWAAS 128 am Zoom (+6 dB) |
| Receiver Gain | Standard 16; senken bei Sättigung; Acconeers Phasen-Apps nutzen 10 | `data_saturated` beobachten (heute nur ausgegeben) |
| Atem-Referenz-App | Profil 3, HWAAS 32, 16 Sweeps/Frame, 20 Hz, Raster 60 mm, 3 Entfernungen; festes Echo per komplexem Tiefpass 0,1 Hz abgezogen, dann Phase, Bandpass 6–60 /min, **20-s-Fenster**, Hamming, FFT, Spitze quadratisch verfeinert; Zustände „keine Person / Bewegung / Abstand bestimmen / Atmung schätzen" (GH: `src/acconeer/exptool/a121/algo/breathing/`) | Vorbild für Zustandslogik (V3.5) und Atmung (V3.4); Acconeers Testtabelle: ≤ 1 /min Abweichung |
| Herzfrequenz | Keine Herz- oder Vital-App von Acconeer, keine µm-Rauschangabe | Jede Puls- oder µm-Aussage braucht eigene Referenz |
| `calibration_needed` | „Sensor calibration needs to be redone"; Regel: Temperatur ±15 °C seit der letzten Kalibrierung | V1.5: Session neu aufsetzen |
| Temperatur und Amplitude | Amplitude ≈ 2^(−ΔT/67 °C) bei P1 (86 °C bei P3) | Feste Amplitudenschwellen driften mit der Temperatur; Schwellen relativ zum Rauschen (wie heute im Raum) sind richtig |
| `tick` | XM125-Server: `ticks_per_second` = 1000 (1 ms) | tick nur zur Lückenprüfung; `ticks_per_second` aus `get_system_info` lesen statt annehmen |
| Frame-Rate | Mit gesetzter `frame_rate` triggert sich der A121 selbst („extremely accurate rate") | Feste Zeitbasis 60 Hz in der DSP ist richtig |

## 2. Ist-Analyse der Kette

```
A121 ──LPUART1 2 Mbaud──► ESP32-S3 cmdIq3()                          ──USB-CDC──► App / PC
        (JSON-Kopf +       ├─ readStreamFrame()  (Flags gelesen, nicht genutzt)
         105 × int16-IQ)   ├─ pushFrame()        je Frame: Loopback-Derotation, Ring 16 s je Zoom-Punkt,
                           │                     Raum-EMA, Telemetrie-Weg (@IQ3)
                           └─ computeVitalReport() je ≈ 1 s, blockiert 270–320 ms:
                                je Zoom-Punkt (24×): Mittelwert-Zentrierung → atan2 + Abwickeln → Trend weg →
                                Bandpass 0,8–3 Hz (filtfilt) → Bewegungsdämpfung → Goertzel 0,88–2,30 Hz
                                und Atemspektrum 0,10–0,60 Hz → gewichtete Summe → Top-3-Spitzen →
                                Verfolgung (±12 /min, Haltezeit 3/10) → ACF, IBI/RMSSD am „besten" Punkt
                                Raum: Rauschpegel, Ziele, Rezoom-Empfehlung → Session neu (löscht Historie)
```

| Teil | Datei | Stand |
|---|---|---|
| Server-Client, Preset | `firmware/esp32s3/src/a121_server_client.h` | robust (Bugs 1–4 behoben) |
| `iq3`-Schleife, Ausgabe | `firmware/esp32s3/src/main.cpp:616-765` | Rechnen und Lesen im selben Ablauf |
| DSP | `firmware/esp32s3/src/a121_esp_dsp.h` | an 2 Aufnahmen einer Person entwickelt |
| Nachrechnung PC/Handy | `host/dsp_replay/`, `android/app/src/main/cpp/` | Handy = PC bytegleich, ESP32 letzte Stelle |
| PC-Forschungsskripte | `host/a121_ultimate_vital.py`, `a121_vitalaufnahme.py`, `a121_anhalteanalyse.py`, `a121_esp_*.py` | 5 Varianten mit eigener DSP |
| Auswertung gegen Referenz | — | **fehlt** (Zählfenster-Auswertung lag im Scratchpad) |
| App | `android/…/live/LiveStore.kt`, `ui/Pages.kt` | zeigt Werte, kein Gütezustand |

Gut gelöst und beizubehalten: feste Zeitbasis über Frame-Zähler statt ESP32-Zeit, Nullphasen-Filter wie scipy,
Bewegungsdämpfung je 1-s-Block, Sperre der Bandränder, Raum-Rauschpegel aus dem unteren Viertel, geglättete Zielwahl
(Bug 7), eine gemeinsame DSP-Quelle für ESP32, PC und Handy, `@RAW`-Aufnahmen für Nachrechnungen, ehrliche Kennzeichnung
„Puls-Kandidat".

## 3. Befunde

Schwere: **hoch** = verfälscht Werte oder verhindert Verbesserung; **mittel** = spürbare Einbußen; **niedrig** = Aufräumen.

### Auswertung und Validierung

**B1 (hoch) Keine Referenzmessung, keine versionierte Auswertung.**
Puls-Referenz war selbst gezählt („laut Testperson unsicher", Zählung 70 gegen gemessen 77 ungeklärt). Die
Oberwellen-Haltezeit wurde am Datensatz entwickelt, an dem sie bewertet wurde („kein unabhängiger Test") [Doku M3 F].
Das Skript für „Anteil innerhalb ±8 /min" ist nicht im Repo. Folge: Jede Änderung kann besser aussehen, ohne besser zu
sein; Werte wie RMSSD 102 ms [Doku M3 D] lassen sich nicht einordnen.

**B2 (hoch) Mittelwert-Zentrierung statt Kreisfit in der Firmware.**
`binDisplacement()` (`a121_esp_dsp.h:584-613`) misst die Phase um den Mittelwert der IQ-Werte im 15-s-Fenster. Für eine
Atembewegung mit Phasenhub β ist dieser Mittelwert um J₀(β) × Radius vom wahren Mittelpunkt entfernt:

| Atemhub | β | Mittelpunkt-Fehler | größter Phasenfehler |
|---|---|---|---|
| 0,5 mm | 0,63 rad | 90 % des Radius | ≈ 440 µm |
| 1,0 mm | 1,27 rad | 64 % | ≈ 270 µm |
| 2,0 mm | 2,54 rad | 7 % | ≈ 26 µm |
| 2,4 mm | 3,04 rad | 27 % | ≈ 110 µm |
| 3,0 mm | 3,80 rad | 40 % | ≈ 160 µm |
| 4,0 mm | 5,07 rad | 15 % | ≈ 60 µm |

Der Fehler wiederholt sich mit der Atembewegung, also als Atem-Oberwellen. [Sim] `host/vital_sim.py demod`, Herzsignal
100 µm, Atmung 15 /min: Herzband-Fehler Mittelwert 13–88 µm, Kåsa-Kreisfit 3–23 µm (≈ wie der wahre Mittelpunkt);
die Atem-Oberwellen liegen mit Mittelwert bis +2,3 dB über der Herzlinie, mit Kreisfit bei −6 dB. Die PC-Skripte nutzen
den Kåsa-Fit (`fit_iq_circle`); `docs/HARDWARE.md` beschreibt ihn auch für `iq3` auf dem ESP32. Der Telemetrie-Weg (`@IQ3`) nutzt
ebenfalls einen gleitenden Mittelwert (`dc_i_`, τ ≈ 8 s). Am echten Signal ist der Kreis verformt (Amplitude schwankt
mit der Atmung, mehrere Streuzentren im Punkt), der Gewinn muss an Aufnahmen bestätigt werden (V1.1).

**B3 (mittel) Mehrere auseinanderlaufende Algorithmus-Fassungen.**
Fünf PC-Skripte mit eigener Kette (Kåsa + DACM + Hilbert-Projektion + HPS, 100 Zoom-Punkte, 60–100 Hz) und die Firmware
(Mittelwert, keine Projektion, 24 Punkte). Erkenntnisse aus der PC-Kette (Kreisfit, Tiefpass der Loopback-Phase,
2. Herz-Oberwelle) sind nicht in der Firmware, die Firmware-Erkenntnisse (Bandrand, Bewegungsdämpfung, Oberwellen-Haltezeit)
nicht in den Skripten.

### Puls

**B4 (hoch) Konfidenz misst Einigkeit korrelierter Punkte, nicht Richtigkeit.**
`agreeAt()` (`a121_esp_dsp.h:493-498`) = gewichteter Anteil der Zoom-Punkte, deren eigene Spitze ±5 /min am Ergebnis
liegt. Die Punkte überlappen fast vollständig (1.4); ein Artefakt oder eine Oberwelle sehen alle. [Sim] Puls 50 → 100
gemeldet mit Konfidenz 98–100 %; Puls 120 → 60 mit 100 %; richtiger Wert bei Puls = 4 × Atmung mit 0 %. Die Doku
beobachtet dasselbe live: „Rhythmische Bewegung wird von allen Punkten gestützt" [Doku M3 F].

**B5 (hoch) Suchband 53–138 /min, wirksam 55–136 /min.**
`kHrFMin = 0,88 Hz`, 143 Schritte, 3 Randschritte gesperrt; Bandpass 0,8–3,0 Hz. Ruhepuls unter 55 (häufig bei
Sportlern, im Liegen, im Schlaf) und über 136 wird nie gefunden; stattdessen eine Oberwelle. Ein Subharmonik-Test
(liegt bei f/2 ebenfalls eine Spitze?) fehlt. [Sim] Puls 50: 0 % innerhalb ±5 /min.

**B6 (mittel) Verfolgung bleibt an starken Fremdspitzen hängen.**
Ein Kandidat ±12 /min am alten Wert mit ≥ 40 % der stärksten Spitze gewinnt immer (`a121_esp_dsp.h:503-516`). Ist der
erste Wert eine Atem-Oberwelle, bleibt die Verfolgung dort, solange diese stark ist. `hr_harm` wird gemeldet, senkt aber
weder Konfidenz noch Vorrang. [Sim] Puls 120 / Atmung 20 mit Kreisfit: dauerhaft 60 (3. Atem-Oberwelle), Konfidenz 100 %.
Die Doku nennt denselben Fall live („hing bei 60–63 /min") [Doku M3 F].

**B7 (mittel) IBI und RMSSD sind nicht belastbar.**
Spitzen auf dem schmalbandig gefilterten Signal (0,8–3 Hz macht jeden Schlag zur Sinuswelle), Schwelle 0,35 × RMS,
keine Interpolation zwischen Abtastwerten, keine Prüfung gegen den verfolgten Puls. Die Quantisierung (16,7 ms) allein
erklärt nur ≈ 2 ms RMSSD-Fehler (√(43² + 12²) ≈ 45 ms); der Hauptfehler sind fehlende und zusätzliche Schläge. [Sim]
RMSSD 61–160 ms bei wahren 43–53 ms. Literatur zu HRV aus Radar: siehe Abschnitt 5.

**B8 (niedrig) Loopback-Derotation ungeprüft und mit anderer PRF.**
`pushFrame()` dreht alle Zoom-Punkte jeden Frame um die rohe Loopback-Phase (`a121_esp_dsp.h:225-233`), mit deren
Rauschen. Die PC-Kette glättet die Loopback-Phase (< 0,8 Hz), „ohne weißes Rauschen einzukoppeln". Acconeer nutzt die
rohe Derotation selbst (Vibration-App, 1.5), aber mit derselben PRF wie die Messung; hier misst der Loopback mit 19,5 MHz,
der Zoom mit 15,6 MHz. Ob die Derotation im Pulsband nützt oder Rauschen hinzufügt, ist ungemessen (V4.1);
`loopback_phase_std_mrad` wird ausgegeben.

### Atmung

**B9 (mittel) Atem anhalten / Apnoe wird nicht erkannt.**
`br_bpm` ist 0 nur ohne lokales Maximum; bei angehaltenem Atem gewinnt eine Restspitze. [Sim] Während 20 s Atem anhalten:
28 /min (Oberwelle des Rests im Fenster) statt „keine Atmung". Die PC-Kette hatte einen Apnoe-Zähler (`apnea_10s_episodes`).

**B10 (niedrig) Atmung nur spektral im 15-s-Fenster.**
Frequenz aus dem größten lokalen Maximum (0,115–0,585 Hz). Langsame Atmung (< 7 /min, z. B. geführtes Atmen mit 6 /min)
liegt außerhalb; keine Atemzug-für-Atemzug-Werte. `kRespMinWin` (3 s) ist definiert, aber ungenutzt: Atmung kommt wie der
Puls erst nach 8 s. Atemtiefe nur aus der Grundwelle am Punkt mit dem klarsten Puls, nicht am Punkt mit der stärksten
Atmung.

### Geschwindigkeit und Verfügbarkeit

**B11 (hoch) Jeder Rezoom löscht die Historie.**
`setZoomStartPoint()` → `resetState()` setzt auch `hr_prev_` zurück. Danach 8 s ohne Wert, die Verfolgung beginnt neu.
T1: 38 Rezooms in 31,7 min, 1632 `@VITAL` bei 1899 Reports (14 % ohne Vitalwert) [Doku]. Das Zoom-Fenster ist 60 mm
breit; die Brust bewegt sich beim Atmen 2–3 mm, beim Zurücklehnen deutlich mehr.

**B12 (mittel) DSP blockiert die Leseschleife.**
`computeVitalReport()` läuft in `cmdIq3()` zwischen zwei `readStreamFrame()`-Aufrufen (`main.cpp:714-759`), 270–320 ms je
Sekunde. Frames stauen sich im 32-KB-Puffer und kommen gebündelt (Empfangspausen bis 337 ms) [Doku T1]. Der ESP32-S3 hat
zwei Kerne; der zweite rechnet nichts.

**B13 (mittel) Rechenaufwand höher als nötig** (Schätzung je Report, [Code]):
- 21 600 × `atan2f` + 21 600 × `sqrtf` (jeder Punkt, jedes Sample, jede Sekunde neu),
- ≈ 260 000 `double`-Operationen (`binDisplacement`, `removeLinearTrend`); der ESP32-S3 rechnet `double` in Software,
- ≈ 1,3 Mio. Goertzel-Schritte (24 Punkte × 244 Frequenzen × 225 Samples) und ≈ 17 000 `cosf` (Hann-Fenster je Punkt neu),
- 24 × Vorwärts-/Rückwärtsfilter über 954 Samples.
Am PC 4,9 ms je Report [Sim], am ESP32 270–320 ms.

**B14 (mittel) Erste Werte und Reaktion langsam.** Erste Werte frühestens nach 8 s, nach einem Rezoom erneut 8 s;
Reaktion auf eine Pulsänderung (66 → 90 in 10 s) [Sim] 10–11 s nach Ende der Änderung.

### Datenqualität und Robustheit

**B15 (hoch) Sensor-Flags in `iq3` ungenutzt.** `readStreamFrame()` liest `calibration_needed`, `frame_delayed`,
`data_saturated` (`a121_server_client.h:347-349`); `cmdIq3()` gibt nur `saturated` in `@IQ3` aus, die DSP nutzt keines.
`sess` warnt bei `calibration_needed` [Code `a121_session_console.h:395`]. Bei 30-min-Läufen stieg die Temperatur
(14 → 19 °C in 30 s Mobilbetrieb [Doku]); ab wann der A121 neu kalibriert werden muss, siehe 1.5.

**B16 (niedrig) Lücken im Datenstrom nur am Frame-Zähler des ESP32 sichtbar.** Der Zähler zählt empfangene Frames;
verliert der Server Frames (`frame_delayed`) oder der ESP32 Bytes, sieht die DSP das nicht. Der Sensor-`tick` wäre der
Prüfwert (Soll 16,7 ms je Frame).

**B19 (hoch) Kein Lebenszeichen-Test.** `valid` heißt nur „genug Frames und eine Spitze im Spektrum". Ob überhaupt eine
Person atmet, prüft die DSP nicht (das Raum-Ziel muss nur für den Rezoom „bewegt" sein). [Sim] Ruhende Szene ohne Atmung
und Herzschlag (Brust-Echo vorhanden, unbewegt; Szenario `leer`): 113 von 120 Reports gültig, Puls-Median 63 /min, Atmung-Median 8 /min,
Konfidenz-Median 50 %, höchstens 87 %. Acconeers Atem-App schaltet in so einem Fall auf „keine Person" (1.5).

### App und Darstellung

**B17 (mittel) Kein Gütezustand in der App.** Puls, RMSSD und Atmung werden immer gezeigt (mit Hinweis „Kandidat").
Es fehlen Zustände wie „suche Person", „Erfassung läuft (x s)", „Bewegung", „Atem angehalten", „unsicher". Eine
Referenzkurve gibt es nicht.

### Aufräumen

**B18 (niedrig)** `hist_um_` wird nur geschrieben; `kRespMinWin` ungenutzt; die `iq3`-Beschreibung in `docs/HARDWARE.md`
(„Kåsa-Kreisfit + DACM", „Hilbert-Projektion") passt nicht zum Code; das Atemspektrum dezimiert ohne Tiefpass (unkritisch, Rauschen).

## 4. Synthetischer Prüfstand

`host/vital_sim.py` (neu) übersetzt den unveränderten Firmware-Header für den PC und eine Variante mit Kåsa-Kreisfit in
`binDisplacement()` und spielt synthetische `iq3`-Frames (105 Punkte, 60 Hz) ein. Modell: Vorderbrust, Bauch, Schulter
(unterschiedliche Atem-/Herzanteile, Amplitude schwankt 8 % mit der Atmung), Tisch, Wand, Möbel; Profil-1/3-Hüllkurven;
Rauschen 25/40; Loopback mit gemeinsamem Phasenfehler; Atmung mit 15 % 2. und 5 % 3. Oberwelle; Herz 100 µm
Spitze-Spitze als Pulsfolge mit 35 ms Streuung.

```bash
.venv/bin/python host/vital_sim.py demod
```

```bash
.venv/bin/python host/vital_sim.py bench
```

Ergebnis 2026-11-01 (Anteil der Reports innerhalb ±5 /min; Konfidenz-Median; „b. F." = Konfidenz bei > 10 /min Fehler):

| Szenario | Original | Kreisfit | Konfidenz Orig. / b. F. | Bemerkung |
|---|---|---|---|---|
| Atmung 15, Puls 72 | 97 % | 100 % | 89 / — | |
| Puls = 4 × Atmung (18 / 72) | **0 %** (MAE 18) | **91 %** (MAE 2,5) | 0 / 0 | Kernfall aus M3 E/F |
| Puls 66 → 90 | 90 %, Reaktion 11 s | 99 %, Reaktion 10 s | 93 / 70 | |
| Puls 50, Atmung 12 | 0 % (gemeldet ≈ 100) | 0 % | **98 / 98** | B4, B5 |
| Puls 120, Atmung 20 | 100 % | **0 %** (gemeldet 60) | 44 / — | B6: Kreisfit allein reicht nicht |
| Atemhub 0,6 mm | 54 % | 100 % | 100 / — | B2 |
| Atemhub 4 mm | 60 % | 100 % | 100 / — | B2 |
| 10 s Bewegung | 77 % | 96 % | 88 / 80 | |
| Atem anhalten 20 s | 93 % | 96 % | 97 / 96 | Atmung gemeldet 28 /min (B9) |
| Keine Atmung, kein Puls (ruhendes Echo) | 113/120 Reports „gültig" | ebenso | 50, max. 87 | Puls-Median 63 /min gemeldet (B19) |

RMSSD gemeldet / wahr in den Szenarien mit Person: Original 61–160 ms, Kreisfit 40–202 ms bei wahren 43–53 ms (B7). Atemfrequenz sonst ±0,2 /min (ideales Modell).

Grenzen: Ein Modell, eine Geometrie, keine echten Körperbewegungen, ideal periodische Atmung. Der Prüfstand zeigt
Logikschwächen (Band, Konfidenz, Verfolgung, Apnoe) unabhängig vom Modell; die Größe der Verbesserungen (Kreisfit)
gilt nur für das Modell und muss an Aufnahmen mit Referenz bestätigt werden. Er eignet sich als schneller
Regressionstest für jede DSP-Änderung (V0.5).

## 5. Stand der Technik (Literatur)

Recherche 2026-11-01 über Suchmaschinen; Volltexte (arXiv, MDPI, PMC, IEEE) waren aus der Audit-Umgebung nicht abrufbar.
Zahlen stammen deshalb aus Kurzfassungen und Suchauszügen; mit **(prüfen)** markierte nur aus einer Quelle. Keine der
Genauigkeiten lässt sich auf diesen Aufbau übertragen; sie zeigen nur, was mit welchem Verfahren erreicht wurde.

### 5.1 Was die Literatur zu den Befunden sagt

| Thema | Befund der Literatur [Lit] | Folge (Plan) |
|---|---|---|
| Mittelpunkt der IQ-Werte | Arcustangens-Demodulation ist nur mit genau entferntem Mittelpunkt (DC) exakt (Park et al. 2007). Im mmWave-Bereich war die geometrische Kreisanpassung unter vier Verfahren am besten (Mercuri et al. 2022). Kåsa ist auf kurzen Bögen zu kleinen Kreisen hin verzerrt; Taubin ist der beste algebraische Fit, Levenberg–Marquardt (geometrisch) der beste auf kurzen Bögen (Al-Sharadqah & Chernov 2009; Zakrzewski et al. 2012) | V1.1: Kreisfit; Taubin/LM als Variante; Prüfung der Bogenlänge |
| Fehlergröße | Mit Mittelpunkt-Fehler r (relativ zum Radius) entstehen Atem-Oberwellen mit Amplitude (λ/4π) · r · \|Jₙ(β)\|; bei 2 mm Atemhub und r = 0,1 ≈ 16 µm bei 4 × Atemfrequenz (Bessel-Modell wie Rittiplang et al. 2020) | bestätigt B2; heute ist r 0,27–0,40 |
| Mehrere Punkte kombinieren | Mehrere Entfernungspunkte verbessern Atmung und Puls (Szmola et al. 2025, Polysomnographie: Atmung MAE 0,29 → 0,20 /min); kohärent ausgerichtet und gewichtet summieren (Pi-ViMo 2023; Maximum Ratio Combining); vor der Demodulation kombinieren, nicht Spektren nachträglich mitteln | V3.1; Diagnosewert „effektive Punktzahl" statt 24-Punkte-Einigkeit |
| Atem-Oberwellen | Die Grundwelle des Herzens ist durch Atem-Oberwellen begrenzt, seine höheren Oberwellen nur durch Rauschen (Rong & Bliss 2019); zweite Ableitung / höhere Oberwellen senken den Fehler um ≈ 20–35 % (Iwata et al. 2025, (prüfen)); adaptive Kerbfilter brauchen eine genaue Atemfrequenz (Tazen et al. 2023); RLS passt sich besser an als LMS (2025) | V1.2 (Band bis 5 Hz, Summe mit 2f), V3.3 |
| Kopplung Herz–Atmung | Ganzzahlige Verhältnisse (z. B. 4:1) kommen durch kardiorespiratorische Synchronisation häufiger vor als zufällig (Schäfer et al. 1998) | Der Fall „Puls = 4 × Atmung" ist kein Pech, sondern zu erwarten; Prüfszenario `harm4` und Metronom-Sitzung R3 |
| Fensterlänge vs. Oberwellen | Rechnung: Bei 15-s-Hann-Fenster und 18 /min Atmung überdecken die Hauptkeulen der Oberwellen ≈ 89 % des Pulsbands (20 s: 67 %, 30 s: 44 %); die Oberwellen-Toleranz der DSP (0,04 Hz) ist ≈ 3-mal schmaler als die Hauptkeule | Oberwellen nicht nur markieren, sondern modellieren (V3.3) und Zeitbereichsmerkmale nutzen (V3.6) |
| Verfolgung | In der PPG-Pulsmessung senkt eine Viterbi-Dekodierung über ein Frequenzgitter den MAE um ≈ 30 % (Temko 2017: 1,97 → 1,37 /min); Radar: Spektral-Viterbi (Ye & Ohtsuki 2021), Kalman (Arsalan et al. 2020, ≈ 10-s-Fenster) | V3.2, danach kürzeres Fenster (V2.5) |
| Fensterlängen | Google Soli (60 GHz, Nest Hub): erster Wert nach 16 s, dann alle 4 s; Acconeer-Atem-App 20 s; Radar-Puls meist 8–10 s; unter ≈ 10 s sinkt die Genauigkeit | Puls 8–12 s mit Verfolgung, Atmung 20–30 s |
| Was „gut" ist | Soli (60 GHz, Schlaf, 62 Personen, 498 h): Puls MAE 1,69 /min, Meditation 1,05 /min; ausdrücklich keine HRV | Ziel ≤ 3 /min ist ehrgeizig, aber nicht abwegig; HRV nicht versprechen |
| IBI / HRV | IBI-Fehler gegen EKG von ≈ 7–15 ms (Labor, UWB/FMCW mit Strahlformung, (prüfen)) bis 83 ms Streuung (Schlaf, Turppa et al. 2020); Quantisierung bei 60 Hz erklärt nur ≈ 12 ms Fehler je Differenz; Artefakt-Regel: IBI > 20 % vom lokalen Median verwerfen; HRV-Kurzzeitwerte sind über ≈ 5 min definiert | V1.6, V3.6; RMSSD erst nach Gurt-Vergleich zeigen |
| Bewegung | Ein-Kanal-Radar kann Körperbewegung kaum herausrechnen (Übersicht Gouveia et al. 2019); Praxis: erkennen und verwerfen; Genauigkeit immer mit Verfügbarkeit angeben. Merkmale: Phasensprung > π/2 je Frame (≈ 75 mm/s bei 60 Hz), Amplitudenänderung > 3 dB; ≥ 70 % saubere Daten im Fenster | V1.3 (Anteil sauberer Daten in der Konfidenz), V3.5 |
| Atmung | Acconeer-App: 20-s-Fenster, FFT, ≤ 1 /min in Acconeers Testtabelle; klinische berührungslose Systeme 0,2–0,4 /min MAE gegen Atemgurt; Apnoe = Amplitudenabfall ≥ 90 % (Hypopnoe ≥ 30 %) über ≥ 10 s (Kang et al. 2020) | V1.4, V3.4 |
| Referenzgerät | Polar H10: 99,6 % richtig erkannte Schläge gegen Holter-EKG (Gilgen-Ammann et al. 2019) | V0.1 |
| Abnahmekriterien | ANSI/CTA-2065: MAPE < 10 %; AAMI EC13 (Puls-Anzeigen): ±10 % oder ±5 /min | Zielwerte in 6 liegen darunter |
| Acconeer und Puls | Keine begutachtete Puls-Validierung für A111/A121 gefunden; Abschlussarbeiten: Lund (A111, 30 Hz, gegen EKG; Ergebnis nicht abrufbar), KTU Kaunas (A121-Felder, Puls-Fehler Mittel −0,12, SD 1,14, Einheit vermutlich /min (prüfen)) | Ergebnisse dieses Projekts wären selbst ein Beitrag; Vorsicht bei Vergleichen |
| Maschinelles Lernen | Datenhungrig (Google: 980 h Vortraining, 37 h Feinabstimmung, MAE 4,1 /min); mit Daten einer Person droht Überanpassung; sinnvoller: kleiner Klassifikator für Güte oder Kandidatenwahl | V3.7 erst mit viel Referenzdaten |

### 5.2 Öffentliche Datensätze (für Vergleiche ohne eigene Hardware)

| Datensatz | Radar | Umfang | Referenz |
|---|---|---|---|
| Schellenberger et al. 2020, *Sci Data* 7:291 | 24 GHz CW, IQ | 30 Personen, 24 h | EKG u. a. |
| Shi et al. 2020, *Sci Data* 7:50 | 24 GHz | 11 Personen, 223 min | EKG, Herzton, Atmung |
| Sadeghi et al. 2024, arXiv:2405.12659 | mmWave FMCW | 10 Personen, auch nach Belastung | Polar H10 |
| Parralejo et al. 2026, *Sci Data* (Zenodo 10.5281/zenodo.18599983) (prüfen) | 2 × 60 GHz FMCW | 110 Personen | EKG |

CW-IQ-Daten passen am besten zum Signal eines Punkts; Wellenlänge umrechnen (24 GHz: 12,5 mm statt 5 mm). Lizenz vor
Nutzung prüfen; Daten nach `~/Data/radar-sensor/public/`, nicht ins Repo.

### 5.3 Quellen (Auswahl)

- Paterniani et al. 2023, Radar-Based Monitoring of Vital Signs: A Tutorial Overview, *Proc. IEEE* 111(3).
  https://iris.unimore.it/handle/11380/1301469
- Park, Boric-Lubecke, Lubecke 2007, Arctangent Demodulation With DC Offset Compensation, *IEEE TMTT* 55.
- Zakrzewski, Raittinen, Vanhala 2012, Comparison of center estimation algorithms…, *IEEE Sensors J.* 12(3).
  https://ieeexplore.ieee.org/document/5720255/
- Mercuri et al. 2022, *Sensors* 22(24):9697. https://www.mdpi.com/1424-8220/22/24/9697
- Al-Sharadqah, Chernov 2009, Error analysis for circle fitting algorithms. https://arxiv.org/abs/0907.0421
- Szmola et al. 2025, Multiple Range Bin Selection, *Sensors* 25(8):2596. https://www.mdpi.com/1424-8220/25/8/2596
- Rong, Bliss 2019, Spectral-Domain Harmonic Signatures, *IEEE TAES* 55(6).
- Iwata, Sumi, Tanaka, Sakamoto 2025, *IEEE Access* 13. https://arxiv.org/abs/2407.07380
- Schäfer, Rosenblum, Kurths, Abel 1998, Heartbeat synchronized with ventilation, *Nature* 392.
- Temko 2017, WFPV / Viterbi, *IEEE TBME* 64(9). https://ieeexplore.ieee.org/document/7867772
- Ye, Ohtsuki 2021, Spectral Viterbi…, *IEEE TMTT* 69(5). https://doi.org/10.1109/TMTT.2021.3054560
- Soli-Studie 2023, *Sci Rep* 13:18008. https://www.nature.com/articles/s41598-023-44714-2
- Turppa et al. 2020, *Sensors* 20:6505. https://www.mdpi.com/1424-8220/20/22/6505
- Gouveia, Vieira, Pinho 2019, Random Motion… in Bio-Radar, *Sensors* 19(3):604. https://www.mdpi.com/1424-8220/19/3/604
- Kang et al. 2020, IR-UWB und Schlafapnoe, *Sci Rep*. https://pubmed.ncbi.nlm.nih.gov/32210266/
- Gilgen-Ammann, Schweizer, Wyss 2019, RR intervals Polar H10, *Eur J Appl Physiol* 119.
  https://link.springer.com/10.1007/s00421-019-04142-5
- Schellenberger et al. 2020, *Sci Data* 7:291. https://www.nature.com/articles/s41597-020-00629-5
- Lund-Abschlussarbeit (A111): https://lup.lub.lu.se/student-papers/record/9094792 ·
  KTU-Abschlussarbeit (A121): https://epubl.ktu.edu/object/elaba:199466836/index.html

## 6. Umsetzungsplan

### Stand der Umsetzung (2026-11-01)

Die Software-Teile aller Phasen sind umgesetzt und am PC geprüft (Python-Suite `host/tests`, Prüfstand
`vital_sim.py bench --check`, Nachrechnung, App-Logik in einem JVM-Projekt). **Nichts davon ist am Gerät gelaufen:**
In der Cloud-Umgebung gab es weder ESP32-Toolchain noch Android-SDK. Die Firmware ist nur mit Stub-Headern am PC
übersetzt (`host/tests/test_firmware_pc.py`), nicht mit `pio run` gebaut und nicht geflasht; von der App sind Logik,
Parser, Speicher und JNI-Bibliotheken am PC getestet, Oberfläche (Compose), Controller und NDK-Build nicht übersetzt.
Alle Zahlen zu Varianten stammen aus dem Prüfstand **[Sim]**; ob sie an echten Daten halten, zeigt erst Tag A/B.

| Phase | umgesetzt (am PC geprüft) | offen (Gerät, Gurt, Aufnahmen, Freigabe) |
|---|---|---|
| V0 | V0.1/V0.2 Gurt (BLE), Marker, Metronom in der App; V0.4 `vital_eval.py`; V0.5 Prüfstand `--check`, Szenario `leer`; V0.3 Messplan M5 (`EXPERIMENTE.md`) | Gurt am Handy, Aufnahmen Tag A/B (V0.3), Basislinie (V0.6) |
| V1 | V1.1–V1.7 im Header (Kreisfit, Pulsband 40–180, Kandidaten/Verfolgung/Konfidenz/Lebenszeichen, Atemzustand, Sensor-Flags und `@RECAL`, IBI/RMSSD, Aufräumen) | Abnahmen an Tag A/B, Live-Lauf |
| V2 | V2.1 Report auf Kern 0 aus Schnappschuss, V2.2 FFT, V2.3 Rezoom ohne Verlust, V2.4/V2.5 schneller Start und Reaktion | Empfangspause und Rechenzeit am ESP32 messen, 30-min-Lauf, V2.3 Schritt 3 (nach V4.4) |
| V3 | V3.4 Atemzüge (`@BREATH`) und V3.5 Zustand (`@STATE`) im Standard; V3.1 Kombination, V3.2 Viterbi, V3.3 Oberwellen-Abzug, V3.6 Vorlage für Einzelschläge als Schalter, Standard aus; V3.7 nicht begonnen (Voraussetzung fehlt) | A/B an Tag A, Übernahme nur nach Tag B |
| V4 | Messplan (`EXPERIMENTE.md`, „V4 Sensor-Konfiguration"), `host/noise_floor.py` (Weg-Rauschen, Aufnahme über `bridge`), iq3-Preset als Übersetzungsschalter, Kennung in `@HELLO` | alle Messungen V4.1–V4.4 |
| V5 | V5.1 Gütezustand und Abweichung gegen den Gurt, V5.2 Handy-DSP live aus `@RAW` (`A121_DSP_HEAVY`), V5.3 alte PC-Skripte als Archiv | Screenshots je Zustand, 30 min live mit CPU ≤ 20 %, NDK-Build |

Einzelergebnisse und Befehle je Aufgabe: `docs/EXPERIMENTE.md` (Prüfstand, Messpläne), `docs/ANDROID_APP.md` (App,
offene Gerätetests), `docs/HARDWARE.md` (Befehle, Schalter).

**Abschluss-Review (2026-11-01), behoben und am PC geprüft:** Puls in der App grau bei ungültigem oder veraltetem
Report (auch in APNOE); keine APNOE mehr im leeren Raum nach dem Gehen (Prüfstand-Szenario `weg`); Prüfstand zählt den
ersten *richtigen* Wert und frühe Fehlwerte (`erst_ok_s`, `frueh_falsch_n`); Handy-DSP liefert auch mitten im Stream
eingeschaltet Werte; Firmware mit `-ffp-contract=off` wie die Nachrechnung; eine Zeit je Report für
`@DSP`/`@ROOM`/`@VITAL`/`@STATE`; Gurt verbindet nach fehlgeschlagener Dienstsuche wieder; Metronom gibt seinen
Tongeber erst nach dem letzten Ton frei. Am Gerät offen: Firmware bauen/flashen, danach `replay-cpp` gegen eine neue
Live-Aufnahme und `compute_us` gegen vorher; Gurt-Wiederverbindung und Metronom am Handy.

### Leitlinien

- Reihenfolge: **messen können → Fehler beheben → schneller → besser.** Ohne V0 kein Urteil über V1–V5.
- Jede DSP-Änderung läuft durch drei Prüfungen, getrennt ausgewiesen: (1) Prüfstand `vital_sim.py bench`
  (Regression), (2) Nachrechnung der Aufnahmen mit Referenz (`vital_eval.py`, Testdaten nur für die Abnahme),
  (3) Live-Lauf am Gerät. Build/Compile ist keine Laufzeitprüfung.
- Eine DSP-Quelle: `a121_esp_dsp.h`. PC-Skripte werden für Forschung genutzt; was sich bewährt, wandert in den Header.
- Protokoll nur hinten erweitern (`docs/ANDROID_APP.md`), damit PC-Skripte und App weiterlaufen.
- Puls bleibt „Kandidat", bis V0 eine Genauigkeit gemessen hat.

### Zielwerte (gegen Referenz, ruhig sitzend 0,5–0,8 m, Brust zum Sensor; erst nach V0 messbar)

| Kennzahl | heute | Ziel |
|---|---|---|
| Puls MAE gegen EKG-Gurt | unbekannt | ≤ 3 /min |
| Anteil Reports innerhalb ±5 /min | unbekannt (gegen Selbstzählung ±8 /min: 39–100 %) | ≥ 90 % |
| Verfügbarkeit (Anteil Zeit mit gültigem Puls) | 86 % (T1, ohne Gütebewertung) | ≥ 90 % mit Konfidenz ≥ Schwelle |
| Konfidenz kalibriert | nein | Reports mit Konfidenz ≥ 70 %: ≥ 90 % innerhalb ±5 /min |
| Atmung MAE (Metronom-Atmung) | unbekannt | ≤ 1 /min |
| Atem anhalten erkannt | nein | ≤ 12 s nach Beginn, keine Fehlalarme bei normaler Atmung |
| erste Werte nach Start / Rezoom | 8 s / 8 s | ≤ 10 s / ≤ 3 s |
| Reaktion auf Pulsänderung | 10–11 s [Sim] | ≤ 7 s |
| Ruhendes Echo ohne Person | Puls als gültig gemeldet [Sim] | kein gültiger Report |
| Empfangspause am ESP32 | bis 337 ms | ≤ 50 ms |
| DSP-Rechenzeit je Report | 270–320 ms | ≤ 80 ms |

Zum Vergleich [Lit]: Normen für Puls-Anzeigen verlangen MAPE < 10 % (ANSI/CTA-2065) bzw. ±10 % oder ±5 /min (AAMI EC13);
Google Soli (60 GHz) erreichte im Schlaf 1,7 /min MAE, ohne HRV.

### Regeln für alle Aufgaben (aus `AGENTS.md`)

- Aufnahmen, Datensätze, Binärdateien nicht ins Repo; Daten nach `~/Data/radar-sensor/` (neu: `ref/` für
  Aufnahmen mit Referenz, `sim/` für Prüfstand-Frames).
- Vor Flashen den Port über `ls -l /dev/serial/by-id` eindeutig bestimmen; Handy nur mit Freigabe des Projektinhabers, Seriennummer live
  aus `adb devices -l`.
- Funkmodule unverändert betreiben. Keine Aussage über Puls-Genauigkeit ohne Referenz; Build und Gerätetest getrennt
  ausweisen.
- `@VITAL` behält seine 15 Felder; neue Werte nur hinten an `@DSP` oder als neue Tags.
- Jede Änderung am DSP-Header: `vital_sim.py bench --check`, `a121_esp_ultimate.py --mode replay-cpp` (Format- und
  Ablaufgleichheit) und `cd android && ./gradlew :app:testDebugUnitTest` laufen lassen.

### Worauf beim Prüfen besonders achten

Fälle, die leicht übersehen werden; jeder bekommt einen Test in der genannten Aufgabe:
1. **Niemand vor dem Sensor** (leerer Stuhl, Person steht auf): Es darf kein gültiger Puls erscheinen; heute erscheint
   er (B19). → Szenario `leer` im Prüfstand (V0.5), Lebenszeichen-Test (V1.3), Zustand „keine Person" (V3.5).
2. **Puls minutenlang genau auf einer Atem-Oberwelle** (z. B. Metronom 18 /min, Puls 72): Wert darf falsch sein, die
   Konfidenz muss die Unsicherheit zeigen (≤ 60 %). → `harm4` (V1.3).
3. **Dauerlauf mit Temperaturänderung** (`calibration_needed`): Neu-Kalibrierung darf den Stream nicht abbrechen, die
   Nachrechnung muss ihr folgen. → V1.5, 30-min-Lauf.
4. **Rezoom während Bewegung oder Atem anhalten:** keine alten Werte als gültig weiterreichen. → V2.3, V3.5.
5. **Rückstau am USB bei `raw` und zweitem Kern:** kein Frameverlust, keine tick-Lücken. → V2.1, 30-min-Lauf.

---

### Phase V0 — Messgrundlage (Voraussetzung für alles Weitere)

#### V0.1 Referenzpuls in der App (EKG-Brustgurt über BLE)

Gerät: **Polar H10** (Standard-BLE-„Heart Rate Service", liefert je Benachrichtigung Puls und RR-Intervalle in
1/1024 s; Begründung und Belege in Abschnitt 5). Andere Gurte mit Heart Rate Service gehen genauso.

**Dateien:**
- Neu: `android/app/src/main/java/de/timo/radarsensor/reference/HrMeasurement.kt` (Parser, reines Kotlin)
- Neu: `android/app/src/main/java/de/timo/radarsensor/reference/HrReference.kt` (BLE-Suche, Verbinden, Benachrichtigungen)
- Neu: `android/app/src/test/java/de/timo/radarsensor/reference/HrMeasurementTest.kt`
- Ändern: `AndroidManifest.xml` (`BLUETOOTH_SCAN` mit `neverForLocation`, `BLUETOOTH_CONNECT`), `record/Recorder.kt`,
  `core/RadarController.kt`, `ui/Pages.kt` (Vital-Tab: Referenzkurve), `debug/DebugReceiver.kt` (`ref_scan`,
  `ref_connect --es addr <mac>`, `ref_status`)

**Schnittstellen:**
- `data class HrMeasurement(val bpm: Int, val rrMs: List<Double>, val contact: Boolean?)`
- `fun parseHrMeasurement(bytes: ByteArray): HrMeasurement?` (Characteristic `0x2A37`: Flags Bit 0 = 16-Bit-Puls,
  Bits 1–2 = Hautkontakt, Bit 3 = Energiefeld vorhanden (2 Byte überspringen), Bit 4 = RR-Werte folgen, je uint16 LE)
- `Recorder.reference(rxNanos: Long, m: HrMeasurement)` → `ref_hr.csv` mit Kopf `rx_ms,hr_bpm,rr_ms,contact`, eine Zeile
  je RR-Wert (ohne RR: `rr_ms` leer); `rx_ms` wie in `rxtime.csv` (ms seit Aufnahmebeginn, `elapsedRealtimeNanos`).

- [ ] Tests schreiben: `parses8BitHrWithRr` (`10 48 00 04` → 72, `[1000.0]`), `parses16BitHrTwoRr`
  (`11 48 00 00 04 00 03` → 72, `[1000.0, 750.0]`), `skipsEnergyExpended` (`18 48 10 00 00 04` → 72, `[1000.0]`),
  `contactBits` (Flags `06` → true, `04` → false, `00` → null), `rejectsTooShort` (`10` → null).
- [ ] `./gradlew :app:testDebugUnitTest` → die neuen Tests schlagen fehl; Parser umsetzen → grün.
- [ ] BLE-Client, Recorder-Datei, Referenzkurve im Vital-Tab, Debug-Befehle.
- [ ] Build: `cd android && ./gradlew :app:testDebugUnitTest :app:assembleDebug` grün.
- [ ] Am Handy (nur mit Freigabe): 10 min `iq3 0 raw` mit Gurt. **Fertig, wenn:** `ref_hr.csv` vorhanden, Summe der RR-Werte
  = Aufnahmedauer ±2 %, Puls in der App = Anzeige der Polar-App ±1 /min.

#### V0.2 Marker und Metronom-Atmung (Atemreferenz)

Eine bekannte Atemfrequenz ist die einfachste Atemreferenz: Die App gibt den Takt vor (Ton), die Person atmet mit.

- Ändern: `record/Recorder.kt`: `fun mark(rxNanos: Long, text: String)` → `events.csv` (`rx_ms,text`).
- Neu: `ui/Metronome.kt` (ToneGenerator, Takte 6/10/15/20 /min; jeder Einatem-Ton → `mark(…, "ein")`).
- Ändern: `ui/Pages.kt`: Knöpfe „Atem anhalten" (Start/Ende), „Bewegung", „Zählfenster", Metronom.
- Test: `RecorderTest.writesEventsCsv` (temporärer Ordner, zwei Marker → zwei Zeilen in Reihenfolge).
- **Fertig, wenn:** Testaufnahme enthält `events.csv`; Abstand der Metronom-Marker = Sollwert ±20 ms.

#### V0.3 Messplan Referenzdatensatz

In `docs/EXPERIMENTE.md` als **M5** eintragen. Jede Sitzung mit Gurt, `iq3 0 raw`, Markern; Ablage
`~/Data/radar-sensor/ref/<datum>_<sitzung>/`.

| Sitzung | Inhalt | Dauer | prüft |
|---|---|---|---|
| R1 | ruhig sitzend, normal atmen, 0,6 m | 5 min | Grundgenauigkeit |
| R2 | 3 × 20 s Atem anhalten | 3 min | Apnoe, Puls ohne Atmung |
| R3 | Metronom 6 / 10 / 15 / 20 /min | je 2 min | Atmung, Oberwellen-Kollision |
| R4 | 1 min Kniebeugen, dann sitzend erholen | 6 min | Pulsbereich 110 → 70, Reaktionszeit |
| R5 | Arm heben, zurücklehnen, husten, sprechen | 3 min | Bewegung, Zustand, Konfidenz |
| R6 | Abstand 0,4 / 0,6 / 0,8 / 1,0 m | je 3 min | Reichweite |
| R7 | ohne Linse / hyperbolische Linse | je 3 min | Linse |
| R8 | liegend, Sensor über der Brust | 5 min | Haltung |

- **Trennung:** Tag A = Entwicklung (darf beliebig oft ausgewertet werden), Tag B = Test (nur zur Abnahme einer Phase).
  Weitere Personen nur mit Einwilligung; Daten pseudonym (`p1`, `p2`).
- **Fertig, wenn:** Plan eingetragen, beide Tage aufgenommen, `esp_record.py --check` ohne Lücken.

#### V0.4 Auswertung gegen Referenz: `host/vital_eval.py`

**Dateien:** neu `host/vital_eval.py`, neu `host/tests/test_vital_eval.py` (`unittest`, keine neue Abhängigkeit).

**Schnittstellen:**
- `load_recording(folder: Path) -> Recording` (Dataclass: `vital` [rx_ms + 15 Felder], `dsp` [rx_ms + Felder],
  `beats_ms` [Schlagzeiten aus `ref_hr.csv`: je Benachrichtigung rückwärts von `rx_ms` aufsummierte RR], `events`).
- `ref_hr_at(beats_ms: np.ndarray, t_end_ms: float, win_s: float = 15.0) -> float` (60 000 / mittleres IBI im Fenster).
- `metrics(est: np.ndarray, ref: np.ndarray, valid: np.ndarray) -> dict` mit `n, coverage_pct, mae, rmse, bias,
  loa_low, loa_high, p5, p10` (Bland-Altman-Grenzen = bias ± 1,96 SD).
- `calibration(conf, abs_err, edges=(0, 30, 50, 70, 90, 101)) -> list[tuple]` (je Konfidenzklasse: Anzahl, Anteil ±5).
- Kommandozeile: `vital_eval.py <ordner>… [--replay <ordner mit a121_esp_dsp.h>] [--md <bericht.md>]`. Mit `--replay`
  wird `stream.log` vorher mit `host/dsp_replay/` und dem angegebenen Header neu gerechnet (Varianten vergleichbar);
  dafür bekommt `run_replay_cpp()` in `host/a121_esp_ultimate.py` den Header-Ordner als Parameter.
- Ausgabe je Sitzung und gesamt: Puls (MAE, ±5, Bland-Altman, Verfügbarkeit, Kalibrierung), Atmung gegen Metronom,
  Apnoe-Erkennung gegen Marker (Verzug, Fehlalarme), Zeit bis zum ersten Wert, Reaktionszeit in R4.

- [ ] Tests: `test_ref_hr_constant` (Schläge alle 800 ms → 75,0 ± 0,01), `test_metrics_basic` (Schätzung 70/72/80,
  Referenz 70/70/70 → MAE 4,0, p5 66,7), `test_coverage` (gültig 1/0/1/1 → 75,0), `test_calibration_bins`.
- [ ] `.venv/bin/python -m unittest host/tests/test_vital_eval.py` → erst rot, nach Umsetzung grün.
- [ ] **Fertig, wenn:** Bericht für eine R1-Aufnahme erzeugt wird und `--replay` mit dem unveränderten Header dieselben
  Zahlen liefert wie die Live-Werte (bis auf Rundung).

#### V0.5 Prüfstand als Regressionstest

- Ändern: `host/vital_sim.py` (Szenario `leer` = ruhendes Echo ohne Atmung und Puls ist schon da): Szenario „keine
  Person" (nur Möbel und Wand) und `--check`: feste Grenzwerte je Szenario (heute: Werte der Tabelle in Abschnitt 4), Exit-Code 1 bei Verschlechterung. Jede spätere DSP-Aufgabe
  verschärft die Grenzwerte, die sie verbessert.
- **Fertig, wenn:** `vital_sim.py bench --check` mit dem heutigen Header Exit 0 liefert und mit einem absichtlich
  verschlechterten Header (z. B. `kHrFMin` 1,0 Hz) Exit 1.

#### V0.6 Basislinie

- Heutige Firmware (`2026-11-01-app1`) auf Tag A und Tag B auswerten, Ergebnis als „Basislinie" in M5.
- **Fertig, wenn:** Tabelle mit MAE, ±5-Anteil, Verfügbarkeit, Kalibrierung je Sitzung in `docs/EXPERIMENTE.md`.

#### V0.7 (optional) Öffentliche Datensätze

- `host/vital_eval.py --public <ordner>`: CW-IQ-Datensatz (Schellenberger et al. 2020, 5.2) auf das `iq3`-Format
  umrechnen (Phase × 12,5 / 4,955 auf 60 GHz skalieren, auf 60 Hz umtasten, als 24 gleiche Zoom-Punkte mit Rauschen) und
  mit der Nachrechnung auswerten. Zeigt, ob eine Änderung nur für eine Testperson gilt.
- **Fertig, wenn:** Bericht über ≥ 10 Personen des Datensatzes für die Basislinie und jede V1-Fassung.

**Entscheidungspunkt nach V0:** Liegt der Puls in R1 schon bei ≥ 90 % innerhalb ±5, verschiebt sich der Schwerpunkt
auf Verfügbarkeit und Geschwindigkeit (V2). Liegt er darunter, zuerst V1.

---

### Phase V1 — Gezielte Korrekturen in `a121_esp_dsp.h`

Jede Aufgabe = ein Commit. Prüfung je Aufgabe: Prüfstand (`--check`), Tag-A-Auswertung (`vital_eval.py --replay`),
5 min live. Abnahme der Phase auf Tag B.

#### V1.1 Kreisfit statt Mittelwert (B2)

- Neu im Header: `static bool circleCenter(const float *ring, uint16_t start, uint16_t N, float mean_i, float mean_q,
  float &ci, float &cq)`: Kåsa-Fit in Bullock-Form, Summen in `float` über (u, v) / s mit s = mittlerer Abstand zum
  Mittelwert (damit `float` reicht, kein `double` auf dem ESP32). Rückfall auf den Mittelwert, wenn die Matrix schlecht
  bestimmt ist (det ≤ 10⁻⁶ · Suu · Svv) oder der Mittelpunkt mehr als 10 × s vom Mittelwert entfernt liegt.
- `binDisplacement()` nutzt den Mittelpunkt; Rückgabe weiter der mittlere Abstand zum (neuen) Mittelpunkt.
- Bogen-Prüfung: Überstreicht die Phase im Fenster < 2 rad, ist der Fit schlecht bestimmt (Literatur 5.1) → letzten
  guten Mittelpunkt dieses Punkts behalten und sein Gewicht halbieren.
- Variante auf Tag A vergleichen: Taubin-Fit (gleicher Aufwand, weniger Verzerrung auf kurzen Bögen) und 3–5
  Gauß-Newton-Schritte auf den geometrischen Abstand. Übernehmen, was auf Tag A besser ist.
- **Wichtig:** Im Prüfstand verschlechtert der Kreisfit allein `tachy` (60 statt 120). V1.1 erst zusammen mit V1.3
  auf das Gerät.
- **Fertig, wenn:** Prüfstand `harm4`, `flach`, `tief` ≥ 90 % innerhalb ±5 (heute 0 / 54 / 60 %), `base` ≥ 97 %;
  Tag A: MAE in keiner Sitzung schlechter als die Basislinie, gesamt besser.

#### V1.2 Pulsband 40–180 /min, 2. Herz-Oberwelle (B5)

- Bandpass neu: `scipy.signal.butter(4, [0.6, 5.0], 'band', fs=60, output='sos')` (Koeffizienten in `kBpSos`, Kommentar
  mit dem Aufruf wie heute).
- Suchbereich 0,67–3,0 Hz (40–180 /min), Spektrum bis 6 Hz für die Oberwellen-Summe
  S(f) = P(f) + 0,5 · P(2f). Kandidaten nach S(f), Bandrand-Sperre beibehalten.
- Rechenaufwand steigt (mehr Frequenzen) → mit V2.2 (FFT) zusammen umsetzen oder Frequenzraster oberhalb 3 Hz auf
  0,02 Hz vergröbern.
- **Fertig, wenn:** Prüfstand `brady` ≥ 90 % (heute 0 %), übrige Szenarien ohne Verschlechterung; Tag A R4
  (Puls über 110) erfasst.

#### V1.3 Kandidatenwahl, Verfolgung und Konfidenz neu (B4, B6)

- Atem-Oberwellen: Kandidaten ±0,04 Hz um m · f_Atem (m = 2..8, nur bei Atem-Spitze/Median ≥ 2) bekommen S × 0,5.
- Verfolgung: Ein naher Kandidat (±12 /min) gewinnt nur mit S ≥ 0,5 · S_max. Liegt der verfolgte Wert auf einer
  Oberwelle und gibt es 3 Reports lang einen Kandidaten ohne Oberwelle mit S ≥ 0,5 · S_verfolgt, wird gewechselt.
- Konfidenz neu: `static float signalQuality(const QualityInputs &q)` mit
  `struct QualityInputs { float peak_db; bool on_harmonic; float hr_spec, hr_acf, hr_ibi, hr_fused, hr_fused_5s_ago;
  float motion_pct; }` → 100 × clamp((peak_db − 3) / 12) × (0,5 bei Oberwelle) × (1 / 0,7 / 0,5 je nach Einigkeit
  Spektrum–AKF–IBI ±5 /min) × (1 − motion_pct / 100) × (0,7, wenn sich der Wert in 5 s um > 8 /min geändert hat).
  Faktoren auf Tag A abstimmen, danach festhalten.
- In die Konfidenz geht auch der Anteil sauberer 1-s-Blöcke ein; unter 70 % ist der Report ungültig (Literatur 5.1).
- Lebenszeichen-Test (B19): `valid` nur, wenn eine Atem-Spitze da ist (Spitze/Median ≥ 2) oder der Atemzustand
  „angehalten" ist und die Pulsspitze ≥ 8 dB über dem Median liegt. Sonst `valid = false`.
- Die alte Punkt-Einigkeit bleibt als Diagnosewert: hinten an `@DSP` als `point_agree_pct`.
- **Fertig, wenn:** Prüfstand: Konfidenz-Median der Reports mit > 10 /min Fehler ≤ 40 % (heute bis 98 %), der richtigen
  ≥ 70 %; `tachy` ≥ 90 % (auch mit V1.1); Szenario `leer` ohne gültigen Report; Tag A: Kalibrierungstabelle steigt mit
  der Konfidenz, Konfidenz ≥ 70 % → ≥ 90 % innerhalb ±5.

#### V1.4 Atem anhalten und Atemzustand (B9)

- Ring mit 60 Werten: Effektivwert des Atemsignals (0,05–0,7 Hz) je Sekunde am Punkt mit der stärksten Atmung.
- `br_state`: 0 normal, 1 keine Atemspitze, 2 Atem angehalten / Apnoe (Amplitude ≥ 90 % unter dem Median der letzten
  60 s, seit ≥ 10 s), 3 Bewegung (motion_pct > 50), 4 flache Atmung (≥ 30 % darunter, seit ≥ 10 s). Definition wie in der
  Schlafapnoe-Literatur (5.1). Bei 2 ist `br_bpm` = 0. Neu hinten an `@DSP`.
- App zeigt den Zustand (Ereignisleiste und Vital-Tab).
- **Fertig, wenn:** Prüfstand `anhalt`: Zustand 2 spätestens 12 s nach Beginn, kein Zustand 2 in `base`, `step`, `flach`,
  `tief`; Tag A R2: alle Anhalte-Abschnitte erkannt, Verzug ≤ 12 s, keine Fehlalarme in R1.

#### V1.5 Sensor-Flags und Lücken (B15, B16)

- `cmdIq3()` zählt je Report `frame_delayed`, `data_saturated`, tick-Abstände > 25 ms (ohne Rezoom) und gibt sie hinten an
  `@DSP` aus (`n_delayed,n_sat,n_tickgap`).
- `calibration_needed`: `@WARN,calibration_needed`, Session mit demselben Zoom-Fenster neu aufsetzen (wie ein Rezoom),
  `@RECAL,t_ms`. `host/dsp_replay/dsp_replay.h` behandelt `@RECAL` wie `@REZOOM`; App-Parser kennt den Tag.
- Gesättigte Frames im Fenster senken die Konfidenz (Faktor 0,5 ab 1 %).
- Herstellerregel für Neu-Kalibrierung: siehe Abschnitt 1.5.
- **Fertig, wenn:** 30-min-Lauf live: Zähler im Log, `replay-cpp` folgt `@RECAL`, kein Abbruch; Strg+R-Test unverändert.

#### V1.6 IBI und RMSSD (B7)

- Spitzen parabolisch zwischen den Abtastwerten verfeinern; Sperrzeit 0,6 × (60 / `hr_fused`) statt fest 360 ms; IBI
  verwerfen, wenn > 20 % vom Median der letzten 5 IBI entfernt (Artefakt-Regel, 5.1). RMSSD nur, wenn im Fenster ≥ 90 %
  der erwarteten Schläge gültig sind, sonst 0. Schläge auf dem breiteren Band aus V1.2 (0,6–5 Hz). Für HRV später ein
  eigenes 2–5-min-Fenster (HRV-Kurzzeitwerte sind so definiert).
- App: RMSSD-Diagramm „experimentell, nicht validiert", ausgeblendet bei 0.
- **Fertig, wenn:** Prüfstand RMSSD gemessen/wahr 0,7–1,3 in `base`, `step`, `flach`, `tief` (heute 1,4–1,7);
  Tag A: IBI gegen Gurt-RR ausgewertet (Median-Fehler dokumentiert, Ziel erst danach festlegen).

#### V1.7 Aufräumen (B18)

- `hist_um_` entfernen, `kRespMinWin` nutzen oder entfernen, Beschreibung der ESP32-Kette in README/HARDWARE korrigieren.

---

### Phase V2 — Geschwindigkeit und Verfügbarkeit

#### V2.1 DSP auf dem zweiten Kern (B12)

- `a121_esp_dsp.h`: `struct ReportSnapshot { uint16_t write_idx, count; uint32_t frame_count; uint8_t best_zoom_bin;
  float room_amp[kRoomPoints], room_mot[kRoomPoints], room_amp_slow[kRoomPoints], room_mot_slow[kRoomPoints]; }`,
  `void takeSnapshot(ReportSnapshot &s) const`, `bool computeVitalReport(const ReportSnapshot &s, VitalReport &rep)`.
  Die alte Signatur `computeVitalReport(VitalReport &)` bleibt (Schnappschuss + Rechnen) für Nachrechnung und Prüfstand.
- `kHistoryLen` 960 → 1080 (18 s): Der Report darf bis zu 3 s rechnen, ohne dass sein Fenster überschrieben wird.
- `main.cpp`: Task `dspTask` mit `xTaskCreatePinnedToCore(…, 0)`; die Leseschleife (Kern 1) legt je 60 Frames einen
  Schnappschuss in eine Queue (Länge 1), druckt Ergebnisse, sobald sie zurückkommen, und führt Rezooms aus. Ein Rezoom
  wartet auf das laufende Ergebnis (er wird ohnehin aus ihm entschieden).
- `@DSP` bekommt hinten `snap_frame` (Frame-Zähler des Schnappschusses); `dsp_replay.h` rechnet den Report an diesem
  Frame (sonst wie heute an der `@DSP`-Zeile).
- **Fertig, wenn:** live `iq3 60` und 30 min: größter tick-Abstand ohne Rezoom ≤ 25 ms, Empfangspause ≤ 50 ms, kein
  Watchdog-Neustart; `replay-cpp` gleich bis auf die letzte Stelle.

#### V2.2 Rechenzeit senken (B13)

- Hann-Fenster und Frequenz-Koeffizienten einmal in `begin()` berechnen.
- `removeLinearTrend()` und `binDisplacement()` in `float` (Σt² geschlossen berechnet).
- `static inline float fastAtan2(float y, float x)` (Polynom, Fehler ≤ 1 · 10⁻⁴ rad ≈ 0,04 µm); Test gegen `atan2` auf
  einem Gitter im Prüfstand.
- Spektren per reeller FFT (1024 Punkte, nullaufgefüllt, 15 Hz → 0,015 Hz je Bin, parabolisch verfeinert) statt Goertzel.
  Eine portable FFT im Header (`a121_fft.h`), damit ESP32, PC und Handy dieselbe Rechnung haben.
- **Fertig, wenn:** `compute_us` live Median ≤ 80 ms; Prüfstand innerhalb der Grenzwerte; Tag A gleich gut (±0,3 /min MAE).

#### V2.3 Rezoom ohne Verlust (B11)

1. **Analyse zuerst (offline):** In der 30-min-Aufnahme T1 (38 Rezooms, `@RAW`) prüfen, ob die Phase an gleichen
   Entfernungen über den Session-Neustart stetig bleibt, mit und ohne Loopback-Derotation. Kleines Skript
   `host/rezoom_check.py`. Ergebnis in M5.
2. Verfolgung behalten: `resetHistory()` getrennt von `resetState()`; `hr_prev_` bleibt, wenn der letzte gültige Wert
   < 30 s alt ist.
3. Breiteres Zoom-Fenster: `kZoomPoints` 24 → 48 (120 mm, 129 Punkte je Frame, ≈ 31 kB/s Nutzdaten). Anpassen:
   Preset, `@RAW`-Doku, `host/esp_record.py` (`ZOOM_POINTS`, `TOTAL_3SUB`), App-Doku in `EspMsg.kt`, Nachrechnung.
   Spektren nur für die 16 Punkte mit der stärksten Atmung (Rechenzeit).
4. Wenn Schritt 1 Stetigkeit zeigt: beim Rezoom überlappende Punkte übernehmen, die ≈ 0,4-s-Lücke linear füllen.
- **Fertig, wenn:** 30 min live: ≥ 95 % der Reports gültig (heute 86 %), Rezooms je 30 min dokumentiert; Tag A ohne
  Verschlechterung.

#### V2.4 Schneller Start (B14)

- Wachsendes Fenster ab 6 s (Konfidenz × N/900), Start der Verfolgung mit dem bisherigen Wert nach Rezoom (V2.3).
- Zielsuche beim Start: 2 s Raum auswerten und sofort zoomen, statt auf 3 Reports des geglätteten Profils zu warten.
- **Fertig, wenn:** Prüfstand erster Wert ≤ 6 s; live erster gültiger Puls ≤ 10 s nach `iq3`.

#### V2.5 Reaktionszeit

- Fensterlänge 10 / 12 / 15 s mit der Verfolgung aus V3.2 auf Tag A (R4) vergleichen; kürzestes Fenster ohne
  MAE-Verlust > 0,5 /min wählen.
- **Fertig, wenn:** Verzug in R4 ≤ 7 s (Zeit, bis der Wert 50 % eines Pulsabfalls nachvollzogen hat).

---

### Phase V3 — Bessere Algorithmen (je Variante A/B auf Tag A, übernommen nur nach Bestätigung auf Tag B)

| # | Vorhaben | Kern | Fertig, wenn |
|---|---|---|---|
| V3.1 | Punktauswahl und Kombination | Punkte nach Atem-SNR und Amplitude wählen; Punkte nach dem Kreisfit phasengleich ausrichten und gewichtet summieren (Gewicht = Amplitude²/Rauschen) oder 1. Hauptkomponente der besten K = 8–16 Punkte, dann **ein** Kanal demodulieren statt 24 Einzelspektren; effektive Punktzahl N_eff = (Σλ)²/Σλ² als Diagnosewert; Raum-Punkte (Profil 3) an der Person als zweiter Kanal und Rückfall beim Rezoom | MAE ≤ heute bei ≤ 50 % Rechenzeit |
| V3.2 | Verfolgung als Viterbi über die letzten 20 Spektren | Zustände 40–180 /min (1 /min), Übergang Gauß σ = 3 /min je s, Emission = normiertes S(f); Konfidenz = Wahrscheinlichkeitsmasse ±5 /min | weniger Sprünge > 15 /min, ±5-Anteil ≥ heute |
| V3.3 | Atem-Oberwellen geführt entfernen | Oberwellen-Regression auf die Atemphase (wie `project_out_respiration_harmonics()` in den PC-Skripten), aber m mit \|m · f_Atem − f_Puls\| < 0,1 Hz auslassen | R3 (Metronom) besser, R1 nicht schlechter |
| V3.4 | Atmung Atemzug für Atemzug | Spitzen/Täler des tiefpassgefilterten Wegs (0,05–0,8 Hz) mit Hysterese; Rate = 60 / Median der letzten 4 Intervalle; Band 4–42 /min; neue Zeile `@BREATH,t_ms,tick,intervall_ms,tiefe_um` | R3 alle Takte (auch 6 /min) ±1 /min; erster Wert nach 2 Atemzügen |
| V3.5 | Zustand der Messung | KEINE_PERSON / SUCHE / ERFASSUNG / GÜLTIG / BEWEGUNG / APNOE aus Raumprofil, Zoom-Amplitude, Bewegungsanteil, `br_state`; neue Zeile `@STATE,t_ms,zustand,seit_ms` | Prüfstand `leer` ohne gültigen Puls; R5 Bewegungsabschnitte als BEWEGUNG |
| V3.6 | Einzelschläge mit Vorlage | Band 0,6–8 Hz, Vorlage aus gemittelten Schlägen, Korrelationsfilter, verfeinerte Spitzen | IBI-Fehler gegen Gurt-RR halbiert gegenüber V1.6 |
| V3.7 | Kleines ML-Modell (nur PC/Handy, Forschung) | erst mit ≥ 5 h Referenzdaten von mehr als einer Person | — |

---

### Phase V4 — Sensor-Konfiguration vermessen (Experimente G1, G5, M3)

| # | Messung | Aufbau | Ergebnis entscheidet |
|---|---|---|---|
| V4.1 | Weg-Rauschen statisch | Metallplatte 0,6 m, je 60 s mit `sess`: Zoom Profil 1/2/3 × HWAAS 16/32/64/128 × 30/60 Hz; Loopback mit 15,6 MHz statt 19,5 MHz, Loopback-Profil 1/4/5 (2 ist verboten), Gain 10/16; Derotation roh / tiefpass / aus; Phase vor und nach Session-Neustart | Rauschboden (µm RMS im Band 0,6–5 Hz), Nutzen der Loopback-Derotation, Stetigkeit für V2.3 |
| V4.2 | Person mit Gurt | 3 beste Konfigurationen aus V4.1, je R1 | `iq3`-Preset (Profil, HWAAS, Frame-Rate) |
| V4.3 | Linse und Abstand | ohne / hyperbolisch × 0,4 / 0,6 / 0,8 / 1,0 m | empfohlener Aufbau, Reichweite für Puls |
| V4.4 | Zoom-Breite | 24 gegen 48 Punkte | V2.3 Schritt 3 |

Datenrate bleibt unter 50 % der Link-Grenze (≈ 100 kB/s), sonst nicht übernehmen.

---

### Phase V5 — App und Handy

| # | Vorhaben | Fertig, wenn |
|---|---|---|
| V5.1 | Gütezustand anzeigen: Puls grau bei Konfidenz < 50 % oder Bewegung, „Erfassung läuft x s", Atemzustand, RMSSD nur experimentell; Referenzkurve und Abweichung live (V0.1) | Screenshot je Zustand aus einer Wiedergabe |
| V5.2 | Live-DSP am Handy aus `@RAW` (A6): derselbe Header per JNI (`nativePushFrame`, `nativeReport`), schwerere Varianten (V3.2, V3.3, längere Fenster) nur dort über `#define A121_DSP_HEAVY`; ESP32- und Handy-Wert nebeneinander | 30 min live, Handy-Rechenzeit ≤ 20 % CPU eines Kerns |
| V5.3 | PC-Skripte: alte Varianten in den Kopfkommentaren als „Archiv" kennzeichnen; neue Auswertungen nur über `vital_eval.py` und Header-Varianten | Doku angepasst |

---

### Reihenfolge und Aufwand

| Schritt | Aufgaben | Aufwand (grob) | braucht |
|---|---|---|---|
| 1 | V0.1, V0.2 (App) parallel zu V0.4, V0.5 (PC) | 2–3 Abende | Polar H10 (≈ 80–90 €) |
| 2 | V0.3 Aufnahmen Tag A und B, V0.6 Basislinie | 2 Sitzungen à 1 h | 1 |
| 3 | V1.1 + V1.3 zusammen, dann V1.2, V1.4, V1.5, V1.6, V1.7 | 3–4 Abende | 2 |
| 4 | V2.1, V2.2 | 2 Abende | 3 |
| 5 | V2.3 (Analyse zuerst), V2.4, V2.5 | 2–3 Abende | 4 |
| 6 | V4.1, V4.2 (kann ab Schritt 2 parallel laufen) | 2 Sitzungen | 1 |
| 7 | V3.x nach Bedarf, V5.x | offen | 3–5 |

Abnahme jeder Phase: Tag-B-Auswertung in `docs/EXPERIMENTE.md` (M5), Gerätetest getrennt vom Build ausgewiesen.

## 7. Offene Fragen und Risiken

- **Eine Person, ein Raum:** Alle bisherigen Daten stammen von einer Testperson am Schreibtisch. Schwellen (Raum-Ziele, Bewegung,
  Konfidenz) können für andere Körper, Kleidung, Haltungen falsch liegen. Gegenmittel: R6–R8, wenn möglich weitere
  Personen.
- **Referenz selbst:** Der Gurt misst elektrisch (EKG-R-Zacke), das Radar mechanisch (Brustbewegung); zwischen beiden
  liegt eine Laufzeit von einigen 10 ms und sie schwankt. Für Puls im 15-s-Fenster unerheblich, für IBI-Vergleiche
  zeitlich ausrichten (Kreuzkorrelation der IBI-Folgen).
- **Bitgleichheit:** Kreisfit, schnelle `atan2` und FFT ändern alle Ausgaben; die Nachrechnung bleibt PC = Handy
  bytegleich, gegen den ESP32 wie heute nur bis auf die letzte Stelle. Abweichungen größer als heute sind ein Fehler.
- **Overfitting:** Parameter nur auf Tag A abstimmen; Tag B nur zur Abnahme. Prüfstand-Grenzwerte sind kein Ersatz.
- **Kabel und Mobilaufbau:** Kabelbewegung erzeugt Artefakte (ANDROID_APP.md); Handy-Beschleunigungssensor (Idee I4) als
  späterer Bewegungsmarker.
- Kein Medizinprodukt; Puls bleibt ein experimenteller Wert, auch wenn die Zielwerte erreicht werden.
