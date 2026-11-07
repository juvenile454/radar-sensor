# Experiments — pushing the A121 to its limits

Capability matrix for phase 1. Tool: Exploration Tool / `acconeer-exptool` (checked:
version 7.18.2 on PyPI, Python ≥ 3.9, on 2026-10-27). Algorithm modules there:
`distance, presence, smart_presence, breathing, phase_tracking, sparse_iq, vibration, speed,
surface_velocity, hand_motion, touchless_button, tank_level, waste_level, parking, obstacle,
cargo, bilateration`.

Fill the result column only after a real measurement. Recordings go to `~/Data/radar-sensor/`.

## Basics

| # | Experiment | Module | Measured quantity | Result |
|---|---|---|---|---|
| G1 | Understand the raw signal: profiles 1–5, step size, HWAAS, sweep rate | sparse_iq | noise, range, max. rate | — |
| G2 | Distance accuracy vs. tape measure (0.1–5 m, metal plate) | distance | deviation in mm | Part 1 (2026-10-30): 1.00 m brick wall, without cap → median 1.0016 m, σ 2.9 mm, span 0.995–1.009 m (100/100 frames). Part 2: about 600 mm (measurement only approximate) → wall 0.5847 m, σ 1.7 mm, only 56/100 frames; additionally an echo at ~1.60 m (cause open). Part 3: 570 mm (tape measure) → 0.5589 m (−11 mm), σ 1.2 mm, 100/100 frames. The echo at ~1.58–1.60 m afterwards only weak and varying (clear only with the flat cover); cause open. 3 m not possible spatially. Part 4 (2026-10-31, via ESP32 + `i2c_distance_detector`, CFAR, range 300–1000 mm, hyperbolic lens, 570 mm tape measure): median 575 mm (+5 mm), σ 1.3 mm, 50/50 measurements, log `2026-10-31_105545_esp_i2c_wand570.log`. Further distances/metal plate open. |
| G3 | Maximum range person / metal / wall | distance, sparse_iq | m | — |
| G4 | Separate two targets (reduce the distance) | distance | min. distance in cm | — |
| G5 | Smallest measurable motion (micrometer screw/loudspeaker) | phase_tracking | µm | — |
| G6 | Beam width without lens / Fresnel / hyperbolic | sparse_iq | angle, range | Echo strength, brick wall ~570 mm (2026-10-30, profile 2, HWAAS 32, 50 frames each; detector 100 frames each, 100/100 detected): bare 2318 / SNR 33.9 dB / 0.5589 m σ 1.2 mm · flat +0.7 dB / 34.2 dB / 0.5627 m σ 1.6 mm · Fresnel +8.7 dB / 42.9 dB / 0.5643 m σ 0.6 mm · pyramid +10.0 dB / 44.3 dB / 0.5637 m σ 0.7 mm · hyperbolic +11.3 dB / 45.2 dB / 0.5585 m σ 0.4 mm. Echo width (FWHM) 78–85 mm everywhere. Distance offset per cap inconsistent (cause open: position/mechanics). Cap swaps at "about the same" position; repeat without rebuilding (hyperbolic): 8508 → 8495, detector 0.5585 → 0.5581 m — the measurement itself is very stable. Angle not measured yet. Script: `host/a121_zielmessung.py`. |

## People and vital signs

