# V6 Puls nach Bewegung wiederfinden — Umsetzungsplan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Puls nach Bewegung in Sekunden statt ~40 s wieder gültig, ohne Fehlwerte, alles hinter Übersetzungsschaltern.

**Architecture:** Änderungen in der einzigen DSP-Quelle `firmware/esp32s3/src/a121_esp_dsp.h`: kleine, frei stehende
Funktionen/Klassen (Bewegungskarte, Gedächtnis, Kandidaten-Bevorzugung, Periodizität, Phasensteigung), einzeln am PC
getestet, dann in `computeVitalReport` unter `#ifdef A121_DSP_REACQ` verdrahtet. Prüfstand (`host/vital_sim.py`) bekommt
Bewegungsmodelle, Szenarien und Kennzahlen; neues Werkzeug `host/reacq_eval.py` misst Ausfälle auf echten Aufnahmen.

**Tech Stack:** C++17 (Header, am PC mit g++ über `host/dsp_replay` und Stub-Headern), Python 3.12 + numpy (`.venv`),
unittest, Kotlin/JUnit (App-Parser), PlatformIO (`pio run`).

**Spec:** `docs/superpowers/specs/2026-11-02-pulse-reacquisition-design.md`

## Global Constraints

- Schalter Standard aus: `A121_DSP_REACQ`, `A121_DSP_REACQ_PERIOD` (nur zusammen mit `A121_DSP_REACQ`), `A121_DSP_PHASE_SLOPE`.
- Ohne Schalter bleibt jede Ausgabezeile bitgleich zum Stand `53c45e6` (Prüfung: `host/tests/test_dsp_replay.py`, Bench-Zahlen unverändert).
- Protokoll nur hinten erweitern; `@VITAL` behält 15 Felder, `@STATE` unverändert.
- Konstanten: `kReacqMinS = 6` s, `kMemS = 60` s, `kMemBandBpm = 15` /min, `kMemMinConf = 50` %, Bewegungskarte 2 von 3 Punkten, `kPeriodK = 5`.
- Rechenzeit je Report ≤ 400 ms (Kern 0); heute 91–152 ms am Gerät.
- Kommentare, Doku, Commit-Texte deutsch; Commits enden mit `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Keine Daten/Binärdateien ins Repo; Aufnahmen liegen unter `~/Data/radar-sensor/`.
- Kein Flashen, keine App-Installation ohne Freigabe des Projektinhabers; Port per `ls -l /dev/serial/by-id`, Handy-Seriennummer per `adb devices -l`.
- Build/Compile ist keine Laufzeitprüfung; im Bericht getrennt ausweisen. Keine Aussage zur Puls-Genauigkeit ohne Gurt (Polar H9 ab ca. 2026-11-05).

Testbefehle (aus Repo-Wurzel):
- Python: `.venv/bin/python -m unittest discover -s host/tests -p 'test_*.py'` (≈ 2 min; einzelne Datei: `.venv/bin/python -m unittest host/tests/<datei>.py`)
- Prüfstand: `.venv/bin/python host/vital_sim.py bench --check [--define A121_DSP_REACQ …]`
- App: `cd android && ./gradlew --offline :app:testDebugUnitTest`
- Firmware: `cd firmware/esp32s3 && pio run`

## Review Focus

1. **Person steht direkt nach einer Bewegung auf** → Gedächtnis darf keinen Puls weitertragen (Task 5 Test `test_cleared_when_target_leaves`, Task 2/6 Szenario `beweg_weg`).
2. **Rezoom auf ein anderes Ziel während des Gedächtnisses** → Gedächtnis gelöscht; Rezoom auf dasselbe Ziel behält es (Task 6 Step `test_rezoom_keeps_or_clears_memory`).
3. **Puls ändert sich während der Bewegung um mehr als 15 /min** → freie Suche statt Hängenbleiben (Task 5 `test_no_preference_outside_band`, Task 2/6 Szenario `beweg_hr`).
4. **Bewegung länger als 60 s** → Gedächtnis abgelaufen, Verhalten wie ohne Schalter (Task 5 `test_expires_after_60_s`).
5. **Aufnahmen älterer Firmware ohne die neuen Felder** → `vital_eval`, Prüfstand und App lesen NaN/`null` statt abzubrechen (Task 1 `test_old_line_without_reacq_fields`, `LineParserTest.dspWithoutReacqFields`).

---

### Task 1: `@DSP`-Felder `win_s` und `reacq`

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (`VitalReport` ≈ Z. 1250–1280, `formatDspLine` ≈ Z. 2559)
- Modify: `host/vital_eval.py` (`DSP_FIELDS`, Z. 39–41)
- Modify: `android/app/src/main/java/de/timo/radarsensor/protocol/EspMsg.kt` (`Dsp`), `.../protocol/LineParser.kt` (`"@DSP"`)
- Test: `host/tests/test_dsp_unit.py` (`DspLineTest`), `host/tests/test_vital_eval.py`, `android/app/src/test/java/de/timo/radarsensor/protocol/LineParserTest.kt`

**Interfaces:**
- Produces: `VitalReport::win_s` (`uint8_t`, s), `VitalReport::reacq` (`uint8_t`: 0 normal, 1 kurzes Fenster nach Bewegung, 2 wiedergefunden über Gedächtnis); Konstanten `kReacqNone = 0`, `kReacqShort = 1`, `kReacqMemory = 2`.
- Zeilenformat mit `A121_DSP_REACQ`: 13 heutige Felder, dann Feld 14 `n_eff` (leer ohne `A121_DSP_COMBINE`), Feld 15 `win_s`, Feld 16 `reacq`.
- `vital_eval.DSP_FIELDS` endet mit `"n_eff", "win_s", "reacq"`; Kotlin `Dsp.winS: Int?`, `Dsp.reacq: Int?` aus `p[15]`, `p[16]`.

- [ ] **Step 1: Failing tests**

`test_dsp_unit.py`, Klasse `DspLineTest`:
```python
def test_reacq_fields_at_end(self):
    out = run_cpp('VitalReport r{}; r.win_s = 7; r.reacq = 2; char b[256]; '
                  'formatDspLine(b, sizeof b, 1000, true, r, 5); puts(b);', ("-DA121_DSP_REACQ",))
    p = out.strip().split(",")
    self.assertEqual(len(p), 17)
    self.assertEqual(p[14], "")
    self.assertEqual((p[15], p[16]), ("7", "2"))

