# Puls nach Bewegung schneller wiederfinden (V6) — Entwurf

Stand 2026-11-02. Gehört zu `docs/AUDIT_VITAL.md` (Phasen V0–V5); Umsetzung in `firmware/esp32s3/src/a121_esp_dsp.h`.

## Ziel

Der Puls soll nach einer Bewegung in Sekunden statt in einer halben Minute wieder gültig sein und dabei keine falschen
Werte liefern. Möglichst automatisch, auf jeder Entfernung, die das Zoom-Fenster verfolgt.

**Gesagt (Projektinhaber):** Hat der Sensor den Puls gefunden, stimmt er (von Hand gezählt). Er verliert ihn bei kleinster
Bewegung und findet ihn sehr langsam wieder. Rhythmus und Schläge sollen besser genutzt werden.
**Annahme:** Wichtigste Größen sind Wiederfindezeit und Anteil gültiger Zeit, ohne mehr Fehlwerte.
**Nicht Ziel:** Genauigkeit steigern (ohne Referenz nicht messbar), Reichweite physikalisch erweitern, TinyML (erst mit
Gurt-Daten, siehe unten).

## Ausgangslage (gemessen, ohne Referenz)

30-min-Aufnahme `phone/2026-11-01_164043_usb_iq3raw_bildschirmaus`, mit Firmware-Stand `2026-11-02-vital1`
nachgerechnet (`vital_eval.replay_lines`):

- 1899 Reports, gültig 211 (11 %); 33 Ausfälle nach einem gültigen Wert, Summe 1711 s.
- Ausfälle mit > 30 % Bewegung im Fenster: 18, Median 40 s, zusammen 1627 s.
- Zustand „Suche“ (555 Reports): 508 davon ohne Atemspitze (`br_state` 1); 182 mit > 30 % Bewegung.
- Minute 6–22 meist „keine Person“ (ob die Testperson da war, ist unbekannt; diese Abschnitte zählen nicht als Ausfall).

Ursachen im Code (`computeReport` u. a.):

1. **Alles oder nichts über 15 s:** `rep.valid` verlangt `motion_pct ≤ kMaxMotionPct` (30 %). 5 bewegte Sekunden im
   15-s-Fenster machen den Report ungültig, bis sie herausgefallen sind (≥ 13 s nach Bewegungsende), danach Haltezeit.
2. **Verfolgung läuft durch Bewegung weiter:** `hr_prev_`/`hr_miss_` werden auch auf bewegten Fenstern fortgeschrieben
   und können auf Artefakte springen.
3. **Kein Gedächtnis:** Nach der Bewegung wird im ganzen Band 40–180 /min neu gesucht.
4. **Atmung als Pflicht:** Lebenszeichen = Atemspitze (`breath_ok`) oder angehaltener Atem mit Pulsspitze. Leichte
   Unruhe verschmiert die Atemspitze, der Puls wird verworfen.

Der Prüfstand bildet das kaum ab: `beweg` (10 s Zufallsbewegung ±3 mm, ohne Abstands- oder Amplitudenwechsel) hat
101/120 gültige Reports.

## Entwurf

Alles hinter Übersetzungsschaltern, Standard aus (siehe „Einführung“): `A121_DSP_REACQ` für 1.–5. und 7.,
`A121_DSP_REACQ_PERIOD` (Variante in 1.) und `A121_DSP_PHASE_SLOPE` (6.). Ohne Schalter bleibt die Ausgabe bitgleich zum
heutigen Stand.

### 1. Bewegungskarte vor den Spektren

- Vor der Schleife über die Zoom-Punkte: für die 3 stärksten Punkte (dieselbe Stärke wie `selectStrongest`) das Weg-
  signal wie heute bilden (Kreisfit, Trend, Bandpass 0,6–5 Hz), je 1-s-Block den Effektivwert, Bezug ruhiges Viertel
  (wie `gateMotion`). Ein Block gilt als bewegt, wenn er bei mindestens 2 der 3 Punkte über `kMotionK` × Bezug liegt.