| # | Experiment | Module | Measured quantity | Result |
|---|---|---|---|---|
| M1 | Presence in the room, zones, sitting still vs. moving | presence, smart_presence | hit rate, range | — |
| M2 | Breathing sitting/lying, 0.3–1.5 m | breathing | breaths/min vs. counting | Sitting ~250–350 mm (sparse-IQ phase, 2026-10-30): 20.0–20.5 /min (peak-to-peak 0.46–1.5 mm). Sitting ~675–760 mm via ESP32 I2C (`vital` / `auto`, 37.1–45.5 Hz, 2026-10-31): **20.5–22.5 /min** (excursion **2.42 mm**). Sitting ~673 mm via A121 USB (`host/a121_ultimate_vital.py`, 3-subsweep sparse IQ @ 60.0 Hz, `2026-10-31_a121_usb_3subsweep_ultimate.npz`): **21.5 /min** (peak-to-peak excursion **2.29 mm**, 0 apnea episodes). Sitting ~0.58–0.60 m, ESP32 alone via LPUART1 (2026-11-01): raw data **18.3–18.4 /min** (excursion 3.4 mm), `iq3` median **20.3 /min**. Until then, `iq3` wrongly returned 6.9 /min in about a quarter of the reports (band edge, bug 6 in `docs/HARDWARE.md`). After the fix, live (2026-11-01, chest ≈ 0.68 m, `2026-11-01_iq3_atmung_live1.log`): **17–24 /min**, on the chest all values within ±2 /min of the cycle count in the same radar signal (no independent reference). |
| M3 | Pulse sitting, 40/60/80 cm, with/without lens | phase_tracking, sparse_iq (own analysis) | BPM vs. smartwatch | **Recording A** (`2026-10-30_vital_A_250mm_hyperbolisch_atmen.npz`, ~250 mm, hyperbolic, 60 s): With phase-stability bin selection (bin 55 instead of 57), I/Q circle fit + regularized DACM demodulation, Hilbert projection of the instantaneous breathing harmonics ($m=1\dots 6$) and acceleration HPS ($f_0\cdot 2f_0\cdot 3f_0$) in `host/a121_vitalaufnahme.py`, the stationary candidate **82.5 /min** (HPS **83.0 /min**) becomes the primary peak (before, the 3rd breathing harmonic at 62 /min was stronger). Sliding 15 s windows show: while breathing varies between 18.0 and 22.5 /min ($4 f_b = 72\dots 90\text{ /min}$), the 83 /min candidate stays frequency-stable. **Recording B** (`2026-10-30_201219_live.npz`, ~20 s breathing / ~17.5 s breath hold / ~20 s breathing): The apparent $\pm 500\text{ }\mu\text{m}$ burst at $t=36\text{–}42\text{ s}$ in the naive amplitude maximum (bin 79, 348 mm) was a destructive multipath dip ($\min|z|=48$). On the most phase-stable bin 110 (425 mm, CV 0.38), the pulse band stays steady at 62–75 µm RMS over the whole breath-hold section (28.1–45.6 s); spectrum **76.0 /min**, acceleration HPS **76.5 /min** (side peaks < 4 %), autocorrelation **75.0 /min** ($r=0.75$), 22 individual beats with $0.770 \pm 0.072\text{ s}$ (CV 9.4 % → **77.9 /min**) and harmonics up to $6 f_0 = 7.52\text{ Hz}$ (`host/a121_anhalteanalyse.py`). **Recording C via ESP32 I2C** (`2026-10-31_esp_i2c_brust_hq_45s.npz`, `2026-10-31_esp_i2c_brust_pos2_40s.npz`, `2026-10-31_esp_auto_multiinfo.npz`, `2026-10-31_esp_auto_reconnected.npz`, ~675–762 mm sitting in front of the PC, `vital`/`auto` @ 37.1–45.5 Hz): Gated scatterer tracking separates the front of the chest and a secondary target without target-swap jumps ($\sigma$ reduced to 0.85–1.4 mm); with a sliding 7 s Hann-Hilbert breathing projection, displacement ($\Delta d$: **79.0 /min**) and echo strength ($\text{dB}$: **81.5 /min**) in `2026-10-31_esp_auto_reconnected.npz` at 729 mm give a consistent fusion candidate at **80.5 /min** (3:1 ahead of the second peak 0.33; autocorrelation **81.8 /min**, confidence 72 %). **Recording D with 3-subsweep sparse IQ via A121 USB** (`2026-10-31_a121_usb_3subsweep_ultimate.npz`, 45 s @ 60.00 Hz, `host/a121_ultimate_vital.py`, sub 0 on-chip loopback + sub 1 2.5 mm vital zoom `PROFILE_1`, `PRF_19_5_MHz`, `phase_enhancement=True`, `iq_imbalance_compensation=True` + sub 2 full-room scan `150–2070 mm`): Auto-search localizes the rib cage at 690 mm (`|IQ|=2308`), coherent multi-bin center at **672.5 mm** (`|IQ|=1004`, 60.0 dB), the loopback compensates **14.6 µm** of thermal PLL drift. After a sliding 7 s Hilbert breathing projection, the 3rd and 5th breathing harmonics are suppressed by >90 %; during normal breathing (21.5 /min, 2.29 mm excursion) a dominant heartbeat candidate shows at **84.5 /min** (spectrum/HPS 0.97 vs. second peak 0.14), autocorrelation **81.8 /min** ($r=0.77$) and in the time domain **28 individual beats in 20 s** with IBI median **716.7 ms** (**83.7 /min**), coefficient of variation **CV 9.4 %**, RMSSD **102.0 ms** and pulse-wave excursion **106.6 µm**. **Recording E, ESP32 alone via LPUART1** (2026-11-01, mobile setup, sitting in front of the sensor; data `iq3_person_2026-11-01.log`, `iq3_breathhold_2026-11-01.log`, `person_scan_2026-11-01.npz`, image `img_herzschlag_atemhalten_2026-11-01.png`). **Reference: pulse counted by hand, 38 beats in 30 s = 76 /min (uncertain according to the test subject).** *Distance:* Raw-data scan (0.25–2.5 m) shows the strongest breathing motion at **580 mm** (3.4 mm excursion, 18.4 /min). `iq3` finds the target without a preset, median **601 mm** (10–90 %: 580–610 mm), first rezoom after 3 s. After that, however, the zoom jumps 5–7 times per minute (partly to 0.71–0.80 m), and every rezoom erases the signal history. *Pulse during normal breathing (70 s):* `iq3` median **81.3 /min** (spectrum 81.8, autocorrelation 74.7, time domain 81.1). The individual values scatter 53.8–128.2 /min, though; only 39 % are within ±8 /min of the reference, 15 % at the edge of the search band. Causes: here the heart rate was ≈ 4 × the breathing rate (4 × 18.3 = 73 /min); in addition rezooms and the DSP pauses. In the raw data, the component at 75 /min is 9–13 dB stronger than the 2nd and 3rd breathing harmonics, a hint of the heartbeat. *Pulse with held breath (~10 s):* `iq3` returns **73–81 /min**, the three estimators are close together, confidence 91–99 %. Own analysis of the ESP32 displacement signal (13 s, time axis from the frame counter): spectrum **72.1 /min** (peak/median 29), **15 individual beats**, IBI 683–1000 ms, median 833 ms = **72.0 /min**. Conclusion: The heartbeat is in the signal, and with held breath the ESP32 isolates it. With normal breathing it does not yet separate it reliably. One measurement, one person, reference uncertain, so no accuracy statement. Analysis pitfall: The timestamps in `@IQ3` are ESP32 processing time. After every DSP pause, frames arrive in bursts, so for analyses use the frame counter × 60 Hz (the same applies analogously to `dtS` in `pushFrame()`). **Recording F, ESP32 DSP reworked** (2026-11-01, `firmware/esp32s3/src/a121_esp_dsp.h`). Reference each time counted by hand (30 s each). Data: `iq3raw_normal_120s_2026-11-01.npz` (raw data, count 76/80, development), `iq3raw_motion_120s_2026-11-01.npz` (raw data, count 70/78, breathing ≈ 20 /min, with 10 s of deliberate motion in between), live logs `iq3_live_newdsp_2026-11-01.log` (86/78), `iq3_rezoomtest_2026-11-01.log` (no count), `iq3_live_harm_2026-11-01.log` (74/78). *Method:* The raw data runs on the PC through the same C++ code as on the ESP32 (result bit-identical). The tracked pulse is scored against the counting windows. In addition, three files from the development dataset with artificially inserted motion (phase rotation of the zoom points: fidgeting with a 63 /min component, 2 mm position change, arm movement). *Changes:* (1) all 24 zoom points individually, zero-phase bandpass 0.8–3 Hz, spectra averaged with weights, time base fixed at 60 Hz; (2) peaks at the band edge do not count (the most common false peak was 54 /min = 0.90 Hz, right at the search limit); (3) motion damping per 1 s block, the reference is the quiet quarter of the window; (4) breathing rate from all points (before, often at the band edge, 6 /min), and a jump to a breathing harmonic ($m = 2\dots 6$) is only accepted after 10 instead of 3 reports. *Result (fraction within ±8 /min, ±5 /min in parentheses):* Development dataset: original 32 % (19 %), first version 84 % (81 %), now **100 % (93 %)**. New dataset (the harmonic hold time was developed on it, so not an independent test): original 42 % (24 %), first version 42 % (8 %), now **89 % (50 %)**. Artificial motion: first version 74–84 %, now 91–100 %. **Live on the ESP32, the only independent check** (`iq3_live_harm`, 100 s, one automatic rezoom to 630–690 mm): median **74.6 /min** at count 74 and **78.4 /min** at count 78, all reports in the counting window within ±5 /min, no jumps above 15 /min, computation time 307 ms per report. At 31–39 s the strongest peak jumped to 55–58 /min (3 × breathing), the tracked value stayed at 78. *What the errors were:* In the rezoom test, the pulse got stuck at 60–63 /min. The reasons were motion in the first 8 s (heart band 700–1100 µm instead of ≈ 200 µm) and a tracking that did not let go. In the new dataset, with fast breathing the 3rd breathing harmonic sat at 55–60 /min and at times covered the pulse completely. *Rejected:* Aborting the hold time when the points no longer support the old value (more jumps, rhythmic motion is supported by all points). Switching to the next candidate on a harmonic (no effect). Computing the harmonics out (also removes the pulse when it is close to $m \cdot f_{\text{breath}}$; live it was at 4 × 19.5). *Open:* In the new dataset, all versions show a stable ≈ 77 /min in window A at count 70; whether the count or the measurement deviates is unresolved. During clear motion the value is wrong (up to 100 /min), but the confidence then drops to 0–12 %. If the pulse stays on a harmonic permanently, the tracking only follows after 10 s. Two person datasets and two live runs of one person: This shows an improvement, but not accuracy. `@DSP` now additionally reports the motion fraction (%) and `hr_harm` (peak lies on a breathing harmonic; this does not mean the value is wrong). **Important:** All results are evidence of a rhythm in the radar signal; **without a reference measurement (smartwatch/pulse oximeter/ECG), no statement about pulse accuracy.** |
| M4 | Gestures / swiping / touchless button | hand_motion, touchless_button | detection rate | — |
| M5 | Reference dataset pulse/breathing with ECG chest strap (plan V0.3 in `docs/VITAL_AUDIT.md`, procedure below) | `iq3 0 raw` + Polar H9 | MAE, ±5 fraction, availability, calibration, apnea delay | — (recordings open) |

### M5 reference dataset

Goal: Measure the accuracy of the pulse (against the ECG chest strap) and of breathing (against a metronome) and check every DSP
change against it. Without this dataset there is no statement about pulse accuracy (`AGENTS.md`).

**Setup:** ESP32-S3 + A121 as in T1, the phone app records `iq3 0 raw` (raw data, so that every header version can be
recomputed). Pair the Polar H9 chest strap (BLE Heart Rate Service) via the app (reference card in the
vital tab or `adb shell am broadcast … ref_scan` / `ref_connect --es addr <mac>`, `docs/ANDROID_APP.md`); the app
writes `ref_hr.csv` (rx_ms, pulse, RR intervals) and `events.csv` (markers) into the same recording folder.
Moisten the strap before starting, wear it for 2 min until the Polar app shows a stable value.