def test_line_unchanged_without_switch(self):
    out = run_cpp('VitalReport r{}; char b[256]; formatDspLine(b, sizeof b, 1000, true, r, 5); puts(b);')
    self.assertEqual(len(out.strip().split(",")), 14)
```
`test_vital_eval.py`: `test_reacq_fields_parsed` (Zeile `@DSP,1000,1,5,88.0,300000,0,0,0,0,0,0,0,60,,7,2` → `dd["win_s"] == [7.0]`, `dd["reacq"] == [2.0]`) und `test_old_line_without_reacq_fields` (Zeile mit 13 Feldern → beide NaN).
`LineParserTest.kt`: `dspWithReacqFields` (`winS == 7`, `reacq == 2`, `nEff == null`) und `dspWithoutReacqFields` (beide `null`).

- [ ] **Step 2: Laufen lassen, schlagen fehl** — `.venv/bin/python -m unittest host/tests/test_dsp_unit.py host/tests/test_vital_eval.py`; `cd android && ./gradlew --offline :app:testDebugUnitTest --tests '*LineParserTest*'`. Erwartet: FAIL (fehlende Felder).

- [ ] **Step 3: Implementieren** — Felder in `VitalReport`; `formatDspLine` bekommt unter `#ifdef A121_DSP_REACQ` eine eigene Zeile mit `%s` für `n_eff` (mit `A121_DSP_COMBINE` Zahl `%.1f`, sonst leer) und `,%u,%u` für `win_s`, `reacq`. `DSP_FIELDS` ergänzen; Kotlin `winS = p.getOrNull(15)?.toIntOrNull()`, `reacq = p.getOrNull(16)?.toIntOrNull()`.

- [ ] **Step 4: Tests grün** — dieselben Befehle; zusätzlich `.venv/bin/python -m unittest host/tests/test_dsp_replay.py` (Ausgabe ohne Schalter bitgleich).

- [ ] **Step 5: Commit** — `git commit -m "@DSP: Felder win_s und reacq hinter A121_DSP_REACQ (V6)"`

