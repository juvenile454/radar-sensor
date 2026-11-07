# Concept — 60 GHz radar on an Android phone

As of 2026-11-07. Status legend: ✅ verified on the device, 🧪 PC/simulation only, ⏳ open.

| Area | Status | Details |
|---|---|---|
| Phase 0 | ✅ | Done. |
| Phases 1 and 2 | ✅ | Largely built out. 3-subsweep sparse IQ with on-chip loopback @ 60 Hz and the sliding 7 s Hilbert breathing projection run on PC and ESP32. |
| Vital values / reference | ⏳ | Vital values exist, no accuracy claim yet. The app records an ECG chest strap (Polar H9) alongside the radar; comparison recordings are still open. |
| Phases 3 and 4: ESP32 ↔ Exploration Server | ✅ | The ESP32-S3 talks to the full Exploration Server over the SDA/SCL wires (`GPIO15`/`GPIO16`); own build from the Acconeer SDK v1.13.0 on LPUART1, `firmware/stm32l431/`. Verified on the device on 2026-11-01 with the A121 on the PC USB: `srv_info`, `stm_reset`, `iq3` (58 Hz), `doppler` (31 Hz) and `bridge` with exptool (2 Mbaud). |
| Server hardening | ✅ | Hardened against UART errors (restart instead of going deaf). |
| Mobile setup | ✅ | Verified the same way with the A121 on the red 5V wire; server link 2 Mbaud, net ≈ 195 kB/s. |
| `sess` command | — | Any sparse IQ session (up to 4 subsweeps, CSM up to 21 kHz sweep rate) can be configured and evaluated directly on the ESP32. |
| Automatic reconnect | ✅ | New on 2026-11-01: automatic reconnect of the server (`@RECONNECT`). The compute pauses of the on-device DSP bundle frames (receive pauses up to 0.34 s) but lose none (see `docs/HARDWARE.md`). |
| Android app (USB) | ✅ | Runs on the phone: live views, background recording (31.7 min with the screen off, no frame loss), reconnect after unplugging (`docs/ANDROID_APP.md`). |
| BLE chest-strap reference | ✅ | Polar H9 over BLE recorded by the app (`ref_hr.csv`); 10 min test on the phone on 2026-11-07 OK (`docs/ANDROID_APP.md`). |
| BLE transport ESP32 → phone | ⏳ | Not started. |

Focus: **make full use of the A121** — with the ESP32-S3, without the Pico.

## Goal

Learning and tinkering project: wire up, solder and program a radar sensor + microcontroller myself, and show the data
live over USB-C on the Galaxy S26 Ultra. First get everything out of the A121 alone; extensions come after.

## Reality check

| Goal | Expectation | Reasoning |
|---|---|---|
| Distance, presence, motion | ✅ | Ready-made detectors in the A121 firmware |
| Breathing rate | ✅ | Acconeer reference app `breathing` |
| Pulse | ⚠️ experimental | A heartbeat moves the chest only ~0.1–0.5 mm, breathing several mm. Needs phase evaluation of the raw data (sparse IQ); only while sitting still, ~30–80 cm, sensor on the chest, ideally with a hyperbolic lens. Own algorithm, validation against a smartwatch. |
| Human outlines live | ❌ | A121 = 1 transmit/1 receive channel → distance only, no direction. Even 3–4 Rx channels give only coarse points. |
| Image (static) | ✅ later | Sensor on a linear axis → synthetic aperture (SAR). Static scenes only; on hold (mechanics). |
| Through wood/drywall | ⚠️ | Thin, dry wood, drywall, plastic, fabric: partially. Brick, concrete, tile, metal, wet material: practically no. |
| Android over USB-C | ✅ | Phone as USB host, device as serial interface (CDC-ACM), app with `usb-serial-for-android` |

Physics cheat sheet: range resolution ≈ c/(2·bandwidth). 24 GHz ISM (250 MHz) ≈ 60 cm,
60 GHz sensors a few cm. Angle/direction only with several antennas or a moving sensor (SAR).

## Sensor decision

**Waveshare A121 Range Sensor Box** (Amazon `B0GJBZSY7P`, €48.99 on 2026-10-27).
Standard version without holder/lenses: `B0GJBQ7RQK`, €40.99.