**Reference without RR (from 2026-11-02):** If a strap or watch sends only pulse values (`ref_hr.csv` with empty `rr_ms`),
`vital_eval.py` takes the mean of the pulse values in the 15 s window (report: "nur Pulswerte ohne RR", i.e. pulse values only, without RR). Then
pulse MAE, ±5 fraction and availability are measurable, not IBI/RMSSD (V1.6/V3.6). An Apple Watch without an iPhone is not usable
(no BLE pulse service, no export); a Polar H9 has been ordered as a strap (RR intervals over BLE).

**Storage:** `~/Data/radar-sensor/ref/<date>_<tag>_<session>_<person>/` (e.g. `2026-11-08_A_R1_p1`).
Persons only by pseudonym (`p1`, `p2`), further persons only with consent.

**Separation:** Day A = development (may be evaluated as often as you like), day B = test (evaluate only for the acceptance of a phase).
Both days with the same sessions, recorded on different days.

| Session | Content | Duration | Marker | checks |
|---|---|---|---|---|
| R1 | sitting still, breathing normally, chest 0.6 m in front of the sensor | 5 min | — | basic accuracy |
| R2 | 3 × 20 s breath hold, 40 s normal in between | 3 min | "Atem anhalten" (breath hold) start/end | apnea detection, pulse without breathing |
| R3 | metronome 6 / 10 / 15 / 20 /min, inhale with every tone | 2 min each | metronome (sets "ein" (in) per tone) | breathing, harmonic collision |
| R4 | 1 min squats (without sensor), then recover sitting right away | 6 min | "Zählfenster" (counting window) start when sitting down | pulse range 110 → 70, delay |
| R5 | 20 s each: raise arm, lean back, cough, talk, quiet in between | 3 min | "Bewegung" (motion) start/end | motion, state, confidence |
| R6 | distance 0.4 / 0.6 / 0.8 / 1.0 m (tape measure) | 3 min each | — | range |
| R7 | without lens / hyperbolic lens, 0.6 m | 3 min each | — | lens |
| R8 | lying, sensor ≈ 0.5 m above the chest | 5 min | — | posture |

Procedure per session: strap connected (pulse visible in the app) → start recording → `iq3 0 raw` → session → stop.
Then check and evaluate:

```bash
.venv/bin/python host/esp_record.py --check ~/Data/radar-sensor/ref/<ordner>
```

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag A>_* --md ~/Data/radar-sensor/ref/bericht_A.md
```

One header variant against the same data (before flashing):

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag A>_* --replay <ordner mit a121_esp_dsp.h>
```

**Done when:** both days recorded, `esp_record.py --check` without gaps, `vital_eval.py` produces a report for every
session.

**Baseline (V0.6):** DSP state of the firmware `2026-11-01-app1` on day A and day B; enter a table per session with pulse MAE,
±5 fraction, availability, calibration (confidence ≥ 70 % → ±5 fraction) here. Because the recordings contain `@RAW`,
this also works with newer firmware: let the old header recompute.

```bash
mkdir -p /tmp/dsp_basis && git show 26c9bff:firmware/esp32s3/src/a121_esp_dsp.h > /tmp/dsp_basis/a121_esp_dsp.h
```

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag>_* --replay /tmp/dsp_basis
```

**Rezoom continuity (V2.3 step 1, open):** Does the phase at the same distances stay continuous across the session restart
at a rezoom (raw and with loopback derotation)? Check on T1 (31.7 min, 38 rezooms, `@RAW`); enter the result here.
Only if the jumps are small (or a common rotation, small scatter) is step 4 worthwhile
(adopt overlapping zoom points, fill the gap).

```bash
.venv/bin/python host/rezoom_check.py ~/Data/radar-sensor/<T1-ordner>
```

**Window length (V2.5, open):** Compare 10 / 12 / 15 s on day A (R4: delay until the value has followed 50 % of a pulse drop;
target ≤ 7 s); choose the shortest window without an MAE loss > 0.5 /min. Variant = header
with `#define A121_HR_WIN 600` (or 720) prepended, in its own folder:

```bash
mkdir -p /tmp/dsp_win10 && { echo '#define A121_HR_WIN 600'; cat firmware/esp32s3/src/a121_esp_dsp.h; } > /tmp/dsp_win10/a121_esp_dsp.h
```

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag A>_R4_* --replay /tmp/dsp_win10
```

Test bench in advance (simulation only, `vital_sim.py bench --hr-win 600|720|900`): response in scenario `step` 7 / 8 / 9 s;
with 10 s, `beweg` (MAE 1.5), `anhalt` (apnea not detected, ±5 fraction 96 %) and `leer` (1 valid report)
violate the limits, with 12 s only `beweg` narrowly (MAE 1.4 instead of ≤ 1.3), 15 s keeps all of them. The default stays 15 s until day A.
With Viterbi tracking (V3.2, `--define A121_DSP_VITERBI`), even 12 s keeps all limits (response 8 s), 10 s
still does not (`beweg`, `anhalt`, `leer`). For day A: also compute 12 s + Viterbi as a separate variant.

**Viterbi tracking (V3.2, open):** Variant behind `#define A121_DSP_VITERBI` (default off): Viterbi over the
last 20 reports, states 40–180 /min, transition Gaussian σ = 3 /min per s plus 2 % "jump anywhere", emission from
the candidates of the report (Gaussian per candidate, weight (value / best)³, floor 10 %), confidence at most the
probability mass ±5 /min. Acceptance on day A (all sessions): fewer jumps > 15 /min between
consecutive valid reports (`vital_eval.py` writes them into the header line of each session), ±5 /min fraction not
worse. Test bench in advance (simulation only): jumps equal (3 / 3), `brady` MAE 2.8 → 1.5 /min (±5: 95 → 97 %),
`beweg` 0.8 → 0.6, otherwise equal; wrong reports (> 10 /min) 9 → 5; computation time on the PC +0.2 ms.

```bash
mkdir -p /tmp/dsp_vit && { echo '#define A121_DSP_VITERBI 1'; cat firmware/esp32s3/src/a121_esp_dsp.h; } > /tmp/dsp_vit/a121_esp_dsp.h
```

```bash
.venv/bin/python host/vital_eval.py ~/Data/radar-sensor/ref/<tag A>_* --replay /tmp/dsp_vit
```

**Guided removal of breathing harmonics (V3.3, open):** Variant behind `#define A121_DSP_HARM_REMOVE` (default off): for each
zoom point before the pulse FFT (15 Hz), subtract the harmonics m = 1..8 of breathing, phase = angle of the analytic
signal of the breathing fundamental in the point's displacement (FFT, band 0.2–1.8 × breathing rate of the previous report). m is kept if
the pulse or 2 × pulse lies within m × [smallest, largest breathing rate in the window] ± 0.1 Hz; only after a valid report with
confidence ≥ 50 % (otherwise the rule would spare a wrong pulse and delete the real one). Acceptance on day A: R3 (metronome)
better, R1 not worse. Calculation in advance (simulation only): with ±23 % rate fluctuation, 12 % of the harmonics remain
(fixed frequency: 100 %), with even breathing ≈ 10 % (fixed frequency 24 %); it takes < 13 % from the pulse. Test bench:
pulse metrics as without the variant (harmonics are weak there), confidence median of correct reports 82 → 100 %,
of wrong ones unchanged at 31 %; computation time on the PC +0.7 ms. As a variant: prepend `#define A121_DSP_HARM_REMOVE 1` as
above.