---

### Task 2: Prüfstand — Bewegungsmodelle, Szenarien, Kennzahlen

**Files:**
- Modify: `host/vital_sim.py` (`make_frames`, `SCENARIOS`, `LIMITS`, `evaluate`, Bericht in `run_bench`)
- Test: `host/tests/test_vital_sim.py`

**Interfaces:**
- `make_frames(..., motion=None | tuple | list[tuple], shift=None, jolts=None)`:
  `motion`: ein `(t0, t1, pp_mm)` oder Liste davon; `shift=(t0, dauer_s, dd_mm, amp_faktor)` verschiebt alle Brust-Streuzentren ab `t0` in `dauer_s` linear um `dd_mm` und skaliert ihre Amplitude auf `amp_faktor`; `jolts=(t0, t1, pp_mm)` erzeugt zufällige Stöße (Länge 0,2–0,5 s, Abstand 0,5–3 s, Halbsinus) am Brust-Echo.
- `reacq_metrics(t_s: np.ndarray, valid: np.ndarray, err: np.ndarray, episodes: list[tuple[float, float]]) -> dict` mit
  `wiederfind_s` (Median über Episoden: Zeit von Episodenende bis zum ersten gültigen Report mit `err ≤ 5`, NaN wenn keiner bis Szenarioende) und `falsch_nach_beweg_n` (gültige Reports mit `err > 10` in `(ende, ende + 30]`).
- `section_metrics(t_s, valid, err, a: float, b: float) -> dict` mit `abschnitt_gueltig_pct`, `abschnitt_mae` (Reports mit `a < t ≤ b`).
- `SCENARIOS` neu: `kurz` (motion (60, 63, 4.0)), `zappel` (motion [(40,42,4.0),(60,62,4.0),(80,82,4.0)]), `lang` (motion (50,70,6.0), T 150), `lage` (shift (60, 2, 8.0, 0.7)), `beweg_hr` (motion (60,70,6.0), hr 70 → 85 linear 60–70 s, T 150), `beweg_weg` (motion (50,55,4.0), leave 60), `tippen` (jolts (60,120,0.3), T 150).
- `LIMITS_REACQ: dict` (nur angewendet, wenn `"A121_DSP_REACQ"` in `DEFINES`): `wiederfind_s ≤ 8` (`kurz`, `zappel`, `lage`), `≤ 10` (`lang`); `falsch_nach_beweg_n ≤ 0` in allen neuen Szenarien; `beweg_weg`: `weg_gueltig_n ≤ 0`; `tippen`: `abschnitt_gueltig_pct ≥ 80`, `abschnitt_mae ≤ 2`.

- [ ] **Step 1: Failing tests** in `test_vital_sim.py`:
```python
class ReacqMetricsTest(unittest.TestCase):
    def test_first_correct_after_episode(self):
        t = np.arange(50.0, 100.0); v = t >= 66; e = np.where(t >= 68, 1.0, 20.0)
        m = vital_sim.reacq_metrics(t, v, e, [(60.0, 63.0)])
        self.assertEqual(m["wiederfind_s"], 5.0)          # 68 − 63
        self.assertEqual(m["falsch_nach_beweg_n"], 2)     # 66, 67 gültig und > 10 daneben
    def test_never_found_is_nan(self):
        t = np.arange(50.0, 100.0)
        m = vital_sim.reacq_metrics(t, np.zeros_like(t, bool), np.ones_like(t), [(60.0, 63.0)])
        self.assertTrue(math.isnan(m["wiederfind_s"]))
class SectionMetricsTest(unittest.TestCase):
    def test_share_and_mae(self):
        t = np.arange(0.0, 10.0); v = t % 2 == 0; e = np.full(10, 1.5)
        m = vital_sim.section_metrics(t, v, e, 0.0, 10.0)
        self.assertAlmostEqual(m["abschnitt_gueltig_pct"], 44.4, places=1)  # t = 2,4,6,8 von 1…9
        self.assertAlmostEqual(m["abschnitt_mae"], 1.5)
class MotionModelTest(unittest.TestCase):
    def test_shift_moves_chest_echo(self):   # Zoom-Phase am stärksten Punkt springt um ≈ 4π·8 mm/λ
    def test_jolts_only_inside_window(self): # Wegsignal außerhalb (60,120) ohne Stöße
    def test_motion_list(self):              # drei Episoden ergeben Bewegung nur in den drei Intervallen
```
(die drei `MotionModelTest`-Fälle über `make_frames` in ein Temp-Verzeichnis und `np.fromfile`; Toleranzen: Phasensprung 10–20 rad nach Abwicklung, Effektivwert außerhalb < 10 % von innerhalb.)