- Acconeer A121 (60 GHz, pulsed coherent radar) + STM32L431CBT6 — electrically like the XM125,
  flashable as "XM125" in the Exploration Tool.
- USB-C via CH342 (2 serial channels), MX1.25 10-pin connector (UART, I2C, INT, WAKE), 5 V / max. 80 mA.
- Box version: holder + hyperbolic lens, Fresnel lens, flat cover.
  The lens focuses the beam → better for pulse; without a lens the beam is wide → room coverage.
- No SMD soldering needed; a soldering iron is enough.

Rejected/on hold: 24 GHz modules (coarse resolution, hardly any raw data), HLK-LD6002
(ready-made pulse values, no raw data), Infineon BGT60TR13C / TI IWR6843 (angle possible, more expensive).

## Architecture

```
A121 board ──MX1.25: UART + I2C──► ESP32-S3 ──USB-C (native, CDC)──► Galaxy S26 Ultra (App)
     │
     └─USB-C (CH342) ► Linux PC: Exploration Tool, recordings, algorithms
```

| Part | Task |
|---|---|
| STM32 on the A121 board | First the Waveshare/Acconeer firmware; later a custom "IQ streamer" firmware (C, Acconeer SDK, free account required), binary over the connector UART |
| ESP32-S3-DevKitC-1 N16R8 (clone, IN-OUT bridge closed) | The only main controller: control the sensor, buffer/check frames, pass them on to the phone; later preprocessing, BLE |
| Android app | Kotlin/Compose, `usb-serial-for-android`, live plots: distance, amplitude/phase, breathing, pulse; recording in the PC format (status and plan: `docs/ANDROID_APP.md`) |
| Linux PC (Python) | Exploration Tool (`acconeer-exptool`), recordings, algorithm development before anything is ported to the phone |

Rough data rate: 40 measurement points × 4 bytes × 100 Hz ≈ 16 kB/s — fits in 921,600 baud (~92 kB/s).

## Phases and completion criteria

| Phase | Content | Done when |
|---|---|---|
| 0 PC bring-up | Board on Linux via USB, Exploration Tool, first live data | Board connected, sparse IQ + distance live |
| 1 Capability matrix | Test all algorithms and limits systematically (`docs/EXPERIMENTS.md`) | Every row has a measured value or a reasoned "not possible" |
| 2 Pulse offline | Sparse IQ/phase recordings (sitting, 40/60/80 cm, with/without lens) + smartwatch reference; Python algorithm | Deviation from the watch measured and documented honestly |
| 3 ESP32 chain | Solder ESP32-S3 ↔ A121 (perfboard); I2C detectors on the ESP32; Android app skeleton; direct test A121 USB on the phone | App shows live values from the ESP32 (distance, presence, breathing) |
| 4 Raw data to the phone | Custom STM32 IQ streamer firmware → ESP32 bridge → app; pulse algorithm ported to Kotlin | App shows raw-data plots + breathing (+ pulse, if phase 2 holds up) |

## On hold

- **Pico:** not part of the project for now (not needed for the A121 focus).
- **SAR scanner:** linear axis + stepper motor + image reconstruction. Only after phase 4;
  decide then whether the ESP32-S3 or the Pico drives the motor. Never power the motor from the phone.
- BLE instead of a cable, battery, enclosure, custom PCB (KiCad), second sensor (bilateration).

## Extension ideas: phone sensors and Wi-Fi

As of 2026-11-01: **ideas only**, nothing built or measured. Order in the table = recommendation.

**Basis for all ideas:** From the start, the app records every data source with the same clock
(Android receive time `elapsedRealtimeNanos` plus ESP32 frame counter or sensor `tick`; for radar analyses
frame counter × frame rate, see `docs/EXPERIMENTS.md` M3, recording E). One recording = one folder with one file
per source plus metadata, exported to `~/Data/radar-sensor/`. Retrofitting later would be tedious.