- Ergebnis: Bitmaske der bewegten Blöcke im Fenster und `clean_tail_s` = ruhige Sekunden seit dem letzten bewegten Block.
- Funktion `motionMap(start, N, use) → {mask, clean_tail_blocks}`; eigenständig testbar über `host/dsp_replay`.
- **Variante Periodizität** (`A121_DSP_REACQ_PERIOD`, nur zusammen mit `A121_DSP_REACQ`; nach Vital-Radio, siehe
  Quellen): Ein zu starker Block zählt nur als bewegt, wenn das Signal um ihn herum (3-s-Ausschnitt, Herzband) nicht
  periodisch ist: Spektrum des Ausschnitts, stärkste Spitze < `kPeriodK` (5) × Mittel der übrigen Bins. Ziel: kleine
  unregelmäßige Bewegungen (Tippen) nicht als Bewegung werten, wenn der Puls sichtbar bleibt. Risiko: rhythmische
  Bewegung (Fußwippen) ist auch periodisch; deshalb nur übernehmen, wenn `zappel`, `lage` und `beweg_hr` ihre Grenzen
  halten und `tippen` besser wird.

### 2. Fensterwahl

- Kein bewegter Block im Fenster: wie heute (N bis 15 s).
- Bewegung im Fenster und `clean_tail_s ≥ kReacqMinS` (6 s): Fenster = ruhiges Endstück (N' = clean_tail, höchstens
  15 s). Rechnung wie im Schnellstart (V2.4): Schwellen für Atemspitze mit N' skaliert, Wert gilt nur mit Bestätigung
  durch Autokorrelation oder Einzelschläge (±5 /min) und nicht auf Atem-Oberwelle m ≤ 3.
- Bewegung und `clean_tail_s < 6 s`: Puls ungültig, Zustand BEWEGUNG wie heute; Atmung, Atemzustand und Raum werden
  wie heute auf dem vollen Fenster gerechnet.
- Die Regel `motion_pct ≤ 30 %` entfällt unter dem Schalter; `motion_pct` wird weiter berechnet und ausgegeben.

### 3. Verfolgung während Bewegung einfrieren

- Ist der Report wegen Bewegung ungültig, werden `hr_prev_`, `hr_miss_`, `alt_count_`, Viterbi-Zustand und `hr_hist_`
  nicht fortgeschrieben.
- Im ersten Report nach der Bewegung (kurzes Fenster) wird `hr_prev_` nur aus einem bestätigten Wert gesetzt.

### 4. Gedächtnis

- Bei jedem gültigen Report mit Konfidenz ≥ `kMemMinConf` (50 %) und Fenster ≥ 8 s: `mem_hr_`, `mem_ibi_ms_`,
  `mem_tot_` (Frame-Zähler) speichern.
- Aktiv, solange `tot_frames − mem_tot_ < kMemS` (60 s) **und** sauberes Echo im Zoom (`echo_ok`) **und** Raum-Ziel im
  Zoom-Fenster (`target_in_zoom`). Fällt eine Bedingung weg (Aufstehen, Rezoom auf ein anderes Ziel), wird das Gedächtnis
  gelöscht. Rezoom auf dasselbe Ziel (V2.3, `kKeepTrackS`) behält es.
- Wirkung bei aktivem Gedächtnis im kurzen Fenster: Kandidaten innerhalb ±`kMemBandBpm` (15 /min) um `mem_hr_` werden
  vor die übrigen sortiert, wenn ihr Wert ≥ 0,5 × bester Kandidat ist. „Wiedergefunden“ = gewählter Kandidat im Band
  **und** Autokorrelation oder Einzelschläge innerhalb ±5 /min. Sonst normale Suche ohne Gedächtnis-Bevorzugung.
- So hängt das Gedächtnis nicht an einem alten Wert: Hat sich der Puls um mehr als 15 /min geändert, greift die freie
  Suche mit ihren bisherigen Regeln.

### 5. Lebenszeichen

`rep.valid` (unter dem Schalter) = Fensterbedingung aus 2. **und** (`breath_ok` **oder** `held_pulse` **oder**
`mem_ok`), mit `mem_ok` = Gedächtnis aktiv **und** Wert wiedergefunden (Bestätigung aus 4.).
Ohne vorherigen gültigen Wert gibt es kein Gedächtnis; die Fälle „leer“, „niemand“ und „weg“ ändern sich dadurch nicht.

### 6. Feinschätzung über die Phasensteigung

Variante `A121_DSP_PHASE_SLOPE` (unabhängig von `A121_DSP_REACQ` schaltbar; nach Vital-Radio): Statt der parabolischen
Verfeinerung der Spektrumsspitze wird die Frequenz des gewählten Kandidaten so bestimmt: im Spektrum des Weg-Signals
(klarster Punkt bzw. kombiniertes Signal) nur die Spitze und ihre beiden Nachbar-Bins behalten, inverse FFT zum
komplexen Zeitsignal, Phase abwickeln, Steigung per Geradenfit (kleinste Quadrate) → Frequenz. Nutzen vor allem im
kurzen Fenster (6 s: Hauptkeule ≈ 20 /min breit). Prüfung: Prüfstand mit und ohne Variante, besonders `step`
(Reaktion), `kurz`, `zappel` und Fensterlänge 10 s (V2.5); übernehmen nur, wenn MAE und Grenzwerte nicht schlechter
werden. Rechenzeit: eine komplexe inverse FFT (1024 Punkte) je Report.

