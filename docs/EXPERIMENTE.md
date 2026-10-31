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
| M2 | Atmung sitzend/liegend, 0,3–1,5 m | breathing | Atemzüge/min vs. Zählen | — |
| M3 | Puls sitzend, 40/60/80 cm, mit/ohne Linse | phase_tracking, sparse_iq (eigene Auswertung) | BPM vs. Smartwatch | Aufnahme A (2026-10-30, ~250 mm, hyperbolisch, 60 s, `host/a121_vitalaufnahme.py`): Atmung 20,5/min, Kandidaten 83/62/min = 4.×/3. Atmungs-Oberwelle → nicht belastbar; **keine Referenz** vorhanden. Aufnahme B (Live-Mitschnitt `2026-10-30_201219_live.npz`, ~20 s atmen / ~20 s anhalten / ~20 s atmen, Ziel 0,35 m): im Anhalte-Abschnitt (26,5–45,7 s) ein dominanter Peak bei 76/min (übrige ≤ 2 %), Autokorrelation 75/min r = 0,85, Spitzenzählung 74/min ± 20 % Intervallstreuung; beim Atmen mehrdeutig (82/64/69). Regelmäßiger Rhythmus ~75/min nachweisbar, **ob es der Puls ist: ohne Referenz offen**. Auswertung: `host/a121_anhalteanalyse.py`. Offen: Referenzmessung. Live-Ansicht: `host/a121_livemonitor.py`. |
| M4 | Gesten / Wischen / berührungsloser Knopf | hand_motion, touchless_button | Erkennungsrate | — |

## Materialien und Umgebung

| # | Experiment | Modul | Messgröße | Ergebnis |
|---|---|---|---|---|
| U1 | Dämpfung: Pappe, Sperrholz, Gipskarton, Kunststoff, Glas, Stoff | sparse_iq | Amplitude vor/hinter Material (dB) | — |
| U2 | Metallgegenstand / Person hinter Gipskarton bzw. Holztür | distance, presence | erkennbar ja/nein, Abstand | — |
| U3 | Vibrationsfrequenz: Lautsprecher, Ventilator, Waschmaschine | vibration | Hz vs. Sollwert | — |
| U4 | Geschwindigkeit: Ventilatorblatt, rollender Ball, Pendel | speed | m/s | — |
| U5 | Wasser-Fließgeschwindigkeit (Wasserhahn, Gartenschlauch) | surface_velocity | m/s plausibel? | — |
| U6 | Füllstand: Eimer/Glas Wasser, Mülleimer | tank_level, waste_level | mm vs. Lineal | — |

## Nicht möglich mit einem Sensor

- `bilateration` braucht zwei Sensoren.
- Winkel/Richtung und Umrisse: siehe `KONZEPT.md`.
