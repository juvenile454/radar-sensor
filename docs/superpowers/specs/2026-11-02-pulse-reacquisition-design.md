# Reacquire the pulse faster after movement (V6) — design

As of 2026-11-02. Belongs to `docs/VITAL_AUDIT.md` (phases V0–V5); implemented in `firmware/esp32s3/src/a121_esp_dsp.h`.

## Goal

After a movement, the pulse should be valid again within seconds instead of half a minute, without producing wrong
values. As automatic as possible, at any distance the zoom window tracks.

**Said (project owner):** Once the sensor has found the pulse, it is correct (counted by hand). It loses the pulse at the
slightest movement and reacquires it very slowly. Rhythm and beats should be used better.
**Assumption:** The most important quantities are reacquisition time and share of valid time, without more wrong values.
**Non-goals:** Improving accuracy (not measurable without a reference), physically extending the range, TinyML (only with
chest strap data, see below).

## Starting point (measured, without reference)

30-min recording `phone/2026-11-01_164043_usb_iq3raw_bildschirmaus`, recomputed with firmware version
`2026-11-02-vital1` (`vital_eval.replay_lines`):

- 1899 reports, 211 valid (11 %); 33 dropouts after a valid value, total 1711 s.
- Dropouts with > 30 % motion in the window: 18, median 40 s, together 1627 s.
- State "search" (555 reports): 508 of them without a breathing peak (`br_state` 1); 182 with > 30 % motion.
- Minute 6–22 mostly "no person" (whether the test subject was there is unknown; these sections do not count as dropouts).

*Addendum 2026-11-03:* With the person filter of `host/reacq_eval.py` (dropouts where the state KEINE_PERSON is in the
majority do not count), the T1 baseline is: 15 motion dropouts, median 23.7 s, total 591 s (next to the hand count above
of 18 / ≈ 40 s / ≈ 1627 s; without the filter 18 / 39.0 s / 1609 s). The goal "median ≤ 15 s" is to be measured against the
numbers with the filter.

Causes in the code (`computeReport` et al.):

1. **All or nothing over 15 s:** `rep.valid` requires `motion_pct ≤ kMaxMotionPct` (30 %). 5 seconds of movement in the
   15-s window make the report invalid until they have dropped out (≥ 13 s after the movement ends), then hold time.
2. **Tracking continues through movement:** `hr_prev_`/`hr_miss_` are also updated on moving windows
   and can jump to artifacts.
3. **No memory:** After the movement, the whole 40–180 /min band is searched anew.
4. **Breathing as a requirement:** Sign of life = breathing peak (`breath_ok`) or held breath with a pulse peak. Slight
   restlessness smears the breathing peak, and the pulse is rejected.

The test bench hardly reproduces this: `beweg` (10 s random movement ±3 mm, without distance or amplitude change) has
101/120 valid reports.

## Design

Everything behind compile switches, default off (see "Rollout"): `A121_DSP_REACQ` for 1.–5. and 7.,
`A121_DSP_REACQ_PERIOD` (variant in 1.) and `A121_DSP_PHASE_SLOPE` (6.). Without the switches, the output stays
bit-identical to the current state.

### 1. Motion map before the spectra

- Before the loop over the zoom points: for the 3 strongest points (same strength as `selectStrongest`), form the
  displacement signal as today (circle fit, trend, bandpass 0.6–5 Hz), per 1-s block the RMS, reference = quiet quarter
  (as in `gateMotion`). A block counts as moving if it is above `kMotionK` × reference at at least 2 of the 3 points.
