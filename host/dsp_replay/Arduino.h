#pragma once
/* Minimaler Ersatz für Arduino.h, damit firmware/esp32s3/src/a121_esp_dsp.h auf PC und Android übersetzt
 * (wie ARDUINO_SHIM in host/a121_esp_ultimate.py). micros() = 0: Rechenzeiten kommen beim Nachrechnen vom ESP32. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Float-Funktionen über double: glibc (PC) und bionic (Android) liefern dann dasselbe Float-Ergebnis, PC- und
 * Handy-Nachrechnung werden bytegleich. Bitgleich zum ESP32 (newlib-Float-libm) ist keine der beiden Varianten; gegen
 * die ESP32-Ausgabe ist diese Variante so gut wie die direkten Float-Funktionen (30-min-Aufnahme 2026-11-01 geprüft). */
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
