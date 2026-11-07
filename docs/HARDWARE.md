# Hardware — Boards, Pins, Wiring

As of 2026-10-27. Check the pins on the real board before the first power-up.

## Parts on hand

| Part | Details | State |
|---|---|---|
| ESP32-S3-DevKitC-1 (possibly a clone), module WROOM-1 **N16R8** | 16 MB flash, 8 MB octal PSRAM, 2× USB-C (native + USB-UART), RGB LED, BOOT/RST | Pin headers soldered |
| Raspberry Pi Pico (RP2040, 2020 version) | Micro-USB, no radio | Pin headers missing; **on hold** |
| Galaxy S26 Ultra | USB-C, USB host (OTG) | — |
| Tools | Soldering iron (no hot air/reflow) | — |

Do not use: the ESP32-S3-CAM from `esp32-laser-triangulation` (belongs to the other project).

## Waveshare A121 Range Sensor

Sources: Waveshare wiki "A121 Range Sensor", product page, schematic PDF (read 2026-10-27).

- Supply 5 V, max. 80 mA; I/O 3.3 V.
- USB-C → CH342 (USB to 2CH TTL):
  - Channel A: STM32 main UART — Exploration Tool and flashing (BOOT button).
  - Channel B: debug UART — output of the UART firmwares; **the same lines are on the connector (TXD/RXD)**.
  - Consequence (per the schematic, untested): Exploration Server raw data only over USB; raw data to the ESP32 needs its own firmware on the debug UART.
- UART firmware: 921,600 baud. I2C firmware: address 0x52 (0x51/0x53 via solder bridge), 100/400 kHz.
- SWDIO/SWCLK pads, BOOT and RESET buttons present.

### Bring-up on the Linux PC (verified 2026-10-30)

- USB ID `1a86:55d2` "USB Dual_Serial", serial no. `<serial>` (differs per board), driver `cdc_acm` (no extra driver).
- `/dev/serial/by-id/usb-1a86_USB_Dual_Serial_<serial>-if00` (ttyACM0) = channel A, Exploration Server
  responds; `-if02` (ttyACM1) = channel B (debug).
- Factory firmware: Exploration Server, `rss_version a121-v1.12.0`, `hardware_name xm125`, max. 2,000,000 baud.
- Access needs the group `dialout` (the user is a member; takes effect after logging in again, until then `sg dialout -c …`).
- Python environment: `.venv` (Python 3.12, `acconeer-exptool[app]` 7.18.2).
- Sparse IQ (profile 3, 40 points, 8 sweeps, HWAAS 16): ~117 frames/s. Distance detector runs.
- **Cause of `LinkError: recv timeout` on the second connect found and fixed (2026-10-31):**
  `ExplorationClient._update_baudrate` in `acconeer-exptool` switches the STM32 from 115,200 to 2,000,000 baud after
  opening, but does not reset it on `close()`. On the next `Client.open()`, `ExploreSerialLink` opens at 115,200 baud
  again and sends a UART `send_break()`, which times out on the first attempt through the CH342 (`cdc_acm`). Fix in
  `host/a121_vitalaufnahme.py` and `host/a121_livemonitor.py`: `override_baudrate=921600`.

### MX1.25 10-pin connector (per the pinout graphic, order from the top)

| # | Signal | Function |
|---|---|---|
| 1 | 5V | Supply |
| 2 | GND | Ground |
| 3 | RXD | UART RX (into the STM32) |
| 4 | TXD | UART TX (out of the STM32) |
| 5 | SDA | I2C data |
| 6 | SCL | I2C clock |
| 7 | MCU_INT | I2C busy signal |
| 8 | WAKE_UP | I2C wake-up |
| 9 | GPIO0 | free |
| 10 | GPIO1 | free |

Pin numbers are not printed on the graphic; the schematic numbers them the other way round (5V = 10).
Confirm 5V/GND on the cable with a multimeter before soldering.

From the schematic netlist (H6, read 2026-10-30, not verified on the board): 1 GPIO1, 2 GPIO0, 3 WAKE_UP,
4 MCU_INT, 5 SCL, 6 SDA, 7 DEBUG_TX, 8 DEBUG_RX, 9 GND, 10 5V — matches the table above (reversed).
Further schematic facts: I2C pull-ups 4.7 kΩ to 3.3 V on the A121 board (R13/R14);
USB VBUS feeds the 5V net through Schottky diode D3 (MBR230) → 5 V from the connector does not flow back into the PC USB.
**WAKE_UP and MCU_INT are connected to the CH342** (added 2026-10-31 from the vector paths of the schematic PDF;
board not re-measured): net WAKE_UP = CH342 DTR0 (pin 23, output) + connector pin 3 + STM32 PA11;
net MCU_INT = CH342 DSR0 (pin 22, input) + connector pin 4 + STM32 PB9. Direct lines, no
series resistors, no pull-ups/pull-downs. CH342: VIO = 3V3 (3.3 V level), VDD5 **and** VBUS detection on the 5V net
→ the CH342 always runs, even without USB, and actively drives WAKE_UP. Connector pins 1/2 = MISC_GPIO1 (PA4) /
MISC_GPIO0 (PB1); NRESET and BOOT0 are not on the connector. Confirmed on the board: GPIO17 cannot be pulled against the line,
neither to HIGH (early 2026-10-31) nor to LOW (after restart). Consequence: the ESP32 does not control WAKE_UP; the
bring-up firmware only reads GPIO17 (`wake` drives only on command and releases immediately on resistance).
Schematic: https://files.waveshare.com/wiki/A121_Range_Sensor/A121_Range_Sensor_schematic.pdf
(sha256 `38e88c1ef357ba4f1ce77ac87b70f4a609cc197ace7a70b9a56eb02349627440`, not in the repo).

## Wiring ESP32-S3 ↔ A121 (plugged in and verified 2026-10-30)

I2C/INT/WAKE as in the Waveshare ESP32-S3 demos (verified in the demo code 2026-10-30: SDA 15, SCL 16,
WAKEUP 17, BUSY 18, address 0x52, 400 kHz). MCU_INT high = STM32 ready.

| A121 | ESP32-S3 | Purpose |
|---|---|---|
| 5V | 5Vin | Supply from USB (clone board: only with the IN-OUT bridge closed, see below) |
| GND | GND | — |
| RXD | GPIO4 (UART1 TX) | Commands → STM32 |
| TXD | GPIO5 (UART1 RX) | Data ← STM32 |
| SDA | GPIO15 | I2C |
| SCL | GPIO16 | I2C |
| WAKE_UP | GPIO17 | Wake-up – driven by the CH342 (DTR0), ESP32 only reads |
| MCU_INT | GPIO18 | Busy |
| GPIO0/1 | — | not connected |

Wire colors of the supplied MX1.25→Dupont cable (verified on the board 2026-10-30; function confirmed:
PWR LED on, all lines high, I2C responds):

| A121 | Wire | ESP32-S3 (DevKitC-1, left header J1) |
|---|---|---|
| 5V | red | 5Vin |
| GND | black | GND |
| RXD | blue | GPIO4 |
| TXD | yellow | GPIO5 |
| SDA | orange | GPIO15 |
| SCL | green | GPIO16 |
| MCU_INT | white | GPIO18 |
| WAKE_UP | purple | GPIO17 |
| GPIO0 | brown | free, insulate the end |
| GPIO1 | gray | free, insulate the end |

### Bring-up procedure (keep this order)

