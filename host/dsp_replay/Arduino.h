#pragma once
/* Minimal replacement for Arduino.h so that firmware/esp32s3/src/a121_esp_dsp.h compiles on PC and Android
 * (like ARDUINO_SHIM in host/a121_esp_ultimate.py). micros() = 0: compute times come from the ESP32 during replay. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Float functions via double: glibc (PC) and bionic (Android) then give the same float result, PC and
 * phone replay become byte-identical. Neither variant is bit-identical to the ESP32 (newlib float libm); against
 * the ESP32 output this variant is as good as the direct float functions (checked with a 30-min recording 2026-11-01). */
static inline float a121_atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
static inline float a121_cosf(float x) { return (float)cos((double)x); }
static inline float a121_powf(float a, float b) { return (float)pow((double)a, (double)b); }
static inline float a121_log10f(float x) { return (float)log10((double)x); }
#define atan2f a121_atan2f
#define cosf a121_cosf
#define powf a121_powf
#define log10f a121_log10f

static inline bool psramFound() { return true; }
static inline void *ps_malloc(size_t sz) { return malloc(sz); }
static inline unsigned long micros() { return 0; }