- Result: bit mask of the moving blocks in the window and `clean_tail_s` = quiet seconds since the last moving block.
- Function `motionMap(start, N, use) → {mask, clean_tail_blocks}`; independently testable via `host/dsp_replay`.
- **Periodicity variant** (`A121_DSP_REACQ_PERIOD`, only together with `A121_DSP_REACQ`; after Vital-Radio, see
  sources): A block that is too strong only counts as moving if the signal around it (3-s excerpt, heart band) is not
  periodic: spectrum of the excerpt, strongest peak < `kPeriodK` (5) × mean of the remaining bins. Goal: do not count small
  irregular movements (typing) as movement when the pulse stays visible. Risk: rhythmic
  movement (foot tapping) is also periodic; therefore adopt it only if `zappel`, `lage` and `beweg_hr` stay within their
  limits and `tippen` gets better.
  *Addendum 2026-11-03:* Implemented with the mean of the bins outside the Hann main lobe (± 2 resolution cells) as the
  reference, not all remaining bins (reasons and numbers in `docs/EXPERIMENTS.md`, V6). Variant rejected.

### 2. Window selection

- No moving block in the window: as today (N up to 15 s).
- Movement in the window and `clean_tail_s ≥ kReacqMinS` (6 s): window = quiet tail (N' = clean_tail, at most
  15 s). Computation as in the quick start (V2.4): thresholds for the breathing peak scaled with N', value valid only with
  confirmation by autocorrelation or single beats (±5 /min) and not on a breathing harmonic m ≤ 3.
- Movement and `clean_tail_s < 6 s`: pulse invalid, state BEWEGUNG as today; breathing, breathing state and room are
  computed on the full window as today.
- The rule `motion_pct ≤ 30 %` is dropped under the switch; `motion_pct` is still computed and output.
- *Addendum 2026-11-03:* Implementation (ruling R8): breathing and the harmonic lock stay on the full window N, only the
  pulse part runs on the tail; reason: in the 6-s window breathing becomes too inaccurate (13.6 instead of 15 /min) and
  the m ≤ 3 lock misses 3 × breathing.

### 3. Freeze tracking during movement

- If the report is invalid because of movement, `hr_prev_`, `hr_miss_`, `alt_count_`, the Viterbi state and `hr_hist_`
  are not updated.
- In the first report after the movement (short window), `hr_prev_` is set only from a confirmed value.

### 4. Memory

- On every valid report with confidence ≥ `kMemMinConf` (50 %) and window ≥ 8 s: store `mem_hr_`, `mem_ibi_ms_`,
  `mem_tot_` (frame counter).
- Active as long as `tot_frames − mem_tot_ < kMemS` (60 s) **and** clean echo in the zoom (`echo_ok`) **and** room target
  in the zoom window (`target_in_zoom`). If a condition drops out (standing up, rezoom to a different target), the memory
  is cleared. Rezoom to the same target (V2.3, `kKeepTrackS`) keeps it.
- Effect with active memory in the short window: candidates within ±`kMemBandBpm` (15 /min) around `mem_hr_` are sorted
  ahead of the others if their value is ≥ 0.5 × the best candidate. "Reacquired" = chosen candidate in the band
  **and** autocorrelation or single beats within ±5 /min. Otherwise normal search without memory preference.
- This way the memory does not cling to an old value: if the pulse has changed by more than 15 /min, the free
  search with its existing rules applies.

### 5. Sign of life

`rep.valid` (under the switch) = window condition from 2. **and** (`breath_ok` **or** `held_pulse` **or**
`mem_ok`), with `mem_ok` = memory active **and** value reacquired (confirmation from 4.).
Without a previous valid value there is no memory; the cases "empty", "nobody" and "gone" do not change as a result.

*Addendum 2026-11-03 (ruling R15):* `mem_ok` additionally requires |reported value − `mem_hr_`| ≤ `kMemSubstBpm` (5 /min,
same tolerance as the confirmation, tighter than the 10 /min error limit of the test bench). A reacquired value farther
away (within the ±15 band) needs `breath_ok` like any other report; the ±15 band for the candidate preference remains.

### 6. Fine estimation via the phase slope