Firmware: `firmware/esp32s3` (PlatformIO, `pio run`), console over the native USB port, command `help`.
Flashing: `pio run -t upload --upload-port <by-id-Port>` (default environment without switches; V6 only with `-e esp32s3-n16r8-reacq`). Afterwards the S3 stays in download mode
(PIO esptool 4.5 cannot restart cleanly over USB-JTAG) → `esptool --chip esp32s3 -p <Port>
--after watchdog-reset read-mac` (esptool ≥ 4.8, `uv tool install esptool`) or press RST.
The first flash over the seller's demo only worked by hand with BOOT+RST (the demo does not use USB-JTAG mode).
2026-11-02: `pio run -t upload` failed twice, reproducibly, when switching to 460,800 baud ("No serial data
received", before writing). The same write sequence worked with esptool 5.6 without switching, with restart:
`esptool --chip esp32s3 --port <by-id-Port> --baud 115200 --after watchdog-reset write-flash -z --flash-mode dio
--flash-freq 80m --flash-size 16MB 0x0 .pio/build/esp32s3-n16r8/bootloader.bin 0x8000 …/partitions.bin 0xe000
~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin 0x10000 …/firmware.bin`
(in the folder `firmware/esp32s3`, after `pio run`). Access to the port needs the group `dialout` in the running
login; with linger active, only after a reboot.
Open the console with DTR/RTS unchanged, otherwise it resets into download mode.

1. **Check the cable:** Is an MX1.25→Dupont cable included in the box? If so, first plug it onto the
   ESP32 pin headers without soldering. Note the wire colors.
2. **Measure 5V/GND:** A121 on the PC via USB, multimeter at the cable end: 5V wire against GND ≈ 4.5–5 V
   (after diode D3). Then unplug USB again. Only then fix colors → signals.
3. **ESP32 alone:** native USB port to the PC, identify the port (`ls -l /dev/serial/by-id`),
   flash the bring-up firmware, `info` (16 MB flash, 8 MB PSRAM expected), `led 48`/`led 38`.
4. **Measure the ESP32 5V pin** while it is powered only through the native port (clone boards).
5. **Wire it up – everything unpowered:** table above. Cross RXD/TXD (A121 RXD ← GPIO4, A121 TXD → GPIO5).
6. **Power on only through the ESP32 USB.** The red LED on the A121 must light up. `scan`: with the factory firmware
   (Exploration Server), **nothing** is expected at 0x52.
7. **Flash the STM32 with I2C firmware** (only with approval from the project owner; reversible): A121 on the PC via USB,
   channel A, `python -m acconeer.exptool.flash flash XM125 -p /dev/ttyACMx -i vendor/firmware/A121_Firmware/i2c_distance_detector.bin`
   (`ttyACMx` = target of `…-if00`, the by-id path is not accepted). Back: the same line with `acc_exploration_server_a121.bin`.
   While the I2C firmware is on it, PC measurements with the Exploration Tool do not work.
   **Done 2026-10-30:** `i2c_distance_detector.bin` (sha256 `e659242d…44e4f`) flashed with approval from the project owner,
   100 % without errors. DFU mode only by hand: hold BOOT, tap RESET, release BOOT
   (the tool expects the port as `/dev/ttyACMx`, not the by-id path). Then RESET to start.
8. **Check I2C:** `scan` → 0x52, `ver` → VERSION/APPLICATION_ID readable.
   **Done 2026-10-30:** A121 powered only through ESP32 5Vin, all lines High (`probe`),
   `scan` → exactly one device 0x52; `ver`: VERSION 0x00010C00 (= 1.12.0), APPLICATION_ID 1,
   DETECTOR_STATUS 0x00000007, PROTOCOL_STATUS 0. Before flashing, SCL was low and MCU_INT low (factory firmware,
   probably phantom powering through GPIO4) – fixed with I2C firmware and TX high-impedance.
9. **UART (optional):** with an `example_*` UART firmware the STM32 prints text on the debug UART;
   `uart` on the ESP32 passes it through to USB. Also usable with `i2c_distance_detector`: command 32
   (`w 0x100 32`) switches logs on, 34 prints the configuration.
10. **Distance measurement over I2C** – see the next section. **Done 2026-10-31** (wall at 570 mm detected).

### I2C distance detector on the ESP32 (as of 2026-10-31)

Console (`help`): `info`, `bus`, `probe`, `scan`, `ver`, `cfg [start end]` (mm), `vital [start end]` (mm, default 450–850),
`zoom a b [p s t]`, `auto [sek]` (fully automatic mobile mode: `WIDE_SEARCH 150–2500 mm` → `TARGET_LOCK` → `VITAL_ZOOM` with auto rezoom),
`dist [n]` (up to 5000), `r <reg>`, `w <reg> <wert>`, `wake 0|1|z`, `uart`, `led 48|38`,
`link [i2c|srv]` (since the evening of 2026-10-31: I2C commands only with `link i2c`, server commands see below).
Registers/commands as in the Waveshare demo (`…/ESP32S3/Arduino/A121_Distance_Detector`): START 0x40, END 0x41,
MAX_STEP_LENGTH 0x42, CLOSE_RANGE_LEAKAGE 0x43, SIGNAL_QUALITY 0x44 (×1000), MAX_PROFILE 0x45,
THRESHOLD_METHOD 0x46 (1 fixed/amplitude, 2 recorded, 3 CFAR, 4 fixed/strength), PEAK_SORTING 0x47 (1 closest, 2 strongest),
FIXED_AMPLITUDE_THRESHOLD 0x49 (×1000), THRESHOLD_SENSITIVITY 0x4A (×1000), FIXED_STRENGTH_THRESHOLD 0x4C (×1000 dB),
COMMAND 0x100 (1 config+calibration, 2 measure, 32/33 UART logs on/off, 34 log config, 1381192737 module reset).

Verified and optimized (2026-10-31):
- **I2C clock & interrupt polling:** bus in `firmware/esp32s3/src/main.cpp` switched to **400 kHz**; `waitNotBusy()`
  polls the hardware pin `MCU_INT` (`GPIO18`, LOW = busy, HIGH = ready) at microsecond granularity instead of fixed
  `delay(10)`/`delay(5)` waits. This reduces the measurement time to ~24.5 ms (**37.1 Hz** continuous sampling rate at `dist 1800`).
- **Full strength resolution:** `dist` now prints `REG_PEAKn_STRENGTH` with the full **0.001 dB resolution** (`%.3f dB`).
- **The configuration takes effect only once after reset.** Later APPLY commands are ignored without an error message
  (status immediately 0x3FF, measurement unchanged). `vital [start end]` runs the module reset (`1381192737`) automatically.
- After a command, `waitNotBusy()` briefly waits for the falling edge of `MCU_INT` (max. 2 ms) and returns immediately
  as soon as `MCU_INT` is HIGH again.
- An ESP32 reset does not reset the A121 (registers are kept).
- Defaults after module reset: max profile 5, CFAR, sensitivity 0.5, signal quality 15, leakage compensation on,
  sorting "strongest".

**Wall test 2026-10-31 (solved):** A121 powered only through ESP32 5Vin, brick wall at 570 mm (tape measure),
hyperbolic lens, sequence module reset → `cfg 300 1000` (CFAR, otherwise default) → `dist 50`:
**50/50 measurements with exactly one target, median 575 mm, σ 1.3 mm, range 572–578 mm**, strength 2.3–3.1 dB,
measurement ~18 ms. (+5 mm vs. the tape measure, +16 mm vs. the PC reference 0.5585 m – different detector configuration,
not evaluated further.) Log: `~/Data/radar-sensor/2026-10-31_105545_esp_i2c_wand570.log`.
Detector configuration per the debug UART (`w 0x100 34`): 2 subsweeps, profile 1 (HWAAS 2) + profile 3 (HWAAS 1),
start point 68 and 120 respectively. Power through the ESP32 and the I2C chain therefore work for a strong target.

**Why the test subject was not detected on day 1 — cause found and solved with `vital` (2026-10-31):**
1. **CFAR sensitivity formula:** In `acconeer/exptool/a121/algo/distance/_processors.py` the mapping is
   `_sensitivity_to_standard_deviations = 8.0 - 7.0 * sensitivity`. Lowering `0.5` to `0.2` on day 1 **raised**
   the threshold from $4.5\,\sigma$ to $6.6\,\sigma$ (made it less sensitive).
2. **CFAR self-masking by `PROFILE_3`:** With the default `max_profile = 5`, the detector picks `PROFILE_3` from 300 mm
   (140 mm half-width, 280 mm CFAR guard window). An extended human torso + chair/table raises
   its own CFAR background that way.
3. **`close_range_leakage_cancellation` (`0x43 = 1`):** If the person already sits in front of the sensor during `cfg`,
   the close-range leakage compensation calibrates away parts of the static echo.
- **Solution (`vital [start end]` in `firmware/esp32s3/src/main.cpp`):** module reset → `0x43 = 0` (leakage calibration off),
  `0x42 = 1` (step length 2.5 mm), `0x44 = 25000` (signal quality 25.0 dB → high HWAAS), `0x45 = 1` (`PROFILE_1`,
  40 mm half-width cleanly separates the front of the chest ~675 mm from torso/shoulder ~730 mm), `0x46 = 4` (`FIXED_STRENGTH`),
  `0x47 = 1` (`CLOSEST`), `0x4C = -25000` (−25.0 dB). Calibration takes only 102 ms; hit rate on the test subject's chest
  at ~650–740 mm: **3608/3608 frames (100.0 %)**. Host script: `host/a121_esp_vital.py`.

**Hang at power-up 2026-10-31:** After plugging in the ESP32, the A121 did not
respond: `scan` empty, MCU_INT and WAKE_UP hard LOW (the ESP32 drives GPIO17 HIGH but reads 0; without the purple wire
GPIO17 is fine), the RESET button did not help. After a fresh power-up (first via A121 USB, then again through
ESP32 5Vin) everything ran normally, `probe` → all six lines HIGH.
Probable cause per the schematic (see above): the CH342 held WAKE_UP LOW through DTR0 → STM32 sleeps, RESET
changes nothing about that. A matching mechanism (hypothesis, not reproduced): according to the CH342 datasheet the chip
reads DTR0 at power-up as a configuration input; if the pin is then LOW (pull-down 3–8 kΩ), it switches to
TNOW0 (RS485 transmit indicator, LOW as long as nothing is being sent) – until the next power cycle. The trigger
could be the ESP32-S3: GPIO17 outputs active LOW for ~60 µs at power-up (ESP32-S3 datasheet v2.2,
table 2-2), and the ESP32 and A121 get power at the same time through 5Vin. The loose green wire (SCL) at the same
time is a less likely cause, but not ruled out.
Until the firmware change on 2026-10-31, `ver`/`cfg` drove GPIO17 HIGH – so that morning against
the CH342 (about 20 min, both sides 3.3 V push-pull). Since then the firmware no longer drives WAKE_UP; `ver`/`cfg`
only report a LOW.
Rule: after re-plugging a wire or on `WAKE_UP ist LOW`, power the A121 down and switch it on again,
then check `probe` (all HIGH) and `ver` before measuring. If that does not help: connect the A121 to the PC via USB briefly
(red wire disconnected beforehand).

### Low-power mode via ESP32 (analysis 2026-10-31)

Sources: Acconeer "I2C Distance Detector User Guide" (as of a121-v1.3.0; our firmware is 1.12.0 – procedure
assumed to be the same there, not verified), XM125 datasheet v1.2, CH342 datasheet 1E, Waveshare schematic.
- The low-power state of `i2c_distance_detector` is controlled **only through WAKE_UP**; there is no
  I2C command for it. Sleep: wait for MCU_INT HIGH → WAKE_UP LOW → MCU_INT goes LOW. Wake: WAKE_UP HIGH →
  wait for MCU_INT HIGH → I2C. Optional register 0x80 "Measure On Wakeup": measures by itself on every wake.
  XM125 datasheet (module, not this board): quiescent current hibernate 17.7 µA, off 6.9 µA.
- On this board the CH342 drives WAKE_UP (see schematic facts). Without a USB host, DTR0 sits at inactive = HIGH
  → **the A121 is permanently awake; the ESP32 cannot put it to sleep today.** Confirmed on the device: GPIO17
  cannot be pulled against the CH342 (`wake 0` → level stays 1).
- Brown/gray (MISC_GPIO0/1) play no role for the standard firmware in power saving; the fact that they are not connected to the
  ESP32 does not limit anything today. NRESET is not on the connector → if the A121 hangs (see above), the ESP32
  has no way to restart it.
- Baseline consumption of the board without A121/STM32 (estimate, not measured): PWR LED via 2.2 kΩ on 3V3 ≈ 0.6 mA,
  CH342 in suspend 0.09–0.16 mA (datasheet), plus quiescent current of the LDO RT9193 → roughly ~1 mA, so far more than
  the hibernate value of the module.

Ways for the ESP32 to control everything (none implemented):

| Option | Intervention | Gains | Drawbacks |
|---|---|---|---|
| A Load switch in the 5V wire (P-MOSFET + transistor, control pin GPIO6/7, LOW = off – both pins output ~60 µs LOW at power-up) | ESP32 side only, A121 unchanged | Board completely off (LED/LDO/CH342 too), A121 restart by the ESP32, fixed power-up order (ESP32 first) against the hang | boot + `cfg` + calibration after every power-up (duration unmeasured); before switching off, all lines high-impedance (phantom powering) |
| B Cut the DTR0 → WAKE_UP line on the A121 board | Cut a trace (location on the board unknown, photo needed) | real hibernate via GPIO17, cause of the hang gone | hard to undo; PC side loses DTR control (probably irrelevant) |
| C Own STM32 firmware: wake via MISC_GPIO0 (brown) instead of PA11 | brown to GPIO6/7; Acconeer SDK + ARM toolchain, flash the STM32 | Hibernate without soldering on the A121, reversible, fits phase 4 | Effort, SDK download (account/license), PA11 stays driven by the CH342 (ignored) |
| D ESP32 as USB host on the A121 USB-C, set DTR via CDC-ACM | no soldering; console moves to the UART port of the ESP32 | WAKE_UP controllable through the CH342 | ESP32 must stay awake as host → pointless for power saving; second cable on the A121 |

Next checks (open): measure the current in the red wire with the multimeter (today: permanently awake). Prove sleep/wake
with 1.12.0: A121 on the PC via USB (red wire disconnected), switch DTR on channel A (`…-if00`) with pyserial,
the ESP32 reads WAKE_UP/MCU_INT and talks I2C. Reproduce the hang: the ESP32 holds GPIO17 LOW with the lowest
drive strength, the red wire is plugged in → if WAKE_UP stays LOW after releasing, the TNOW0 hypothesis
is confirmed.

Day 1 (2026-10-30): sensor pointed at the test subject (distance not measured), range 100–1500 mm:
- CFAR (default, also sensitivity 0.2): over ~30 measurements **0 targets** (cause found on 2026-10-31, see above).
- Fixed amplitude threshold 1.0 / 3.0 / 10.0: 5–10 "targets" per measurement at changing distances (the ×1000 scaling was
  well below the noise floor of ~25–80 units).
- With `vital 450 850` (`FIXED_STRENGTH = -25 dB`, `PROFILE_1`, `step_length = 1`, `leakage_cancel = 0`) on 2026-10-31
  fully solved: 100 % hit rate on the chest at 37.1 Hz over ESP32 I2C (`host/a121_esp_vital.py`).

**Current firmware on the A121 (as of 2026-11-01, ~08:40):** **`acc_exploration_server_a121-v1.13.0_lpuart1.bin`**, hardened (SHA256 `b29e328d…1757`, restart on every server UART error, see "Hardening 2026-11-01"), replaces the version of 2026-10-31 (`5dfefa8f…3827`, kept as `…_lpuart1_2026-10-31.bin`). Own build from the Acconeer SDK, see below, flashed over channel A with approval from the project owner (100 %). Server only on LPUART1 = SDA/SCL wires; **USB channels A and B do not respond as a server** (verified). After RESET, "Acconeer Exploration Server" appears on channel B (921,600 baud, PA9) → firmware starts, the break wait loop is left. The LPUART1 path to the ESP32 has been verified on the device since 2026-11-01 (see "ESP32 test on the device 2026-11-01"). Before that: `acc_exploration_server_a121_usart1_esp32.bin` (until 23:50). For `vital`/`auto`/`zoom` (I2C), `i2c_distance_detector.bin` (`a121-v1.12.0`, 109,816 bytes) has to be flashed again. Note on USB flashing with the (unpowered) ESP32 cable plugged in but without the red wire: because of the ESD protection diodes of the unpowered ESP32-S3 on the signal lines, the CH342 deterministically swallows the very first start bit when the direction changes (`STM32 TX → CH342 RX`) (`8,68 µs` at 115,200 8E1), so that `ACK 0x79` is decoded as `0xDE`, `NACK 0x1F` as `0xFC` and `GET_ID 79 01 04 35 79` as `de c0 b0 a3 ff`, while the forward direction (`CH342 TX → STM32 RX`), including hardware parity and XOR checksums, stays 100 % bit-exact.

### Full A121 sparse IQ & 3-subsweep access via ESP32-S3 (`USART1`, 2026-10-31)

In the factory server firmware (`acc_exploration_server_a121.bin`) the Exploration Server runs only on `USART2` (`PA2`/`PA3` → exclusively CH342 channel A USB), while `USART1` (`PA9`/`PA10` → 10-pin MX1.25 connector `TXD`/`RXD` ↔ ESP32-S3 `GPIO5`/`GPIO4`) is initialized only as an unused debug port without DMA.

To give the ESP32-S3 **100 % access to all A121 hardware functions** over the existing 10-pin cable (raw complex 16-bit sparse IQ data, up to 4 subsweeps per frame, on-chip loopback, profiles 1–5, PRF 19.5–5.2 MHz, HWAAS 1–511, phase enhancement, IQ imbalance compensation, continuous sweep mode & double buffering, idle states), the following are available as of now:

1. **STM32L431 `USART1` Exploration Server firmware (historical, superseded by the SDK rebuild in `firmware/stm32l431/`):**
   - Originally created with a binary patcher; that was removed from the repo, the way now is `build_server.py` with the
     official SDK.
   - Put the Exploration Server with full DMA support (`DMA1_Channel5` RX / `DMA1_Channel4` TX, `IRQ 14/15/37`, `GPIO_PULLUP` on `PA9`/`PA10`, `UART_HWCONTROL_NONE`) directly on `USART1` (`GPIO5`/`GPIO4` on the ESP32-S3) and supported baud rates from `115 200` to `2 000 000` baud as well as soft reset via UART break.
2. **ESP32-S3 dual-stack firmware (`firmware/esp32s3/src/main.cpp`, `a121_server_client.h`, `a121_esp_dsp.h`):**
   - **Switched on the evening of 2026-10-31, verified on the device on 2026-11-01:** The server link runs over `Serial2` on the
     SDA/SCL wires (`GPIO15` RX ← PB11, `GPIO16` TX → PB10) and needs the planned LPUART1 server firmware from the SDK
     (`firmware/stm32l431/README.md`). With the USART1 variant the ESP32 no longer talks.
     After startup, GPIO15/16 are high-impedance (`link` = free). `link i2c|srv` decides who uses GPIO15/16; server commands switch to `srv` themselves, I2C commands are blocked in `srv` mode
     (the server drives the SDA wire push-pull). `uart` receives the debug logs only on GPIO5 now; GPIO4 stays high-impedance.
   - Keeps all existing I2C commands (`vital`, `zoom`, `auto`, `dist`, `cfg`, `ver`, `scan`) and adds the sparse IQ stack:
     - `srv_info`: Connects on `GPIO15`/`GPIO16` (switches up from 115,200 to 921,600 baud) and reads `get_system_info` & `get_sensor_info`.
     - `stm_reset`: Sends a 15 ms UART break on `GPIO16` (`PB10`). The framing error resets the STM32 (USART1 variant and SDK rebuild, both confirmed over channel B). Every server connect of the ESP32 has started with this break since 2026-11-01.
     - `iq3 [sek] [start_mm] [raw]`: Starts the **3-subsweep sparse IQ session @ 60 Hz** (subsweep 0 on-chip loopback + subsweep 1 2.5 mm vital zoom with 24 points (compile switch `A121_ZOOM_POINTS=48` prepared for 120 mm, spectra then only for the 16 points with the strongest breathing; choice after measurement V4.4) + subsweep 2 room scan 0.30–2.67 m, 80 × 30 mm) (preset zoom profile 1 / HWAAS 32 / 15.6 MHz, loopback profile 1 / HWAAS 8 / gain 16 / 19.5 MHz, as compile switches `A121_IQ3_ZOOM_PROFILE`, `A121_IQ3_ZOOM_HWAAS`, `A121_IQ3_LOOP_PROFILE`, `A121_IQ3_LOOP_GAIN`, `A121_IQ3_LOOP_PRF` for measurement V4.2, reported in `@HELLO` as `iq3=…`) and computes on the ESP32-S3 (PSRAM, `a121_esp_dsp.h`; since V2.1 reports run in their own task on core 0 from a snapshot of 60 frames each, reading on core 1 continues): loopback phase rotation per frame; per zoom point, phase unwrapped around the circle center of the IQ values (Kåsa circle fit), bandpass 0.6–5 Hz, harmonic sum S(f) = P(f) + 0.5 · P(2f) weighted over all zoom points; pulse candidates 40–180 /min with a penalty for breathing harmonics and octave check, tracking with hold time (variant Viterbi over 20 reports behind `A121_DSP_VITERBI`, V3.2, default off; variant "remove breathing harmonics guided by the breathing phase" behind `A121_DSP_HARM_REMOVE`, V3.3, default off; variant "combine zoom points into one channel before demodulation" behind `A121_DSP_COMBINE`, V3.1, default off, then with the effective point count as the 15th field in `@DSP`; variant "single beats with template in the 0.6–8 Hz band" behind `A121_DSP_IBI_TEMPLATE`, V3.6, default off; `A121_DSP_HEAVY` switches Viterbi and harmonic subtraction on together, only for the phone DSP in the app, V5.2; switch `A121_DSP_REACQ`, V6 "reacquire pulse after motion", default off: motion map, pulse only on the quiet final segment from 6 s, memory of the last confident value for 60 s; PlatformIO environment `esp32s3-n16r8-reacq` (`pio run -e esp32s3-n16r8-reacq`), `@HELLO` then reports the firmware with the suffix `-reacq`; only with the switch two fields are appended to `@DSP` (16th and 17th field, counted as for `n_eff`, after `n_eff`, which stays empty without `A121_DSP_COMBINE`): `win_s` (pulse window used in s, 0 = none) and `reacq` (0 normal, 1 short window after motion, 2 reacquired via the memory); without the switch they are absent. The variants `A121_DSP_REACQ_PERIOD` and `A121_DSP_PHASE_SLOPE` are rejected and stay off, numbers in `docs/EXPERIMENTS.md`); autocorrelation and single beats (IBI, RMSSD experimental) after subtracting the breathing harmonics; confidence from peak height, harmonic, agreement of the three estimators, motion and saturation; breathing rate, breathing state (normal / no breathing peak / held / motion / shallow), signs-of-life test; sensor flags (late and saturated frames, tick gaps), recalibration on `calibration_needed` (`@RECAL`) and rezoom (`@REZOOM`; since V2.3 pulse tracking is kept in this case if the last valid value is younger than 30 s). Output: `@IQ3` per frame (with `raw` additionally `@RAW`), `@ROOMPROF` 5/s (room profile), `@DSP`/`@ROOM`/`@VITAL`/`@STATE` 1/s (`@STATE`: state of the measurement, V3.5), `@BREATH` per breath (peaks of the displacement
0.05–0.8 Hz with hysteresis, V3.4); formats at the top and end of `a121_esp_dsp.h`, status and plan in `docs/VITAL_AUDIT.md`, evaluation with `host/vital_eval.py` and `host/a121_esp_ultimate.py`. (Until 2026-11-01 this held a chain with DACM and Hilbert projection; that belongs to the PC scripts, not the firmware.)
     - `doppler [sek] [mm] [hz]`: Enables `continuous_sweep_mode=true` + `double_buffering=true` (64 sweeps/frame @ up to 4,000 Hz) for coherent micro-Doppler/vibration measurements.
     - `bridge`: Transparent USB-CDC ↔ server UART bridge (incl. automatic baud rate synchronization), so that PC scripts such as `host/a121_ultimate_vital.py` can also stream directly through the ESP32-S3.
     - `sess …` (since 2026-11-01, `a121_session_console.h`): any sparse IQ session without a PC. The configuration lives in RAM
       and is set field by field: `fr`, `sr`, `spf`, `csm`, `dbuf`, `fidle`/`sidle`, `subs 1..4`, per subsweep `range a b` (mm),
       `start`, `n`, `step`, `profile`, `hwaas`, `gain`, `prf`, `tx`, `loop`, `phase`, `iqimb`.
       `sess` shows the configuration with mm range and expected data rate (incl. ~150 B header per frame) and warns above the
       link limit. `sess check` only sends it to the server and shows metadata or the reason for the error from the server log.
       `sess run [s] [stat|peak|amp]` streams and evaluates: amplitude averaged over all sweeps, strongest point per subsweep.
       Every second `@SESS_STAT`, at the end `@SESS_END` with UART errors and backlog. The output is throttled
       (`peak` ≤ 100/s, `amp` ≤ 10/s) so that the ESP32 never falls behind the data stream. At most 4096 points per frame.
     - `gpio [0|1|z] [0|1|z]`: Reads or switches `MISC_GPIO0` (`GPIO6` ↔ `PB1`, brown) and `MISC_GPIO1` (`GPIO7` ↔ `PA4`, gray).
     - **App protocol `proto=1` (since 2026-11-01, firmware `2026-11-01-app1`, additions only, PC scripts run unchanged):**
       `hello` → `@HELLO,proto=1,fw=…,build=…,link=…,server=…,mac=…,up_ms=…,modes=…` (`up_ms` shows whether the ESP32 has
       restarted). `@IQ3` and `@DOPPLER` have a frame counter and the sensor `tick` at the end. `iq3 [sek] [start_mm] raw`
       additionally sends `@RAW,frame,tick,n,<Base64>` per frame (105 complex int16, ≈ 35 kB/s instead of ≈ 5 kB/s).
       After 3 frame timeouts the ESP32 reconnects to the server (`@RECONNECT,t_ms,versuch,ok`, after which `tick`
       starts from the beginning). Ctrl+R (0x12) during a stream deliberately restarts the server for testing.
     - Opening the port does not reset the ESP32; only DTR = 0 with RTS = 1 triggers a reset (verified on the PC). PC scripts
       therefore open with the default values; the app sets only DTR and never touches RTS.

**Verified on the device (2026-10-31, PC only via CH342 channel B, ESP32 not connected):**
- Server `a121-v1.12.0`, `max_baudrate` 2,000,000. exptool at 921,600 baud: 16/16 sessions OK after reset, 100 frames each @ 100 Hz.
- **Every framing error on PA10 resets the STM32** (server then at 115,200 baud, responds after ≲ 1 s).
  This holds for a break as well as for bytes with the wrong baud rate. A baud search therefore triggers a reset itself.
  Without a reset, the server stays at the last baud rate set.
- Before almost every response the server sends a log line `{"status":"log",...}`. Clients must skip it.
- The server encodes the enums like exptool: `"profile":1`, `"prf":"15_6_MHz"`, `"ready"`, free rate `0.0`.
  `step_length` must be a divisor or a multiple of 24; the server rejects `16` with "invalid config".
- A Python replica of the ESP32 client logic (same JSON and sequence, incl. the reset path) ran against the real server:
  `iq3` preset 300 frames @ 60.0 Hz, 420 B/frame. `doppler` preset (CSM + double buffering, 64×4 @ 2000 Hz)
  60 frames @ 31 Hz, 1024 B/frame. No flags (saturated/delayed/calibration_needed).
- An earlier hang (no response at any baud rate, no reset via framing error) could only be solved with the RESET button
  and did not occur again afterwards. Probable cause: see "Bug 1" in the ESP32 test 2026-11-01.

**Open hardware risk before ESP32 operation over USART1:** Per the schematic, PA10 (`DEBUG_RX`, connector pin 8)
is connected not only to ESP32 GPIO4 but also, through **R9 = 22 Ω, directly to CH342 TXD1**. The CH342 always runs (5V net) and
drives TXD1 HIGH when idle. Then ESP32 TX and CH342 TX work against each other (push-pull across only 22 Ω). The
LOW level at PA10 thus becomes unreliable, and both drivers are heavily loaded. This is the same mechanism as with
WAKE_UP/DTR0. Not re-measured. To clarify before continuous operation: desolder R9 (channel B can then only listen), or
put the server on the I2C lines instead of USART1.

**Decision 2026-10-31 (evening): server on the I2C wires, rebuilt from the Acconeer XM125 SDK.**
- Pins verified: SDA/SCL on the connector = **PB11/PB10** (schematic nets `I2C2_SDA`/`I2C2_SCL`; `i2c_distance_detector.bin`
  sets the GPIOB mask 0x0C00 to AF4). The earlier statement PB13/PB14 in `firmware/stm32l431/xm125_waveshare_bsp.h` was wrong.
  Only R13/R14 are on the lines, no CH342.
- Peripheral: **LPUART1** (AF8: PB11 TX → ESP32 GPIO15, PB10 RX ← GPIO16) with DMA2 channel 6/7. USART3 is practically ruled out:
  it would need DMA1 channel 2/3, and the Exploration Server already uses those for SPI1 to the A121.
- Way: official XM125 SDK **a121-v1.13.0** (downloaded by the project owner) + `firmware/stm32l431/build_server.py`. The SDK itself confirms
  the pins (`main.h`: `I2C_SCL` = PB10, `I2C_SDA` = PB11); the LPUART1 assignment (AF8, PB10 RX/PB11 TX) per ST
  `STM32_open_pin_data`, DMA2 channel 6 TX / channel 7 RX with request 4 per CubeMX-generated L4 code.
- **Built 2026-10-31 23:40, flashed ~23:50 (start confirmed via debug log, LPUART1 path to the ESP32 still untested):**
  - Check: the SDK built unchanged (`--variant original`) is **bit-identical** to the prebuilt `out/acc_exploration_server_a121.bin`
    of the SDK (109,168 bytes) → compiler, ST drivers and SDK match Acconeer's own build exactly.
  - `~/Data/radar-sensor/acc_exploration_server_a121-v1.13.0_lpuart1.bin`, 109,760 bytes,
    SHA256 `5dfefa8f5c9e8666bbda2b394705ec0352861a4b1344ef56dcfd891502ea3827`. Built with `-Werror` without warnings.
  - Verified on the ELF: vectors IRQ 68/69/70 → `DMA2_Channel6/7_IRQHandler`, `LPUART1_IRQHandler`; server send/receive paths,
    baud rate switching and break reset (`HAL_UART_ErrorCallback`) use `hlpuart1`; the break check at startup reads GPIOB bit 10.
  - In this build: server only on LPUART1 (USB channel A no longer responds), `printf` logs still on USART1/PA9
    (CH342 channel B and ESP32 GPIO5 read along), I2C2 is switched off again after the SDK init.
- ESP32 side switched; verified on the device on 2026-11-01 (see below).
- State on the device 2026-10-31, ~23:00: The flashed USART1 variant responds at no baud rate (channels A and B), so the same
  hang as above. Before further tests: RESET button on the A121.

**ESP32 test on the device 2026-11-01 (A121 on PC USB, red wire disconnected, ESP32 on native USB `…12:34:56:78:9A:BC`):**
- Result: The LPUART1 server runs over the SDA/SCL wires. Verified with the ESP32 firmware of 2026-11-01 (with the fixes below):
  - `srv_info`: `a121-v1.13.0`, `hw` xm125, `max_baudrate` 2,000,000, sensor connected; the ESP32 switches to 921,600 baud by itself.
  - `stm_reset`: The break on GPIO16 resets the STM32 (channel B: "Acconeer Exploration Server" + "Waiting for host line
    break"), after which the ESP32 reconnects. A connect takes ≈ 0.4 s.
  - `iq3 10`: 583 frames in 10 s (58 Hz, target 60), no saturation, no `frame_timeout`, `@REZOOM` works.
    In every second in which `computeVitalReport()` computes, a gap of 190–320 ms appears between two frames.
    The frames are not lost in the process, they back up in the 32 KB UART buffer. This should still be optimized.
    The `@VITAL` values are **not evaluated**: there was no reference measurement, and nobody sat in front of the sensor on purpose.
  - `doppler 5`: 156 frames in 5 s (31 Hz, 64 sweeps @ 2000 Hz), no warnings.
  - `bridge` + exptool 7.18.2 on the PC (`a121.Client.open(serial_port=…)`, default: switch to 2,000,000 baud):
    setup with calibration OK, 90 frames 16 × 60 @ 30.09 Hz, no flags. `+++EXIT` ends the bridge.
  - Two ESP32 restarts in a row (esptool hard reset as when flashing) with the server running at 921,600 baud
    → `srv_info` worked each time without the RESET button.
- **Bug 1, fixed (ESP32): server deaf after an ESP32 restart.** The client tried the first handshake at the last
  remembered baud rate (921,600 after startup), but the freshly started server listens at 115,200. The wrong bytes cause
  noise or overrun errors on the STM32. In DMA mode the ST HAL then aborts reception on **every** UART error and turns
  the error interrupts off. The SDK's `HAL_UART_ErrorCallback`, however, restarts only on a framing error. Consequence: the server
  stays deaf, even a break no longer helps, only the RESET button. Seen three times on the device (plugging in in the morning, twice
  after flashing the ESP32). The USB variant does not notice any of this, because exptool always starts at 115,200. This was probably
  also the earlier hang of the USART1 variant; that is not proven.
  Fix: `connectAndNegotiateBaud()` now always sends the break first (the framing error reliably triggers a restart)
  and then talks only at 115,200. In addition, GPIO15/16 are high-impedance after startup (link "free"). I2C starts only on the first
  I2C command, so that `Wire.begin()` puts no edges on PB10 during boot.
  The actual weak point in the STM32 has since been fixed as well, see "Hardening 2026-11-01".
- **Bug 2, fixed (ESP32): exptool `setup_session` timed out in the bridge.** HWCDC (USB of the ESP32, Arduino 2.0.17)
  has a 256-byte receive buffer and silently discards everything above that. The bridge wrote byte by byte to a UART without
  a transmit buffer (only the 128-byte FIFO). The exptool setup command is 464 bytes long, more than 384, and so the line ending
  was lost. Proof: sent in 64-byte chunks, the server accepted the same command. Fix: `Serial.setRxBufferSize(4096)`,
  `setTxBufferSize(4096)` for the server UART, and the bridge forwards block by block.

**Hardening 2026-11-01 (STM32, flashed ~08:40 with approval from the project owner):** 7th edit in `build_server.py`. `HAL_UART_ErrorCallback`
now restarts on **every** error of the server UART (`ErrorCode != HAL_UART_ERROR_NONE` instead of only a framing error).
- Image `acc_exploration_server_a121-v1.13.0_lpuart1.bin`, 109,760 bytes, SHA256
  `b29e328d52eeeafc97e28e89051cb8ee74d527fb6a17a706b39ef1488cbc1757`. The linker warning "LOAD segment with RWX permissions"
  also appears in the bit-identical `original` build, so it is not new.
- Verified on the ELF: LTO inlined the callback at all three call sites (`UART_DMAAbortOnError`, `UART_DMAError`,
  `HAL_UART_IRQHandler`). Everywhere it now reads `huart == &hlpuart1` and `ErrorCode (+0x88) != 0`, followed by `NVIC_SystemReset`.
- Verified on the device:
  - Normal operation as before: `srv_info`; `iq3 10` with 575 frames (57 Hz, without saturation); `doppler 5` with 31 Hz; exptool via
    `bridge` with 90 frames at 30.03 Hz. On channel B only the expected restarts appeared, one per connect due to the break.
    During the measurements the STM32 did not restart a single time.
  - Deliberate baud error: Through the bridge the server was set to 115,200, while the bridge stayed at 921,600 (trick:
    `"baudrate" : 115200` with spaces is not recognized by the bridge). Then `get_system_info` was sent three times at the wrong rate.
    Result: the STM32 restarted after 0.1 s (channel B: "Acconeer Exploration Server" without "Waiting for host line break", so
    caused by the error and not by a break) and was reachable again afterwards. With the old firmware it stayed deaf in exactly
    this case, three times.
  - What cannot be seen from outside is which error triggered the restart (framing or noise).
- Side effect: A real disturbance in the middle of a measurement now restarts the STM32 instead of making it deaf. The running
  session is then gone. The ESP32 client notices this from `frame_timeout` and has to reconnect; automatic reconnecting
  during `iq3`/`doppler` does not exist yet.

**Mobile setup on the device 2026-11-01 (~09:00): A121 powered only through the red wire from ESP32 5Vin, no A121 USB**
(only `…12:34:56:78:9A:BC` visible on the PC, the CH342 does not enumerate without USB). STM32 debug log via the yellow wire (`uart`, GPIO5).
- `probe`/`bus`: all lines HIGH, MCU_INT LOW (server firmware does not use it). `srv_info` and `stm_reset` work.
  The debug log shows the restart ("Acconeer Exploration Server", "Waiting for host line break").
- ESP32's own server UART at **2 Mbaud** since 2026-11-01 (`SRV_BAUD`, previously 921,600; the debug UART stays at 921,600):
  - `iq3 30`: 1783 frames in 30 s (59.4 Hz), without saturation. Still 24 gaps of up to 354 ms, one per DSP report
    (compute time on the ESP32, not the link). Sensor temperature rose from 14 to 19 °C.
  - `doppler 10`: 31.2 Hz (64 sweeps @ 2 kHz). `doppler 10 500 4000`: 62.3 Hz (64 sweeps @ 4 kHz = 16 ms per frame, so gapless).
- **Throughput through the bridge** (exptool 7.18.2, 2 Mbaud; sparse IQ, profile 3, HWAAS 1, 4 s per step each; script was in the scratchpad, not in the repo):
  | requested | achieved | `frame_delayed` |
  |---|---|---|
  | 16×60 @ 30 Hz = 115 kB/s | 30.0 Hz = 115 kB/s | 0/121 |
  | 32×100 @ 10 Hz = 128 kB/s | 10.0 Hz = 128 kB/s | 0/41 |
  | 32×100 @ 20–60 Hz = 256–768 kB/s | 15.2 Hz ≈ 194–195 kB/s | 60/61 |
  The limit is the 2 Mbaud UART (200 kB/s gross). Above that the server throttles and marks the frames as `frame_delayed`,
  as intended. Two runs in a row with 5,061,804 bytes each to the PC: no UART error (overflow, buffer, framing,
  parity), no incomplete USB write, at most 129 bytes were pending in the ESP32 buffer.
- **Bug 3, fixed (ESP32): Under overload, data was lost in the bridge** (exptool aborted with "Cannot decode header").
  The counters (`@BRIDGE_STATS` after `+++EXIT`) showed an incomplete USB write and no UART errors. HWCDC by default has
  only a 256-byte transmit buffer and discards data if the PC does not pick up for 100 ms.
  Fix: `Serial.setTxBufferSize(16384)`, `setTxTimeoutMs(500)`, and the bridge resends the remainder after an incomplete write.
- Not verified: current draw in the red wire, continuous operation over hours, operation on the phone instead of the PC.

**`sess` on the device 2026-11-01 (mobile setup):**
- Default (40 points, 10 Hz): 10.00 Hz. Two subsweeps 0.3–1.5 m (profile 2, 10 mm) + 1.5–7.0 m (profile 5, 30 mm, PRF 6.5 MHz):
  20.00 Hz, stable reflection at ≈ 650 mm. Large frame 4 × 1000 points (16 kB) at 5 Hz: clean.
- Overload 32 × 100 at 30 Hz (384 kB/s): the ESP32 takes in 15.2 Hz (≈ 195 kB/s, link limit), the server reports `frame_delayed`.
  No UART error, no backlog.
- Error cases: `step 16` → "Invalid step_length", start 12.5 m → "Start point or range too far". More than 4096 points
  are rejected by the ESP32 before the setup. CSM without `sr` → "Invalid settings for continuous_sweep_mode".
- **Continuous sweep mode, 1 point, profile 1, HWAAS 1** (`max_sweep_rate` per metadata 24,673 Hz):
  | sweep_rate | spf | achieved | delayed |
  |---|---|---|---|
  | 2,000 Hz, without `dbuf` | 64 | 30.36 Hz | all |
  | 2,000–21,000 Hz, with `dbuf` | 64 | 31.2 … 323.8 Hz (target −0.1 … −1.4 %) | 0 |
  | 22,000–24,000 Hz, with `dbuf` | 64 / 256 | effectively ≈ 21.7–23.6 kHz sweeps | all |
  Consequences: CSM needs double buffering, and gapless operation works up to **21 kHz sweep rate**, independent of the frame size.
  The bottleneck is in the sensor/STM32 path, not in the link (≈ 150 kB/s) and not in the ESP32 (backlog 0). With 5 points
  the link limits: 137 Hz × (1280 B + ~150 B header) ≈ 196 kB/s.
- **Bug 4, fixed (ESP32): The client read every byte of the frame header individually from the UART driver.** At 375 frames/s
  the 32 KB buffer overflowed (4873 FIFO overflows, rate collapsed to 12 Hz). Fix: 1 KB intermediate buffer in the client, filled in
  blocks. Afterwards 369 frames/s with backlog 0. `iq3` (58.5 Hz), `doppler` (31.2 Hz) and bridge (≈ 195 kB/s) checked again afterwards.

**`iq3` outputs the room, target search corrected (2026-11-01, mobile setup):**
- New lines: `@ROOMPROF` every 12 frames (5/s) with smoothed amplitude and motion of all 80 room points (0.30–2.67 m,
  30 mm) and `@ROOM` per report (1/s) with noise level and up to 8 targets (distance, amplitude, motion, moving yes/no).
  The time axis is the sensor `tick` (ms), which runs gaplessly even across compute pauses. Before, only the number of targets was printed.
- **Bug 5, fixed (ESP32 DSP): The target search in the room scan saw almost only noise.** The fixed thresholds (value > 120, motion > 12)
  were below the noise (amplitude ≈ 55, motion ≈ 55), and only the 4 nearest peaks counted. The list was therefore always
  filled with noise peaks up to ≈ 1.2 m, and a noise peak in the zoom window counted as a "moving target nearby" and
  prevented the rezoom. Now: noise level = lower quarter of the 80 points, target from 2 × noise, "moving" from 3 ×, the 8 strongest,
  distance refined parabolically, rezoom onto the strongest moving target.
- PC verification (`host/a121_esp_ultimate.py --mode verify-cpp`, the same header compiled for the PC, `iq3raw_normal_120s` and
  `iq3raw_motion_120s`): tracked pulse in all 242 reports as before (the pulse/breathing part is unchanged). Zoom window
  correct (570–630 mm): old and new no rezoom. Window deliberately set wrong: old at 400 mm (both recordings) and 1000 mm
  (`motion`) never a rezoom; new at 400, 750, 1000, 1500 and 2250 mm after 3–4 s a recommendation onto the person
  (window start 562–688 mm, person at 0.60–0.67 m).
- On the device (`iq3 30 500`, person seated at ≈ 0.7 m, `2026-11-01_iq3_raum_live1.log/.png`): 1794 frames in 30 s (59.8 Hz), no
  saturation; 149 room profiles at 200 ms intervals, one gap of 307 ms at the rezoom. Rezoom after 3 s from 500–560 to 632–692 mm,
  none afterwards. Compute time per report still ≈ 306 ms.
- Limits: thresholds seen on two recordings and one live run of the same person in the same room. Weak static echoes
  (2–2.6 × noise, e.g. at 0.42 m and 1.0–1.3 m) appear and disappear at the threshold.

**Bug 6, fixed (ESP32 DSP): The breathing rate often stuck at the lower edge of the search band (6.9 /min)**, in `iq3raw_normal` in
30 of 113 reports, in `iq3raw_motion` in 35 of 115, live in 10 of 18. Two causes, each proven on the recordings
(Python replica that matches the C++ code to within 0.1 /min):
- The spectra of the 24 zoom points were summed unweighted. Weak points (|z| 120–300) had 140–240 phase jumps in
  15 s and an apparent displacement signal of 8–11 mm peak-to-peak (unwrapping error). Their low-frequency content masked the breathing of the
  strong points, which e.g. at t = 75 s cleanly showed 17 /min.
- The largest bin in the range was searched. With posture changes (steps of 2–5 mm in the displacement signal) the power keeps rising toward low
  frequencies, so the largest bin is then the edge although there is no peak there.
- Fix: weight amplitude² per point (inverse phase noise) and only true local maxima; without a peak `br` = 0.
  Tested individually: weight alone 30→15 and 35→34 edge values, local maxima alone 0/0 (2 reports without a value); both together 0/0.
- PC verification: edge values 30→0 and 35→0, no reports without a value. Tracked pulse unchanged in all 228 reports, only `hr_harm`
  is set more often (30→40, 41→43), because the harmonic check now gets the correct breathing rate. Against the counted
  breathing cycles of the strong points: `normal` 60–75 s 17.6 counted / 17 /min, 80–95 s 19.8 / 21; `motion` 0–20 s 20.7 / 20–21.
- On the device (`iq3 60 500`, person seated, `2026-11-01_iq3_atmung_live1.log/.png`): 40 breathing values, none at the edge, 9.7–24.3 /min.
  Comparison with the zero crossings of the `@IQ3` displacement signal in the same 15 s window: 29 of 40 within ±3 /min, at 31–56 s
  (zoom 650–710 mm, on the chest) all 26 within ±2 /min. Deviations at 12–22 s (7 reports; there the
  counted cycles also fluctuate between 11 and 19 /min) and at 56.5 s. Compute time per report median 291 ms (before ≈ 306 ms).
- Limits: no reference, the cycle counting comes from the same radar signal (plausibility only). With clear motion
  (`motion` 46–58 s) there is still a breathing value (23–26 /min) that is not reliable there.

**Bug 7, fixed (ESP32 DSP): With two nearby echoes the rezoom chose the wrong one and got stuck there.** In the run
`2026-11-01_iq3_atmung_live1` the first rezoom went to 745–805 mm, the chest was at ≈ 0.66 m; the window stayed there for 19 s.
- Cause 1, snapshot: There were two moving echoes, the chest at 0.66 m (amplitude fluctuating 440–1220, motion 600–960)
  and a second at 0.78 m (amplitude steady ≈ 950, motion 200–570). The decision was made in a single report:
  0.774 m with 1096 + 8 × 596 = 5864 against 0.656 m with 529 + 8 × 660 = 5809, because the chest happened to have an amplitude dip.
- Cause 2, proximity rule: Every moving echo at the window prevented the rezoom, so the second echo held the window in place.
- The wrong window was worse: pulse confidence median 78 % against 99 % on the chest, pulse peak/median 9.2 against 15.8 dB
  (different time segments of the same run).
- Criteria compared on the 5 Hz room profiles of four logs (chest chosen / choice at t = 3 s in the failing run): snapshot
  today 92 % / wrong; smoothed over 3 s "amplitude + 8 × motion" 99–100 % / right; smoothed motion only likewise; strongest
  echo 85–88 % / wrong (the second echo was stronger on average, but moved less). Smoothing 1–8 s: right everywhere.
- Fix: The zoom target comes from an additionally smoothed room profile (τ ≈ 3 s), and only this target holds the window (±60 mm).
  Simulation on the logs: start on the second echo (745 mm) trapped until 26 s today, new after 3 s on the chest; in all
  other cases the same rezooms as before, no jumping back and forth.
- PC verification (`iq3raw_normal`/`motion`): vital values bit-identical. Window correct: no rezoom. Window at 400–2250 mm: chest
  found after 3 s, window start 580–635 mm (before 562–688 mm).
- On the device (`iq3 45 745`, person seated, `2026-11-01_iq3_rezoom_live1.log/.png`): one rezoom after 3 s to 635–695 mm, none afterwards,
  although moving echoes appeared at 0.74–0.79 m at 16, 25 and 43 s. 59.9 Hz, breathing 11.5–24.8 /min without an edge value,
  pulse confidence median 94 %, compute time median 319 ms. The second echo was weaker this time than in the failing run; the hard case
  is proven only by the simulation on the log.

Downloads (not in the repo, located in `vendor/`, gitignored), fetched 2026-10-30:
- `A121_Firmware.zip` sha256 `fea5bcd0b7d39149c0cc02bf82303abd5cd6d92f209643ba426b18db58da356a`
  (https://files.waveshare.com/wiki/A121_Range_Sensor/A121_Firmware.zip)
- `A121_Range_Sensor_Demo.zip` sha256 `481b587bf8e388d36846ab44a4603a2a2cd60a3a35245d674eaaaf5b1f66e144`
  (https://files.waveshare.com/wiki/A121_Range_Sensor/A121_Range_Sensor_Demo.zip)

For the own server build, fetched 2026-10-31 (versions per `doc/XM125 Software User Guide.pdf` of the SDK):
- `acconeer_xm125_a121-v1_13_0.zip` sha256 `0f4e74b1d3af18583b650c9f27865838a8bf11e93387889775689940664aa3f4`
  (Acconeer Developer Site, downloaded with the project owner's account) → `vendor/acconeer_xm125_a121-v1_13_0.zip`, unpacked to
  `vendor/acconeer_sdk/a121-v1.13.0/xm125/`. License of the Acconeer parts: BSD-3-Clause (`LICENSES/license_acconeer.txt`).
- STM32CubeL4 v1.18.1, only `Drivers/` → `vendor/stm32cube/STM32Cube_FW_L4_V1.18.1/Drivers/`, via Git at the commits of
  tag v1.18.1: `STMicroelectronics/stm32l4xx-hal-driver` @ `41027c61f69401afe4937db0ebd5d0cdbffbea0d` (BSD-3),
  `STMicroelectronics/cmsis-device-l4` @ `a2530753e86dd326a75467d28feb92e2ba7d0df2` (Apache-2.0),
  `Drivers/CMSIS/Include` from `STMicroelectronics/STM32CubeL4` @ `d4c40b36185bd0d75e33c94ebc8337b634bbcc94` (tag v1.18.1).
- Arm GNU Toolchain 13.3.Rel1 `arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz`
  sha256 `95c011cee430e64dd6087c75c800f04b9c49832cc1000127a92a97f9c8d83af4` (matches ARM's `.sha256asc`,
  https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/) → `~/.local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/`.

ESP32-S3 pins to avoid: GPIO19/20 (native USB), GPIO35–37 (octal PSRAM), GPIO43/44 (UART0 to the
USB-UART bridge), strapping pins 0/3/45/46. RGB LED on GPIO48 or 38 depending on the board revision.

## Power supply from the phone

- Phone → native USB-C port of the ESP32-S3 → 5V pin → A121 board.
- Rough demand: A121 ≤ 80 mA + ESP32-S3 ~100–250 mA (less without radio) — uncritical for phone OTG.
- On clone boards, check whether the 5V pin also carries voltage when powered through the native port (multimeter).
- **Verified 2026-10-30 (the project owner's board):** Pin labeled "5Vin"; on both USB ports only 1.2–1.3 V
  (leakage voltage) instead of 5 V → pure input, probably via a diode. The A121 stays unpowered that way (PWR LED off).
  The board is a DevKitC-1 clone (no Espressif logo, module "S3-N16R8", photo 2026-10-30) with an **open
  solder bridge "IN-OUT"** at the bottom left next to the RGB LED. Closed, it bridges a diode according to community sources
  (YD-ESP32-S3 docs), then USB VBUS is on 5Vin – after that never external 5 V and USB at the same time.
  Alternative: original Espressif ESP32-S3-DevKitC-1-N16R8 (5V0 = USB VBUS via diode, ~4.7 V).
  **IN-OUT closed on 2026-10-30 (self-soldered):** 5Vin–GND = 4.74 V with USB power.
  Rules since then: never USB + external 5 V at the same time; with the red wire plugged in, do not additionally power the A121 via USB-C.
  Temporarily on the PC: power the A121 through its own USB-C, red wire disconnected, signals + GND stay.
- **Verified 2026-11-01 on the Galaxy S26 Ultra:** native USB port of the ESP32 connected to the phone with a C-to-C cable → phone is
  power source + host, all power LEDs (ESP32 and A121) on, ESP32 detected as `303a:1001` (CDC-ACM + JTAG). The
  USB-C socket of the clone therefore needs no OTG adapter. Data flow with the app verified: 31.7 min of `iq3 … raw`
  over USB without frame loss (`docs/ANDROID_APP.md`). Unplugging restarts the ESP32 (it runs on the phone's power); the
  phone does not charge in the meantime, consumption not measured.
- Never connect both USB ports of the ESP32 at the same time (e.g. phone + PC): with the IN-OUT bridge closed,
  two 5 V sources could be connected (not measured).
- Always power motors (later scanner) separately; only GND in common.

## Pico (on hold)

Not part of the current A121 focus. Possible later roles: SWD debug probe for the STM32
("debugprobe" firmware on the SWDIO/SWCLK/GND pads) or stepper motor control for the SAR scanner.
Flashing the STM32 works without the Pico over USB (BOOT button, channel A).
