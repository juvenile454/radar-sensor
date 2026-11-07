# V6 Reacquire the pulse after movement — implementation plan

> Implementation plan as written before the work (2026-11-02), kept as a record. The checkboxes were not updated;
> what was built and measured is in `docs/VITAL_AUDIT.md` (V6) and `docs/EXPERIMENTS.md`.

**Goal:** Pulse valid again within seconds after movement instead of ~40 s, without wrong values, everything behind compile switches.

**Architecture:** Changes in the single DSP source `firmware/esp32s3/src/a121_esp_dsp.h`: small, free-standing
functions/classes (motion map, memory, candidate preference, periodicity, phase slope), each tested on the PC, then wired
into `computeVitalReport` under `#ifdef A121_DSP_REACQ`. The test bench (`host/vital_sim.py`) gets
motion models, scenarios and metrics; the new tool `host/reacq_eval.py` measures dropouts on real recordings.

**Tech Stack:** C++17 (header, on the PC with g++ via `host/dsp_replay` and stub headers), Python 3.12 + numpy (`.venv`),
unittest, Kotlin/JUnit (app parser), PlatformIO (`pio run`).

**Spec:** `docs/design/specs/2026-11-02-pulse-reacquisition-design.md`

## Global Constraints

- Switches default off: `A121_DSP_REACQ`, `A121_DSP_REACQ_PERIOD` (only together with `A121_DSP_REACQ`), `A121_DSP_PHASE_SLOPE`.
- Without the switches, every output line stays bit-identical to the state `53c45e6` (check: `host/tests/test_dsp_replay.py`, bench numbers unchanged).
- Extend the protocol only at the end; `@VITAL` keeps 15 fields, `@STATE` unchanged.
- Constants: `kReacqMinS = 6` s, `kMemS = 60` s, `kMemBandBpm = 15` /min, `kMemMinConf = 50` %, motion map 2 of 3 points, `kPeriodK = 5`.
- Computation time per report ≤ 400 ms (core 0); today 91–152 ms on the device.
- No data/binary files in the repo; recordings live under `~/Data/radar-sensor/`.
- No flashing, no app installation without approval from the project owner; port via `ls -l /dev/serial/by-id`, phone serial number via `adb devices -l`.
- A build/compile is not a runtime check; report them separately. No claim about pulse accuracy without a chest strap (Polar H9 from about 2026-11-05).

Test commands (from the repo root):
- Python: `.venv/bin/python -m unittest discover -s host/tests -p 'test_*.py'` (≈ 2 min; single file: `.venv/bin/python -m unittest host/tests/<file>.py`)
- Test bench: `.venv/bin/python host/vital_sim.py bench --check [--define A121_DSP_REACQ …]`
- App: `cd android && ./gradlew --offline :app:testDebugUnitTest`
- Firmware: `cd firmware/esp32s3 && pio run`

## Review Focus

1. **Person stands up right after a movement** → the memory must not carry a pulse forward (Task 5 test `test_cleared_when_target_leaves`, Task 2/6 scenario `beweg_weg`).
2. **Rezoom to a different target during the memory** → memory cleared; rezoom to the same target keeps it (Task 6 step `test_rezoom_keeps_or_clears_memory`).
3. **Pulse changes by more than 15 /min during the movement** → free search instead of getting stuck (Task 5 `test_no_preference_outside_band`, Task 2/6 scenario `beweg_hr`).
4. **Movement longer than 60 s** → memory expired, behavior as without the switch (Task 5 `test_expires_after_60_s`).
5. **Recordings of older firmware without the new fields** → `vital_eval`, the test bench and the app read NaN/`null` instead of aborting (Task 1 `test_old_line_without_reacq_fields`, `LineParserTest.dspWithoutReacqFields`).

---

