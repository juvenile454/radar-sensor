# Vital-sign detection audit: findings and implementation plan

As of 2026-11-01 (implementation of the software parts: section 6, "Status of implementation"). Goal: better vital-sign
detection (breathing, pulse), more accurate data, faster detection, reliable evaluation. Reviewed: docs, firmware (ESP32 DSP,
server client, `iq3` loop), PC scripts, the C++ replay and the app. Also reviewed: vendor docs (Acconeer), the literature
and a synthetic test bench (`host/vital_sim.py`).

**Important:** The audit had no measurement data (it lives in `~/Data/radar-sensor/`). Numbers are therefore marked
with their source: **[Code]** read, **[Docs]** from `docs/`, **[Sim]** from the test bench (model, not a
measurement), **[Vendor]** Acconeer docs, **[Lit]** literature. There is still no statement about pulse accuracy;
that needs the reference measurement (finding B1).

## Summary

1. **Without a reference device, nothing can be improved, only changed.** All pulse optimizations were developed and
   checked against self-counted pulse and on the same dataset. First step: ECG chest strap (e.g. Polar H10, delivers
   beat intervals over BLE) in the app, versioned evaluation, separate development and test data.
2. **The ESP32 DSP centers the IQ values on the window mean instead of a circle fit** (`binDisplacement`). With a
   breathing excursion of 2.4–3.4 mm, as measured, the mean lies 27–40 % of the circle radius off the center. This creates
   breathing harmonics in the pulse band. [Sim] Heart-band error at 3 mm excursion 38 µm instead of 14 µm, at 0.5 mm 88 µm instead of 3 µm
   (heart signal 100 µm); pulse = 4 × breathing: 0 % instead of 91 % of reports within ±5 /min. The PC scripts have
   the circle fit, the firmware does not (HARDWARE.md describes it for `iq3` anyway).
3. **The confidence says nothing about correctness.** It counts how many of the 24 zoom points see the same peak;
   the points are 2.5 mm apart and see almost the same signal. [Sim] 98–100 % confidence at 50 /min error.
   Without a breathing person (only a static echo) the DSP reports [Sim] a valid pulse in 113 of 120 reports (median 63 /min),
   confidence median 50 %, up to 87 %.
4. **Pulse below 55 /min cannot be captured** (search band 53–138 /min, edge bins blocked). [Sim] Pulse 50 → output 100
   (2nd harmonic) with 98 % confidence.
5. **Availability:** Every rezoom deletes 15 s of history; after that, 8 s without pulse. In the 30-min recording (T1),
   267 of 1899 reports (14 %) had no `@VITAL` with 38 rezooms [Docs].
6. **Speed:** The DSP takes 270–320 ms per second in the read loop [Docs]; frames pile up and the
   output arrives in bursts. Pulse response to a change ≈ 10–11 s [Sim], first values after 8 s.
7. **Data quality:** `calibration_needed` and `frame_delayed` are ignored in `iq3` [Code] (only `sess` warns);
   on a 30-min continuous run with a temperature rise this is a risk. Breath hold is not detected: [Sim] 28 /min instead of
   "no breathing". RMSSD is [Sim] systematically 40–230 % too high.

Implementation in phases: **V0** measurement basis (reference, evaluation), **V1** targeted DSP fixes, **V2** speed
and availability, **V3** better algorithms, **V4** measure the sensor configuration, **V5** app/phone DSP. Details in
section 6.

---

## 1. Fundamentals

### 1.1 Measurement principle and quantities

| Quantity | Value | Source |
|---|---|---|
| Wavelength λ at 60.5 GHz | 4.955 mm | [Code] `kWavelengthM` |
| Path per radian of phase (λ/4π) | 394 µm/rad | [Code] `kRadToUm` |
| Full phase revolution | λ/2 = 2.48 mm path | Physics |
| Chest breathing excursion (seated, 0.6–0.7 m) | 2.3–3.4 mm peak-to-peak → 0.9–1.4 revolutions | [Docs] M2, M3 |
| Heart component in the path signal | 62–107 µm (RMS or excursion) → 0.16–0.27 rad | [Docs] M3 B, D |
| Breathing rate measured | 17–24 /min (0.28–0.40 Hz) | [Docs] M2 |
| Pulse (self-counted) | 70–86 /min (1.17–1.43 Hz) | [Docs] M3 E, F |
| Chest distance | 0.58–0.76 m | [Docs] |
| Echo amplitude chest / room noise | 1000–3300 / ≈ 55 (raw values) | [Docs] Bug 5, M3 D |

Consequences:
- Breathing moves the chest **10–30 times** more than the heartbeat. Any distortion of the breathing by a few percent
  lands in the size of the heart signal.
- One breath rotates the IQ values of a point almost once around the circle. The phase must therefore be measured around the **circle center**
  (the static echo in the same point), not around the origin or a mean (finding B2).

### 1.2 Frequencies: breathing harmonics in the pulse band

A breath is not sinusoidal (inhaling faster than exhaling, pause). Its harmonics m × breathing rate fall into the
pulse band:

| Breathing /min | Harmonics in the range 40–180 /min |
|---|---|
| 12 | 48, 60, 72, 84, 96 |
| 15 | 45, 60, 75, 90, 105, 120 |
| 18 | 54, 72, 90, 108, 126, 144 |
| 20 | 40, 60, 80, 100, 120, 140, 160 |

At 18–20 /min breathing there is a harmonic every 18–20 /min; a pulse of 72–80 almost always hits one of them (M3 E/F:
"pulse ≈ 4 × breathing"). With exact coincidence this cannot be separated by the spectrum alone; only less
distortion (B2), the 2nd heart harmonic, tracking over time and time-domain features help (section 6, V3).

### 1.3 Window length, resolution and lag

| Window | Hann main lobe (±) | −3 dB width | Lag (≈ half the window) |
|---|---|---|---|
| 8 s | ±15 /min | 10.8 /min | 4 s |
| 10 s | ±12 /min | 8.6 /min | 5 s |
| 15 s (today) | ±8 /min | 5.8 /min | 7.5 s |
| 20 s | ±6 /min | 4.3 /min | 10 s |
| 30 s | ±4 /min | 2.9 /min | 15 s |

Today: 15 s window + smoothing 0.7/0.3 + hold time 3 (or 10) reports → response to a pulse change ≈ 10–11 s [Sim].
For breathing, a 15 s window at 15 /min contains only 3.75 breaths; for slow breathing (< 8 /min) it is too short.

### 1.4 Sensor configuration `iq3` [Code]

`make3SubsweepPreset()` in `firmware/esp32s3/src/a121_server_client.h`, 60 Hz, 1 sweep per frame, gain 16:

| Subsweep | Points | Grid | Profile | HWAAS | PRF | Phase enh. | Purpose |
|---|---|---|---|---|---|---|---|
| 0 | 1 (start 0) | — | 1 | 8 | 19.5 MHz | off | Loopback, phase reference |
| 1 | 24 from zoom start | 2.5 mm (60 mm) | 1 | 32 | 15.6 MHz | on | Vital zoom |
| 2 | 80 from 0.30 m | 30 mm (up to 2.67 m) | 3 | 16 | 15.6 MHz | on | Room, target search |

Data rate 105 points × 4 B × 60 Hz ≈ 25 kB/s payload, with ≈ 150 B header per frame ≈ 34 kB/s; link limit
≈ 195 kB/s [Docs] → **a good 5× reserve** for a wider zoom window or more points (section 6, V2.3/V4).

Profile 1 has a 43 mm half-power width [Vendor, datasheet]. At a 2.5 mm grid, neighboring points overlap almost
completely: the 24 zoom points (60 mm) contain only about 1.4 independent range cells. This matters for finding B4.

### 1.5 Vendor information (Acconeer) and what it means for `iq3`

Sources: Sphinx sources of docs.acconeer.com and code in `acconeer/acconeer-python-exploration` (tag v7.18.2 = SDK
1.13.0), A121 datasheet v1.3. docs.acconeer.com itself was not reachable from the audit environment.
Abbreviations: `GH:` = https://github.com/acconeer/acconeer-python-exploration/blob/v7.18.2/

