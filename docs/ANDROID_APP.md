# Android app — plan and status

As of 2026-11-01. Goal: see everything the A121 and ESP32-S3 deliver live on the Galaxy S26 Ultra, record it and
evaluate it. USB-C first, BLE later as a second transport.

**Status:** A0–A4 implemented and verified on the phone. A5 (optional) runs on the phone but is bit-identical only to
the PC, not to the ESP32 (see results). Debug build only, sideload onto my own phone only.

## Verified starting point (2026-11-01)

- **Phone:** SM-S948B, Android 17 (API 37), One UI 9.0; features `usb.host`, `bluetooth_le` (read via ADB).
- **USB OTG (A0):** C-to-C cable, the phone is power source + host, all power LEDs on. ESP32 detected as `303a:1001`
  "USB JTAG/serial debug unit" (serial no. `12:34:56:78:9A:BC`): interface 0 CDC control, 1 CDC data (bulk, 64 B),
  2 vendor (JTAG). Data flow verified since A3.
- **ADB:** wireless over Tailscale (`<Tailscale-IP des Handys>`); the port changes after every off/on, find it with
  `adb mdns services`. Always take the serial number live from `adb devices -l`.
- **ESP32 + A121 on the PC:** own Exploration Server `a121-v1.13.0`, 2 Mbaud. `iq3 10` → 595 frames in 10 s
  (59.4 Hz), in total ≈ 4.9 kB/s of text (log `2026-11-01_app_machbarkeit_iq3_probe.log`).
- **Tools:** Android SDK Platform 36/37, Build-Tools 37, JDK 17, Gradle 9.7.1; since A5 NDK `28.2.13676358`
  (default of AGP 9.4) and CMake 3.22.1.

## Key decisions

| Topic | Decision | Reason |
|---|---|---|
| Transport | USB-CDC with `usb-serial-for-android` 3.11.0 (MIT). `Transport` interface (USB, TCP for development, replay); BLE later as another implementation | Firmware stays usable as on the PC, bandwidth is enough for raw data |
| Protocol | The existing line protocol `@TAG,field,…` stays. Extend only with new tags or fields **at the end** | PC scripts read fields by position (`@VITAL` with a fixed field count) |
| Evaluation | The ESP32 keeps computing (DSP verified there). The app displays, records and recomputes; raw data optional. Recomputing on the phone (A5) uses the same firmware header, no second version | Do not maintain a second DSP version |
| Time base | Sensor `tick` + frame counter; Android receive time (`elapsedRealtimeNanos`) only in addition. After an ESP32 or server restart (counters start over) the app's time axis continues with an offset | After DSP pauses, frames arrive bundled (EXPERIMENTS M3, recording E) |
| Recording | One folder per recording: `stream.log` (lines exactly as from the ESP32, i.e. like the PC logs), `rxtime.csv` (line number → receive time), `meta.json` (device, source, `@HELLO`, commands with line number, start/end) | PC scripts read phone recordings without changes |
| Storage | App folder `Android/data/de.timo.radarsensor/files/recordings/`, fetched with `adb pull` over Tailscale to `~/Data/radar-sensor/phone/` | No storage permission, nothing in the repo |
| App skeleton | `android/`, package `de.timo.radarsensor`, Kotlin + Compose. AGP 9.4.0 (Kotlin built in), Kotlin 2.4.10, Compose BOM 2026.08.00 (as in OrbitLens). compileSdk/targetSdk 37, minSdk 34. Debug build only, sideload | Target device is API 37 and can be tested live; my own device only |
| Plots | Own Compose canvas drawings (lines, waterfall as a bitmap) | Smooth at 60 Hz, no chart library |
| Background | Foreground service type `connectedDevice` + WakeLock; after unplugging, the service waits for the device. "Trennen" (disconnect) ends everything and turns off automatic USB connecting. "Bildschirm an lassen" (keep screen on) is a setting | Samsung otherwise kills background apps; recording with the screen off verified (A4) |
| Reconnecting | Same source: counters/display stay, the most recently started stream (`iq3`, `doppler`, `sess run`) restarts. Different source: display is cleared | Unplugging restarts the ESP32 (the phone powers it) |