Variant `A121_DSP_PHASE_SLOPE` (switchable independently of `A121_DSP_REACQ`; after Vital-Radio): Instead of the parabolic
refinement of the spectrum peak, the frequency of the chosen candidate is determined as follows: in the spectrum of the
displacement signal (clearest point or combined signal) keep only the peak and its two neighboring bins, inverse FFT to
the complex time signal, unwrap the phase, slope by line fit (least squares) → frequency. Mainly useful in the
short window (6 s: main lobe ≈ 20 /min wide). Check: test bench with and without the variant, especially `step`
(response), `kurz`, `zappel` and window length 10 s (V2.5); adopt only if MAE and limits do not get
worse. Computation time: one complex inverse FFT (1024 points) per report.

### 7. Output

`@DSP` gets two fields at the end (after `n_eff`, which stays empty without `A121_DSP_COMBINE`):

- `win_s`: length of the pulse window used, in s (integer, 0 = no pulse window).
- `reacq`: 0 = normal, 1 = short window after movement, 2 = reacquired via memory.

Extend `vital_eval.DSP_FIELDS`, `host/dsp_replay`, and the app parser (`EspMsg.kt`) with the fields; missing fields = NaN/empty
as before. `@VITAL` and `@STATE` unchanged.

### Constants (starting values, to be checked on the test bench and on day A)

| Name | Value | Meaning |
|---|---|---|
| `kReacqMinS` | 6 s | shortest quiet tail for a pulse |
| `kMemS` | 60 s | lifetime of the memory |
| `kMemBandBpm` | 15 /min | search band around the remembered value |
| `kMemMinConf` | 50 % | confidence from which a value goes into the memory |
| `kMemSubstBpm` | 5 /min | Addendum 2026-11-03 (ruling R15): largest distance from the memory at which a reacquired value replaces the breathing peak as sign of life |
| Motion map | 2 of 3 points | block counts as moving |
| `kPeriodK` | 5 | Periodicity: peak / mean of the remaining bins (variant `A121_DSP_REACQ_PERIOD`) |

## Verification

Three separate checks as in the audit (guidelines); a build is not a runtime check.

### Test bench (`host/vital_sim.py`)

New scenarios (120–180 s each, pulse and breathing known):

| Name | Content |
|---|---|
| `kurz` | 3 s movement at 60 s |
| `zappel` | three 2-s movements at 40, 60, 80 s |
| `lang` | 20 s movement at 50 s |
| `lage` | posture change at 60 s: distance jumps by 8 mm, echo amplitude −30 %, 2 s transition |
| `beweg_hr` | 10 s movement at 60 s, pulse jumps from 70 to 85 during it |
| `beweg_weg` | 5 s movement at 50 s, person leaves at 60 s |
| `tippen` | 60–120 s irregular small movement (arms/shoulders, ±0.3 mm at the chest echo, jolts 0.2–0.5 s), no posture change |

New metrics in `bench`: `wiederfind_s` (end of each movement to the first valid value within ±5 /min of the
truth, median over the movements), `falsch_nach_beweg_n` (valid reports > 10 /min off in the 30 s after
the end of the movement).
*Addendum 2026-11-03 (ruling R15):* `falsch_in_beweg_n` (valid reports > 10 /min off in (start, end] of the movement),
limit 0 in all new scenarios with the switch, only reported without the switch; `erst_nach_beweg_fehler` (|first valid value
after the movement − true instantaneous pulse|) converts the limit for `beweg_hr` below (≤ 5).

Limits (`--check`): `wiederfind_s ≤ 8` in `kurz`, `zappel`, `lage`; `≤ 10` in `lang`; `falsch_nach_beweg_n = 0`
everywhere; in `beweg_hr` first valid value after the movement within ±5 /min of 85 (not 70); in `beweg_weg` no
valid report from 76 s (leaving at 60 s + 16 s, as in `weg`: until then there is still data of the person in the 15-s window); in `tippen` at least 80 % valid in the 60–120 s section with MAE ≤ 2 /min. All previous
limits continue to apply, with and without the switch. Variants (`A121_DSP_REACQ_PERIOD`, `A121_DSP_PHASE_SLOPE`) run as
their own columns in the bench report (`--define`).