| Topic | Vendor statement [Vendor] | Consequence for the project |
|---|---|---|
| Phase enhancement | Determined from a loopback measurement during sensor calibration and corrects the phase pattern **across distance** ("phase coherent data in the distance domain"); no statement about stability from frame to frame (GH: `docs/radar_data_and_control/a121/in_depth_topics/loopback.rst`, `subsweep_config.py`) | Correct for the zoom (points can be combined coherently). After every recalibration (every session start, i.e. every rezoom) the phase can jump → start the section anew or align via loopback (V2.3) |
| Loopback per frame | Acconeer uses exactly this derotation in the vibration app ("low frequency enhancement", `frame * exp(-1j*angle(loopback))`, GH: `src/acconeer/exptool/a121/algo/vibration/_processor.py`) against phase jitter, there with the **same PRF** for loopback and measurement; the distance detector combines phase enhancement and loopback | The technique is legitimate but not free: the noise of the loopback measurement is added. Today 19.5 MHz loopback versus 15.6 MHz zoom (never so at Acconeer). Loopback with profile 2 is forbidden |
| Profiles | Half-power width P1 43 mm, P2 80 mm, P3 138 mm, P4 203 mm, P5 346 mm (datasheet table 10); P3 has ≈ 8 dB more signal per HWAAS than P1, ≈ 10 dB per measurement time | Profile 1 is only needed if 43 mm resolution improves the pulse (separating chest from shoulder). Profile 3 at the zoom is a serious alternative (V4.1/V4.2) |
| HWAAS | SNR +3 dB per doubling, measurement time grows linearly | Zoom today ≈ 1.5 ms, whole frame ≈ 3–3.6 ms (time model from `timing.rst`): at 60 Hz (16.7 ms) there is room for HWAAS 128 at the zoom (+6 dB) |
| Receiver gain | Default 16; lower it on saturation; Acconeer's phase apps use 10 | Watch `data_saturated` (today only printed) |
| Breathing reference app | Profile 3, HWAAS 32, 16 sweeps/frame, 20 Hz, 60 mm grid, 3 distances; static echo removed by a complex low-pass at 0.1 Hz, then phase, band-pass 6–60 /min, **20 s window**, Hamming, FFT, peak refined quadratically; states "no person / motion / determine distance / estimate breathing" (GH: `src/acconeer/exptool/a121/algo/breathing/`) | Model for the state logic (V3.5) and breathing (V3.4); Acconeer's test table: ≤ 1 /min deviation |
| Heart rate | No heart or vital app from Acconeer, no µm noise figure | Every pulse or µm statement needs its own reference |
| `calibration_needed` | "Sensor calibration needs to be redone"; rule: temperature ±15 °C since the last calibration | V1.5: restart the session |
| Temperature and amplitude | Amplitude ≈ 2^(−ΔT/67 °C) at P1 (86 °C at P3) | Fixed amplitude thresholds drift with temperature; thresholds relative to the noise (as in the room today) are correct |
| `tick` | XM125 server: `ticks_per_second` = 1000 (1 ms) | Use tick only for gap checks; read `ticks_per_second` from `get_system_info` instead of assuming it |
| Frame rate | With `frame_rate` set, the A121 triggers itself ("extremely accurate rate") | A fixed 60 Hz time base in the DSP is correct |

## 2. Analysis of the current chain

```
A121 ──LPUART1 2 Mbaud──► ESP32-S3 cmdIq3()                          ──USB-CDC──► App / PC
        (JSON header +     ├─ readStreamFrame()  (flags read, not used)
         105 × int16 IQ)   ├─ pushFrame()        per frame: loopback derotation, 16 s ring per zoom point,
                           │                     room EMA, telemetry path (@IQ3)
                           └─ computeVitalReport() every ≈ 1 s, blocks 270–320 ms:
                                per zoom point (24×): mean centering → atan2 + unwrapping → remove trend →
                                band-pass 0.8–3 Hz (filtfilt) → motion damping → Goertzel 0.88–2.30 Hz
                                and breathing spectrum 0.10–0.60 Hz → weighted sum → top-3 peaks →
                                tracking (±12 /min, hold time 3/10) → ACF, IBI/RMSSD at the "best" point
                                room: noise level, targets, rezoom recommendation → new session (deletes history)
```

| Part | File | Status |
|---|---|---|
| Server client, preset | `firmware/esp32s3/src/a121_server_client.h` | robust (bugs 1–4 fixed) |
| `iq3` loop, output | `firmware/esp32s3/src/main.cpp:616-765` | Computing and reading in the same flow |
| DSP | `firmware/esp32s3/src/a121_esp_dsp.h` | developed on 2 recordings of one person |
| Replay PC/phone | `host/dsp_replay/`, `android/app/src/main/cpp/` | Phone = PC byte-identical, ESP32 up to the last digit |
| PC research scripts | `host/a121_ultimate_vital.py`, `a121_vitalaufnahme.py`, `a121_anhalteanalyse.py`, `a121_esp_*.py` | 5 variants with their own DSP |
| Evaluation against reference | — | **missing** (count-window evaluation lived in the scratchpad) |
| App | `android/…/live/LiveStore.kt`, `ui/Pages.kt` | shows values, no quality state |

Well solved and to be kept: fixed time base via frame counter instead of ESP32 time, zero-phase filter like scipy,
motion damping per 1 s block, blocking of the band edges, room noise level from the lower quarter, smoothed target selection
(bug 7), one shared DSP source for ESP32, PC and phone, `@RAW` recordings for replays, honest labeling
"pulse candidate".

## 3. Findings

Severity: **high** = falsifies values or prevents improvement; **medium** = noticeable losses; **low** = cleanup.

### Evaluation and validation

**B1 (high) No reference measurement, no versioned evaluation.**
The pulse reference was self-counted ("uncertain according to the test subject", count 70 versus measured 77 unresolved). The
harmonic hold time was developed on the dataset on which it was assessed ("no independent test") [Docs M3 F].
The script for "share within ±8 /min" is not in the repo. Consequence: every change can look better without being
better; values like RMSSD 102 ms [Docs M3 D] cannot be put into context.

**B2 (high) Mean centering instead of circle fit in the firmware.**
`binDisplacement()` (`a121_esp_dsp.h:584-613`) measures the phase around the mean of the IQ values in the 15 s window. For a
breathing motion with phase excursion β, this mean is J₀(β) × radius away from the true center:

| Breathing excursion | β | Center error | largest phase error |
|---|---|---|---|
| 0.5 mm | 0.63 rad | 90 % of the radius | ≈ 440 µm |
| 1.0 mm | 1.27 rad | 64 % | ≈ 270 µm |
| 2.0 mm | 2.54 rad | 7 % | ≈ 26 µm |
| 2.4 mm | 3.04 rad | 27 % | ≈ 110 µm |
| 3.0 mm | 3.80 rad | 40 % | ≈ 160 µm |
| 4.0 mm | 5.07 rad | 15 % | ≈ 60 µm |

The error repeats with the breathing motion, i.e. as breathing harmonics. [Sim] `host/vital_sim.py demod`, heart signal
100 µm, breathing 15 /min: heart-band error with mean 13–88 µm, Kåsa circle fit 3–23 µm (≈ like the true center);
the breathing harmonics lie up to +2.3 dB above the heart line with the mean, and at −6 dB with the circle fit. The PC scripts use
the Kåsa fit (`fit_iq_circle`); `docs/HARDWARE.md` also describes it for `iq3` on the ESP32. The telemetry path (`@IQ3`) also uses
a moving mean (`dc_i_`, τ ≈ 8 s). On the real signal the circle is deformed (amplitude fluctuates
with the breathing, several scattering centers in the point); the gain must be confirmed on recordings (V1.1).

**B3 (medium) Several diverging algorithm versions.**
Five PC scripts with their own chain (Kåsa + DACM + Hilbert projection + HPS, 100 zoom points, 60–100 Hz) and the firmware
(mean, no projection, 24 points). Findings from the PC chain (circle fit, low-pass of the loopback phase,
2nd heart harmonic) are not in the firmware; the firmware findings (band edge, motion damping, harmonic hold time)
are not in the scripts.

### Pulse

**B4 (high) Confidence measures agreement of correlated points, not correctness.**
`agreeAt()` (`a121_esp_dsp.h:493-498`) = weighted share of the zoom points whose own peak is within ±5 /min of the result.
The points overlap almost completely (1.4); an artifact or a harmonic is seen by all of them. [Sim] Pulse 50 → 100
reported with confidence 98–100 %; pulse 120 → 60 with 100 %; the correct value at pulse = 4 × breathing with 0 %. The docs
observe the same live: "rhythmic motion is supported by all points" [Docs M3 F].

