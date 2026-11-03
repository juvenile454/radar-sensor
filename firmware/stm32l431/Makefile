# Makefile for the STM32L431CBT6 (XM125) firmware on the Waveshare A121 Range Sensor Board
#
# The flashed server firmware is built by build_server.py from the official Acconeer XM125 SDK
# (see README.md). This Makefile now only checks main.c:
#   `make verify-headers`  syntax check of main.c (Cortex-M4F hard-float)

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