**Combine zoom points (V3.1, open):** Variant behind `#define A121_DSP_COMBINE` (default off): shift the 12 points with
the strongest motion around their circle centers (points without a clean echo stay out), 1st
principal component of their IQ values (aligns the points in phase and weights them by amplitude, with equal
noise per point = maximum ratio combining), then demodulate **one** channel instead of 24 individual spectra. `@DSP` then carries
the effective number of points N_eff = (Σλ)² / Σλ² of the covariance as the 15th field (≈ 1: all points see the same echo, with
a person or a static echo; ≈ 12: noise only); `vital_eval.py` reports its median per session. Acceptance on day A:
pulse MAE ≤ without the variant at ≤ 50 % computation time (ESP32 live, `compute_us` in `@DSP`). Test bench in advance (simulation only):
computation time on the PC 1.15 → 0.48 ms (42 %); pulse MAE `base` 0.36 → 0.29, `brady` 2.81 → 1.51, `tief` 0.60 → 0.34, `step`
0.74 → 0.77, `anhalt` 0.41 → 0.51 (4 more valid reports during the breath hold, error 2–4 /min), otherwise equal; confidence median of
correct reports 82 → 94 %, of wrong ones 31 → 25 %. First value in `step` and `tief` after 7 instead of 6 s: The combined
channel is breathing-heavy (the principal component maximizes power, and that is carried by breathing); in the 6 s window the
strongest peak there lies on a low breathing harmonic. Since V3.1, a provisional value (V2.4) is never accepted on a
breathing harmonic m ≤ 3, even without the switch (no effect on the test bench there): ACF and individual beats of the same signal
confirmed them too (`base` with the variant: 46 instead of 71 /min valid). Room points as a second channel are not implemented
(the DSP keeps no history for them). As a variant: prepend `#define A121_DSP_COMBINE 1` as above.

**Individual beats with a template (V3.6, open):** Variant behind `#define A121_DSP_IBI_TEMPLATE` (default off): heart
signal of the clearest point in the band 0.6–8 Hz (instead of 0.6–5 Hz), breathing harmonics subtracted as before; template = mean of the
sections ±0.3 × beat interval around the first peaks (without DC component), correlation filter with the template, its
peaks refined parabolically, then the same artifact rules as V1.6. Acceptance on day A (all sessions with a strap):
IBI error against strap RR halved compared to V1.6. Tool: `vital_eval.py` writes "Einzelschläge gegen
Gurt-RR" (individual beats against strap RR) for each session (for each valid report, IBI median and RMSSD against the strap RR in the same 15 s window; reports whose IBI is only the
fallback value 60,000 / pulse do not count); with `--replay` and the switch as above, compare against the default.
Calculation in advance (simulation only, error per beat against the true beats, 6 windows): in white noise a median factor of
0.81 (V1.6 is already close to the Cramér-Rao bound there, ≈ 7 ms per beat at σ = 0.15 × peak-to-peak; no method can do
better), with breathing residues in the heart band 0.58 (0.44–0.82) — so it does not halve everywhere. Test bench: fraction of
reports with RMSSD (≥ 90 % valid beats), e.g. `base` 17 → 64 %, `tachy` 3 → 75 %, `tief` 20 → 85 %; RMSSD
measured / true closer to 1 (`tachy` 1.30 → 0.98); pulse MAE `brady` 2.8 → 3.2, `tief` 0.6 → 0.3. `tief` rejects
the provisional 6 s value 40.3 /min (true 71), which the default accepts: Its IBI confirmation uses
the candidate's lockout time (0.6 × 60 / 40 s), skips beats and so confirms the wrong value. The confirmation
stays as it is (a lockout time independent of the candidate, or only the ACF, lowered the fraction of correct
provisional values in the test bench 83 → 70 % or shifted the first value in four scenarios); the test bench honestly counts
`erst_ok_s` (first value ≤ 5 /min off) and `frueh_falsch_n` (values > 10 /min off in the first 15 s) for this, with the
current state as the limit: `tief` 7 s / 1 wrong value (confidence 28 %, so gray), `brady` 14 s / 6 (double pulse 8–13 s,
confidence 18–43 %), otherwise 0.

**Breathing breath by breath (V3.4, open):** Acceptance on R3 (day A): in all tempos 6 / 10 / 15 / 20 /min, the rate
of every `@BREATH` line (60 / median of the last 4 intervals) within ±1 /min of the metronome, first value after the start
after 2 breaths. For this, `vital_eval.py` writes "Atmung Atemzug für Atemzug" (breathing breath by breath) for each session (against the markers "ein" (in),
breaths in breath-hold sections counted too; R2 checks that none occur during the hold). Test bench in advance (simulation
only): MAE 0.0–0.15 /min, all within ±1 /min, first value after 2.0 breaths (also `takt6`, 6 /min, where
spectral breathing with a band from 7 /min is off by 8 /min); exceptions: `flach` (0.6 mm) only after 4 breaths (the
telemetry beam centers with a sliding mean and only settles after ≈ 9 s with a small arc),
`anhalt` 2 breaths directly after the hold 17–19 instead of 16 /min; no line without a person (gate: only in ERFASSUNG,
GÜLTIG, APNOE).

— open (needs strap and recordings)

| Session | Day | Pulse MAE | ±5 /min | Availability | Confidence ≥ 70 %: ±5 | Breathing MAE | Apnea detected / delay |
|---|---|---|---|---|---|---|---|
| — | — | — | — | — | — | — | — |

### V4 Sensor configuration (measurement plan, open)

Plan V4.1–V4.4 in `docs/VITAL_AUDIT.md` (experiments G1, G5, M3). Data goes to `~/Data/radar-sensor/v4/`,
not into the repo. Before every recording, identify the ESP32 unambiguously (`ls -l /dev/serial/by-id`, exactly one
`usb-Espressif…-if00`); the tools abort if it is not exactly one. Operate the sensor only in its permitted
settings (profile, HWAAS, PRF, gain); exptool checks every configuration before setup.

**V4.1 Displacement noise, static.** Metal plate 0.6 m in front of the sensor (tape measure), room empty, nothing moves; ESP32
and sensor switched on for 5 min (temperature). `sess run` delivers only amplitudes, no phase; therefore
`host/noise_floor.py record` records with exptool via the ESP32 bridge `bridge` (loopback + 24 zoom points from
`--start-mm`, 60 s each; without arguments like the `iq3` preset: zoom profile 1, HWAAS 32, gain 16, 15.6 MHz; loopback
profile 1, HWAAS 8, gain 16, 19.5 MHz). First determine the position of the echo with `iq3 10` or `sess run 5 peak` and
choose `--start-mm` so that it lies in the middle of the 60 mm (plate at 600 mm → `--start-mm 570`).

| Block | Recordings | Values |
|---|---|---|
| Zoom | 24 | `--profile` 1 / 2 / 3 × `--hwaas` 16 / 32 / 64 / 128 × `--rate` 30 / 60 |
| Loopback | 5 | at zoom profile 1, HWAAS 32, 60 Hz: `--loop-profile 4 --loop-prf 15.6`, `--loop-profile 5 --loop-prf 15.6`, `--loop-prf 15.6`, `--loop-gain 10`, and the base setting once more at the end (drift) |
| Firmware path | 1 | `iq3 60 570 raw` with `host/esp_record.py` (same setting as zoom 1/32/60, but through the firmware) |
| Restart | — | phase before/after session restart: `host/rezoom_check.py` (V2.3 step 1) |

Exptool rules (checked offline, `host/tests/test_noise_floor.py`): 19.5 MHz only with profile 1; loopback not with
profile 2; the PRF of a subsweep must not be smaller than that of the following one — so loopback with 15.6 MHz works only
because the zoom has 15.6 MHz. Whether the sensor manages 60 Hz at HWAAS 128 only the measurement shows: `record` counts frames
with flags (saturated, late, calibration); a setting with flags counts as not reachable.