**B5 (high) Search band 53–138 /min, effective 55–136 /min.**
`kHrFMin = 0,88 Hz`, 143 steps, 3 edge steps blocked; band-pass 0.8–3.0 Hz. Resting pulse below 55 (common in
athletes, when lying down, asleep) and above 136 is never found; a harmonic is found instead. A subharmonic test
(is there also a peak at f/2?) is missing. [Sim] Pulse 50: 0 % within ±5 /min.

**B6 (medium) Tracking gets stuck on strong foreign peaks.**
A candidate ±12 /min from the old value with ≥ 40 % of the strongest peak always wins (`a121_esp_dsp.h:503-516`). If the
first value is a breathing harmonic, the tracking stays there as long as it is strong. `hr_harm` is reported but lowers
neither confidence nor priority. [Sim] Pulse 120 / breathing 20 with circle fit: permanently 60 (3rd breathing harmonic), confidence 100 %.
The docs name the same case live ("stuck at 60–63 /min") [Docs M3 F].

**B7 (medium) IBI and RMSSD are not reliable.**
Peaks on the narrow-band filtered signal (0.8–3 Hz turns every beat into a sine wave), threshold 0.35 × RMS,
no interpolation between samples, no check against the tracked pulse. Quantization (16.7 ms) alone
explains only ≈ 2 ms of RMSSD error (√(43² + 12²) ≈ 45 ms); the main error is missing and extra beats. [Sim]
RMSSD 61–160 ms at true 43–53 ms. Literature on HRV from radar: see section 5.

**B8 (low) Loopback derotation unchecked and with a different PRF.**
`pushFrame()` rotates all zoom points every frame by the raw loopback phase (`a121_esp_dsp.h:225-233`), including its
noise. The PC chain smooths the loopback phase (< 0.8 Hz), "without coupling in white noise". Acconeer uses the raw derotation
itself (vibration app, 1.5), but with the same PRF as the measurement; here the loopback measures at 19.5 MHz,
the zoom at 15.6 MHz. Whether the derotation helps in the pulse band or adds noise is unmeasured (V4.1);
`loopback_phase_std_mrad` is printed.

### Breathing

**B9 (medium) Breath hold / apnea is not detected.**
`br_bpm` is 0 only without a local maximum; with held breath a residual peak wins. [Sim] During a 20 s breath hold:
28 /min (harmonic of the residue in the window) instead of "no breathing". The PC chain had an apnea counter (`apnea_10s_episodes`).

**B10 (low) Breathing only spectral in the 15 s window.**
Frequency from the largest local maximum (0.115–0.585 Hz). Slow breathing (< 7 /min, e.g. guided breathing at 6 /min)
is outside; no breath-by-breath values. `kRespMinWin` (3 s) is defined but unused: breathing, like the
pulse, only arrives after 8 s. Breathing depth only from the fundamental at the point with the clearest pulse, not at the point with the strongest
breathing.

### Speed and availability

**B11 (high) Every rezoom deletes the history.**
`setZoomStartPoint()` → `resetState()` also resets `hr_prev_`. After that, 8 s without a value, and the tracking starts anew.
T1: 38 rezooms in 31.7 min, 1632 `@VITAL` for 1899 reports (14 % without a vital value) [Docs]. The zoom window is 60 mm
wide; the chest moves 2–3 mm when breathing, clearly more when leaning back.

**B12 (medium) DSP blocks the read loop.**
`computeVitalReport()` runs in `cmdIq3()` between two `readStreamFrame()` calls (`main.cpp:714-759`), 270–320 ms per
second. Frames pile up in the 32 KB buffer and arrive in bursts (receive pauses up to 337 ms) [Docs T1]. The ESP32-S3 has
two cores; the second computes nothing.

**B13 (medium) Computational load higher than necessary** (estimate per report, [Code]):
- 21,600 × `atan2f` + 21,600 × `sqrtf` (every point, every sample, recomputed every second),
- ≈ 260,000 `double` operations (`binDisplacement`, `removeLinearTrend`); the ESP32-S3 computes `double` in software,
- ≈ 1.3 million Goertzel steps (24 points × 244 frequencies × 225 samples) and ≈ 17,000 `cosf` (Hann window recomputed per point),
- 24 × forward/backward filter over 954 samples.
On the PC 4.9 ms per report [Sim], on the ESP32 270–320 ms.

**B14 (medium) First values and response slow.** First values after 8 s at the earliest, after a rezoom again 8 s;
response to a pulse change (66 → 90 in 10 s) [Sim] 10–11 s after the end of the change.

### Data quality and robustness

**B15 (high) Sensor flags in `iq3` unused.** `readStreamFrame()` reads `calibration_needed`, `frame_delayed`,
`data_saturated` (`a121_server_client.h:347-349`); `cmdIq3()` prints only `saturated` in `@IQ3`, the DSP uses none.
`sess` warns on `calibration_needed` [Code `a121_session_console.h:395`]. On 30-min runs the temperature rose
(14 → 19 °C in 30 s of mobile operation [Docs]); for when the A121 must be recalibrated, see 1.5.

**B16 (low) Gaps in the data stream visible only at the ESP32 frame counter.** The counter counts received frames;
if the server loses frames (`frame_delayed`) or the ESP32 loses bytes, the DSP does not see it. The sensor `tick` would be the
check value (nominal 16.7 ms per frame).

**B19 (high) No sign-of-life test.** `valid` only means "enough frames and a peak in the spectrum". Whether a
person is breathing at all is not checked by the DSP (the room target only has to be "moving" for the rezoom). [Sim] Static scene without breathing
and heartbeat (chest echo present, motionless; scenario `leer`): 113 of 120 reports valid, pulse median 63 /min, breathing median 8 /min,
confidence median 50 %, at most 87 %. Acconeer's breathing app switches to "no person" in such a case (1.5).

### App and display

**B17 (medium) No quality state in the app.** Pulse, RMSSD and breathing are always shown (with the note "Kandidat", i.e. candidate).
States are missing such as "searching for person", "acquiring (x s)", "motion", "breath held", "uncertain". There is no
reference curve.

### Cleanup

**B18 (low)** `hist_um_` is only written; `kRespMinWin` unused; the `iq3` description in `docs/HARDWARE.md`
("Kåsa circle fit + DACM", "Hilbert projection") does not match the code; the breathing spectrum decimates without a low-pass (uncritical, noise).

## 4. Synthetic test bench

`host/vital_sim.py` (new) compiles the unchanged firmware header for the PC, plus a variant with a Kåsa circle fit in
`binDisplacement()`, and feeds in synthetic `iq3` frames (105 points, 60 Hz). Model: front chest, abdomen, shoulder
(different breathing/heart components, amplitude fluctuates 8 % with the breathing), table, wall, furniture; profile 1/3 envelopes;
noise 25/40; loopback with a common phase error; breathing with 15 % 2nd and 5 % 3rd harmonic; heart 100 µm
peak-to-peak as a pulse train with 35 ms jitter.

```bash
.venv/bin/python host/vital_sim.py demod
```

```bash
.venv/bin/python host/vital_sim.py bench
```

Result 2026-11-01 (share of reports within ±5 /min; confidence median; "at err." = confidence at > 10 /min error):

| Scenario | Original | Circle fit | Confidence orig. / at err. | Remark |
|---|---|---|---|---|
| Breathing 15, pulse 72 | 97 % | 100 % | 89 / — | |
| Pulse = 4 × breathing (18 / 72) | **0 %** (MAE 18) | **91 %** (MAE 2.5) | 0 / 0 | Core case from M3 E/F |
| Pulse 66 → 90 | 90 %, response 11 s | 99 %, response 10 s | 93 / 70 | |
| Pulse 50, breathing 12 | 0 % (reported ≈ 100) | 0 % | **98 / 98** | B4, B5 |
| Pulse 120, breathing 20 | 100 % | **0 %** (reported 60) | 44 / — | B6: circle fit alone is not enough |
| Breathing excursion 0.6 mm | 54 % | 100 % | 100 / — | B2 |
| Breathing excursion 4 mm | 60 % | 100 % | 100 / — | B2 |
| 10 s motion | 77 % | 96 % | 88 / 80 | |
| Breath hold 20 s | 93 % | 96 % | 97 / 96 | Breathing reported 28 /min (B9) |
| No breathing, no pulse (static echo) | 113/120 reports "valid" | same | 50, max. 87 | Pulse median 63 /min reported (B19) |

RMSSD reported / true in the scenarios with a person: original 61–160 ms, circle fit 40–202 ms at true 43–53 ms (B7). Breathing rate otherwise ±0.2 /min (ideal model).