### Task 1: `@DSP` fields `win_s` and `reacq`

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (`VitalReport` ≈ l. 1250–1280, `formatDspLine` ≈ l. 2559)
- Modify: `host/vital_eval.py` (`DSP_FIELDS`, l. 39–41)
- Modify: `android/app/src/main/java/io/github/juvenile454/radarsensor/protocol/EspMsg.kt` (`Dsp`), `.../protocol/LineParser.kt` (`"@DSP"`)
- Test: `host/tests/test_dsp_unit.py` (`DspLineTest`), `host/tests/test_vital_eval.py`, `android/app/src/test/java/io/github/juvenile454/radarsensor/protocol/LineParserTest.kt`

**Interfaces:**
- Produces: `VitalReport::win_s` (`uint8_t`, s), `VitalReport::reacq` (`uint8_t`: 0 normal, 1 short window after movement, 2 reacquired via memory); constants `kReacqNone = 0`, `kReacqShort = 1`, `kReacqMemory = 2`.
- Line format with `A121_DSP_REACQ`: 13 current fields, then field 14 `n_eff` (empty without `A121_DSP_COMBINE`), field 15 `win_s`, field 16 `reacq`.
- `vital_eval.DSP_FIELDS` ends with `"n_eff", "win_s", "reacq"`; Kotlin `Dsp.winS: Int?`, `Dsp.reacq: Int?` from `p[15]`, `p[16]`.

- [ ] **Step 1: Failing tests**

`test_dsp_unit.py`, class `DspLineTest`:
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
`test_vital_eval.py`: `test_reacq_fields_parsed` (line `@DSP,1000,1,5,88.0,300000,0,0,0,0,0,0,0,60,,7,2` → `dd["win_s"] == [7.0]`, `dd["reacq"] == [2.0]`) and `test_old_line_without_reacq_fields` (line with 13 fields → both NaN).
`LineParserTest.kt`: `dspWithReacqFields` (`winS == 7`, `reacq == 2`, `nEff == null`) and `dspWithoutReacqFields` (both `null`).

- [ ] **Step 2: Run them, they fail** — `.venv/bin/python -m unittest host/tests/test_dsp_unit.py host/tests/test_vital_eval.py`; `cd android && ./gradlew --offline :app:testDebugUnitTest --tests '*LineParserTest*'`. Expected: FAIL (missing fields).

- [ ] **Step 3: Implement** — fields in `VitalReport`; under `#ifdef A121_DSP_REACQ`, `formatDspLine` gets its own line with `%s` for `n_eff` (with `A121_DSP_COMBINE` the number `%.1f`, otherwise empty) and `,%u,%u` for `win_s`, `reacq`. Extend `DSP_FIELDS`; Kotlin `winS = p.getOrNull(15)?.toIntOrNull()`, `reacq = p.getOrNull(16)?.toIntOrNull()`.

- [ ] **Step 4: Tests green** — same commands; additionally `.venv/bin/python -m unittest host/tests/test_dsp_replay.py` (output without the switch bit-identical).

- [ ] **Step 5: Commit** — `git commit -m "@DSP: fields win_s and reacq behind A121_DSP_REACQ (V6)"`

---

### Task 2: Test bench — motion models, scenarios, metrics

**Files:**
- Modify: `host/vital_sim.py` (`make_frames`, `SCENARIOS`, `LIMITS`, `evaluate`, report in `run_bench`)
- Test: `host/tests/test_vital_sim.py`

**Interfaces:**
- `make_frames(..., motion=None | tuple | list[tuple], shift=None, jolts=None)`:
  `motion`: one `(t0, t1, pp_mm)` or a list of them; `shift=(t0, dauer_s, dd_mm, amp_faktor)` moves all chest scattering centers from `t0` linearly by `dd_mm` over `dauer_s` and scales their amplitude to `amp_faktor`; `jolts=(t0, t1, pp_mm)` generates random jolts (length 0.2–0.5 s, spacing 0.5–3 s, half-sine) at the chest echo.
- `reacq_metrics(t_s: np.ndarray, valid: np.ndarray, err: np.ndarray, episodes: list[tuple[float, float]]) -> dict` with
  `wiederfind_s` (median over episodes: time from the end of the episode to the first valid report with `err ≤ 5`, NaN if none until the end of the scenario) and `falsch_nach_beweg_n` (valid reports with `err > 10` in `(ende, ende + 30]`).
