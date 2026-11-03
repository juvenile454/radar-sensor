#!/usr/bin/env python3
"""Builds the Acconeer Exploration Server for the Waveshare A121 board from the official XM125 SDK.

Variants:
  original  SDK unchanged (server on USART2 = CH342 channel A / USB). Checks toolchain and build environment;
            the result is compared with the prebuilt image from the SDK.
  lpuart1   Server on LPUART1 over the SDA/SCL wires (PB11 TX -> ESP32 GPIO15, PB10 RX <- GPIO16).

Procedure: copy the SDK to firmware/stm32l431/build/<variant>/ (gitignored), for lpuart1 change the few places
(each with an exact check of the original text) and add src/xm125_lpuart1_link.c, then `make`.
Only this script and our own C file go into the repo, no SDK files (AGENTS.md).

Prerequisites (versions per doc/XM125 Software User Guide of the SDK):
  vendor/acconeer_sdk/a121-v1.13.0/xm125              Acconeer XM125 SDK a121-v1.13.0
  vendor/stm32cube/STM32Cube_FW_L4_V1.18.1/Drivers     HAL/CMSIS from STM32CubeL4 v1.18.1
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

# (file relative to the SDK, original text, replacement). Each original text must occur exactly once.
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
        # At startup the server waits until a break from the host is over – now on PB10 instead of PA3.
        "Src/applications/acc_exploration_server_stm32.c",
        "HAL_GPIO_ReadPin(UART_RX_GPIO_Port, UART_RX_Pin)",
        "HAL_GPIO_ReadPin(SERVER_UART_RX_GPIO_Port, SERVER_UART_RX_Pin)",
    ),
    (
        # Hardening (2026-11-01): In DMA mode the ST HAL aborts reception on EVERY UART error (noise, overrun, parity,
        # framing) and switches off the error interrupts. The SDK only restarts on a framing
        # error (break); after any other error the server stays deaf until RESET is pressed.
        # Hence restart on every error. A break still acts as before.
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
    # Do not copy out/ (prebuilt images) and doc/: fresh build, smaller folder
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