Limits: one model, one geometry, no real body movements, ideally periodic breathing. The test bench shows
logic weaknesses (band, confidence, tracking, apnea) independent of the model; the size of the improvements (circle fit)
holds only for the model and must be confirmed on recordings with a reference. It is suitable as a fast
regression test for every DSP change (V0.5).

## 5. State of the art (literature)

Research 2026-11-01 via search engines; full texts (arXiv, MDPI, PMC, IEEE) were not retrievable from the audit environment.
Numbers therefore come from abstracts and search excerpts; those marked **(verify)** come from only one source. None of the
accuracies can be transferred to this setup; they only show what was achieved with which method.

### 5.1 What the literature says about the findings

| Topic | Literature finding [Lit] | Consequence (plan) |
|---|---|---|
| Center of the IQ values | Arctangent demodulation is exact only with the center (DC) removed exactly (Park et al. 2007). In the mmWave range, geometric circle fitting was the best of four methods (Mercuri et al. 2022). Kåsa is biased toward small circles on short arcs; Taubin is the best algebraic fit, Levenberg–Marquardt (geometric) the best on short arcs (Al-Sharadqah & Chernov 2009; Zakrzewski et al. 2012) | V1.1: circle fit; Taubin/LM as a variant; check of the arc length |
| Error size | With center error r (relative to the radius), breathing harmonics arise with amplitude (λ/4π) · r · \|Jₙ(β)\|; at 2 mm breathing excursion and r = 0.1 ≈ 16 µm at 4 × breathing rate (Bessel model as in Rittiplang et al. 2020) | confirms B2; today r is 0.27–0.40 |
| Combining several points | Several range points improve breathing and pulse (Szmola et al. 2025, polysomnography: breathing MAE 0.29 → 0.20 /min); sum coherently aligned and weighted (Pi-ViMo 2023; maximum ratio combining); combine before demodulation, do not average spectra afterwards | V3.1; diagnostic value "effective point count" instead of 24-point agreement |
| Breathing harmonics | The fundamental of the heart is limited by breathing harmonics, its higher harmonics only by noise (Rong & Bliss 2019); second derivative / higher harmonics lower the error by ≈ 20–35 % (Iwata et al. 2025, (verify)); adaptive notch filters need an accurate breathing rate (Tazen et al. 2023); RLS adapts better than LMS (2025) | V1.2 (band up to 5 Hz, sum with 2f), V3.3 |
| Heart–breathing coupling | Integer ratios (e.g. 4:1) occur more often than by chance through cardiorespiratory synchronization (Schäfer et al. 1998) | The case "pulse = 4 × breathing" is not bad luck but to be expected; test scenario `harm4` and metronome session R3 |
| Window length vs. harmonics | Calculation: with a 15 s Hann window and 18 /min breathing, the main lobes of the harmonics cover ≈ 89 % of the pulse band (20 s: 67 %, 30 s: 44 %); the harmonic tolerance of the DSP (0.04 Hz) is ≈ 3 times narrower than the main lobe | Do not only mark harmonics but model them (V3.3) and use time-domain features (V3.6) |
| Tracking | In PPG pulse measurement, Viterbi decoding over a frequency grid lowers the MAE by ≈ 30 % (Temko 2017: 1.97 → 1.37 /min); radar: spectral Viterbi (Ye & Ohtsuki 2021), Kalman (Arsalan et al. 2020, ≈ 10 s window) | V3.2, then shorter window (V2.5) |
| Window lengths | Google Soli (60 GHz, Nest Hub): first value after 16 s, then every 4 s; Acconeer breathing app 20 s; radar pulse mostly 8–10 s; below ≈ 10 s the accuracy drops | Pulse 8–12 s with tracking, breathing 20–30 s |
| What "good" is | Soli (60 GHz, sleep, 62 people, 498 h): pulse MAE 1.69 /min, meditation 1.05 /min; explicitly no HRV | Target ≤ 3 /min is ambitious but not absurd; do not promise HRV |
| IBI / HRV | IBI error against ECG of ≈ 7–15 ms (lab, UWB/FMCW with beamforming, (verify)) up to 83 ms scatter (sleep, Turppa et al. 2020); quantization at 60 Hz explains only ≈ 12 ms error per difference; artifact rule: discard IBI > 20 % from the local median; short-term HRV values are defined over ≈ 5 min | V1.6, V3.6; show RMSSD only after the strap comparison |
| Motion | Single-channel radar can hardly compute out body motion (review Gouveia et al. 2019); in practice: detect and discard; always state accuracy together with availability. Features: phase jump > π/2 per frame (≈ 75 mm/s at 60 Hz), amplitude change > 3 dB; ≥ 70 % clean data in the window | V1.3 (share of clean data in the confidence), V3.5 |
| Breathing | Acconeer app: 20 s window, FFT, ≤ 1 /min in Acconeer's test table; clinical contactless systems 0.2–0.4 /min MAE against a breathing belt; apnea = amplitude drop ≥ 90 % (hypopnea ≥ 30 %) over ≥ 10 s (Kang et al. 2020) | V1.4, V3.4 |
| Reference device | Polar H10: 99.6 % correctly detected beats against Holter ECG (Gilgen-Ammann et al. 2019) | V0.1 |
| Acceptance criteria | ANSI/CTA-2065: MAPE < 10 %; AAMI EC13 (pulse displays): ±10 % or ±5 /min | Target values in 6 are below this |
| Acconeer and pulse | No peer-reviewed pulse validation found for A111/A121; theses: Lund (A111, 30 Hz, against ECG; result not retrievable), KTU Kaunas (A121 fields, pulse error mean −0.12, SD 1.14, unit probably /min (verify)) | Results of this project would themselves be a contribution; be careful with comparisons |
| Machine learning | Data-hungry (Google: 980 h pretraining, 37 h fine-tuning, MAE 4.1 /min); with data from one person overfitting looms; more sensible: a small classifier for quality or candidate selection | V3.7 only with a lot of reference data |

### 5.2 Public datasets (for comparisons without own hardware)

| Dataset | Radar | Size | Reference |
|---|---|---|---|
| Schellenberger et al. 2020, *Sci Data* 7:291 | 24 GHz CW, IQ | 30 people, 24 h | ECG etc. |
| Shi et al. 2020, *Sci Data* 7:50 | 24 GHz | 11 people, 223 min | ECG, heart sound, breathing |
| Sadeghi et al. 2024, arXiv:2405.12659 | mmWave FMCW | 10 people, also after exertion | Polar H10 |
| Parralejo et al. 2026, *Sci Data* (Zenodo 10.5281/zenodo.18599983) (verify) | 2 × 60 GHz FMCW | 110 people | ECG |

CW IQ data fit the signal of a single point best; convert the wavelength (24 GHz: 12.5 mm instead of 5 mm). Check the license before
use; data goes to `~/Data/radar-sensor/public/`, not into the repo.

### 5.3 Sources (selection)

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
- Soli study 2023, *Sci Rep* 13:18008. https://www.nature.com/articles/s41598-023-44714-2
- Turppa et al. 2020, *Sensors* 20:6505. https://www.mdpi.com/1424-8220/20/22/6505
- Gouveia, Vieira, Pinho 2019, Random Motion… in Bio-Radar, *Sensors* 19(3):604. https://www.mdpi.com/1424-8220/19/3/604
- Kang et al. 2020, IR-UWB and sleep apnea, *Sci Rep*. https://pubmed.ncbi.nlm.nih.gov/32210266/
- Gilgen-Ammann, Schweizer, Wyss 2019, RR intervals Polar H10, *Eur J Appl Physiol* 119.
  https://link.springer.com/10.1007/s00421-019-04142-5
- Schellenberger et al. 2020, *Sci Data* 7:291. https://www.nature.com/articles/s41597-020-00629-5
- Adib et al. 2015, Vital-Radio, *ACM CHI*. https://doi.org/10.1145/2702123.2702200 (periodicity test, fine estimate
  via the phase slope; evaluated in `docs/design/specs/2026-11-02-pulse-reacquisition-design.md`)
- Lund thesis (A111): https://lup.lub.lu.se/student-papers/record/9094792 ·
  KTU thesis (A121): https://epubl.ktu.edu/object/elaba:199466836/index.html

## 6. Implementation plan

### Status of implementation (2026-11-01)

