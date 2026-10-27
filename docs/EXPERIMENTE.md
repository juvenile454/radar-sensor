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
| G2 | Entfernungsgenauigkeit vs. Maßband (0,1–5 m, Metallplatte) | distance | Abweichung mm | — |
| G3 | Maximale Reichweite Mensch / Metall / Wand | distance, sparse_iq | m | — |
| G4 | Zwei Ziele trennen (Abstand verringern) | distance | min. Abstand cm | — |
| G5 | Kleinste messbare Bewegung (Mikrometerschraube/Lautsprecher) | phase_tracking | µm | — |
| G6 | Strahlbreite ohne Linse / Fresnel / hyperbolisch | sparse_iq | Winkel, Reichweite | — |

## Mensch und Vitalwerte

| # | Experiment | Modul | Messgröße | Ergebnis |
|---|---|---|---|---|
| M1 | Präsenz im Raum, Zonen, ruhig sitzend vs. bewegt | presence, smart_presence | Trefferquote, Reichweite | — |
| M2 | Atmung sitzend/liegend, 0,3–1,5 m | breathing | Atemzüge/min vs. Zählen | — |
| M3 | Puls sitzend, 40/60/80 cm, mit/ohne Linse | phase_tracking, sparse_iq (eigene Auswertung) | BPM vs. Smartwatch | — |
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