## Data → views

Tabs (the UI labels are German): Übersicht (overview), Signal, Raum (room), Vital, Doppler, Experten (experts), Diagnose (diagnostics), Konsole (console), Aufnahmen (recordings).

| Source (command) | Lines | Rate | View |
|---|---|---|---|
| `iq3` | `@IQ3` (time, temperature, loopback amplitude, path µm, zoom amplitude, saturation, frame, tick, late) | 60 Hz | Signal: path curve, amplitudes, saturation, late frames |
| `iq3 … raw` | additionally `@RAW` (105 complex int16 per frame, Base64; 129 with `A121_ZOOM_POINTS=48`) | 60 Hz | recording only (replay, PC) |
| `iq3` | `@ROOMPROF` (80 amplitudes + 80 motion values, 0.30–2.67 m) | 5 Hz | Room: profile + waterfall |
| `iq3` | `@ROOM` (noise, targets: distance, strength, motion, moving) | 1 Hz | Room: target markers; overview: main target |
| `iq3` | `@VITAL` (breathing, pulse fused/spectrum/ACF/IBI, IBI median, RMSSD, confidence, breathing depth, heart amplitude, SNR, loopback phase noise, target distance, number of targets) | 1 Hz | Overview/Vital; pulse **always as a "candidate"** with confidence |
| `iq3` | `@DSP` (valid, point, confidence, compute time, motion share, harmonic, point agreement, breathing state 0–4, late / saturated frames and tick gaps per report, frame count of the snapshot; with switch `A121_DSP_COMBINE` additionally the effective point count, V3.1) | 1 Hz | Vital + Diagnostics, breathing state in the Vital tab, "paused" as an event |
| `iq3` | `@STATE,t_ms,zustand,seit_ms` (0 no person, 1 search, 2 acquisition, 3 valid, 4 motion, 5 apnea; V3.5) | 1 Hz | Quality state in Overview and the Vital tab (V5.1): state text ("acquisition running x s", "valid for x s"), pulse gray at confidence < 50 %, motion or without a valid measurement |
| `iq3` | `@BREATH,t_ms,tick,intervall_ms,tiefe_um,rate` per breath (interval to the previous peak, depth peak − trough, rate = 60 / median of the last 4 intervals; only in acquisition/valid/apnea; V3.4) | per breath | Breathing breath by breath in the Vital tab (V5.1, "per breath" points in the breathing chart, last breath as a line) |
| `iq3` | `@IQ3_START`, `@REZOOM`, `@RECAL` (recalibration after `calibration_needed`), `@RECONNECT`, `@WARN` (among others `dsp_busy`: report dropped, `dsp_late`: report too late, invalid), `FEHLER…` (error), `@END` | event | Event bar, diagnostics |
| `doppler` | `@DOPPLER_START`, `@DOPPLER` (with frame, tick) | ~31 Hz | Doppler |
| `sess …` | `@SESS_META`, `@SESS_SUB`, `@PEAK`, `@AMP`, `@SESS_STAT`, `@SESS_END` | per session | Experts |
| `help`, `info`, `bus`, `srv_info`, `hello`, … | free text / `@HELLO` | — | Console (any command can be sent freely, quick commands, server restart test Ctrl+R) |
| `bridge` | raw protocol of the Exploration Server | up to ≈ 195 kB/s | not implemented |
| Mode B (`dist`, `vital`, `auto`) | `@F`, `@Z`, … | — | later; needs a different A121 flash |

Diagnostics: data rate, amount received, frame loss (gaps in the frame counter), reconnects and frame timeouts,
unreadable lines, DSP compute time, temperature, lines per day, events.

## Firmware extensions (ESP32, additions only) — implemented in `2026-11-01-app1`