The software parts of all phases are implemented and checked on the PC (Python suite `host/tests`, test bench
`vital_sim.py bench --check`, replay, app logic in a JVM project). **None of it has run on the device:**
the cloud environment had neither the ESP32 toolchain nor the Android SDK. The firmware was compiled on the PC only with stub headers
(`host/tests/test_firmware_pc.py`), not built with `pio run` and not flashed; of the app, logic,
parser, storage and JNI libraries are tested on the PC; UI (Compose), controller and NDK build were not compiled.
All numbers on variants come from the test bench **[Sim]**; whether they hold on real data is shown only by day A/B.

| Phase | implemented (checked on the PC) | open (device, strap, recordings, approval) |
|---|---|---|
| V0 | V0.1/V0.2 strap (BLE), markers, metronome in the app; V0.4 `vital_eval.py`; V0.5 test bench `--check`, scenario `leer`; V0.3 measurement plan M5 (`EXPERIMENTS.md`) | Strap on the phone, recordings day A/B (V0.3), baseline (V0.6) |
| V1 | V1.1–V1.7 in the header (circle fit, pulse band 40–180, candidates/tracking/confidence/sign of life, breathing state, sensor flags and `@RECAL`, IBI/RMSSD, cleanup) | Acceptance on day A/B, live run |
| V2 | V2.1 report on core 0 from a snapshot, V2.2 FFT, V2.3 rezoom without loss, V2.4/V2.5 fast start and response | Measure receive pause and compute time on the ESP32, 30-min run, V2.3 step 3 (after V4.4) |
| V3 | V3.4 breaths (`@BREATH`) and V3.5 state (`@STATE`) as default; V3.1 combination, V3.2 Viterbi, V3.3 harmonic subtraction, V3.6 template for single beats as a switch, default off; V3.7 not started (prerequisite missing) | A/B on day A, adoption only after day B |
| V4 | Measurement plan (`EXPERIMENTS.md`, "V4 sensor configuration"), `host/noise_floor.py` (path noise, recording via `bridge`), iq3 preset as a compile switch, identifier in `@HELLO` | all measurements V4.1–V4.4 |
| V5 | V5.1 quality state and deviation against the strap, V5.2 phone DSP live from `@RAW` (`A121_DSP_HEAVY`), V5.3 old PC scripts as archive | Screenshots per state, 30 min live with CPU ≤ 20 %, NDK build |
| V6 (2026-11-03) | Reacquire pulse after motion behind `A121_DSP_REACQ` (default off): motion map, pulse on the quiet tail, memory; `@DSP` with `win_s`/`reacq`; `host/reacq_eval.py`; test bench scenarios `kurz`…`tippen`; firmware environment `esp32s3-n16r8-reacq` (`pio run` compiled, `-reacq` in `@HELLO`); variants `A121_DSP_REACQ_PERIOD`, `A121_DSP_PHASE_SLOPE` measured and rejected; final review fix: the memory value replaces the breathing peak only up to 5 /min from the remembered value (`kMemSubstBpm`), metric `falsch_in_beweg_n`, `reacq_eval.py --ohne-karte`. Numbers: `EXPERIMENTS.md`, "V6" | Run on the device (not flashed), and measure the stack reserve of the DSP task once (`uxTaskGetStackHighWaterMark`), manual count, Polar H9 day A; test bench **[Sim]** with the switch 5 violations (`lang` 2 wrong values during and 2 after the motion, `tippen` 7 during and 4 after the section, section MAE 4.4); T1 **[Replay]** with the same counting rule with the switch **worse**: valid 20.7 → 13.4 %, total downtime 660 → 729 s, median of the motion outages 23.7 → 35.9 s (target ≤ 15 s not reached), only jumps 13 → 9 better; echo after motion often unclean; the memory survives a rezoom to a different target only if all reports before it were within the first 6 s after a previous rezoom |

Row V6 (addendum 2026-11-03): firmware of both environments (`esp32s3-n16r8`, `esp32s3-n16r8-reacq`) built with `pio run`, Python suite, test bench
and Android unit tests run on the PC; **none of it has run on the device** and no number has a reference.

Individual results and commands per task: `docs/EXPERIMENTS.md` (test bench, measurement plans), `docs/ANDROID_APP.md` (app,
open device tests), `docs/HARDWARE.md` (commands, switches).

**Final review (2026-11-01), fixed and checked on the PC:** Pulse in the app gray on an invalid or stale
report (also in APNOE); no more APNOE in the empty room after walking away (test bench scenario `weg`); the test bench counts the
first *correct* value and early wrong values (`erst_ok_s`, `frueh_falsch_n`); phone DSP also delivers values when switched on
in the middle of the stream; firmware with `-ffp-contract=off` like the replay; one time per report for
`@DSP`/`@ROOM`/`@VITAL`/`@STATE`; strap reconnects after a failed service discovery; the metronome releases its
tone generator only after the last tone. Firmware identifier of this state in `@HELLO`: `fw=2026-11-02-vital1`
(`2026-11-01-app1` = baseline before the audit). **Flashed 2026-11-02 and checked live for 60 s:** replay identical
to the live output in all report lines, `compute_us` median 91 ms instead of 270–320 ms (details `ANDROID_APP.md`,
A5). Open on the device: 30-min run, app with the new firmware on the phone, strap reconnection and metronome on the phone.

### Guidelines

- Order: **be able to measure → fix errors → faster → better.** Without V0 no judgment on V1–V5.
- Every DSP change goes through three checks, reported separately: (1) test bench `vital_sim.py bench`
  (regression), (2) replay of the recordings with reference (`vital_eval.py`, test data only for acceptance),
  (3) live run on the device. Build/compile is not a runtime check.
- One DSP source: `a121_esp_dsp.h`. PC scripts are used for research; what proves itself moves into the header.
- Extend the protocol only at the end (`docs/ANDROID_APP.md`) so that PC scripts and the app keep working.
- Pulse stays a "candidate" until V0 has measured an accuracy.

### Target values (against reference, sitting still 0.5–0.8 m, chest facing the sensor; measurable only after V0)

| Metric | today | Target |
|---|---|---|
| Pulse MAE against ECG strap | unknown | ≤ 3 /min |
| Share of reports within ±5 /min | unknown (against self-count ±8 /min: 39–100 %) | ≥ 90 % |
| Availability (share of time with valid pulse) | 86 % (T1, without quality assessment) | ≥ 90 % with confidence ≥ threshold |
| Confidence calibrated | no | Reports with confidence ≥ 70 %: ≥ 90 % within ±5 /min |
| Breathing MAE (metronome breathing) | unknown | ≤ 1 /min |
| Breath hold detected | no | ≤ 12 s after onset, no false alarms during normal breathing |
| first values after start / rezoom | 8 s / 8 s | ≤ 10 s / ≤ 3 s |
| Response to pulse change | 10–11 s [Sim] | ≤ 7 s |
| Static echo without a person | pulse reported as valid [Sim] | no valid report |
| Receive pause on the ESP32 | up to 337 ms | ≤ 50 ms |
| DSP compute time per report | 270–320 ms | ≤ 80 ms |

For comparison [Lit]: standards for pulse displays require MAPE < 10 % (ANSI/CTA-2065) or ±10 % or ±5 /min (AAMI EC13);
Google Soli (60 GHz) achieved 1.7 /min MAE in sleep, without HRV.

### Rules for all tasks (from `AGENTS.md`)

- Recordings, datasets, binary files not into the repo; data to `~/Data/radar-sensor/` (new: `ref/` for
  recordings with reference, `sim/` for test bench frames).
- Before flashing, identify the port unambiguously via `ls -l /dev/serial/by-id`; phone only with approval of the project owner, serial number live
  from `adb devices -l`.
- Operate radio modules unchanged. No statement about pulse accuracy without a reference; report build and device test
  separately.
- `@VITAL` keeps its 15 fields; new values only at the end of `@DSP` or as new tags.
- Every change to the DSP header: run `vital_sim.py bench --check`, `a121_esp_ultimate.py --mode replay-cpp` (format and
  flow equality) and `cd android && ./gradlew :app:testDebugUnitTest`.

### What to watch especially when checking

Cases that are easily overlooked; each gets a test in the named task:
1. **Nobody in front of the sensor** (empty chair, person gets up): no valid pulse may appear; today it appears
   (B19). → Scenario `leer` in the test bench (V0.5), sign-of-life test (V1.3), state "no person" (V3.5).
2. **Pulse exactly on a breathing harmonic for minutes** (e.g. metronome 18 /min, pulse 72): the value may be wrong, the
   confidence must show the uncertainty (≤ 60 %). → `harm4` (V1.3).
3. **Continuous run with temperature change** (`calibration_needed`): recalibration must not break the stream, the
   replay must follow it. → V1.5, 30-min run.