```bash
sg dialout -c ".venv/bin/python host/noise_floor.py record --out ~/Data/radar-sensor/v4/v41/p1_h32_60.npz --start-mm 570 --profile 1 --hwaas 32 --rate 60"
```

```bash
.venv/bin/python host/noise_floor.py eval ~/Data/radar-sensor/v4/v41/*.npz ~/Data/radar-sensor/v4/v41/iq3_* > ~/Data/radar-sensor/v4/v41/bericht.txt
```

Evaluation per recording: displacement RMS in the band 0.6–5 Hz (like the DSP's pulse band), median of the strong zoom points and best
point, each for derotation `aus` (off), `roh` (raw, like the firmware) and `tiefpass` (low-pass; loopback phase smoothed at 0.8 Hz, as
formerly on the PC). Decision: noise floor per setting (smallest median without flags); derotation `roh` stays if
it lowers the median against `aus`; `tiefpass` goes into the firmware if it is at least 10 % below `roh`;
loopback setting like the best of the five.

**V4.2 Person with strap.** Compile the 3 best settings from V4.1 at 60 Hz (the DSP only computes at 60 Hz) each as a
firmware variant and record R1 (5 min, M5) with each. Zoom profile/HWAAS and loopback profile/gain/PRF are
compile switches in `firmware/esp32s3/src/a121_server_client.h` (`A121_IQ3_ZOOM_PROFILE`, `A121_IQ3_ZOOM_HWAAS`,
`A121_IQ3_LOOP_PROFILE`, `A121_IQ3_LOOP_GAIN`, `A121_IQ3_LOOP_PRF`; default 1 / 32 / 1 / 16 / 19.5 MHz), e.g. in
`platformio.ini` as `build_flags = -DA121_IQ3_ZOOM_HWAAS=64`. `@HELLO` reports the setting as `iq3=p1h64/lp1g16/19_5_MHz`
(it is in `meta.json` of the recording). Flash only after unambiguous port identification. Evaluation with `vital_eval.py`
as in M5; decision: pulse MAE and availability in R1, in a tie the smaller noise floor. The result becomes the
new default preset. 30 Hz only if V4.1 shows clearly less noise there — for that, the DSP (filter,
window) would have to be redesigned for 30 Hz (separate plan).

**V4.3 Lens and distance.** Like R6/R7 in M5 (without / hyperbolic lens × 0.4 / 0.6 / 0.8 / 1.0 m, 3 min each with strap),
with the preset from V4.2. Decision: recommended setup = smallest pulse MAE at availability ≥ 80 %; range =
largest distance with pulse MAE ≤ 5 /min.

**V4.4 Zoom width.** Firmware with `A121_ZOOM_POINTS=48` against 24 (default), R1 and R5 each on day A; evaluate with
`vital_eval.py` (for recomputation with 48 points, a header copy with `#define A121_ZOOM_POINTS 48`). Decision
for V2.3 step 3: 48 only if R5 (motion) has more valid values at the same pulse MAE in R1.

— open (needs metal plate, strap, recordings; tools checked with simulation only)

| Measurement | Result |
|---|---|
| V4.1 Noise floor | — |
| V4.2 Preset | — |
| V4.3 Setup / range | — |
| V4.4 Zoom width | — |

## Materials and environment

| # | Experiment | Module | Measured quantity | Result |
|---|---|---|---|---|
| U1 | Attenuation: cardboard, plywood, plasterboard, plastic, glass, fabric | sparse_iq | amplitude before/behind material (dB) | — |
| U2 | Metal object / person behind plasterboard or wooden door | distance, presence | detectable yes/no, distance | — |
| U3 | Vibration frequency: loudspeaker, fan, washing machine | vibration | Hz vs. target value | — |
| U4 | Speed: fan blade, rolling ball, pendulum | speed | m/s | — |
| U5 | Water flow speed (tap, garden hose) | surface_velocity | m/s plausible? | — |
| U6 | Fill level: bucket/glass of water, trash can | tank_level, waste_level | mm vs. ruler | — |

## Transfer to the phone (Android app)

Setup: ESP32-S3 (`2026-11-01-app1`) connected C-to-C to the Galaxy S26 Ultra, A121 via the red 5V wire, app via USB CDC.
Details and tools: `docs/ANDROID_APP.md`. Recordings under `~/Data/radar-sensor/phone/`.

| # | Experiment | Measured quantity | Result |
|---|---|---|---|
| T1 | Endurance run `iq3 0 raw` over USB, recording in the background, screen off | frame loss, rate, pauses | 2026-11-01, 31.7 min: 113,289 frames, 59.64 Hz by tick, **0 gaps**, 0 reconnects, ≈ 41 kB/s. Largest tick interval 440 ms (all 38 intervals > 100 ms at `@REZOOM`), largest receive pause 337 ms (DSP report ESP32 Ø 270 ms, max. 323 ms). Battery 83 → 80 % in 39 min (read off) |
| T2 | Unplug/plug in the cable during `iq3` | restart | 2026-11-01, 2×: ESP32 restarts (phone power), app reconnects by itself and restarts `iq3`; 0 gaps within the 3 streams. Pause until the new stream 34.9 s (with permission dialog) or 8.2 s (without; incl. time without cable) |
| T3 | Recompute the ESP32 DSP on the phone (NDK, raw data from T1) | equality phone / PC / ESP32 | 2026-11-01: phone = PC byte-identical; against ESP32 51,715 / 128,197 lines equal, the rest mostly last digit, displacement ≤ 0.21 µm, breath depth ≤ 1.6 µm, IBI in 1 / 1632 `@VITAL` by 16.7 ms. Computation time on the phone 28.4 s for 31.7 min |

## V6 Reacquisition (baseline)

Measurement tool: `host/reacq_eval.py` (plan `2026-11-02-pulse-reacquisition`, task 3). An outage is a sequence of @DSP reports
with `ok == 0` after a valid report; duration = first invalid report until the next valid one. Cause `bewegung` (motion)
(in the outage `motion_pct > 30` or state BEWEGUNG; with `A121_DSP_REACQ` additionally `win_s == 0` from 6 s after start/rezoom, ruling
R10; `--ohne-karte` (without map) counts by the base rule without `win_s`, ruling R16), otherwise `keine_atemspitze` (no breathing peak) (`br_state == 1` in the majority), otherwise `rest`.
Runs with more than 50 % state KEINE_PERSON do not count (default); `--keine-person-zaehlen` (count no-person) turns that off. Valid fraction without
state 0. All numbers: firmware state of this branch (a121_esp_dsp.h without `A121_DSP_REACQ`), `stream.log` recomputed
(`--replay firmware/esp32s3/src`), time = receive time, reference missing (no statement about accuracy, only about availability).

```
.venv/bin/python host/reacq_eval.py <T1> <2026-11-02_livetest_vital1> <2026-11-02_vital1_iq3raw_60s> --replay firmware/esp32s3/src
```

| Recording | valid (without state 0) | outages (all / motion) | motion: median / sum | keine_atemspitze: n, median / sum | rest: n, median / sum | jumps > 15 /min |
|---|---|---|---|---|---|---|
| T1 `2026-11-01_164043_usb_iq3raw_bildschirmaus` (31.7 min) | 20.7 % | 30 / 15 | 23.7 s / 590.9 s | 14, 2.0 s / 60.1 s | 1, 9.2 s / 9.2 s | 13 |
| `2026-11-02_171254_livetest_vital1` | 89.5 % | 0 / 0 | – | – | – | 0 |
| `2026-11-02_vital1_iq3raw_60s` (60 s) | 32.1 % | 4 / 1 | 17.2 s / 17.2 s | 1, 3.0 s / 3.0 s | 2, 7.2 s / 14.4 s | 1 |

Without the person filter (`--keine-person-zaehlen`), T1 has 33 outages, of them **18 motion outages, median 39.0 s, sum 1609 s**
(spec figure of the manual analysis: 18, ≈ 40 s, ≈ 1627 s; the 18 s difference probably fits 1 s per outage, if the manual analysis counted the duration from the
last valid instead of from the first invalid report; not verified). The person filter removes three motion outages (state KEINE_PERSON in the
majority: one of 829 s and two shorter ones, 1018 s together); 15 remain. The
recomputed DSP considers only 11 % of the T1 reports valid (211 of 1899); the recording firmware of 2026-11-01 had 1632 valid
(`ok` in the original log), which is why the baseline is explicitly that of the current header state. There is no pulse reference in
any of these recordings. Computation on the PC only (replay), no run on the device.

### V6 variant periodicity (`A121_DSP_REACQ_PERIOD`): rejected

Idea (Vital-Radio, Adib et al. 2015): A motion-map block that is too strong only counts as moving if the signal around it
(3 s excerpt at the strongest point, 15 Hz, Hann, heart band 0.674–3.003 Hz) is not periodic; the target was `tippen`
(small irregular bumps). Measurement: `.venv/bin/python host/vital_sim.py bench --check --define A121_DSP_REACQ [--define A121_DSP_REACQ_PERIOD]`,
simulation only (PC computation, no run on the device). With the switch but without the period check (`kPeriodK` = 100), the output is line-identical to
`A121_DSP_REACQ` alone.

Threshold: With all remaining bins in the band as the reference, a pure sine in the 3 s excerpt is only peak/mean ≈ 4.7 (Hann main lobe
1.3 Hz of a 2.3 Hz band), noise up to 7: `kPeriodK` = 5 then separates nothing. What is implemented is therefore "remaining bins = outside the
main lobe of the peak (± 2 resolution cells)". Then the sine lies at 420–1080, white noise on average at 5 (500 seeds: up to 39,
about a third above 5, because 3 s has only ≈ 9 independent bins in the band), irregular bumps on average at 6 (up to 167).