- `section_metrics(t_s, valid, err, a: float, b: float) -> dict` with `abschnitt_gueltig_pct`, `abschnitt_mae` (reports with `a < t ≤ b`).
- `SCENARIOS` new: `kurz` (motion (60, 63, 4.0)), `zappel` (motion [(40,42,4.0),(60,62,4.0),(80,82,4.0)]), `lang` (motion (50,70,6.0), T 150), `lage` (shift (60, 2, 8.0, 0.7)), `beweg_hr` (motion (60,70,6.0), hr 70 → 85 linear 60–70 s, T 150), `beweg_weg` (motion (50,55,4.0), leave 60), `tippen` (jolts (60,120,0.3), T 150).
- `LIMITS_REACQ: dict` (applied only if `"A121_DSP_REACQ"` is in `DEFINES`): `wiederfind_s ≤ 8` (`kurz`, `zappel`, `lage`), `≤ 10` (`lang`); `falsch_nach_beweg_n ≤ 0` in all new scenarios; `beweg_weg`: `weg_gueltig_n ≤ 0`; `tippen`: `abschnitt_gueltig_pct ≥ 80`, `abschnitt_mae ≤ 2`.

- [ ] **Step 1: Failing tests** in `test_vital_sim.py`:
```python
class ReacqMetricsTest(unittest.TestCase):
    def test_first_correct_after_episode(self):
        t = np.arange(50.0, 100.0); v = t >= 66; e = np.where(t >= 68, 1.0, 20.0)
        m = vital_sim.reacq_metrics(t, v, e, [(60.0, 63.0)])
        self.assertEqual(m["wiederfind_s"], 5.0)          # 68 − 63
        self.assertEqual(m["falsch_nach_beweg_n"], 2)     # 66, 67 valid and off by > 10
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
    def test_shift_moves_chest_echo(self):   # zoom phase at the strongest point jumps by ≈ 4π·8 mm/λ
    def test_jolts_only_inside_window(self): # displacement outside (60,120) without jolts
    def test_motion_list(self):              # drei Episoden ergeben Bewegung nur in den drei Intervallen
```
(the three `MotionModelTest` cases via `make_frames` into a temp directory and `np.fromfile`; tolerances: phase jump 10–20 rad after unwrapping, RMS outside < 10 % of inside.)

- [ ] **Step 2: Run them** — `.venv/bin/python -m unittest host/tests/test_vital_sim.py` → FAIL (`reacq_metrics` missing).

- [ ] **Step 3: Implement** — functions and parameters as above; `evaluate` calls `reacq_metrics` with the episodes from `motion`/`shift`/`jolts` (`shift` as episode `(t0, t0 + dauer)`) and `section_metrics` for `tippen` (60, 120); `run_bench` appends `LIMITS_REACQ` if the switch is set and shows `wiederfind_s`, `falsch_nach_beweg_n` in the extra column.

- [ ] **Step 4: Measure the baseline and record it as the limit without the switch** — `.venv/bin/python host/vital_sim.py bench --scen kurz zappel lang lage beweg_hr beweg_weg tippen`. For the new scenarios in `LIMITS`, enter only the existing mandatory values (`zustand_beweg_fehl`, breaths) and `falsch_nach_beweg_n` ≤ measured value (comment "baseline V6, without switch"); note the measured `wiederfind_s` in the comment. `erst_s`/`erst_ok_s`/`frueh_falsch_n` apply as for the other scenarios.

- [ ] **Step 5: Tests and bench green** — `.venv/bin/python -m unittest host/tests/test_vital_sim.py`; `.venv/bin/python host/vital_sim.py bench --check` → `ERGEBNIS: OK`.

- [ ] **Step 6: Commit** — `git commit -m "Test bench V6: motion models, scenarios kurz/zappel/lang/lage/beweg_hr/beweg_weg/tippen, reacquisition metrics"`

---

### Task 3: `host/reacq_eval.py` — dropouts on real recordings

**Files:**
- Create: `host/reacq_eval.py`
- Test: `host/tests/test_reacq_eval.py`