- **F1** `hello` → `@HELLO,proto=1,fw=…,build=…,link=srv|i2c|frei,server=…,mac=…,up_ms=…,modes=…`; since V4 at the end
  `iq3=p1h32/lp1g16/19_5_MHz` (iq3 preset: zoom profile/HWAAS, loopback profile/gain/PRF).
- **F2** `@IQ3` and `@DOPPLER` extended at the end with frame counter and sensor `tick`.
- **F3** `iq3 [sek] [start_mm] raw` → per frame `@RAW,frame,tick,n,<Base64>` (105 points = 420 B), the app measures
  ≈ 41 kB/s in total instead of ≈ 5 kB/s.
- **F4** After 3 frame timeouts the ESP32 reconnects to the server (`@RECONNECT,t_ms,versuch,ok`); Ctrl+R (0x12)
  during a stream deliberately restarts the server for testing.
- **Reset trap resolved:** Only DTR = 0 with RTS = 1 resets the ESP32 (verified on the PC with `up_ms`). The app sets
  only DTR and never touches RTS; reconnecting on the phone without a reset verified. The app always reads, even without
  a recording (a TX timeout of 500 ms would otherwise slow down the DSP loop).
- PC scripts run unchanged (`--mode verify-cpp` and `--mode plot` verified with new logs); `tick_time()` in
  `host/a121_esp_ultimate.py` copes with `@RECONNECT` and a restarting `tick`.

## Phases

| Phase | Content | ESP32 plugged into | Done when (verified) | Status |
|---|---|---|---|---|
| A0 | USB basic test | phone | power + detection | ✅ 2026-11-01 |
| A1 | F1–F4; test recordings of all modes to `~/Data/radar-sensor/app-fixtures/` | PC | flashed, live runs ok, PC scripts still run unchanged | ✅ flashed and verified live |
| A2 | App skeleton; protocol module in pure Kotlin with JVM tests; replay of recordings; development relay `host/esp_tcp_bridge.py` (ESP32 on the PC → TCP only on the Tailscale IP → debug app) | PC | tests green; app installed; replay and live over Tailscale show plots (screenshot) | ✅ 28 JVM tests green; replay + TCP live by screenshot |
| A3 | Live over USB: detect device, permission, DTR/RTS without reset, start/stop, console, reconnect after unplugging | phone | 10 min `iq3` without frame loss per counter; unplugging/plugging in recovers; no unintended restart | ✅ (31.7 min without frame loss, see A4) |
| A4 | All views; recording in the background service; export | phone | 30 min recording with the screen off complete, fetched with `adb pull`, `host/a121_esp_ultimate.py --mode plot` evaluates it | ✅ |
| A5 (optional) | DSP header into the app via NDK; recompute raw data on the phone | PC + phone | result bit-identical to ESP32/PC | ⚠️ bit-identical to the PC, not to the ESP32 |

Later: BLE transport, pulse reference (as soon as a device is available), ideas I1–I5 from `CONCEPT.md`.

## Results (2026-11-01)

Pulse values in all recordings are rhythm candidates; no reference available.

- **A1:** Fixtures in `app-fixtures/`: `2026-11-01_iq3raw_20s` (1195 frames), `2026-11-01_iq3_30s_reconnecttest`,
  `2026-11-01_doppler_8s`, `2026-11-01_txt_*` (help, info, bus, srv_info, sess, sess run 5 stat/peak/amp, hello).
- **A2 (TCP over Tailscale):** Recording in the app `phone/2026-11-01_161356_tcp_iq3raw_test`: 2786 frames, 0 gaps,
  `@RAW` matches `@IQ3` (|IQ| ≤ 0.05 deviation = rounding); verify-cpp and plot run with it.