- [ ] **Step 2: Laufen lassen** — `.venv/bin/python -m unittest host/tests/test_vital_sim.py` → FAIL (`reacq_metrics` fehlt).

- [ ] **Step 3: Implementieren** — Funktionen und Parameter wie oben; `evaluate` ruft `reacq_metrics` mit den Episoden aus `motion`/`shift`/`jolts` (`shift` als Episode `(t0, t0 + dauer)`) und `section_metrics` für `tippen` (60, 120); `run_bench` hängt `LIMITS_REACQ` an, wenn der Schalter gesetzt ist, und zeigt `wiederfind_s`, `falsch_nach_beweg_n` in der Zusatzspalte.

- [ ] **Step 4: Basislinie messen und als Grenze ohne Schalter festhalten** — `.venv/bin/python host/vital_sim.py bench --scen kurz zappel lang lage beweg_hr beweg_weg tippen`. Für die neuen Szenarien in `LIMITS` nur die bisherigen Pflichtwerte (`zustand_beweg_fehl`, Atemzüge) und `falsch_nach_beweg_n` ≤ gemessener Wert eintragen (Kommentar „Basislinie V6, ohne Schalter“), die gemessenen `wiederfind_s` im Kommentar notieren. `erst_s`/`erst_ok_s`/`frueh_falsch_n` gelten wie für die übrigen Szenarien.

- [ ] **Step 5: Tests und Bench grün** — `.venv/bin/python -m unittest host/tests/test_vital_sim.py`; `.venv/bin/python host/vital_sim.py bench --check` → `ERGEBNIS: OK`.

- [ ] **Step 6: Commit** — `git commit -m "Prüfstand V6: Bewegungsmodelle, Szenarien kurz/zappel/lang/lage/beweg_hr/beweg_weg/tippen, Wiederfind-Kennzahlen"`

---

### Task 3: `host/reacq_eval.py` — Ausfälle auf echten Aufnahmen

**Files:**
- Create: `host/reacq_eval.py`
- Test: `host/tests/test_reacq_eval.py`

**Interfaces:**
- Consumes: `vital_eval.load_recording(folder, lines)`, `vital_eval.replay_lines(folder, header_dir)`, `vital_eval.count_jumps`.
- Produces:
  - `@dataclass Dropout: start_s: float; dur_s: float; cause: str` (`"bewegung"` wenn im Ausfall ein Report `motion_pct > 30` oder Zustand 4 hat, sonst `"keine_atemspitze"` wenn `br_state == 1` überwiegt, sonst `"rest"`).
  - `dropouts(t_s, ok, motion_pct, br_state, state) -> list[Dropout]`: Läufe von `ok == 0` nach einem gültigen Report; Läufe, in denen > 50 % der Reports Zustand 0 („keine Person“) haben, zählen nicht.
  - `summary(t_s, ok, hr, state, drops) -> dict`: `anteil_gueltig_pct` (ohne Zustand 0), `n`, `median_s`, `summe_s` je Ursache, `spruenge`.
  - CLI: `reacq_eval.py <ordner>… [--replay <header-ordner>] [--define NAME]…` — mit `--define` wird eine Header-Kopie mit vorangestellten `#define`-Zeilen in ein Temp-Verzeichnis geschrieben und als `--replay` benutzt. Ausgabe je Ordner eine Zeile plus Tabelle der Ursachen.

- [ ] **Step 1: Failing tests** — `test_dropout_after_valid_only` (Folge ok `0,0,1,1,0,0,0,1` → ein Ausfall, 3 s), `test_cause_motion`, `test_cause_no_breath_peak`, `test_no_person_not_counted`, `test_summary_share_without_no_person`, `test_define_header_copy` (Kopie beginnt mit `#define A121_DSP_REACQ`).