**Interfaces:**
- Consumes: `vital_eval.load_recording(folder, lines)`, `vital_eval.replay_lines(folder, header_dir)`, `vital_eval.count_jumps`.
- Produces:
  - `@dataclass Dropout: start_s: float; dur_s: float; cause: str` (`"bewegung"` if a report with `motion_pct > 30` or state 4 occurs in the dropout, otherwise `"keine_atemspitze"` if `br_state == 1` predominates, otherwise `"rest"`).
  - `dropouts(t_s, ok, motion_pct, br_state, state) -> list[Dropout]`: runs of `ok == 0` after a valid report; runs in which > 50 % of the reports have state 0 ("no person") do not count.
  - `summary(t_s, ok, hr, state, drops) -> dict`: `anteil_gueltig_pct` (without state 0), `n`, `median_s`, `summe_s` per cause, `spruenge`.
  - CLI: `reacq_eval.py <folder>… [--replay <header-folder>] [--define NAME]…` — with `--define`, a header copy with prepended `#define` lines is written to a temp directory and used as `--replay`. Output: one line per folder plus a table of causes.

- [ ] **Step 1: Failing tests** — `test_dropout_after_valid_only` (sequence ok `0,0,1,1,0,0,0,1` → one dropout, 3 s), `test_cause_motion`, `test_cause_no_breath_peak`, `test_no_person_not_counted`, `test_summary_share_without_no_person`, `test_define_header_copy` (copy begins with `#define A121_DSP_REACQ`).

- [ ] **Step 2: Run them** — `.venv/bin/python -m unittest host/tests/test_reacq_eval.py` → FAIL (module missing).

- [ ] **Step 3: Implement** as in Interfaces.

- [ ] **Step 4: Tests green; measure the baseline on T1 and 2026-11-02**
```bash
.venv/bin/python host/reacq_eval.py ~/Data/radar-sensor/phone/2026-11-01_164043_usb_iq3raw_bildschirmaus ~/Data/radar-sensor/phone/2026-11-02_171254_livetest_vital1 ~/Data/radar-sensor/app-fixtures/2026-11-02_vital1_iq3raw_60s --replay firmware/esp32s3/src
```
Expected on T1 (state of the spec): motion dropouts 18, median ≈ 40 s, total ≈ 1627 s; enter the numbers in `docs/EXPERIMENTS.md` under a new section "V6 reacquisition (baseline)".

- [ ] **Step 5: Commit** — `git commit -m "reacq_eval.py: measure dropouts after movement on recordings (V6), baseline T1"`

---

### Task 4: Motion map

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (new free function near `gateMotion`, member in `UltimateEspDsp`)
- Test: `host/tests/test_dsp_unit.py` (new class `MotionMapTest`)

**Interfaces:**
- Produces:
  - `struct MotionMap { uint32_t mask; uint8_t n_blocks; uint8_t clean_tail_blocks; };` (bit k = block k moving, block 0 = oldest)
  - `inline MotionMap motionMapFromRms(const float *rms, uint8_t n_pts, uint8_t nb, float k = kMotionK, uint8_t votes = 2)` — `rms` row by row `n_pts × nb`; per point reference = lower quarter of its blocks (as in `gateMotion`); block moving if `rms > k × reference` at ≥ `votes` points; `clean_tail_blocks` = blocks after the last moving one (all if none).
  - Member `MotionMap motionMap(uint16_t start, uint16_t N, const bool *use)` — 3 strongest points from `use`/strength as in `selectStrongest`, displacement signal as in the main loop (`binDisplacement`, `removeLinearTrend`, `sosFiltFilt`), block RMS per 1-s block (`kGateBlk`), then `motionMapFromRms`. Compiled only under `#ifdef A121_DSP_REACQ`.
  - Constants `kReacqMinS = 6U`, `kMotionVotes = 2U`.

