# Makefile für die STM32L431CBT6 (XM125) Firmware auf dem Waveshare A121 Range Sensor Board
#
# Unterstützt zwei Build-Wege:
#   1. `make patch-server` (Standard, funktioniert sofort ohne Acconeer Developer-Account):
#      Erzeugt aus `../../vendor/firmware/A121_Firmware/acc_exploration_server_a121.bin`
#      die USART1-gepatchte Exploration-Server-Firmware
#      `$(HOME)/Data/radar-sensor/acc_exploration_server_a121_usart1_esp32.bin`,
#      die über den 10-Pin MX1.25-Stecker (PA9/PA10 <-> ESP32 GPIO5/GPIO4) mit vollem
#      DMA-Support (DMA1_Channel4 TX / DMA1_Channel5 RX) läuft.
#
#   2. `make sdk-build` (wenn das Acconeer XM125 C-SDK unter `../../vendor/acconeer_sdk/` liegt):
#      Übersetzt `main.c` gegen `libacconeer_a121.a` und STM32CubeL4 mit `arm-none-eabi-gcc`.

TOOLCHAIN_DIR ?= $(HOME)/.platformio/packages/toolchain-gccarmnoneeabi/bin
CC            := $(TOOLCHAIN_DIR)/arm-none-eabi-gcc
OBJCOPY       := $(TOOLCHAIN_DIR)/arm-none-eabi-objcopy
OBJDUMP       := $(TOOLCHAIN_DIR)/arm-none-eabi-objdump
SIZE          := $(TOOLCHAIN_DIR)/arm-none-eabi-size

ACCONEER_SDK_DIR ?= ../../vendor/acconeer_sdk
OUTPUT_BIN       ?= $(HOME)/Data/radar-sensor/acc_exploration_server_a121_usart1_esp32.bin

CFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
          -O2 -Wall -Wextra -ffunction-sections -fdata-sections -std=c11 \
          -DSTM32L431xx -I.

.PHONY: all patch-server verify-headers disasm clean

all: patch-server verify-headers

patch-server:
	python3 ../../host/build_stm32_usart1_server.py --output $(OUTPUT_BIN)

verify-headers:
	$(CC) $(CFLAGS) -fsyntax-only main.c
	@echo "[OK] C-Syntax- und Header-Prüfung (Cortex-M4F hard-float) erfolgreich."

disasm: $(OUTPUT_BIN)
	$(OBJDUMP) -D -b binary -m arm -M force-thumb --adjust-vma=0x08000000 \
		--start-address=0x08005324 --stop-address=0x08005404 $(OUTPUT_BIN)

clean:
	rm -f *.o *.elf