- **A3 (USB):** Permission via dialog, afterwards "Always open with Radar Sensor" set. All tabs live, plus
  `doppler 8` and `sess run 5 amp` via the console. Unplug test (`phone/2026-11-01_163341_usb_abziehtest`, unplugged and
  replugged 2× during the recording): the ESP32 restarts (`up_ms=732`), the app reconnects by itself and starts `iq3` again.
  17,042 frames in 3 streams, 0 gaps within the streams. Pause from last frame to new stream 34.9 s (with the
  permission dialog) or 8.2 s (without the dialog; includes the time without a cable, not measured separately).
- **A4 (30 min, screen off):** `phone/2026-11-01_164043_usb_iq3raw_bildschirmaus`, `iq3 0 raw` over USB,
  1902.7 s, 113,289 frames, 59.64 Hz by tick, **0 gaps, 0 reconnects**, ≈ 41 kB/s, `stream.log` 77.6 MB.
  Screen was `Dozing` during the checks. Largest tick gap 440 ms: all 38 gaps > 100 ms fall on
  `@REZOOM`. Longest receive pause 337 ms (DSP report on the ESP32: mean 270 ms, max. 323 ms). `esp_record.py --check`
  OK, `--mode plot` evaluates it (9440 room profiles, 1899 reports, 1632 `@VITAL`; image not inspected by hand).
  Battery 83 → 80 % (16:33–17:12, read off, not measured; the ESP32 hangs on the phone, which does not charge).
- **A5 (DSP on the phone):** `libradardsp.so` (arm64) from `host/dsp_replay/` + `firmware/esp32s3/src/a121_esp_dsp.h`.
  For the 30 min recording: phone 28.4 s, PC 8 s. **Phone and PC produce byte-identical results** (`dsp_phone.log`,
  md5 equal). Against the live output of the ESP32, 51,715 of 128,197 lines are equal:

  | Line | equal | largest deviation |
  |---|---|---|
  | `@IQ3` | 37,318 / 113,289 | path 0.21 µm (accumulated phase) |
  | `@ROOMPROF` | 9413 / 9440 | 1 in the last digit |
  | `@DSP` | 1877 / 1899 | 0.1 confidence (last digit) |
  | `@ROOM`, `@REZOOM` | 1899 / 1899, 38 / 38 | — |
  | `@VITAL` | 1170 / 1632 | breathing depth in 358 lines (76 more than 1 digit, max. 1.6 µm); IBI/RMSSD in 1 line (IBI 16.7 ms) |

  Suspected cause: different float math libraries (newlib on the ESP32, glibc/bionic). A variant with
  fused multiply-add instructions (FMA) changes almost nothing about this. The shim computes `atan2f`, `cosf`, `powf`,
  `log10f` via double; with that, PC and phone are equal, and against the ESP32 as close as the float functions allow.
  Since the final review of the vital plan, the firmware is also compiled with `-ffp-contract=off` (`platformio.ini`;
  before, GCC on the ESP32-S3 was allowed to fuse into `madd.s`, the recomputation was not).
  **Verified on the device 2026-11-02** (firmware `2026-11-02-vital1`, `iq3 60 raw` on the PC via `esp_record.py`, 60 s,
  `app-fixtures/2026-11-02_vital1_iq3raw_60s`): 3569 frames, 0 gaps, 59.5 Hz. `replay-cpp` against the live output:
  `@DSP` 59/59, `@VITAL` 17/17, `@STATE` 59/59, `@BREATH` 5/5, `@ROOM` 59/59, `@ROOMPROF` 297/297, `@REZOOM` 5/5
  equal; `@IQ3` 3531/3569 (rest 1 in the last digit). `compute_us` per report median 91 ms, max. 155 ms (before:
  270–320 ms in the read loop; now in its own task on core 0). Only 60 s and without a reference; 30 min run open.
  Same test through the app on the phone (USB, recording via debug command, `phone/2026-11-02_171254_livetest_vital1`):
  3594 frames, 0 gaps, 59.9 Hz; recomputation `@DSP`/`@STATE`/`@BREATH`/`@ROOM` equal, `@VITAL` 50/51 (1 in the
  last digit); `compute_us` median 152 ms, max. 155 ms (51 instead of 17 `@VITAL`, presumably a person in front of the sensor).