- [ ] **Step 1: Failing tests** (via `run_cpp(..., ("-DA121_DSP_REACQ",))`):
  `test_quiet_has_full_tail` (3 points × 15 equal blocks → `mask 0`, `clean_tail 15`), `test_motion_needs_two_votes` (block 8 only at point 0 × 10 → not moving; at points 0 and 1 → moving, `clean_tail 6`), `test_tail_after_last_motion` (blocks 3 and 11 moving → `clean_tail 3`).

- [ ] **Step 2: Run them** → FAIL (`motionMapFromRms` unknown).

- [ ] **Step 3: Implement**.

- [ ] **Step 4: Tests green; bit-identical without the switch** — `.venv/bin/python -m unittest host/tests/test_dsp_unit.py host/tests/test_dsp_replay.py`.

- [ ] **Step 5: Commit** — `git commit -m "DSP V6: Bewegungskarte aus 3 Punkten (motionMapFromRms)"`

---

### Task 5: Memory and candidate preference

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (new class near `BreathStateTracker`, free function near candidate selection)
- Test: `host/tests/test_dsp_unit.py` (`ReacqMemoryTest`, `PreferMemoryTest`)

**Interfaces:**
- Produces:
  - `class ReacqMemory { public: void clear(); void store(float hr_bpm, float ibi_ms, uint32_t tot_frames); bool active(uint32_t tot_frames, bool echo_ok, bool target_in_zoom); float hr() const; float ibi_ms() const; };` — `active` clears the memory itself if `tot_frames − stored ≥ kMemS × kFsHz` or `!echo_ok` or `!target_in_zoom`.
  - `inline int8_t preferMemoryCandidate(const float *cand_bpm, const float *cand_score, uint8_t n, float mem_hr, float band = kMemBandBpm, float min_rel = 0.5f)` — index of the strongest candidate with `|bpm − mem_hr| ≤ band` and `score ≥ min_rel × score[0]`, otherwise −1.
  - Constants `kMemS = 60.0f`, `kMemBandBpm = 15.0f`, `kMemMinConf = 50.0f`.

- [ ] **Step 1: Failing tests**:
  `test_store_and_active`, `test_expires_after_60_s` (`active(t0 + 60*60)` → false, afterwards `hr()` 0), `test_cleared_when_echo_lost`, `test_cleared_when_target_leaves`, `test_prefers_near_candidate` (candidates 140 (1.0), 74 (0.6), 50 (0.3); memory 72 → 1), `test_no_preference_outside_band` (memory 72, candidates 90 (1.0), 120 (0.8) → −1), `test_weak_near_candidate_ignored` (74 with 0.4 → −1).

- [ ] **Step 2: Run them** → FAIL.
- [ ] **Step 3: Implement** (under `#ifdef A121_DSP_REACQ`).
- [ ] **Step 4: Tests green** — `.venv/bin/python -m unittest host/tests/test_dsp_unit.py`.
- [ ] **Step 5: Commit** — `git commit -m "DSP V6: memory (ReacqMemory) and candidate preference"`

---

### Task 6: Wiring into `computeVitalReport`

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (`computeVitalReport` ≈ l. 1611–2095, `resetHistory`/`resetTracking` ≈ l. 1342–1410, header comment l. 1–52)
- Test: `host/tests/test_vital_sim.py` (new class `ReacqBenchTest`), `host/tests/test_dsp_unit.py`

**Interfaces:**
- Consumes: Task 1 (`win_s`, `reacq`), Task 4 (`motionMap`, `kReacqMinS`), Task 5 (`ReacqMemory mem_`, `preferMemoryCandidate`).
- Behavior under `#ifdef A121_DSP_REACQ` (spec 2.–5.):
  1. After determining `use[]`: `MotionMap mm = motionMap(start, N, use)`.
  2. No moving block → window as today, `win_s = N/60`, `reacq = 0`.
  3. Moving and `clean_tail_blocks ≥ kReacqMinS` → pulse part (spectra, candidates, ACF, single beats) on the tail `start' = start + N − tail×60`, `N' = tail×60`; breathing/breathing state/room still on `N`. Treated as provisional (`prov`): valid only with ACF or IBI confirmation ±5 /min and not on a breathing harmonic m ≤ 3. `reacq = 1`, `win_s = tail`.
  4. Moving and tail < 6 s → pulse invalid, tracking state (`hr_prev_`, `hr_miss_`, `alt_count_`, `vit_`, `hr_hist_`) unchanged, `win_s = 0`.
  5. Memory: in case 3 with `mem_.active(...)`, `preferMemoryCandidate` first; if the chosen candidate is in the band and confirmed → `reacq = 2`, `hr_prev_` = confirmed value. After every valid report with `confidence_pct ≥ kMemMinConf` and `N ≥ kHrStableWin`: `mem_.store(hr, ibi, tot_frames)`.
  6. `rep.valid` = window condition **and** (`breath_ok` **or** `held_pulse` **or** `reacq == 2`); the 30 % rule is dropped.
  7. `resetTracking()` calls `mem_.clear()`; `resetHistory()` with retained tracking (V2.3) keeps the memory.