| Scenario | valid REACQ → +PERIOD | wiederfind_s | falsch_nach_beweg_n | nie_gefunden_n | Limits |
|---|---|---|---|---|---|
| beweg | 100 → 105 /120 | 6 → 11 | 0 → 9 | 0 → 0 | newly violated: hr_p5 91.4 (≥ 95), hr_mae 2.2 (≤ 1.3) |
| kurz | 102 → 115 /120 | 8 → 1 | 0 → 7 | 0 → 0 | newly violated: falsch_nach_beweg_n 7 (≤ 0 and ≤ 2) |
| zappel | 81 → 104 /120 | 7 → 1 | 0 → 7 | 0 → 0 | newly violated: falsch_nach_beweg_n 7 (≤ 0) |
| lang | 125 → 142 /150 | 6 → 6 | 2 → 1 | 0 → 0 | still violated (≤ 0) |
| lage | 104 → 111 /120 | 2 → 1 | 0 → 0 | 0 → 0 | holds |
| beweg_hr | 124 → 142 /150 | 7 → 1 | 0 → 0 | 0 → 0 | holds (pulse MAE 0.4 → 0.9) |
| beweg_weg | 46 → 55 /120 | 6 → 1 | 0 → 2 | 0 → 0 | newly violated: falsch_nach_beweg_n 2 (≤ 0) |
| tippen | 137 → 144 /150 | 5 → 5 | 4 → 4 | 0 → 0 | section valid 88 → 100 %, section MAE 4.4 → 4.0 (limit ≤ 2.0 still violated) |

The remaining scenarios (base, anhalt, …) show no new violation. Result: With `kPeriodK` = 5, `beweg`, `kurz`, `zappel` and
`beweg_weg` break their limits, because the simulated motion is a sine, hence periodic, and therefore no longer
counts as motion; `tippen` keeps its 4 wrong values (only the valid fraction and the MAE in the section are slightly better). For orientation
(not adopted, threshold fixed at 5 according to the design): `kPeriodK` = 20 leaves `kurz` at 2 wrong values (limit ≤ 0), `lang` at 1 and `tippen`
unchanged; `kPeriodK` = 100 is line-identical to REACQ alone. **Rejected**: Limits of `zappel` (and `beweg`, `kurz`,
`beweg_weg`) not kept, `tippen` not fixed (in `tippen` the state BEWEGUNG never occurs; the wrong values
come from the normal tracking, according to task 6). The code stays behind the switch as an experiment; there is no bench test for it. Computation time: one FFT (1024) of the existing `fft_` per block counted as moving, at most 15 per report.

### V6 variant phase slope (`A121_DSP_PHASE_SLOPE`): rejected

Idea (Vital-Radio, Adib et al. 2015): The frequency of the selected candidate (`cf[0]`) does not come from the parabola around the
spectrum peak, but from the slope of the unwrapped phase of the narrow band: heart signal of the clearest point
(`best_h_`, or the combined channel) over the pulse window, 15 Hz, Hann, zero-padded to 1024, complex FFT, keep only the peak and its
two neighboring bins, inverse FFT, unwrap the phase of the first `m` values, straight line (least squares) → Hz
(`phaseSlopeHz`, own `ComplexFft<1024>`). Candidate selection, octave check and the other candidates remain. The peak is searched uphill from the
start bin (at most 4 bins), because the peak of the single-point signal can lie next to that of the combined spectrum
(the unit test starts at 1.2 Hz with a true 1.23 Hz). Measurement: `.venv/bin/python host/vital_sim.py bench --check
[--hr-win 600] [--define A121_DSP_REACQ] --define A121_DSP_PHASE_SLOPE`, simulation only (PC computation, no run on the device). Without the switch,
the output is line-identical to the state before, with `A121_DSP_REACQ` only likewise.

Result per scenario (pulse MAE in /min / fraction ≤ 5 /min in %; PS = phase slope; 10 s = `--hr-win 600`):

| Scenario | without | PS | REACQ | REACQ+PS | 10 s without | 10 s PS |
|---|---|---|---|---|---|---|
| base | 0.4 / 99 | 0.3 / 100 | 0.3 / 100 | 0.3 / 100 | 0.4 / 99 | 0.6 / 100 |
| harm4 | 0.3 / 100 | 0.4 / 100 | 0.3 / 100 | 0.4 / 100 | 0.4 / 100 | 0.5 / 100 |
| step | 0.7 / 100 | 0.8 / 100 | 0.7 / 100 | 0.8 / 100 | 0.5 / 100 | 0.8 / 100 |
| brady | 2.8 / 95 | 3.1 / 95 | 2.8 / 95 | 3.1 / 95 | 2.0 / 96 | 2.8 / 96 |
| tachy | 0.9 / 100 | 0.9 / 100 | 0.9 / 100 | 0.9 / 100 | 0.9 / 100 | 0.9 / 100 |
| flach | 0.5 / 100 | 0.5 / 100 | 0.5 / 100 | 0.5 / 100 | 0.6 / 100 | 0.6 / 100 |
| tief | 0.6 / 99 | 0.6 / 99 | 0.6 / 99 | 0.6 / 99 | 0.6 / 99 | 0.7 / 99 |
| beweg | 0.8 / 98 | 1.0 / 98 | 0.4 / 100 | 0.7 / 99 | 1.5 / 95 | 1.8 / 94 |
| anhalt | 0.4 / 100 | 0.5 / 99 | 0.7 / 98 | 0.9 / 97 | 1.1 / 96 | 1.3 / 95 |
| tachy6 | 1.0 / 100 | 1.0 / 100 | 1.0 / 100 | 1.0 / 100 | 1.2 / 100 | 1.2 / 100 |
| takt6 | 0.3 / 100 | 0.3 / 100 | 0.3 / 100 | 0.3 / 100 | 0.3 / 100 | 0.3 / 100 |
| weg | 0.4 / 100 | 0.5 / 100 | 0.4 / 100 | 0.5 / 100 | 0.5 / 100 | 0.7 / 100 |
| kurz | 0.9 / 98 | 1.0 / 97 | 0.4 / 100 | 0.5 / 99 | 1.2 / 97 | 1.8 / 94 |
| zappel | 3.9 / 89 | 4.0 / 88 | 3.1 / 96 | 3.3 / 95 | 5.2 / 93 | 6.0 / 91 |
| lang | 1.5 / 90 | 1.5 / 90 | 0.8 / 97 | 0.9 / 97 | 2.4 / 89 | 2.4 / 90 |
| lage | 0.3 / 100 | 0.4 / 100 | 0.3 / 100 | 0.3 / 100 | 0.4 / 100 | 0.6 / 100 |
| beweg_hr | 0.4 / 100 | 0.5 / 100 | 0.4 / 100 | 0.5 / 100 | 0.7 / 99 | 0.9 / 100 |
| beweg_weg | 0.8 / 96 | 1.2 / 94 | 0.3 / 100 | 0.6 / 100 | 0.7 / 98 | 1.1 / 98 |
| tippen | 2.6 / 92 | 3.1 / 90 | 2.7 / 91 | 3.3 / 88 | 2.0 / 92 | 1.9 / 90 |

