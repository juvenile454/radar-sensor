# STM32L431 (A121-Board): Exploration Server aus dem Acconeer-SDK

Stand 2026-11-01: **gebaut, geflasht und über den ESP32 am Gerät geprüft** (`srv_info`, `stm_reset`, `iq3`, `doppler`,
`bridge` + exptool; Ergebnisse und zwei behobene ESP32-Bugs in `docs/HARDWARE.md`, „ESP32-Test am Gerät 2026-11-01“).
Mobil (rote Ader statt A121-USB) am 2026-11-01 ebenso geprüft, inkl. Durchsatzgrenze ≈ 195 kB/s bei 2 Mbaud.
**Gehärtet am 2026-11-01:** Der Server startet bei jedem UART-Fehler neu. Vorher machte ein Noise- oder Overrun-Fehler ihn taub,
bis RESET gedrückt wurde; neu gestartet hat er nur bei einem Break. Am Gerät mit absichtlichem Baudfehler geprüft.
Image: `~/Data/radar-sensor/acc_exploration_server_a121-v1.13.0_lpuart1.bin` (SHA256 `b29e328d…1757`);
Stand vom 2026-10-31 als `…_lpuart1_2026-10-31.bin` (SHA256 `5dfefa8f…3827`).

## Ziel

Der ESP32-S3 soll den vollen Exploration Server des A121 nutzen: rohe Sparse-IQ-Daten, bis zu 4 Subsweeps,
Loopback, Continuous Sweep Mode, alle Profile und PRFs. Dafür wird der Server aus dem offiziellen
**Acconeer XM125 SDK** neu gebaut, und zwar so, dass seine UART auf den **SDA/SCL-Adern** des 10-Pin-Steckers liegt:

| STM32 | Funktion (AF8) | Ader | ESP32-S3 |
|---|---|---|---|
| PB11 | LPUART1_TX | orange (SDA) | GPIO15 = RX (`Serial2`) |
| PB10 | LPUART1_RX | grün (SCL) | GPIO16 = TX (`Serial2`) |

Warum nicht die TXD/RXD-Adern (USART1, PA9/PA10)? PA10 hängt laut Schaltplan zusätzlich über R9 (22 Ω) am
TXD1 des CH342, der die Leitung immer HIGH treibt. Ein ESP32-TX auf PA10 arbeitet dagegen.
An PB10/PB11 hängen nur die I2C-Pull-ups R13/R14 (4,7 kΩ nach 3V3).

Warum LPUART1 und nicht USART3 (auch PB10/PB11)? USART3 bräuchte DMA1 Kanal 2/3, und die nutzt der Server
schon für SPI1 zum A121. LPUART1 nutzt DMA2 Kanal 6/7 (laut RM0394, im SDK gegenprüfen).

Belege für die Pins: Schaltplan-Netze `I2C2_SCL` = PB10, `I2C2_SDA` = PB11; die Hersteller-Firmware
`i2c_distance_detector.bin` stellt GPIOB-Maske 0x0C00 auf AF4 (I2C2). Siehe `xm125_waveshare_bsp.h`.

## Voraussetzungen (vorhanden seit 2026-10-31, nicht im Repo; Quellen und Prüfsummen in `docs/HARDWARE.md`)

| Was | Wo | Version |
|---|---|---|
| Acconeer XM125 SDK | `vendor/acconeer_sdk/a121-v1.13.0/xm125/` | a121-v1.13.0 (passt zu exptool 7.18.2 = SDK 1.13.0) |
| STM32CubeL4 `Drivers/` | `vendor/stm32cube/STM32Cube_FW_L4_V1.18.1/` | v1.18.1 (laut SDK-Handbuch) |
| Arm GNU Toolchain | `~/.local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/` | 13.3.Rel1 (damit baut Acconeer die Bibliotheken) |

## Build

```bash
python3 firmware/stm32l431/build_server.py --variant original
python3 firmware/stm32l431/build_server.py --variant lpuart1
```