- [ ] **Step 1: Failing tests** — `ReacqBenchTest.test_bench_with_reacq_within_limits` (like `test_variant_within_limits`: `run_bench` with `DEFINES=["A121_DSP_REACQ"]`, `check=True` → 0) and `test_rezoom_keeps_or_clears_memory` (`run_cpp` with `UltimateEspDsp`: establish a valid state via synthetic frames from `vital_sim.make_frames` scenario `base` up to 30 s, then `setZoomStartPoint` by ±0 points → `mem_` active; by +40 points → cleared after the next report; access via a test query `bool reacqMemoryActive() const`, only with `A121_DSP_REACQ`).

- [ ] **Step 2: Run them** → FAIL (`LIMITS_REACQ` limits violated / query missing).

- [ ] **Step 3: Implement** as above; extend the header comment with an item "V6 (A121_DSP_REACQ)".

- [ ] **Step 4: Checks**
  - `.venv/bin/python host/vital_sim.py bench --check` → OK (unchanged without the switch)
  - `.venv/bin/python host/vital_sim.py bench --check --define A121_DSP_REACQ` → OK; note `wiederfind_s` per scenario
  - `.venv/bin/python -m unittest discover -s host/tests -p 'test_*.py'` → OK
  - `.venv/bin/python host/reacq_eval.py <T1> --replay firmware/esp32s3/src --define A121_DSP_REACQ` → motion dropouts median ≤ 15 s, jumps no more than baseline (Task 3)
  - If limits are reached only with other constants: change the constant, add value and reason in the comment and in the spec table; observe the day-B rule (tune only on the test bench/T1).

- [ ] **Step 5: Commit** — `git commit -m "DSP V6: Wiederfinden nach Bewegung hinter A121_DSP_REACQ"`

---

### Task 7: Periodicity variant (`A121_DSP_REACQ_PERIOD`)

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (free function; call in `motionMap`)
- Test: `host/tests/test_dsp_unit.py` (`SegmentPeriodicTest`), `host/tests/test_vital_sim.py`

**Interfaces:**
- Produces: `inline bool segmentPeriodic(const float *x, uint16_t n, RealFft &fft, float *spec, float k = kPeriodK)` — `x`: 3-s excerpt of the band-filtered displacement signal, decimated to 15 Hz, Hann; spectrum `fft.power`; heart-band bins 0.674–3.003 Hz; true if the strongest peak ≥ `k` × mean of the remaining bins in the band. Constant `kPeriodK = 5.0f`.
- In `motionMap`: a block that is moving by strength counts as moving, with the switch set, only if `segmentPeriodic` is false for the 3-s excerpt around it (at the strongest point).

- [ ] **Step 1: Failing tests** — `test_sine_is_periodic` (1.2 Hz), `test_noise_is_not_periodic` (fixed seed), `test_impulses_irregular_not_periodic` (jolts at random intervals).
- [ ] **Step 2: Run them** → FAIL.
- [ ] **Step 3: Implement**.
- [ ] **Step 4: Comparison** — `bench --check --define A121_DSP_REACQ` and `bench --check --define A121_DSP_REACQ --define A121_DSP_REACQ_PERIOD`; adopt (switch stays, result in `docs/EXPERIMENTS.md`) only if all limits hold and `tippen` gets better; otherwise record the result and mark the variant as rejected. Add the test `test_period_variant_within_limits` only if adopted.
- [ ] **Step 5: Commit** — `git commit -m "DSP V6: variant periodicity in the motion map"`