- [ ] **Step 2: Laufen lassen** — `.venv/bin/python -m unittest host/tests/test_reacq_eval.py` → FAIL (Modul fehlt).

- [ ] **Step 3: Implementieren** wie in Interfaces.

- [ ] **Step 4: Tests grün; Basislinie an T1 und 2026-11-02 messen**
```bash
.venv/bin/python host/reacq_eval.py ~/Data/radar-sensor/phone/2026-11-01_164043_usb_iq3raw_bildschirmaus ~/Data/radar-sensor/phone/2026-11-02_171254_livetest_vital1 ~/Data/radar-sensor/app-fixtures/2026-11-02_vital1_iq3raw_60s --replay firmware/esp32s3/src
```
Erwartet an T1 (Stand Spec): Bewegungs-Ausfälle 18, Median ≈ 40 s, Summe ≈ 1627 s; Zahlen in `docs/EXPERIMENTS.md` unter neuem Abschnitt „V6 Wiederfinden (Basislinie)“ eintragen.

- [ ] **Step 5: Commit** — `git commit -m "reacq_eval.py: Ausfälle nach Bewegung auf Aufnahmen messen (V6), Basislinie T1"`

---

### Task 4: Bewegungskarte

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (neue freie Funktion nahe `gateMotion`, Member in `UltimateEspDsp`)
- Test: `host/tests/test_dsp_unit.py` (neue Klasse `MotionMapTest`)

**Interfaces:**
- Produces:
  - `struct MotionMap { uint32_t mask; uint8_t n_blocks; uint8_t clean_tail_blocks; };` (Bit k = Block k bewegt, Block 0 = ältester)
  - `inline MotionMap motionMapFromRms(const float *rms, uint8_t n_pts, uint8_t nb, float k = kMotionK, uint8_t votes = 2)` — `rms` zeilenweise `n_pts × nb`; je Punkt Bezug = unteres Viertel seiner Blöcke (wie `gateMotion`); Block bewegt, wenn bei ≥ `votes` Punkten `rms > k × Bezug`; `clean_tail_blocks` = Blöcke nach dem letzten bewegten (alle, wenn keiner).
  - Member `MotionMap motionMap(uint16_t start, uint16_t N, const bool *use)` — 3 stärkste Punkte aus `use`/Stärke wie `selectStrongest`, Wegsignal wie in der Hauptschleife (`binDisplacement`, `removeLinearTrend`, `sosFiltFilt`), Block-RMS je 1-s-Block (`kGateBlk`), dann `motionMapFromRms`. Nur unter `#ifdef A121_DSP_REACQ` übersetzt.
  - Konstanten `kReacqMinS = 6U`, `kMotionVotes = 2U`.

- [ ] **Step 1: Failing tests** (über `run_cpp(..., ("-DA121_DSP_REACQ",))`):
  `test_quiet_has_full_tail` (3 Punkte × 15 Blöcke gleich → `mask 0`, `clean_tail 15`), `test_motion_needs_two_votes` (Block 8 nur bei Punkt 0 × 10 → nicht bewegt; bei Punkt 0 und 1 → bewegt, `clean_tail 6`), `test_tail_after_last_motion` (Blöcke 3 und 11 bewegt → `clean_tail 3`).

- [ ] **Step 2: Laufen lassen** → FAIL (`motionMapFromRms` unbekannt).

- [ ] **Step 3: Implementieren**.

- [ ] **Step 4: Tests grün; ohne Schalter bitgleich** — `.venv/bin/python -m unittest host/tests/test_dsp_unit.py host/tests/test_dsp_replay.py`.

- [ ] **Step 5: Commit** — `git commit -m "DSP V6: Bewegungskarte aus 3 Punkten (motionMapFromRms)"`

---

### Task 5: Gedächtnis und Kandidaten-Bevorzugung

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (neue Klasse nahe `BreathStateTracker`, freie Funktion nahe Kandidatenwahl)
- Test: `host/tests/test_dsp_unit.py` (`ReacqMemoryTest`, `PreferMemoryTest`)