4. **Rezoom during motion or breath hold:** do not pass on old values as valid. → V2.3, V3.5.
5. **Backlog on USB with `raw` and a second core:** no frame loss, no tick gaps. → V2.1, 30-min run.

---

### Phase V0 — Measurement basis (prerequisite for everything else)

#### V0.1 Reference pulse in the app (ECG chest strap over BLE)

Device: **Polar H10** (planned; a Polar H9 with the same service is used, standard BLE "Heart Rate Service", delivers pulse and RR intervals per notification in
1/1024 s; rationale and evidence in section 5). Other straps with the Heart Rate Service work the same way.

**Files:**
- New: `android/app/src/main/java/io/github/juvenile454/radarsensor/reference/HrMeasurement.kt` (parser, pure Kotlin)
- New: `android/app/src/main/java/io/github/juvenile454/radarsensor/reference/HrReference.kt` (BLE scan, connect, notifications)
- New: `android/app/src/test/java/io/github/juvenile454/radarsensor/reference/HrMeasurementTest.kt`
- Change: `AndroidManifest.xml` (`BLUETOOTH_SCAN` with `neverForLocation`, `BLUETOOTH_CONNECT`), `record/Recorder.kt`,
  `core/RadarController.kt`, `ui/Pages.kt` (vital tab: reference curve), `debug/DebugReceiver.kt` (`ref_scan`,
  `ref_connect --es addr <mac>`, `ref_status`)

**Interfaces:**
- `data class HrMeasurement(val bpm: Int, val rrMs: List<Double>, val contact: Boolean?)`
- `fun parseHrMeasurement(bytes: ByteArray): HrMeasurement?` (characteristic `0x2A37`: flags bit 0 = 16-bit pulse,
  bits 1–2 = skin contact, bit 3 = energy field present (skip 2 bytes), bit 4 = RR values follow, each uint16 LE)
- `Recorder.reference(rxNanos: Long, m: HrMeasurement)` → `ref_hr.csv` with header `rx_ms,hr_bpm,rr_ms,contact`, one line
  per RR value (without RR: `rr_ms` empty); `rx_ms` as in `rxtime.csv` (ms since recording start, `elapsedRealtimeNanos`).

- [ ] Write tests: `parses8BitHrWithRr` (`10 48 00 04` → 72, `[1000.0]`), `parses16BitHrTwoRr`
  (`11 48 00 00 04 00 03` → 72, `[1000.0, 750.0]`), `skipsEnergyExpended` (`18 48 10 00 00 04` → 72, `[1000.0]`),
  `contactBits` (flags `06` → true, `04` → false, `00` → null), `rejectsTooShort` (`10` → null).
- [ ] `./gradlew :app:testDebugUnitTest` → the new tests fail; implement the parser → green.
- [ ] BLE client, recorder file, reference curve in the vital tab, debug commands.
- [ ] Build: `cd android && ./gradlew :app:testDebugUnitTest :app:assembleDebug` green.
- [ ] On the phone (only with approval): 10 min `iq3 0 raw` with strap. **Done when:** `ref_hr.csv` exists, sum of the RR values
  = recording duration ±2 %, pulse in the app = display of the Polar app ±1 /min.

#### V0.2 Markers and metronome breathing (breathing reference)

A known breathing rate is the simplest breathing reference: the app sets the beat (tone), the person breathes along.

- Change: `record/Recorder.kt`: `fun mark(rxNanos: Long, text: String)` → `events.csv` (`rx_ms,text`).
- New: `ui/Metronome.kt` (ToneGenerator, beats 6/10/15/20 /min; each inhale tone → `mark(…, "ein")`).
- Change: `ui/Pages.kt`: buttons "Atem anhalten" (breath hold, start/end), "Bewegung" (motion), "Zählfenster" (count window), metronome.
- Test: `RecorderTest.writesEventsCsv` (temporary folder, two markers → two lines in order).
- **Done when:** test recording contains `events.csv`; spacing of the metronome markers = nominal value ±20 ms.

#### V0.3 Measurement plan reference dataset

Enter it in `docs/EXPERIMENTS.md` as **M5**. Every session with strap, `iq3 0 raw`, markers; storage
`~/Data/radar-sensor/ref/<date>_<session>/`.

| Session | Content | Duration | tests |
|---|---|---|---|
| R1 | sitting still, normal breathing, 0.6 m | 5 min | Basic accuracy |
| R2 | 3 × 20 s breath hold | 3 min | Apnea, pulse without breathing |
| R3 | Metronome 6 / 10 / 15 / 20 /min | 2 min each | Breathing, harmonic collision |
| R4 | 1 min squats, then recover sitting | 6 min | Pulse range 110 → 70, response time |
| R5 | Raise arm, lean back, cough, talk | 3 min | Motion, state, confidence |
| R6 | Distance 0.4 / 0.6 / 0.8 / 1.0 m | 3 min each | Range |
| R7 | without lens / hyperbolic lens | 3 min each | Lens |
| R8 | lying down, sensor above the chest | 5 min | Posture |

- **Separation:** Day A = development (may be evaluated any number of times), day B = test (only for accepting a phase).
  Other people only with consent; data pseudonymous (`p1`, `p2`).
- **Done when:** plan entered, both days recorded, `esp_record.py --check` without gaps.

#### V0.4 Evaluation against reference: `host/vital_eval.py`

**Files:** new `host/vital_eval.py`, new `host/tests/test_vital_eval.py` (`unittest`, no new dependency).

**Interfaces:**
- `load_recording(folder: Path) -> Recording` (dataclass: `vital` [rx_ms + 15 fields], `dsp` [rx_ms + fields],
  `beats_ms` [beat times from `ref_hr.csv`: per notification, RR summed backward from `rx_ms`], `events`).
- `ref_hr_at(beats_ms: np.ndarray, t_end_ms: float, win_s: float = 15.0) -> float` (60,000 / mean IBI in the window).
- `metrics(est: np.ndarray, ref: np.ndarray, valid: np.ndarray) -> dict` with `n, coverage_pct, mae, rmse, bias,
  loa_low, loa_high, p5, p10` (Bland-Altman limits = bias ± 1.96 SD).
- `calibration(conf, abs_err, edges=(0, 30, 50, 70, 90, 101)) -> list[tuple]` (per confidence class: count, share ±5).
- Command line: `vital_eval.py <folder>… [--replay <folder with a121_esp_dsp.h>] [--md <report.md>]`. With `--replay`,
  `stream.log` is first recomputed with `host/dsp_replay/` and the given header (variants comparable);
  for this, `run_replay_cpp()` in `host/a121_esp_ultimate.py` takes the header folder as a parameter.
- Output per session and overall: pulse (MAE, ±5, Bland-Altman, availability, calibration), breathing against metronome,
  apnea detection against markers (lag, false alarms), time to first value, response time in R4.

- [ ] Tests: `test_ref_hr_constant` (beats every 800 ms → 75.0 ± 0.01), `test_metrics_basic` (estimate 70/72/80,
  reference 70/70/70 → MAE 4.0, p5 66.7), `test_coverage` (valid 1/0/1/1 → 75.0), `test_calibration_bins`.
- [ ] `.venv/bin/python -m unittest host/tests/test_vital_eval.py` → first red, green after implementation.
- [ ] **Done when:** a report is produced for an R1 recording and `--replay` with the unchanged header gives the same
  numbers as the live values (up to rounding).

#### V0.5 Test bench as regression test

- Change: `host/vital_sim.py` (scenario `leer` = static echo without breathing and pulse is already there): scenario "no
  person" (only furniture and wall) and `--check`: fixed limits per scenario (today: values of the table in section 4), exit code 1 on degradation. Every later DSP task
  tightens the limits that it improves.
- **Done when:** `vital_sim.py bench --check` with today's header returns exit 0 and with a deliberately
  degraded header (e.g. `kHrFMin` 1.0 Hz) exit 1.

#### V0.6 Baseline

- Evaluate today's firmware (`2026-11-01-app1`) on day A and day B, result as "baseline" in M5.
- **Done when:** table with MAE, ±5 share, availability, calibration per session in `docs/EXPERIMENTS.md`.

#### V0.7 (optional) Public datasets

- `host/vital_eval.py --public <folder>`: convert a CW IQ dataset (Schellenberger et al. 2020, 5.2) to the `iq3` format
  (scale phase × 12.5 / 4.955 to 60 GHz, resample to 60 Hz, as 24 identical zoom points with noise) and
  evaluate with the replay. Shows whether a change holds only for one test subject.