| # | Idea | What the app shows | Benefit | Limits | Effort |
|---|---|---|---|---|---|
| I1 | Pulse via camera: finger on the rear camera + light (PPG) | Pulse curve and single beats next to the radar pulse | Comparison with the same clock, better than counting by hand | Not a measuring device itself → no accuracy claim; for that, still use a chest strap, oximeter or ECG | small–medium |
| I2 | All-round view: phone fixed to the sensor, turn slowly; gyroscope/compass provide the angle | Top-down room map (angle × distance), like a coarse lidar | Makes up for the A121's missing direction | Sharpness depends on the beam width (G6, angle not yet measured); sharper with a hyperbolic lens | medium |
| I3 | Wall map: move phone + sensor across a wall, position from camera tracking (ARCore) | Echo strength over the wall surface, e.g. behind drywall (U2) | Coarse imaging without a linear axis | Tracking accuracy probably cm (not checked) → no real SAR image, that would need < 1 mm (wavelength 5 mm) | medium–large |
| I4 | Phone accelerometer | "Sensor moved" marker on the time axis | Detect and discard motion errors in the mobile setup | Only useful if phone and sensor are rigidly connected | small |
| I5 | Wi-Fi sensing (CSI) on the ESP32, without a router (see below) | Waterfall diagram (strength of about 50 subcarriers over time) + motion index next to the radar | Compare the same motion at 2.4 GHz (coarse) and 60 GHz (mm-accurate) | Mainly sees motion close to the phone and ESP32 | medium–large |
| I6 | GPS | Location of the recording in the metadata | Only for measurements outdoors | Hardly useful indoors | small |

### I5: Wi-Fi sensing between ESP32 and phone

- **Setup:** Phone hotspot on **2.4 GHz** (the ESP32-S3 supports 2.4 GHz only). The ESP32 connects and pings
  the phone 50–100 times per second. On each reply it delivers the CSI (amplitude and phase per subcarrier).
  The values go into the app together with the radar data over the USB cable (raw about 13 kB/s at 100 Hz).
- **Roles:** Only the ESP32 can measure; Android does not expose CSI, only a coarse signal strength. The phone is
  the transmitter. The reverse (ESP32 as its own Wi-Fi network, phone connects to it) also works but is more awkward:
  Android reports "no internet", the app has to send the packets itself, and power saving makes the timing irregular.
- **Tooling checked (2026-11-01, not on the device):** The installed Arduino core (`framework-arduinoespressif32`
  3.20017, PlatformIO `espressif32@6.9.0`) has `CONFIG_ESP32_WIFI_CSI_ENABLED=y`. No switch to ESP-IDF needed.
  Official examples: Espressif `esp-csi`.
- **Limit:** Wi-Fi sensing mainly detects motion on and next to the transmitter–receiver path. At ~20 cm distance
  the direct link covers almost everything. Expected: hand near the device, walking past, standing up. Rather not: the whole
  apartment, through walls, breathing reliably. With a longer USB cable (1–2 m) and the person between phone and ESP32
  more is visible. Room-wide only with a transmitter at another location (router or second ESP32).
- **Open:** Does the hotspot stay on without mobile data? How much extra power does the ESP32 draw from the
  phone with Wi-Fi (transmit: peaks of a few 100 mA, estimate)? Is there enough compute time next to the DSP (already pauses
  up to ~300 ms today)?
- **Rules:** Own network only. Do not capture foreign devices, MAC addresses or content. Do not put hotspot credentials
  into the repo; enter them through the console. The radio module is only read out, not modified.
  Wi-Fi (2.4 GHz) and radar (60 GHz) do not interfere.
- **First test (before the app):** Console command `csi` on the ESP32 (connect to the hotspot, ping, output values over USB)
  and a PC script for the waterfall diagram.
- **Also possible:** The ESP32 scans Wi-Fi channels and networks live (Android apps may only do 4 scans in 2 minutes).
  Later it can also send the data wirelessly to the phone (see "On hold").

## Purchases

| What | When | Price |
|---|---|---|
| Waveshare A121 Range Sensor Box | delivered, in operation since 2026-10-30 | €48.99 |
| USB-C↔USB-C data cable (phone↔ESP32) | now, if not available | — |
| Test material: plywood, drywall offcut, cardboard, glass pane, metal sheet, tape measure | phase 1 | mostly available |
| Perfboard, 2.54 mm female headers | phase 3, for the fixed wiring | a few € |
| Multimeter | available (Benning MM 3, no auto-ranging) | — |

## Limits and rules

- Operate radio modules unmodified (no antenna/power changes).
- "Seeing through walls" only in your own rooms or with consent.
- Not a medical device; pulse values are experimental data.