(`leer`, `niemand` without a pulse value; sum of the MAE over the 19 scenarios: 19.5 → 21.6 alone, 16.9 → 19.4 with REACQ.)

| Comparison | Violations before → after | new |
|---|---|---|
| PS alone against without switch (15 s) | 0 → 7 | `erst_ok_s` 7 s (limit ≤ 6) in `anhalt`, `weg`, `kurz`; `weg` `erst_s` 7 s; `zappel` `erst_ok_s` 11 s (≤ 7); `lang` `falsch_nach_beweg_n` 5 (≤ 3); `tippen` `falsch_nach_beweg_n` 6 (≤ 4) |
| REACQ+PS against REACQ alone (15 s) | 3 → 9 | as above (`erst_ok_s` in `anhalt`, `weg`, `kurz`, `zappel`, `weg` `erst_s`); `tippen` wrong 4 → 6, `abschnitt_mae` 4.4 → 4.9 |
| PS against without at 10 s | 9 → 15 (limits apply to 15 s) | `beweg` `hr_p5` 93.6 (≥ 95), `anhalt` `hr_p5` 94.9, `erst_ok_s` in `anhalt`/`weg`/`kurz`/`zappel` 7–16 s, `kurz` wrong 5 (≤ 2), `zappel` `frueh_falsch_n` 8 (≤ 3), `alle` `konf_richtig` 66 (≥ 70) |

Cause: On the 1024 grid (0.0146 Hz), the three kept bins lie on the flat top of the Hann main lobe (±0.33 Hz at 6 s).
The narrow band is `e^{j·2π·k0·n/1024}` times an almost real, slowly varying envelope; the slope is therefore essentially
the frequency of the center bin, and only a small correction comes from the difference of the neighboring bins (reasoning, not measured separately). The estimate snaps to the grid: On
a clean sine (unit test, 6 / 10 / 15 s) the error is up to 0.007 Hz (0.4 /min, half a bin), independent of the
window length; the parabola over the power reaches 0.0001 Hz on the same signal. The hoped-for finer frequency in the short window
does not materialize; another reason may be that the signal comes from the clearest point while the candidates come from the combined spectrum of all points (not examined separately).
For orientation (not adopted, measured at `kPhaseSlopeWalk` = 0, i.e. without peak search from the start bin): PS alone
0 → 3 violations (`brady` `erst_ok_s` 15 s / `frueh_falsch_n` 7, `tippen` wrong 5), sum MAE 19.5 → 20.2; in the 10 s window 9 → 9, sum MAE
49.0 → 50.0; there too it is no better than the parabola.

**Rejected**: The test bench gets worse in both window lengths (pulse MAE higher in the 15 s window in 12 of 19 scenarios, lower in one; in the 10 s window in 13 of 19, lower in one; new violations), the adoption rule (MAE and all limits not worse)
is not met. The verdict applies only to the 3-bin variant of the design: k0 ± 1 of a 1024 FFT snaps to the center bin on the flat top of the
Hann main lobe; a band of about ± 2 · 1024 / m bins (m = window length in samples) was not tried against the parabola. The code stays behind the switch as an experiment; there is no bench test for it (`PhaseSlopeTest` in `host/tests/test_dsp_unit.py` only checks the function). Cost with
the switch: 14 KB RAM (`ComplexFft<1024>` 6 KB, two work arrays of 4 KB each), two complex FFTs (1024) and at most 225 phases per report
(estimated under 2 ms on the ESP32-S3, not measured); without the switch none of that. All numbers are simulation; statements about accuracy
on humans are only possible with a pulse reference.

### V6 overall check: switch `A121_DSP_REACQ` (2026-11-03)

State of the branch `feature/v6-wiederfinden`; simulation and recomputation on the PC only, **no reference, no run on the device**. Statements about
the accuracy of the pulse are therefore not possible, only about availability and behavior on the test bench. Only
`A121_DSP_REACQ` is adopted (motion map, pulse on the quiet end segment, memory); `A121_DSP_REACQ_PERIOD` and `A121_DSP_PHASE_SLOPE` are
rejected (sections above) and stay off.

**Test bench** (`.venv/bin/python host/vital_sim.py bench --check [--define A121_DSP_REACQ]`, 15 s window, scenarios with motion;
`wiederfind_s` = end of motion until the first valid value within ± 5 /min of the truth, median; `falsch danach` (wrong afterwards) = valid reports > 10 /min
off in the 30 s after; `falsch in` (wrong during) = the same in (start, end] of the motion (`falsch_in_beweg_n`, ruling R15, with the switch limit 0, without
only reported); `nie_gefunden_n` is 0 everywhere; state after the final review fix, simulation):

| Scenario | valid without → with | pulse MAE without → with | wiederfind_s without → with | wrong afterwards without → with | wrong during without → with | Limits with switch |
|---|---|---|---|---|---|---|
| beweg | 101 → 100 /120 | 0.8 → 0.4 | 11 → 6 | 2 → 0 | 0 → 0 | holds |
| kurz | 104 → 102 /120 | 0.9 → 0.4 | 14 → 8 | 2 → 0 | 0 → 0 | holds (limit ≤ 8) |
| zappel | 114 → 81 /120 | 3.9 → 3.1 | 1 → 7 | 10 → 0 | 0 → 0 | holds (limit ≤ 8) |
| lang | 143 → 125 /150 | 1.5 → 0.8 | 11 → 6 | 3 → 2 | 0 → 2 | **violated**: wrong afterwards 2, wrong during 2 (limits 0) |
| lage | 111 → 104 /120 | 0.3 → 0.3 | 1 → 2 | 0 → 0 | 0 → 0 | holds |
| beweg_hr | 126 → 124 /150 | 0.4 → 0.4 | 12 → 7 | 0 → 0 | 0 → 0 | holds; the pulse jumps from 70 to 85 in the process, first valid value afterwards (77 s) 81.4 /min, 3.6 off the true pulse 85 (limit `erst_nach_beweg_fehler` ≤ 5) |
| beweg_weg | 51 → 46 /120 | 0.8 → 0.3 | 3 → 6 | 2 → 0 | 0 → 0 | holds |
| tippen | 144 → 137 /150 | 2.6 → 2.7 | 5 → 5 | 4 → 4 | 7 → 7 | **violated**: wrong afterwards 4, wrong during 7 (limits 0), section MAE 4.4 (limit ≤ 2.0) |

