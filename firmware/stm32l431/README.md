# STM32L431 (A121 board): Exploration Server from the Acconeer SDK

As of 2026-11-01: **built, flashed and verified on the device via the ESP32** (`srv_info`, `stm_reset`, `iq3`, `doppler`,
`bridge` + exptool; results and two fixed ESP32 bugs in `docs/HARDWARE.md`, "ESP32 test on the device 2026-11-01").
Mobile setup (red wire instead of the A121 USB) verified the same way on 2026-11-01, incl. the throughput limit of ≈ 195 kB/s at 2 Mbaud.
**Hardened on 2026-11-01:** The server restarts on every UART error. Before, a noise or overrun error made it deaf
until RESET was pressed; it restarted only on a break. Verified on the device with a deliberate baud error.
Image: `~/Data/radar-sensor/acc_exploration_server_a121-v1.13.0_lpuart1.bin` (SHA256 `b29e328d…1757`);
the build of 2026-10-31 is `…_lpuart1_2026-10-31.bin` (SHA256 `5dfefa8f…3827`).

## Goal

The ESP32-S3 should use the full Exploration Server of the A121: raw sparse IQ data, up to 4 subsweeps,
loopback, continuous sweep mode, all profiles and PRFs. For this, the server is rebuilt from the official
**Acconeer XM125 SDK** so that its UART sits on the **SDA/SCL wires** of the 10-pin connector:

| STM32 | Function (AF8) | Wire | ESP32-S3 |
|---|---|---|---|
| PB11 | LPUART1_TX | orange (SDA) | GPIO15 = RX (`Serial2`) |
| PB10 | LPUART1_RX | green (SCL) | GPIO16 = TX (`Serial2`) |

Why not the TXD/RXD wires (USART1, PA9/PA10)? According to the schematic, PA10 is also connected through R9 (22 Ω) to the
TXD1 of the CH342, which always drives the line HIGH. An ESP32 TX on PA10 fights against it.
Only the I2C pull-ups R13/R14 (4.7 kΩ to 3V3) are attached to PB10/PB11.

Why LPUART1 and not USART3 (also PB10/PB11)? USART3 would need DMA1 channel 2/3, and the server already uses those
for SPI1 to the A121. LPUART1 uses DMA2 channel 6/7 (per RM0394, to be cross-checked in the SDK).

Evidence for the pins: schematic nets `I2C2_SCL` = PB10, `I2C2_SDA` = PB11; the vendor firmware
`i2c_distance_detector.bin` sets GPIOB mask 0x0C00 to AF4 (I2C2). See `xm125_waveshare_bsp.h`.

## Prerequisites (present since 2026-10-31, not in the repo; sources and checksums in `docs/HARDWARE.md`)

| What | Where | Version |
|---|---|---|
| Acconeer XM125 SDK | `vendor/acconeer_sdk/a121-v1.13.0/xm125/` | a121-v1.13.0 (matches exptool 7.18.2 = SDK 1.13.0) |
| STM32CubeL4 `Drivers/` | `vendor/stm32cube/STM32Cube_FW_L4_V1.18.1/` | v1.18.1 (per the SDK manual) |
| Arm GNU Toolchain | `~/.local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/` | 13.3.Rel1 (Acconeer builds its libraries with it) |

## Build

```bash
python3 firmware/stm32l431/build_server.py --variant original
python3 firmware/stm32l431/build_server.py --variant lpuart1
```

- `original` builds the SDK unchanged. Result 2026-10-31: **bit-identical** to the prebuilt image in the SDK, so the environment is correct.
- `lpuart1` copies the SDK to `build/lpuart1/` (gitignored), changes seven places (each only if the original text occurs
  exactly once) and adds `src/xm125_lpuart1_link.c`:
  - `main.h`: `EXPLORATION_SERVER_UART_HANDLE` = `hlpuart1`, pin names `SERVER_UART_RX/TX` (= PB10/PB11)
  - `main.c`: `xm125_lpuart1_init()` before `acconeer_main()` (releases I2C2, starts LPUART1 at 115,200 baud, without RTS/CTS)
  - `stm32l4xx_hal_msp.c`: SDK `HAL_UART_Msp(De)Init` renamed; our version handles LPUART1 and passes the rest on
  - `acc_exploration_server_stm32.c`: wait for the end of a host break on PB10 instead of PA3
  - `acc_exploration_server_stm32.c`: `HAL_UART_ErrorCallback` restarts on every error, not only on a framing error.
    In DMA mode the HAL no longer receives after any error.
  - `makefile`: our file added to the server target
- Output: `~/Data/radar-sensor/acc_exploration_server_a121-v1.13.0_<variant>.bin` + `.sha256`.
- Baud rates 115,200 to 2,000,000 (LPUART1 at 80 MHz PCLK1). `printf` logs stay on USART1/PA9.

## Flashing (only with approval from the project owner)

1. Check the port unambiguously: `ls -l /dev/serial/by-id` → `usb-1a86_USB_Dual_Serial_<serial>-if00` = channel A.
2. Connect the A121 to the PC via its own USB-C; unplug the red 5V wire to the ESP32 beforehand.
3. Hold BOOT, tap RESET, release BOOT.
4. `python -m acconeer.exptool.flash flash XM125 -p /dev/ttyACMx -i ~/Data/radar-sensor/acc_exploration_server_a121-v1.13.0_lpuart1.bin`
   (`ttyACMx` = target of `-if00`).
5. You can go back at any time the same way with `acc_exploration_server_a121.bin` or `i2c_distance_detector.bin`.

Afterwards the server **no longer answers over USB**; PC tools work through the ESP32 `bridge`.

## Verifying on the device (afterwards)

1. Keep the setup from flashing: A121 on the PC via its own USB-C, **red wire unplugged**, all signal wires and GND connected.
   ESP32 on the PC via its own USB; flash the ESP32 firmware `firmware/esp32s3` (server link GPIO15/16).
2. Optional: on the PC, also read channel B (`-if02`, 921,600 baud): the `printf` logs of the STM32 (e.g. "Acconeer Exploration Server"
   after a reset) come out on PA9.
3. ESP32: `srv_info`, then `iq3 10`, `doppler 5`, `stm_reset`; on the PC `bridge` + exptool.
4. Only then mobile: A121 USB unplugged, red wire connected (never both at the same time, HARDWARE.md "Power supply").
Steps 1–4 done on 2026-11-01 (results in `docs/HARDWARE.md`).

## Other files here

- `xm125_waveshare_bsp.h` – pin notes for the board (I2C2 pins corrected on 2026-10-31, LPUART1 added).
- `main.c`, `a121_universal_protocol.h` – draft of a custom streamer firmware with its own protocol.
  Not the chosen path; the ESP32 client speaks the Exploration Server protocol.
- `Makefile` – only `verify-headers` (syntax check of `main.c`). An earlier binary patcher for the USART1 variant
  has been removed; the way is the SDK rebuild with `build_server.py`.