### 7. Ausgabe

`@DSP` bekommt hinten zwei Felder (nach `n_eff`, das ohne `A121_DSP_COMBINE` leer bleibt):

- `win_s`: Länge des genutzten Pulsfensters in s (ganze Zahl, 0 = kein Pulsfenster).
- `reacq`: 0 = normal, 1 = kurzes Fenster nach Bewegung, 2 = wiedergefunden über Gedächtnis.

`vital_eval.DSP_FIELDS`, `host/dsp_replay`, App-Parser (`EspMsg.kt`) um die Felder ergänzen; fehlende Felder = NaN/leer
wie bisher. `@VITAL` und `@STATE` unverändert.

### Konstanten (Startwerte, am Prüfstand und an Tag A zu prüfen)

| Name | Wert | Bedeutung |
|---|---|---|
| `kReacqMinS` | 6 s | kürzestes ruhiges Endstück für einen Puls |
| `kMemS` | 60 s | Lebensdauer des Gedächtnisses |
| `kMemBandBpm` | 15 /min | Suchband um den gemerkten Wert |
| `kMemMinConf` | 50 % | Konfidenz, ab der ein Wert ins Gedächtnis geht |
| Bewegungskarte | 2 von 3 Punkten | Block gilt als bewegt |
| `kPeriodK` | 5 | Periodizität: Spitze / Mittel der übrigen Bins (Variante `A121_DSP_REACQ_PERIOD`) |

## Prüfung

Drei getrennte Prüfungen wie im Audit (Leitlinien); Build ist keine Laufzeitprüfung.

### Prüfstand (`host/vital_sim.py`)

Neue Szenarien (je 120–180 s, Puls und Atmung bekannt):

| Name | Inhalt |
|---|---|
| `kurz` | 3 s Bewegung bei 60 s |
| `zappel` | drei 2-s-Bewegungen bei 40, 60, 80 s |
| `lang` | 20 s Bewegung bei 50 s |
| `lage` | Haltungswechsel bei 60 s: Abstand springt um 8 mm, Echo-Amplitude −30 %, 2 s Übergang |
| `beweg_hr` | 10 s Bewegung bei 60 s, Puls springt dabei von 70 auf 85 |
| `beweg_weg` | 5 s Bewegung bei 50 s, Person geht bei 60 s |
| `tippen` | 60–120 s unregelmäßige kleine Bewegung (Arme/Schultern, ±0,3 mm am Brust-Echo, Stöße 0,2–0,5 s), kein Lagewechsel |

Neue Kennzahlen in `bench`: `wiederfind_s` (Ende jeder Bewegung bis zum ersten gültigen Wert innerhalb ±5 /min der
Wahrheit, Median über die Bewegungen), `falsch_nach_beweg_n` (gültige Reports > 10 /min daneben in den 30 s nach
Bewegungsende).

Grenzwerte (`--check`): `wiederfind_s ≤ 8` in `kurz`, `zappel`, `lage`; `≤ 10` in `lang`; `falsch_nach_beweg_n = 0`
überall; in `beweg_hr` erster gültiger Wert nach der Bewegung innerhalb ±5 /min von 85 (nicht 70); in `beweg_weg` kein
gültiger Report ab 76 s (Gehen bei 60 s + 16 s, wie `weg`: bis dahin liegen noch Daten der Person im 15-s-Fenster); in `tippen` mindestens 80 % gültig im Abschnitt 60–120 s mit MAE ≤ 2 /min. Alle bisherigen
Grenzwerte gelten weiter, mit und ohne Schalter. Varianten (`A121_DSP_REACQ_PERIOD`, `A121_DSP_PHASE_SLOPE`) laufen als
eigene Spalten im Bench-Bericht (`--define`).

### Nachrechnung echter Aufnahmen (ohne Referenz)

Neues Werkzeug `host/reacq_eval.py <ordner>… [--replay <header-ordner>]`: Ausfälle nach gültigem Wert (Anzahl, Median,
Summe), getrennt nach Ursache (Bewegung / keine Atemspitze / Rest), Anteil gültig ohne Abschnitte „keine Person“,
Sprünge > 15 /min zwischen aufeinanderfolgenden gültigen Reports. Vergleich Header ohne und mit `A121_DSP_REACQ` auf
T1 und den Aufnahmen vom 2026-11-02.
Ziel an T1: Median der Bewegungs-Ausfälle ≤ 15 s (heute 40 s), Sprünge nicht mehr als heute.