Without the switch, `bench --check` reports "OK (alle Grenzwerte eingehalten)" (OK, all limits kept). With the switch, it ends with exit 1 ("VERSCHLECHTERT", i.e. worsened) and exactly five
violations (`lang` wrong afterwards 2 and wrong during the motion 2; `tippen` wrong afterwards 4, wrong during the motion 7, section MAE 4.4); before the
final review there were three, the two new ones come solely from the new metric `falsch_in_beweg_n` (the behavior is unchanged, only counted differently). In `lang`, the
four wrong values are at 60, 63 s (during the motion) and 71, 72 s (afterwards), 83–85 instead of ≈ 72.5 /min, all with `reacq` = 2: The motion map does not see the
second half of the 20 s motion, the end segment still lies in the motion, the strongest candidate lies in the band ± 15 around the memory (≈ 73)
and ACF/individual beats on the same moving data confirm it. All four have a breathing peak (`breath_ok`, breathing on the full
window, ruling R8; checked with a copy of the header with output); the rule `kMemSubstBpm` (below) therefore changes nothing for them, the test bench is
line-identical with and without it. The valid fraction drops in the motion scenarios (most in `zappel`, 114 → 81); presumably because a value in the
short window has to be confirmed (not examined separately).

**Final review fix (ruling R15)**: A value found again via the memory (`reacq` = 2) replaces the breathing peak as a sign of life only
if it is at most `kMemSubstBpm` = 5 /min from the remembered pulse; otherwise it needs `breath_ok` like every other report. The band
± 15 for the candidate preference stays.

**T1** (`.venv/bin/python host/reacq_eval.py ~/Data/radar-sensor/phone/2026-11-01_164043_usb_iq3raw_bildschirmaus --replay firmware/esp32s3/src [--define A121_DSP_REACQ] [--ohne-karte]`,
31.7 min, person filter as above, time = receive time; recomputation on the PC, no reference). Counting rule "Basis" (base) = motion only via
`motion_pct > 30` or state BEWEGUNG (`--ohne-karte`), "R10" = additionally `win_s == 0`. Without the switch there is no `win_s`, both rules
count the same there. Only rows of the same rule are comparable.

| T1 | Rule | valid (without state 0) | outages (all / motion) | motion: median / sum | keine_atemspitze: n, median / sum | rest: n, median / sum | total outage time | jumps > 15 /min |
|---|---|---|---|---|---|---|---|---|
| without switch | Basis = R10 | 20.7 % | 30 / 15 | 23.7 s / 590.9 s | 14, 2.0 s / 60.1 s | 1, 9.2 s / 9.2 s | 660.2 s | 13 |
| with `A121_DSP_REACQ` | Basis | 13.4 % | 29 / 14 | **35.9 s** / 659.8 s | 4, 2.0 s / 35.9 s | 11, 1.0 s / 33.2 s | 728.8 s | 9 |
| with `A121_DSP_REACQ` | R10 | 13.4 % | 29 / 20 | 28.3 s / 716.8 s | 2, 1.5 s / 3.0 s | 7, 1.0 s / 9.1 s | 728.8 s | 9 |
| for comparison: with switch before R15 (state `3ca400a`) | Basis | 13.7 % | 31 / 14 | 35.9 s / 659.8 s | 4, 2.0 s / 16.0 s | 13, 1.0 s / 50.0 s | 725.8 s | 9 |
| for comparison: with switch before R15 (state `3ca400a`) | R10 | 13.7 % | 31 / 22 | 15.7 s / 713.8 s | 2, 1.5 s / 3.0 s | 7, 1.0 s / 9.1 s | 725.8 s | 9 |

**On T1 the switch is worse:** Availability (valid 20.7 → 13.4 %, outage time over all causes 660 → 729 s) and the like-for-like
median of the motion outages (base rule 23.7 → 35.9 s) get worse; only the jumps > 15 /min go down (13 → 9). The 15.7 s mentioned
earlier were counted with the rule R10, which cannot apply at all without the switch, and are not a like-for-like comparison. The goal of the
design (median ≤ 15 s) is not reached. R15 removes three reports from the memory path on T1 (`t_ms` 157,354, 158,354, 170,354;
72.2 / 72.0 / 65.0 /min, `reacq` = 2, more than 5 /min from the remembered value and without a breathing peak); counted by R10, this makes the
median grow from 15.7 to 28.3 s and the outage time by 3 s. Whether these three values were correct cannot be said without a reference. Whether the switch
is a gain overall (fewer wrong values) or a loss (real values missing) can only be decided with the reference (Polar H9,
expected about 2026-11-05). No statement about accuracy. On T1 the echo after a motion is often not clean (`echo_ok` = 0); this
limits reacquisition, because the memory depends on `echo_ok` (design, section 4).

**Computation time on the PC** (column `us` of the test bench, median per scenario, two runs, x86 PC, no ESP32): 884 → 974 µs and 887 → 972 µs, i.e.
**about + 10 %**; per scenario + 8 to + 27 % (most in `weg`, `beweg_weg`), `leer` 9 % less. + 15–25 % was expected. The number only says
something about the ratio; the computation time on the ESP32 (design: ≤ 400 ms per report, today 91–152 ms) has not been measured.

**Firmware build** (compile and link with `pio run` only, **no runtime check on the device**, nothing flashed):

| Environment | RAM (static) | Flash |
|---|---|---|
| `esp32s3-n16r8` | 74,644 B (22.8 %) | 407,717 B (6.2 %) |
| `esp32s3-n16r8-reacq` (`-DA121_DSP_REACQ`) | 75,572 B (23.1 %) | 410,309 B (6.3 %) |

Difference + 928 B RAM and + 2592 B flash (after the final review fix; before, 410,237 B); buffers allocated at runtime (heap/PSRAM) are not counted by `pio`. `@HELLO` reports the firmware with
the switch as `2026-11-02-vital1-reacq`, without it unchanged as `2026-11-02-vital1`. New fields in `@DSP` only with the switch: `win_s`, `reacq` (`docs/HARDWARE.md`).

**Open**: `lang` still delivers 4 wrong values (2 during, 2 after the motion; R15 changes nothing about that, see above); `tippen` is unsolved
(7 wrong values during, 4 after the section of bumps; the periodicity variant did not help); on T1 the switch is worse by the same counting rule
(above); on T1 an unclean echo after the motion limits reacquisition. Rezoom: The memory survives a rezoom
to a different target only if all reports before this rezoom fell within the first 6 s after a previous rezoom; these reports end before the
`mem_.active` check (too little data). Otherwise a single report with the target outside the zoom window (`target_in_zoom` = 0) has already cleared the
memory (`ReacqBenchTest.test_rezoom_keeps_or_clears_memory`). With `A121_DSP_VITERBI` (also via `A121_DSP_HEAVY`) together with
`A121_DSP_REACQ`, the frozen Viterbi path can pull `hr_prev_` back to the value before the motion after a confirmed end segment; no
shipped environment combines this (comment in the header); resolve before such a combination.
The next step is a run of the firmware `esp32s3-n16r8-reacq` on the device (after approval) with a hand count, and then Polar H9 day A. On the
first run with `-reacq`, print `uxTaskGetStackHighWaterMark` of the DSP task once: The stack reserve is unmeasured (stack of the DSP task
10,240 B; frame of `computeVitalReport` ≈ 1.3 KB and `motionMap` ≈ 0.7 KB, determined on the x86 PC, not on the ESP32).

## Not possible with one sensor

- `bilateration` needs two sensors.
- Angle/direction and outlines: see `CONCEPT.md`.