- `original` baut das SDK unverändert. Ergebnis 2026-10-31: **bitgleich** zum vorgebauten Image im SDK, die Umgebung stimmt also.
- `lpuart1` kopiert das SDK nach `build/lpuart1/` (gitignored), ändert sieben Stellen (jede nur, wenn der Originaltext genau
  einmal vorkommt) und legt `src/xm125_lpuart1_link.c` dazu:
  - `main.h`: `EXPLORATION_SERVER_UART_HANDLE` = `hlpuart1`, Pin-Namen `SERVER_UART_RX/TX` (= PB10/PB11)
  - `main.c`: `xm125_lpuart1_init()` vor `acconeer_main()` (gibt I2C2 frei, startet LPUART1 mit 115 200 Baud, ohne RTS/CTS)
  - `stm32l4xx_hal_msp.c`: SDK-`HAL_UART_Msp(De)Init` umbenannt; unsere Version macht LPUART1 und reicht den Rest weiter
  - `acc_exploration_server_stm32.c`: Warten auf das Ende eines Host-Breaks auf PB10 statt PA3
  - `acc_exploration_server_stm32.c`: `HAL_UART_ErrorCallback` startet bei jedem Fehler neu, nicht nur bei einem Framing-Error.
    Im DMA-Modus empfängt die HAL nach jedem Fehler nicht mehr.
  - `makefile`: unsere Datei zum Server-Target
- Ausgabe: `~/Data/radar-sensor/acc_exploration_server_a121-v1.13.0_<variante>.bin` + `.sha256`.
- Baudraten 115 200 bis 2 000 000 (LPUART1 an 80 MHz PCLK1). `printf`-Logs bleiben auf USART1/PA9.

## Flashen (nur mit Freigabe des Projektinhabers)

1. Port eindeutig prüfen: `ls -l /dev/serial/by-id` → `usb-1a86_USB_Dual_Serial_5C75025105-if00` = Kanal A.
2. A121 per eigenem USB-C an den PC, rote 5V-Ader zum ESP32 vorher ab.
3. BOOT halten, RESET tippen, BOOT loslassen.
4. `python -m acconeer.exptool.flash flash XM125 -p /dev/ttyACMx -i ~/Data/radar-sensor/acc_exploration_server_a121-v1.13.0_lpuart1.bin`
   (`ttyACMx` = Ziel von `-if00`).
5. Zurück jederzeit genauso mit `acc_exploration_server_a121.bin` oder `i2c_distance_detector.bin`.

Danach antwortet der Server **nicht mehr über USB**; PC-Tools laufen über die ESP32-`bridge`.

## Prüfen am Gerät (danach)

1. Aufbau vom Flashen lassen: A121 per eigenem USB-C am PC, **rote Ader ab**, alle Signal-Adern und GND gesteckt.
   ESP32 per eigenem USB am PC, ESP32-Firmware `firmware/esp32s3` flashen (Server-Link GPIO15/16).
2. Optional am PC Kanal B (`-if02`, 921 600 Baud) mitlesen: Die `printf`-Logs des STM32 (z. B. „Acconeer Exploration Server“
   nach einem Reset) kommen über PA9.
3. ESP32: `srv_info`, dann `iq3 10`, `doppler 5`, `stm_reset`; am PC `bridge` + exptool.
4. Erst danach mobil: A121-USB ab, rote Ader an (nie beides gleichzeitig, HARDWARE.md „Stromversorgung“).
Schritte 1–4 erledigt am 2026-11-01 (Ergebnisse in `docs/HARDWARE.md`).

## Weitere Dateien hier

- `xm125_waveshare_bsp.h` – Pin-Notizen zum Board (I2C2-Pins am 2026-10-31 korrigiert, LPUART1 ergänzt).
- `main.c`, `a121_universal_protocol.h` – Entwurf einer eigenen Streamer-Firmware mit eigenem Protokoll.
  Nicht der gewählte Weg; der ESP32-Client spricht das Exploration-Server-Protokoll.
- `Makefile` – nur `verify-headers` (Syntaxprüfung von `main.c`). Ein früherer Binär-Patcher für die USART1-Variante
  ist entfernt; der Weg ist der SDK-Neubau mit `build_server.py`.
