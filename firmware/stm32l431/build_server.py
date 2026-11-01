#!/usr/bin/env python3
"""Baut den Acconeer Exploration Server für das Waveshare-A121-Board aus dem offiziellen XM125-SDK.

Varianten:
  original  SDK unverändert (Server auf USART2 = CH342 Kanal A / USB). Prüft Toolchain und Build-Umgebung;
            das Ergebnis wird mit dem vorgebauten Image aus dem SDK verglichen.
  lpuart1   Server auf LPUART1 über die SDA/SCL-Adern (PB11 TX -> ESP32 GPIO15, PB10 RX <- GPIO16).

Ablauf: SDK nach firmware/stm32l431/build/<variante>/ kopieren (gitignored), bei lpuart1 die wenigen Stellen
ändern (jede mit exakter Prüfung des Originaltexts) und src/xm125_lpuart1_link.c dazulegen, dann `make`.
Ins Repo kommen nur dieses Skript und unsere eigene C-Datei, keine SDK-Dateien (AGENTS.md).

Voraussetzungen (Versionen laut doc/XM125 Software User Guide des SDK):
  vendor/acconeer_sdk/a121-v1.13.0/xm125              Acconeer XM125 SDK a121-v1.13.0
  vendor/stm32cube/STM32Cube_FW_L4_V1.18.1/Drivers     HAL/CMSIS aus STM32CubeL4 v1.18.1
  ~/.local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi   Arm GNU Toolchain 13.3.Rel1
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
SDK_VERSION = "a121-v1.13.0"
SDK_DIR = REPO / "vendor" / "acconeer_sdk" / SDK_VERSION / "xm125"
CUBE_DIR = REPO / "vendor" / "stm32cube" / "STM32Cube_FW_L4_V1.18.1"
GCC_BIN = pathlib.Path.home() / ".local" / "opt" / "arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi" / "bin"
DATA_DIR = pathlib.Path("~/Data/radar-sensor").expanduser()
TARGET = "out/acc_exploration_server_a121.elf"

# (Datei relativ zum SDK, Originaltext, Ersatz). Jeder Originaltext muss genau einmal vorkommen.
LPUART1_EDITS: list[tuple[str, str, str]] = [
    (
        "Inc/main.h",
        "#define EXPLORATION_SERVER_UART_HANDLE huart2\n",
        "#define EXPLORATION_SERVER_UART_HANDLE hlpuart1\n"
        "\n"
        "// radar-sensor (build_server.py): Server auf LPUART1 über die SDA/SCL-Adern, s. xm125_lpuart1_link.c\n"
        "#define SERVER_UART_RX_Pin       I2C_SCL_Pin  // PB10 = LPUART1_RX\n"
        "#define SERVER_UART_RX_GPIO_Port I2C_SCL_GPIO_Port\n"
        "#define SERVER_UART_TX_Pin       I2C_SDA_Pin  // PB11 = LPUART1_TX\n"
        "#define SERVER_UART_TX_GPIO_Port I2C_SDA_GPIO_Port\n"
        "void xm125_lpuart1_init(void);\n"
        "void xm125_sdk_HAL_UART_MspInit(UART_HandleTypeDef *huart);\n"
        "void xm125_sdk_HAL_UART_MspDeInit(UART_HandleTypeDef *huart);\n",
    ),
    (
        "Src/main.c",
        "\tacconeer_main(0, NULL);\n",
        "\txm125_lpuart1_init();\n\tacconeer_main(0, NULL);\n",
    ),
    (
        "Src/stm32l4xx_hal_msp.c",
        "void HAL_UART_MspInit(UART_HandleTypeDef* huart)\n",
        "void xm125_sdk_HAL_UART_MspInit(UART_HandleTypeDef* huart)\n",
    ),
    (
        "Src/stm32l4xx_hal_msp.c",
        "void HAL_UART_MspDeInit(UART_HandleTypeDef* huart)\n",
        "void xm125_sdk_HAL_UART_MspDeInit(UART_HandleTypeDef* huart)\n",
    ),
    (
        # Beim Start wartet der Server, bis ein Break des Hosts vorbei ist – jetzt auf PB10 statt PA3.
        "Src/applications/acc_exploration_server_stm32.c",
        "HAL_GPIO_ReadPin(UART_RX_GPIO_Port, UART_RX_Pin)",
        "HAL_GPIO_ReadPin(SERVER_UART_RX_GPIO_Port, SERVER_UART_RX_Pin)",
    ),
    (
        # Härtung (2026-11-01): Im DMA-Modus bricht die ST-HAL bei JEDEM UART-Fehler (Noise, Overrun, Parity,
        # Framing) den Empfang ab und schaltet die Fehler-Interrupts ab. Das SDK startet nur bei einem Framing-
        # Error (Break) neu, nach jedem anderen Fehler bleibt der Server taub, bis RESET gedrückt wird.
        # Deshalb Neustart bei jedem Fehler. Ein Break wirkt weiter wie vorher.
        "Src/applications/acc_exploration_server_stm32.c",
        "\t\t/* Check framing error bit to detect break condition */\n"
        "\t\tif ((huart->ErrorCode & HAL_UART_ERROR_FE) == HAL_UART_ERROR_FE)\n",
        "\t\t/* radar-sensor: Neustart bei jedem Fehler (Break = Framing-Error, aber auch Noise/Overrun),\n"
        "\t\t   weil die HAL im DMA-Modus danach nicht mehr empfängt. */\n"
        "\t\tif (huart->ErrorCode != HAL_UART_ERROR_NONE)\n",
    ),
    (
        "makefile",
        "\tacc_exploration_server_stm32.c \\\n",
        "\tacc_exploration_server_stm32.c \\\n\txm125_lpuart1_link.c \\\n",
    ),
]


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_inputs() -> None:
    problems = []
    if not (SDK_DIR / "VERSION").is_file() or (SDK_DIR / "VERSION").read_text().strip() != SDK_VERSION:
        problems.append(f"SDK {SDK_VERSION} fehlt unter {SDK_DIR}")
    for sub in ("Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_uart.c",
                "Drivers/CMSIS/Include/cmsis_gcc.h",
                "Drivers/CMSIS/Device/ST/STM32L4xx/Source/Templates/gcc/startup_stm32l431xx.s"):
        if not (CUBE_DIR / sub).is_file():
            problems.append(f"STM32CubeL4-Datei fehlt: {CUBE_DIR / sub}")
    if not (GCC_BIN / "arm-none-eabi-gcc").is_file():
        problems.append(f"Arm GNU Toolchain fehlt: {GCC_BIN}")
    if problems:
        sys.exit("FEHLER:\n  " + "\n  ".join(problems))


def apply_edits(build_sdk: pathlib.Path) -> None:
    for rel, old, new in LPUART1_EDITS:
        path = build_sdk / rel
        text = path.read_text()
        count = text.count(old)
        if count != 1:
            sys.exit(f"FEHLER: '{old.strip()}' kommt in {rel} {count}x vor (erwartet 1) – SDK-Version prüfen.")
        path.write_text(text.replace(old, new))
        print(f"  geändert: {rel}")
    shutil.copy2(HERE / "src" / "xm125_lpuart1_link.c", build_sdk / "Src" / "integration" / "xm125_lpuart1_link.c")
    print("  hinzugefügt: Src/integration/xm125_lpuart1_link.c")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--variant", choices=("lpuart1", "original"), default="lpuart1")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    check_inputs()
    build_root = HERE / "build" / args.variant
    build_sdk = build_root / "xm125"
    if build_root.exists():
        shutil.rmtree(build_root)
    # out/ (vorgebaute Images) und doc/ nicht mitkopieren: frischer Build, kleiner Ordner
    shutil.copytree(SDK_DIR, build_sdk, ignore=shutil.ignore_patterns("out", "doc"))
    print(f"SDK {SDK_VERSION} kopiert nach {build_sdk}")

    if args.variant == "lpuart1":
        apply_edits(build_sdk)

    cmd = ["make", "-C", str(build_sdk), f"-j{args.jobs}",
           f"GNU_INSTALL_ROOT={GCC_BIN}", f"STM32CUBE_FW_L4_ROOT={CUBE_DIR}", TARGET]
    print("Starte:", " ".join(cmd))
    subprocess.run(cmd, check=True)

    built = build_sdk / "out" / "acc_exploration_server_a121.bin"
    out_name = f"acc_exploration_server_{SDK_VERSION}_{args.variant}.bin"
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    dest = DATA_DIR / out_name
    shutil.copy2(built, dest)
    digest = sha256(dest)
    (DATA_DIR / (out_name + ".sha256")).write_text(f"{digest}  {out_name}\n")
    print(f"\nImage: {dest}\n  Größe: {dest.stat().st_size} Bytes\n  SHA256: {digest}")
    print((build_sdk / "out" / "acc_exploration_server_a121_size.txt").read_text())

    if args.variant == "original":
        ref = SDK_DIR / "out" / "acc_exploration_server_a121.bin"
        same = ref.is_file() and sha256(ref) == digest
        print(f"Vergleich mit vorgebautem SDK-Image ({ref.stat().st_size} Bytes): "
              f"{'bitgleich' if same else 'NICHT bitgleich'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