- **Done when:** report on ≥ 10 people of the dataset for the baseline and each V1 version.

**Decision point after V0:** If the pulse in R1 is already ≥ 90 % within ±5, the focus shifts
to availability and speed (V2). If it is below that, V1 first.

---

### Phase V1 — Targeted fixes in `a121_esp_dsp.h`

Each task = one commit. Check per task: test bench (`--check`), day A evaluation (`vital_eval.py --replay`),
5 min live. Acceptance of the phase on day B.

#### V1.1 Circle fit instead of mean (B2)

- New in the header: `static bool circleCenter(const float *ring, uint16_t start, uint16_t N, float mean_i, float mean_q,
  float &ci, float &cq)`: Kåsa fit in Bullock form, sums in `float` over (u, v) / s with s = mean distance to the
  mean (so that `float` suffices, no `double` on the ESP32). Fall back to the mean if the matrix is poorly
  conditioned (det ≤ 10⁻⁶ · Suu · Svv) or the center is more than 10 × s from the mean.
- `binDisplacement()` uses the center; it still returns the mean distance to the (new) center.
- Arc check: if the phase sweeps < 2 rad in the window, the fit is poorly conditioned (literature 5.1) → keep the last
  good center of this point and halve its weight.
- Compare variants on day A: Taubin fit (same effort, less bias on short arcs) and 3–5
  Gauss-Newton steps on the geometric distance. Adopt whatever is better on day A.
- **Important:** In the test bench the circle fit alone makes `tachy` worse (60 instead of 120). Put V1.1 on the device only together with V1.3.
- **Done when:** test bench `harm4`, `flach`, `tief` ≥ 90 % within ±5 (today 0 / 54 / 60 %), `base` ≥ 97 %;
  day A: MAE in no session worse than the baseline, better overall.

#### V1.2 Pulse band 40–180 /min, 2nd heart harmonic (B5)

- New band-pass: `scipy.signal.butter(4, [0.6, 5.0], 'band', fs=60, output='sos')` (coefficients in `kBpSos`, comment
  with the call as today).
- Search range 0.67–3.0 Hz (40–180 /min), spectrum up to 6 Hz for the harmonic sum
  S(f) = P(f) + 0.5 · P(2f). Candidates by S(f), keep the band edge block.
- Computational load rises (more frequencies) → implement together with V2.2 (FFT) or coarsen the frequency grid above 3 Hz to
  0.02 Hz.
- **Done when:** test bench `brady` ≥ 90 % (today 0 %), other scenarios without degradation; day A R4
  (pulse above 110) captured.

#### V1.3 Candidate selection, tracking and confidence redone (B4, B6)

- Breathing harmonics: candidates ±0.04 Hz around m · f_breathing (m = 2..8, only if breathing peak/median ≥ 2) get S × 0.5.
- Tracking: a nearby candidate (±12 /min) wins only with S ≥ 0.5 · S_max. If the tracked value is on a
  harmonic and for 3 reports there is a candidate without a harmonic with S ≥ 0.5 · S_tracked, switch.
- Confidence redone: `static float signalQuality(const QualityInputs &q)` with
  `struct QualityInputs { float peak_db; bool on_harmonic; float hr_spec, hr_acf, hr_ibi, hr_fused, hr_fused_5s_ago;
  float motion_pct; }` → 100 × clamp((peak_db − 3) / 12) × (0.5 on a harmonic) × (1 / 0.7 / 0.5 depending on agreement
  spectrum–ACF–IBI ±5 /min) × (1 − motion_pct / 100) × (0.7 if the value changed by > 8 /min within 5 s).
  Tune the factors on day A, then freeze them.
- The share of clean 1 s blocks also enters the confidence; below 70 % the report is invalid (literature 5.1).
- Sign-of-life test (B19): `valid` only if a breathing peak is present (peak/median ≥ 2) or the breathing state
  is "held" and the pulse peak is ≥ 8 dB above the median. Otherwise `valid = false`.
- The old point agreement stays as a diagnostic value: at the end of `@DSP` as `point_agree_pct`.
- **Done when:** test bench: confidence median of the reports with > 10 /min error ≤ 40 % (today up to 98 %), of the correct ones
  ≥ 70 %; `tachy` ≥ 90 % (also with V1.1); scenario `leer` without a valid report; day A: calibration table rises with
  the confidence, confidence ≥ 70 % → ≥ 90 % within ±5.

#### V1.4 Breath hold and breathing state (B9)

- Ring with 60 values: RMS of the breathing signal (0.05–0.7 Hz) per second at the point with the strongest breathing.
- `br_state`: 0 normal, 1 no breathing peak, 2 breath held / apnea (amplitude ≥ 90 % below the median of the last
  60 s, for ≥ 10 s), 3 motion (motion_pct > 50), 4 shallow breathing (≥ 30 % below, for ≥ 10 s). Definition as in the
  sleep apnea literature (5.1). In state 2, `br_bpm` is 0. New at the end of `@DSP`.
- The app shows the state (event bar and vital tab).
- **Done when:** test bench `anhalt`: state 2 at the latest 12 s after onset, no state 2 in `base`, `step`, `flach`,
  `tief`; day A R2: all breath-hold sections detected, lag ≤ 12 s, no false alarms in R1.

#### V1.5 Sensor flags and gaps (B15, B16)

- `cmdIq3()` counts `frame_delayed`, `data_saturated` and tick gaps > 25 ms (without rezoom) per report and prints them at the end of
  `@DSP` (`n_delayed,n_sat,n_tickgap`).
- `calibration_needed`: `@WARN,calibration_needed`, restart the session with the same zoom window (like a rezoom),
  `@RECAL,t_ms`. `host/dsp_replay/dsp_replay.h` treats `@RECAL` like `@REZOOM`; the app parser knows the tag.
- Saturated frames in the window lower the confidence (factor 0.5 from 1 %).
- Vendor rule for recalibration: see section 1.5.
- **Done when:** 30-min run live: counters in the log, `replay-cpp` follows `@RECAL`, no abort; Ctrl+R test unchanged.

#### V1.6 IBI and RMSSD (B7)

- Refine peaks parabolically between samples; refractory time 0.6 × (60 / `hr_fused`) instead of fixed 360 ms; discard IBI
  if it is > 20 % from the median of the last 5 IBI (artifact rule, 5.1). RMSSD only if ≥ 90 %
  of the expected beats in the window are valid, otherwise 0. Beats on the wider band from V1.2 (0.6–5 Hz). For HRV later
  a separate 2–5 min window (short-term HRV values are defined that way).
- App: RMSSD chart "experimental, not validated", hidden at 0.
- **Done when:** test bench RMSSD measured/true 0.7–1.3 in `base`, `step`, `flach`, `tief` (today 1.4–1.7);
  day A: IBI evaluated against strap RR (median error documented, set the target only afterwards).

#### V1.7 Cleanup (B18)

- Remove `hist_um_`, use or remove `kRespMinWin`, correct the description of the ESP32 chain in README/HARDWARE.

---

### Phase V2 — Speed and availability

#### V2.1 DSP on the second core (B12)

- `a121_esp_dsp.h`: `struct ReportSnapshot { uint16_t write_idx, count; uint32_t frame_count; uint8_t best_zoom_bin;
  float room_amp[kRoomPoints], room_mot[kRoomPoints], room_amp_slow[kRoomPoints], room_mot_slow[kRoomPoints]; }`,
  `void takeSnapshot(ReportSnapshot &s) const`, `bool computeVitalReport(const ReportSnapshot &s, VitalReport &rep)`.
  The old signature `computeVitalReport(VitalReport &)` stays (snapshot + compute) for the replay and the test bench.
- `kHistoryLen` 960 → 1080 (18 s): the report may compute for up to 3 s without its window being overwritten.
- `main.cpp`: task `dspTask` with `xTaskCreatePinnedToCore(…, 0)`; the read loop (core 1) puts a snapshot into a queue (length 1)
  every 60 frames, prints results as soon as they come back, and performs rezooms. A rezoom
  waits for the running result (it is decided from it anyway).
- `@DSP` gets `snap_frame` at the end (frame counter of the snapshot); `dsp_replay.h` computes the report at this
  frame (otherwise as today at the `@DSP` line).
- **Done when:** live `iq3 60` and 30 min: largest tick gap without rezoom ≤ 25 ms, receive pause ≤ 50 ms, no
  watchdog restart; `replay-cpp` equal up to the last digit.

#### V2.2 Reduce compute time (B13)