### Replay of real recordings (without reference)

New tool `host/reacq_eval.py <folder>… [--replay <header-folder>]`: dropouts after a valid value (count, median,
total), split by cause (movement / no breathing peak / rest), share valid without "no person" sections,
jumps > 15 /min between consecutive valid reports. Compare header without and with `A121_DSP_REACQ` on
T1 and the recordings of 2026-11-02.
Goal on T1: median of the motion dropouts ≤ 15 s (today 40 s), jumps no more than today.
*Addendum 2026-11-03 (ruling R16):* The comparison without/with the switch only holds under the same counting rule; `--ohne-karte` counts
as without the switch (`win_s == 0` not as movement). Result on T1 in `docs/EXPERIMENTS.md`, "V6 overall check".

### Live and reference

- Device: build firmware with the switch on, `iq3 0 raw` via the app, `replay-cpp` the same, `compute_us` ≤ 400 ms per report.
- Polar H9 (from about 2026-11-05): day A per measurement plan M5, plus session R5 (movement) evaluated with `vital_eval.py`
  without/with the switch (`--replay`): pulse MAE and share within ±5 in the 30 s after movement must not be worse than in the
  rest of the session by more than 2 /min or 10 percentage points, respectively.
- Until then: hand count with markers `zaehl_start`/`zaehl_ende` as a sample, explicitly not an accuracy claim.

### Further tests

- `host/tests`: unit tests for `motionMap` (masks, `clean_tail`), memory lifecycle (set, expire on echo loss
  and target outside, rezoom), new `@DSP` fields in `vital_eval` and `dsp_replay`; replay stays PC = phone
  byte-identical.
- `android`: `LineParserTest` for the new fields.

## Rollout

1. All switches off: implementation, tests, test bench with and without switches, replay. Compare variants
   (`A121_DSP_REACQ_PERIOD`, `A121_DSP_PHASE_SLOPE`) one by one against `A121_DSP_REACQ` alone; adopt only
   what holds the limits and is better.
2. Test bench and T1 goal met: flash a build with the switch on and check live (project owner operates, hand count).
3. Polar H9 day A confirmed: switch default on, raise the firmware identifier; day B for acceptance.

## Risks

- **Short window, coarse resolution:** 6 s give 0.17 Hz (10 /min) bin spacing; wrong values possible. Countermeasures:
  confirmation requirement, memory band, test bench `beweg_hr`.
- **Memory holds a wrong value:** A wrong stored value would be preferred. Countermeasures: store only values with confidence
  ≥ 50 % and window ≥ 8 s, confirmation required, 60 s lifetime.
- **Motion map too sensitive/insensitive:** tune on the test bench and on T1 (day A is development, day B only
  acceptance).
- **Computation time:** The motion map computes 3 points up front (today all 24 run, with 48 zoom points 16); then one pass over the chosen
  window. Expected + 15–25 %; limit ≤ 400 ms per report (core 0, 1 report/s), today 91–152 ms.

## Sources

- Adib, Mao, Kabelac, Katabi, Miller 2015, Smart Homes that Monitor Breathing and Heart Rate (Vital-Radio), *ACM CHI*,
  https://doi.org/10.1145/2702123.2702200. FMCW 5.46–7.25 GHz, 8-cm range steps, periodicity test per 30-s window
  (peak ≥ 5 × mean of the remaining bins, otherwise window rejected), pulse in the 10-s window with fine estimation via the
  phase slope. Median accuracy 98.5 % (pulse, 1 m) against a pulse oximeter, quiet sections only, 2 min each; moving
  time is discarded, not reacquired. The range of 8 m and measurement through walls apply to their hardware, not to
  the A121 at 60 GHz.

## Later (not part of this design)

TinyML on the ESP32-S3 (e.g. block classification "clean/moving" or beat detection), as soon as several hours of
recordings with `@RAW` and Polar H9 RR are available; evaluate then against this design.