---

### Task 8: Phase slope variant (`A121_DSP_PHASE_SLOPE`)

**Files:**
- Modify: `firmware/esp32s3/src/a121_esp_dsp.h` (free function; refinement of the chosen candidate)
- Test: `host/tests/test_dsp_unit.py` (`PhaseSlopeTest`), `host/tests/test_vital_sim.py`

**Interfaces:**
- Produces: `inline float phaseSlopeHz(const float *x, uint16_t m, float fs, float f0_hz, ComplexFft<1024> &cfft, float *re, float *im)` — `x`: `m` values (decimated, windowed), zero-padded to 1024; forward FFT; keep only bin `k0 = round(f0/fs·1024)` and neighbors `k0±1` (set the rest, including negative frequencies, to 0); inverse FFT; unwrap the phase over the first `m` values; slope by least squares → Hz. Returns `f0_hz` if the mean amplitude is < 1e-9.
- Under the switch, `phaseSlopeHz` replaces the parabolic refinement for the chosen candidate (displacement signal of the clearest point `best_h_`, or the combined channel); candidate selection and octave check unchanged.

- [ ] **Step 1: Failing tests** — `test_sine_6_s` (1.23 Hz, 6 s at 15 Hz, start 1.2 → |error| < 0.01 Hz), `test_sine_with_breath_harmonic` (pulse 1.2 Hz + 4 × stronger component at 0.75 Hz → |error| < 0.02 Hz), `test_silent_returns_start`.
- [ ] **Step 2: Run them** → FAIL.
- [ ] **Step 3: Implement**.
- [ ] **Step 4: Comparison** — `bench --check` with `--define A121_DSP_PHASE_SLOPE` alone, with `A121_DSP_REACQ`, and with `--hr-win 600`; adopt only if MAE and all limits are not worse; result (including the 10-s window) in `docs/EXPERIMENTS.md`.
- [ ] **Step 5: Commit** — `git commit -m "DSP V6: variant fine estimate from the phase slope"`

---

### Task 9: Overall check, firmware build, docs

**Files:**
- Modify: `firmware/esp32s3/platformio.ini` (new environment), `docs/EXPERIMENTS.md`, `docs/VITAL_AUDIT.md` (status), `docs/HARDWARE.md` (switches at the `iq3` command)

**Interfaces:**
- `platformio.ini`: environment `[env:esp32s3-n16r8-reacq]` with `extends = env:esp32s3-n16r8` and `build_flags = ${env:esp32s3-n16r8.build_flags} -DA121_DSP_REACQ` (plus adopted variants from Task 7/8).

- [ ] **Step 1: All checks** — Python suite, `bench --check` without/with switches, `cd android && ./gradlew --offline :app:testDebugUnitTest`, `cd firmware/esp32s3 && pio run` and `pio run -e esp32s3-n16r8-reacq` (build, no runtime check).
- [ ] **Step 2: Computation time on the PC** — `bench` column `us` with/without switch; expectation + 15–25 %. On the device only after approval.
- [ ] **Step 3: Docs** — `EXPERIMENTS.md` section "V6 reacquisition": bench table per variant, T1 before/after (`reacq_eval.py`), note "simulation only/without reference"; `VITAL_AUDIT.md` status table gets a row V6; `HARDWARE.md` at the `iq3` command the new switches and fields.
- [ ] **Step 4: Commit** — `git commit -m "V6: full check, build environment with switch, docs"`
- [ ] **Step 5: Handover to the project owner** — report: test bench, T1, build kept separate from the device; proposed next step: flash firmware `esp32s3-n16r8-reacq` (after approval, port via `ls -l /dev/serial/by-id`, flashing route with esptool 5.6 from `HARDWARE.md`), live with hand count; Polar H9 day A for acceptance.