**Interfaces:**
- Produces:
  - `class ReacqMemory { public: void clear(); void store(float hr_bpm, float ibi_ms, uint32_t tot_frames); bool active(uint32_t tot_frames, bool echo_ok, bool target_in_zoom); float hr() const; float ibi_ms() const; };` — `active` löscht das Gedächtnis selbst, wenn `tot_frames − gespeichert ≥ kMemS × kFsHz` oder `!echo_ok` oder `!target_in_zoom`.
  - `inline int8_t preferMemoryCandidate(const float *cand_bpm, const float *cand_score, uint8_t n, float mem_hr, float band = kMemBandBpm, float min_rel = 0.5f)` — Index des stärksten Kandidaten mit `|bpm − mem_hr| ≤ band` und `score ≥ min_rel × score[0]`, sonst −1.
  - Konstanten `kMemS = 60.0f`, `kMemBandBpm = 15.0f`, `kMemMinConf = 50.0f`.

- [ ] **Step 1: Failing tests**:
  `test_store_and_active`, `test_expires_after_60_s` (`active(t0 + 60*60)` → false, danach `hr()` 0), `test_cleared_when_echo_lost`, `test_cleared_when_target_leaves`, `test_prefers_near_candidate` (Kandidaten 140 (1,0), 74 (0,6), 50 (0,3); Gedächtnis 72 → 1), `test_no_preference_outside_band` (Gedächtnis 72, Kandidaten 90 (1,0), 120 (0,8) → −1), `test_weak_near_candidate_ignored` (74 mit 0,4 → −1).

- [ ] **Step 2: Laufen lassen** → FAIL.
- [ ] **Step 3: Implementieren** (unter `#ifdef A121_DSP_REACQ`).
- [ ] **Step 4: Tests grün** — `.venv/bin/python -m unittest host/tests/test_dsp_unit.py`.
- [ ] **Step 5: Commit** — `git commit -m "DSP V6: Gedächtnis (ReacqMemory) und Kandidaten-Bevorzugung"`

---

### Task 6: Verdrahtung in `computeVitalReport`

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (`computeVitalReport` ≈ Z. 1611–2095, `resetHistory`/`resetTracking` ≈ Z. 1342–1410, Kopfkommentar Z. 1–52)
- Test: `host/tests/test_vital_sim.py` (neue Klasse `ReacqBenchTest`), `host/tests/test_dsp_unit.py`

**Interfaces:**
- Consumes: Task 1 (`win_s`, `reacq`), Task 4 (`motionMap`, `kReacqMinS`), Task 5 (`ReacqMemory mem_`, `preferMemoryCandidate`).
- Verhalten unter `#ifdef A121_DSP_REACQ` (Spec 2.–5.):
  1. Nach dem Ermitteln von `use[]`: `MotionMap mm = motionMap(start, N, use)`.
  2. Kein bewegter Block → Fenster wie heute, `win_s = N/60`, `reacq = 0`.
  3. Bewegt und `clean_tail_blocks ≥ kReacqMinS` → Pulsteil (Spektren, Kandidaten, AKF, Einzelschläge) auf dem Endstück `start' = start + N − tail×60`, `N' = tail×60`; Atmung/Atemzustand/Raum weiter auf `N`. Behandlung wie vorläufig (`prov`): gültig nur mit AKF- oder IBI-Bestätigung ±5 /min und nicht auf Atem-Oberwelle m ≤ 3. `reacq = 1`, `win_s = tail`.
  4. Bewegt und Endstück < 6 s → Puls ungültig, Verfolgungszustand (`hr_prev_`, `hr_miss_`, `alt_count_`, `vit_`, `hr_hist_`) unverändert, `win_s = 0`.
  5. Gedächtnis: im Fall 3 mit `mem_.active(...)` zuerst `preferMemoryCandidate`; ist der gewählte Kandidat im Band und bestätigt → `reacq = 2`, `hr_prev_` = bestätigter Wert. Nach jedem gültigen Report mit `confidence_pct ≥ kMemMinConf` und `N ≥ kHrStableWin`: `mem_.store(hr, ibi, tot_frames)`.
  6. `rep.valid` = Fensterbedingung **und** (`breath_ok` **oder** `held_pulse` **oder** `reacq == 2`); die 30-%-Regel entfällt.
  7. `resetTracking()` ruft `mem_.clear()`; `resetHistory()` mit beibehaltener Verfolgung (V2.3) behält das Gedächtnis.

