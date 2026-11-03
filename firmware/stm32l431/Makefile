# Makefile für die STM32L431CBT6 (XM125) Firmware auf dem Waveshare A121 Range Sensor Board
#
# Die geflashte Server-Firmware entsteht über build_server.py aus dem offiziellen Acconeer-XM125-SDK
# (siehe README.md). Dieses Makefile prüft nur noch main.c:
#   `make verify-headers`  Syntaxprüfung von main.c (Cortex-M4F hard-float)

TOOLCHAIN_DIR ?= $(HOME)/.platformio/packages/toolchain-gccarmnoneeabi/bin
CC            := $(TOOLCHAIN_DIR)/arm-none-eabi-gcc

CFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
          -O2 -Wall -Wextra -ffunction-sections -fdata-sections -std=c11 \
          -DSTM32L431xx -I.

.PHONY: all verify-headers clean

all: verify-headers

verify-headers:
	$(CC) $(CFLAGS) -fsyntax-only main.c
	@echo "[OK] C-Syntax- und Header-Prüfung (Cortex-M4F hard-float) erfolgreich."

clean:
	rm -f *.o *.elf