## Operation and tools

- **Build + test:** `cd android && ./gradlew :app:testDebugUnitTest :app:assembleDebug` (JVM tests read the
  fixtures from `RADAR_FIXTURES` or `~/Data/radar-sensor/app-fixtures`).
- **Install (only with approval):** `adb -s <serial> install -r app/build/outputs/apk/debug/app-debug.apk`.
- **Debug control without a screen** (debug build only, shell only because of permission `DUMP`):
  `adb shell am broadcast -a de.timo.radarsensor.DEBUG -n de.timo.radarsensor/.debug.DebugReceiver --es cmd <befehl>`
  with `status`, `usb`, `disconnect`, `send --es line "'iq3 0 raw'"`, `stop`, `rec_start --es name <name>`, `rec_stop`,
  `dsp_replay --es name <aufnahmeordner>`; ECG strap (V0.1) `ref_scan`, `ref_connect --es addr <mac>`, `ref_disconnect`,
  `ref_status`; markers and metronome (V0.2) `mark --es text anhalten_start`, `metro --ei rate 15` (6/10/15/20, 0 = off);
  phone DSP (V5.2) `phone_dsp --ez on true|false`, `phone_dsp_status` (pulse, state, CPU share, ESP32 value alongside).
  The reply is in the `data="…"` of the broadcast output.
- **ECG strap and markers (V0.1/V0.2, Vital tab):** "Gurt suchen" (scan for strap) finds devices with the Heart Rate Service (e.g. Polar
  H10), tapping connects; if the connection drops, Android reconnects by itself. Every notification goes into
  the running recording (`ref_hr.csv`: `rx_ms,hr_bpm,rr_ms,contact`, one line per RR value) and as a green "Gurt" (strap) curve
  into the pulse chart. The buttons "Atem anhalten" (hold breath), "Bewegung" (motion), "Zählfenster" (counting window) write start/end to `events.csv`
  (`anhalten_start`/`_ende`, `bewegung_…`, `zaehlen_…`, which is how `host/vital_eval.py` reads them); the metronome (6/10/15/20
  /min) beeps for inhaling and writes `ein` for each tone. Both files are created only with the first value, time axis like
  `rxtime.csv`. The app asks for Bluetooth permissions (scan without location) at startup.
- **Phone DSP live (V5.2, Vital tab):** "Handy-DSP ein" (phone DSP on) recomputes every received frame of an `iq3 … raw` stream
  on the phone with the same firmware header (library `libradardsp_live.so`, `host/dsp_replay/dsp_live.h`), but
  with the heavier variants (`A121_DSP_HEAVY` = Viterbi tracking V3.2 + subtraction of the breathing harmonics V3.3). It
  computes at the same points as the ESP32 (at its `@DSP` lines, from the same snapshot) and follows
  rezoom/recalibration; the values appear as a yellow "Handy" (phone) curve next to "fused" in the pulse chart, plus
  confidence, state, "Handy − ESP32" and "Handy − Gurt" (phone − strap) (60 s) and the CPU share of one core (thread CPU time per
  second). Switching on in the middle of a stream works (start and last rezoom lines are fed in afterwards, first value after
  ≈ 6 s). Own compute thread, reception does not wait. Off by default (battery). Not in the recording: the PC recomputes the same
  values from `stream.log` (`vital_eval.py --replay` with a header copy, `#define A121_DSP_HEAVY 1` at the top, as in `EXPERIMENTS.md`); phone = PC was
  byte-identical in A5, not verified for this variant.
- **Fetching recordings:** `adb pull /sdcard/Android/data/de.timo.radarsensor/files/recordings/<ordner>
  ~/Data/radar-sensor/phone/`. Writing into the app folder (e.g. fixtures for replay) only via
  `run-as de.timo.radarsensor`.