### Live und Referenz

- Gerät: Firmware mit Schalter an bauen, `iq3 0 raw` über die App, `replay-cpp` gleich, `compute_us` ≤ 400 ms je Report.
- Polar H9 (ab ca. 2026-11-05): Tag A nach Messplan M5, dazu Sitzung R5 (Bewegung) ausgewertet mit `vital_eval.py`
  ohne/mit Schalter (`--replay`): Puls-MAE und Anteil ±5 in den 30 s nach Bewegung dürfen nicht schlechter sein als im
  Rest der Sitzung um mehr als 2 /min bzw. 10 Prozentpunkte.
- Bis dahin: Handzählung mit Markern `zaehl_start`/`zaehl_ende` als Stichprobe, ausdrücklich keine Genauigkeitsaussage.

### Weitere Tests

- `host/tests`: Unit-Tests für `motionMap` (Masken, `clean_tail`), Gedächtnis-Ablauf (setzen, verfallen bei Echo-Verlust
  und Ziel außerhalb, Rezoom), neue `@DSP`-Felder in `vital_eval` und `dsp_replay`; Nachrechnung bleibt PC = Handy
  bytegleich.
- `android`: `LineParserTest` für die neuen Felder.

## Einführung

1. Alle Schalter aus: Implementierung, Tests, Prüfstand mit und ohne Schalter, Nachrechnung. Varianten
   (`A121_DSP_REACQ_PERIOD`, `A121_DSP_PHASE_SLOPE`) einzeln gegen `A121_DSP_REACQ` allein vergleichen; nur übernehmen,
   was die Grenzwerte hält und besser ist.
2. Prüfstand und T1-Ziel erfüllt: Build mit Schalter an flashen und live prüfen (Projektinhaber bedient, Handzählung).
3. Polar H9 Tag A bestätigt: Schalter Standard an, Firmware-Kennung erhöhen; Tag B zur Abnahme.

## Risiken

- **Kurzes Fenster, grobe Auflösung:** 6 s geben 0,17 Hz (10 /min) Bin-Abstand; Fehlwerte möglich. Gegenmittel:
  Bestätigungspflicht, Gedächtnis-Band, Prüfstand `beweg_hr`.
- **Gedächtnis hält Fehlwert:** Ein falscher gespeicherter Wert würde bevorzugt. Gegenmittel: nur Werte mit Konfidenz
  ≥ 50 % und Fenster ≥ 8 s speichern, Bestätigung nötig, 60 s Lebensdauer.
- **Bewegungskarte zu empfindlich/unempfindlich:** am Prüfstand und an T1 abstimmen (Tag A ist Entwicklung, Tag B nur
  Abnahme).
- **Rechenzeit:** Die Bewegungskarte rechnet 3 Punkte vorab (heute laufen alle 24, bei 48 Zoom-Punkten 16); danach ein Durchlauf über das gewählte
  Fenster. Erwartet + 15–25 %; Grenze ≤ 400 ms je Report (Kern 0, 1 Report/s), heute 91–152 ms.

## Quellen

- Adib, Mao, Kabelac, Katabi, Miller 2015, Smart Homes that Monitor Breathing and Heart Rate (Vital-Radio), *ACM CHI*,
  https://doi.org/10.1145/2702123.2702200. FMCW 5,46–7,25 GHz, 8-cm-Entfernungsstufen, Periodizitätstest je 30-s-Fenster
  (Spitze ≥ 5 × Mittel der übrigen Bins, sonst Fenster verworfen), Puls im 10-s-Fenster mit Feinschätzung über die
  Phasensteigung. Median-Genauigkeit 98,5 % (Puls, 1 m) gegen Pulsoximeter, nur ruhige Abschnitte, je 2 min; bewegte
  Zeit wird verworfen, nicht wiedergefunden. Reichweite 8 m und Messung durch Wände gelten für deren Hardware, nicht für
  den A121 bei 60 GHz.

## Später (nicht Teil dieses Entwurfs)

TinyML auf dem ESP32-S3 (z. B. Blockklassifikation „sauber/bewegt“ oder Schlagdetektion), sobald mehrere Stunden
Aufnahmen mit `@RAW` und Polar-H9-RR vorliegen; Bewertung dann gegen diesen Entwurf.