- Compute the Hann window and frequency coefficients once in `begin()`.
- `removeLinearTrend()` and `binDisplacement()` in `float` (Σt² computed in closed form).
- `static inline float fastAtan2(float y, float x)` (polynomial, error ≤ 1 · 10⁻⁴ rad ≈ 0.04 µm); test against `atan2` on
  a grid in the test bench.
- Spectra by real FFT (1024 points, zero-padded, 15 Hz → 0.015 Hz per bin, parabolically refined) instead of Goertzel.
  One portable FFT in the header (`a121_fft.h`) so that ESP32, PC and phone share the same computation.
- **Done when:** `compute_us` live median ≤ 80 ms; test bench within the limits; day A equally good (±0.3 /min MAE).

#### V2.3 Rezoom without loss (B11)

1. **Analysis first (offline):** In the 30-min recording T1 (38 rezooms, `@RAW`), check whether the phase at the same
   distances stays continuous across the session restart, with and without loopback derotation. Small script
   `host/rezoom_check.py`. Result in M5.
2. Keep the tracking: `resetHistory()` separate from `resetState()`; `hr_prev_` stays if the last valid value is
   < 30 s old.
3. Wider zoom window: `kZoomPoints` 24 → 48 (120 mm, 129 points per frame, ≈ 31 kB/s payload). Adjust:
   preset, `@RAW` docs, `host/esp_record.py` (`ZOOM_POINTS`, `TOTAL_3SUB`), app docs in `EspMsg.kt`, replay.
   Spectra only for the 16 points with the strongest breathing (compute time).
4. If step 1 shows continuity: on rezoom, take over overlapping points, fill the ≈ 0.4 s gap linearly.
- **Done when:** 30 min live: ≥ 95 % of the reports valid (today 86 %), rezooms per 30 min documented; day A without
  degradation.

#### V2.4 Fast start (B14)

- Growing window from 6 s (confidence × N/900), start the tracking with the previous value after a rezoom (V2.3).
- Target search at start: evaluate 2 s of room and zoom immediately, instead of waiting for 3 reports of the smoothed profile.
- **Done when:** test bench first value ≤ 6 s; live first valid pulse ≤ 10 s after `iq3`.

#### V2.5 Response time

- Compare window length 10 / 12 / 15 s with the tracking from V3.2 on day A (R4); choose the shortest window without
  MAE loss > 0.5 /min.
- **Done when:** lag in R4 ≤ 7 s (time until the value has followed 50 % of a pulse drop).

---

### Phase V3 — Better algorithms (each variant A/B on day A, adopted only after confirmation on day B)

| # | Task | Core | Done when |
|---|---|---|---|
| V3.1 | Point selection and combination | Choose points by breathing SNR and amplitude; align points in phase after the circle fit and sum weighted (weight = amplitude²/noise) or 1st principal component of the best K = 8–16 points, then demodulate **one** channel instead of 24 single spectra; effective point count N_eff = (Σλ)²/Σλ² as a diagnostic value; room points (profile 3) at the person as a second channel and fallback on rezoom | MAE ≤ today at ≤ 50 % compute time |
| V3.2 | Tracking as Viterbi over the last 20 spectra | States 40–180 /min (1 /min), transition Gaussian σ = 3 /min per s, emission = normalized S(f); confidence = probability mass ±5 /min | fewer jumps > 15 /min, ±5 share ≥ today |
| V3.3 | Remove breathing harmonics in a guided way | Harmonic regression on the breathing phase (like `project_out_respiration_harmonics()` in the PC scripts), but skip m with \|m · f_breathing − f_pulse\| < 0.1 Hz | R3 (metronome) better, R1 not worse |
| V3.4 | Breathing breath by breath | Peaks/troughs of the low-pass filtered path (0.05–0.8 Hz) with hysteresis; rate = 60 / median of the last 4 intervals; band 4–42 /min; new line `@BREATH,t_ms,tick,intervall_ms,tiefe_um` | R3 all beats (also 6 /min) ±1 /min; first value after 2 breaths |
| V3.5 | State of the measurement | KEINE_PERSON / SUCHE / ERFASSUNG / GÜLTIG / BEWEGUNG / APNOE (no person / search / acquiring / valid / motion / apnea) from room profile, zoom amplitude, motion share, `br_state`; new line `@STATE,t_ms,zustand,seit_ms` | Test bench `leer` without valid pulse; R5 motion sections as BEWEGUNG |
| V3.6 | Single beats with template | Band 0.6–8 Hz, template from averaged beats, correlation filter, refined peaks | IBI error against strap RR halved compared to V1.6 |
| V3.7 | Small ML model (PC/phone only, research) | only with ≥ 5 h of reference data from more than one person | — |

---

### Phase V4 — Measure the sensor configuration (experiments G1, G5, M3)

| # | Measurement | Setup | Result decides |
|---|---|---|---|
| V4.1 | Path noise, static | Metal plate 0.6 m, 60 s each with `sess`: zoom profile 1/2/3 × HWAAS 16/32/64/128 × 30/60 Hz; loopback with 15.6 MHz instead of 19.5 MHz, loopback profile 1/4/5 (2 is forbidden), gain 10/16; derotation raw / low-pass / off; phase before and after session restart | Noise floor (µm RMS in the band 0.6–5 Hz), benefit of the loopback derotation, continuity for V2.3 |
| V4.2 | Person with strap | 3 best configurations from V4.1, R1 each | `iq3` preset (profile, HWAAS, frame rate) |
| V4.3 | Lens and distance | without / hyperbolic × 0.4 / 0.6 / 0.8 / 1.0 m | recommended setup, range for pulse |
| V4.4 | Zoom width | 24 versus 48 points | V2.3 step 3 |

Data rate stays below 50 % of the link limit (≈ 100 kB/s), otherwise do not adopt.

---

### Phase V5 — App and phone

| # | Task | Done when |
|---|---|---|
| V5.1 | Show quality state: pulse gray at confidence < 50 % or motion, "acquiring x s", breathing state, RMSSD only experimental; reference curve and deviation live (V0.1) | Screenshot per state from a playback |
| V5.2 | Live DSP on the phone from `@RAW` (A6): same header via JNI (`nativePushFrame`, `nativeReport`), heavier variants (V3.2, V3.3, longer windows) only there via `#define A121_DSP_HEAVY`; ESP32 and phone value side by side | 30 min live, phone compute time ≤ 20 % CPU of one core |
| V5.3 | PC scripts: mark old variants as "archive" in the header comments; new evaluations only via `vital_eval.py` and header variants | Docs adjusted |

---

### Order and effort

| Step | Tasks | Effort (rough) | needs |
|---|---|---|---|
| 1 | V0.1, V0.2 (app) in parallel with V0.4, V0.5 (PC) | 2–3 evenings | Polar H10 (≈ 80–90 €) |
| 2 | V0.3 recordings day A and B, V0.6 baseline | 2 sessions of 1 h | 1 |
| 3 | V1.1 + V1.3 together, then V1.2, V1.4, V1.5, V1.6, V1.7 | 3–4 evenings | 2 |
| 4 | V2.1, V2.2 | 2 evenings | 3 |
| 5 | V2.3 (analysis first), V2.4, V2.5 | 2–3 evenings | 4 |
| 6 | V4.1, V4.2 (can run in parallel from step 2) | 2 sessions | 1 |
| 7 | V3.x as needed, V5.x | open | 3–5 |

Acceptance of each phase: day B evaluation in `docs/EXPERIMENTS.md` (M5), device test reported separately from the build.

## 7. Open questions and risks

- **One person, one room:** All data so far comes from one test subject at a desk. Thresholds (room targets, motion,
  confidence) may be wrong for other bodies, clothing, postures. Countermeasure: R6–R8, other people if
  possible.
- **The reference itself:** The strap measures electrically (ECG R wave), the radar mechanically (chest motion); between the two
  there is a delay of some 10 ms, and it varies. Irrelevant for pulse in a 15 s window; for IBI comparisons
  align them in time (cross-correlation of the IBI sequences).
- **Bit-identity:** Circle fit, fast `atan2` and FFT change all outputs; the replay stays PC = phone
  byte-identical, against the ESP32 as today only up to the last digit. Deviations larger than today are a bug.
- **Overfitting:** Tune parameters only on day A; day B only for acceptance. Test bench limits are no substitute.
- **Cable and mobile setup:** Cable motion creates artifacts (ANDROID_APP.md); phone accelerometer (idea I4) as a
  later motion marker.
- Not a medical device; pulse stays an experimental value, even if the target values are reached.