- **PC:** `host/esp_record.py` (recording in the app format, `--check` checks gaps/ticks/`@RAW`, `--npz` for verify-cpp),
  `host/esp_tcp_bridge.py` (ESP32 on the PC → app over Tailscale, port 7777), `host/a121_esp_ultimate.py --mode
  replay-cpp --log <stream.log>` (same recomputation as "DSP neu" (recompute DSP) in the app).

## Open

- BLE transport; measure battery consumption on the phone.
- Quality state (V5.1): logic (pulse gray, state text, deviation radar − strap over 60 s, pairs ±2 s) and parser
  `@STATE`/`@BREATH` tested on the PC; display (Pages.kt) not compiled. Acceptance "screenshot per state from a
  replay" open (phone or emulator).
- ECG strap, markers, metronome (V0.1/V0.2): checked on the phone on 2026-11-07 with a Polar H9 (S26, app 0.1.0, firmware
  `2026-11-02-vital1`, recording `phone/2026-11-07_105959_polar_h9_abnahme_10min_b`, 615.7 s `iq3 0 raw` with metronome
  15 /min): `ref_hr.csv` present, 616 notifications, 1071 RR, sum of RR = duration **+0.06 %**, no gap > 3 s, no RR
  outliers; metronome markers 3990.4–4010.9 ms (nominal 4000 ±20 ms); `esp_record.py --check` OK (36 896 frames,
  0 gaps, 59.98 Hz, `@RAW` consistent); pulse in the app = Polar app ±1 /min, confirmed by eye by the project owner (no
  logged comparison). The H9 leaves `contact` empty. Pitfalls: while the Polar Flow app is connected, the H9 does not advertise,
  so `ref_scan` finds nothing; `ref_connect --es addr <mac>` works anyway (Android shares the link between both apps;
  the address is in `adb logcat`). Any input during a running stream stops it (`@END`), so a new `iq3 … raw` only
  starts when sent in mode "bereit"; in a first attempt (`…_abnahme_10min`) the command only stopped the old stream.
  In that first attempt the first metronome interval was 3978.6 ms (start-up), all others within ±10 ms.
- Phone DSP live (V5.2): live path on the PC equal to the recomputation (`host/tests/test_dsp_live.py`, compute time on the PC
  0.29 % of one core for 120 s of simulated data with `A121_DSP_HEAVY`); JNI library compiled on the PC against the JDK `jni.h`
  and tested from Kotlin (feeding, restart, CPU share). Not compiled: NDK build, controller,
  debug commands, UI. Open on the phone (only with approval): 30 min `iq3 0 raw` live with "Handy-DSP ein",
  CPU share ≤ 20 % of one core (`phone_dsp_status`), phone and ESP32 value side by side (screenshot). Estimate from
  A5 (phone ≈ 3.5 × slower than the PC): ≈ 1 % — not measured.
- Button "DSP neu" (recompute DSP) under Recordings only verified via debug command, not looked at (phone was locked).
- Doppler tab: the amplitude axis labels end below the maximum (the lines themselves are not clipped).

## Risks and rules

- **Never both USB ports of the ESP32 at the same time** (IN-OUT bridge closed, two 5 V sources possibly connected).
- On the phone the phone does not charge; consumption unmeasured (roughly 1.5–2 W load).
- Unplugging restarts the ESP32; running streams start with a new frame counter and tick.
- Cable movement causes measurement artifacts (later the phone accelerometer, idea I4).
- Pulse remains a rhythm candidate; no accuracy claim without a reference measurement.
- Build/tests on the PC are not a runtime check on the phone; report the two separately.

## Approvals

- **Test phase (project owner, 2026-11-01):** install/update the radar debug app, start/stop, read logs,
  screenshots, confirm the USB access prompt via ADB. Do not touch other apps and settings.
- Flash the ESP32 only after identification via `/dev/serial/by-id`.