- [ ] **Step 1: Failing tests** — `ReacqBenchTest.test_bench_with_reacq_within_limits` (wie `test_variant_within_limits`: `run_bench` mit `DEFINES=["A121_DSP_REACQ"]`, `check=True` → 0) und `test_rezoom_keeps_or_clears_memory` (`run_cpp` mit `UltimateEspDsp`: gültigen Zustand herstellen über synthetische Frames aus `vital_sim.make_frames` Szenario `base` bis 30 s, dann `setZoomStartPoint` auf ±0 Punkte → `mem_` aktiv; auf +40 Punkte → nach nächstem Report gelöscht; Zugriff über eine Test-Abfrage `bool reacqMemoryActive() const`, nur mit `A121_DSP_REACQ`).

- [ ] **Step 2: Laufen lassen** → FAIL (Grenzen `LIMITS_REACQ` verletzt / Abfrage fehlt).

- [ ] **Step 3: Implementieren** wie oben; Kopfkommentar um Punkt „V6 (A121_DSP_REACQ)“ ergänzen.

- [ ] **Step 4: Prüfungen**
  - `.venv/bin/python host/vital_sim.py bench --check` → OK (ohne Schalter unverändert)
  - `.venv/bin/python host/vital_sim.py bench --check --define A121_DSP_REACQ` → OK; `wiederfind_s` je Szenario notieren
  - `.venv/bin/python -m unittest discover -s host/tests -p 'test_*.py'` → OK
  - `.venv/bin/python host/reacq_eval.py <T1> --replay firmware/esp32s3/src --define A121_DSP_REACQ` → Bewegungs-Ausfälle Median ≤ 15 s, Sprünge nicht mehr als Basislinie (Task 3)
  - Werden Grenzen nur mit anderen Konstanten erreicht: Konstante ändern, Wert und Grund im Kommentar und in der Spec-Tabelle nachtragen; Tag-B-Regel beachten (Abstimmen nur an Prüfstand/T1).

- [ ] **Step 5: Commit** — `git commit -m "DSP V6: Wiederfinden nach Bewegung hinter A121_DSP_REACQ"`

---

### Task 7: Variante Periodizität (`A121_DSP_REACQ_PERIOD`)

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (freie Funktion; Aufruf in `motionMap`)
- Test: `host/tests/test_dsp_unit.py` (`SegmentPeriodicTest`), `host/tests/test_vital_sim.py`

**Interfaces:**
- Produces: `inline bool segmentPeriodic(const float *x, uint16_t n, RealFft &fft, float *spec, float k = kPeriodK)` — `x`: 3-s-Ausschnitt des bandgefilterten Wegsignals, auf 15 Hz dezimiert, Hann; Spektrum `fft.power`; Herzband-Bins 0,674–3,003 Hz; wahr, wenn stärkste Spitze ≥ `k` × Mittel der übrigen Bins im Band. Konstante `kPeriodK = 5.0f`.
- In `motionMap`: ein nach Stärke bewegter Block gilt bei gesetztem Schalter nur als bewegt, wenn `segmentPeriodic` für den 3-s-Ausschnitt um ihn (am stärksten Punkt) falsch ist.

- [ ] **Step 1: Failing tests** — `test_sine_is_periodic` (1,2 Hz), `test_noise_is_not_periodic` (fester Seed), `test_impulses_irregular_not_periodic` (Stöße in zufälligen Abständen).
- [ ] **Step 2: Laufen lassen** → FAIL.
- [ ] **Step 3: Implementieren**.
- [ ] **Step 4: Vergleich** — `bench --check --define A121_DSP_REACQ` und `bench --check --define A121_DSP_REACQ --define A121_DSP_REACQ_PERIOD`; übernehmen (Schalter bleibt, Ergebnis in `docs/EXPERIMENTS.md`) nur, wenn alle Grenzen halten und `tippen` besser wird; sonst Ergebnis eintragen und Variante als verworfen kennzeichnen. Test `test_period_variant_within_limits` nur hinzufügen, wenn übernommen.
- [ ] **Step 5: Commit** — `git commit -m "DSP V6: Variante Periodizität in der Bewegungskarte"`

---

### Task 8: Variante Phasensteigung (`A121_DSP_PHASE_SLOPE`)

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (freie Funktion; Verfeinerung des gewählten Kandidaten)
- Test: `host/tests/test_dsp_unit.py` (`PhaseSlopeTest`), `host/tests/test_vital_sim.py`

**Interfaces:**
- Produces: `inline float phaseSlopeHz(const float *x, uint16_t m, float fs, float f0_hz, ComplexFft<1024> &cfft, float *re, float *im)` — `x`: `m` Werte (dezimiert, gefenstert), auf 1024 mit Nullen aufgefüllt; Vorwärts-FFT; nur Bin `k0 = round(f0/fs·1024)` und Nachbarn `k0±1` behalten (übrige, auch negative Frequenzen, 0); inverse FFT; Phase über die ersten `m` Werte abwickeln; Steigung per kleinsten Quadraten → Hz. Rückgabe `f0_hz`, wenn die Amplitude im Mittel < 1e-9.
- Unter Schalter ersetzt `phaseSlopeHz` die parabolische Verfeinerung für den gewählten Kandidaten (Wegsignal des klarsten Punkts `best_h_`, bzw. kombinierter Kanal); Kandidatenwahl und Oktav-Prüfung unverändert.

- [ ] **Step 1: Failing tests** — `test_sine_6_s` (1,23 Hz, 6 s bei 15 Hz, Start 1,2 → |Fehler| < 0,01 Hz), `test_sine_with_breath_harmonic` (Puls 1,2 Hz + 4 × stärkere Komponente 0,75 Hz → |Fehler| < 0,02 Hz), `test_silent_returns_start`.
- [ ] **Step 2: Laufen lassen** → FAIL.
- [ ] **Step 3: Implementieren**.
- [ ] **Step 4: Vergleich** — `bench --check` mit `--define A121_DSP_PHASE_SLOPE` allein, mit `A121_DSP_REACQ`, und mit `--hr-win 600`; übernehmen nur, wenn MAE und alle Grenzen nicht schlechter; Ergebnis (auch 10-s-Fenster) in `docs/EXPERIMENTS.md`.
- [ ] **Step 5: Commit** — `git commit -m "DSP V6: Variante Feinschätzung über die Phasensteigung"`

---

### Task 9: Gesamtprüfung, Firmware-Build, Doku

**Files:**
- Modify: `firmware/esp32s3/platformio.ini` (neue Umgebung), `docs/EXPERIMENTS.md`, `docs/VITAL_AUDIT.md` (Stand), `docs/HARDWARE.md` (Schalter beim Befehl `iq3`)

**Interfaces:**
- `platformio.ini`: Umgebung `[env:esp32s3-n16r8-reacq]` mit `extends = env:esp32s3-n16r8` und `build_flags = ${env:esp32s3-n16r8.build_flags} -DA121_DSP_REACQ` (plus übernommene Varianten aus Task 7/8).

- [ ] **Step 1: Alle Prüfungen** — Python-Suite, `bench --check` ohne/mit Schaltern, `cd android && ./gradlew --offline :app:testDebugUnitTest`, `cd firmware/esp32s3 && pio run` und `pio run -e esp32s3-n16r8-reacq` (Build, keine Laufzeitprüfung).
- [ ] **Step 2: Rechenzeit am PC** — `bench` Spalte `us` mit/ohne Schalter; Erwartung + 15–25 %. Am Gerät erst nach Freigabe.
- [ ] **Step 3: Doku** — `EXPERIMENTS.md` Abschnitt „V6 Wiederfinden“: Bench-Tabelle je Variante, T1 vorher/nachher (`reacq_eval.py`), Hinweis „nur Simulation/ohne Referenz“; `VITAL_AUDIT.md` Stand-Tabelle um Zeile V6; `HARDWARE.md` beim Befehl `iq3` die neuen Schalter und Felder.
- [ ] **Step 4: Commit** — `git commit -m "V6: Gesamtprüfung, Build-Umgebung mit Schalter, Doku"`
- [ ] **Step 5: Übergabe an den Projektinhaber** — Bericht: Prüfstand, T1, Build getrennt von Gerät; Vorschlag nächster Schritt: Firmware `esp32s3-n16r8-reacq` flashen (nach Freigabe, Port per `ls -l /dev/serial/by-id`, Flashweg mit esptool 5.6 aus `HARDWARE.md`), live mit Handzählung; Polar-H9-Tag-A für die Abnahme.
