// Syntax-Stub für firmware/esp32s3 (nur g++ -fsyntax-only am PC; KEIN Ersatz für einen ESP32-Build)
#pragma once
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <functional>
#include <initializer_list>
#include <algorithm>
typedef uint8_t byte;
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define INPUT_PULLDOWN 0x09
#define HIGH 1
#define LOW 0
#define SERIAL_8N1 0x800001c
#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif
#define F(x) (x)
unsigned long millis();
unsigned long micros();
void delay(uint32_t);
void delayMicroseconds(uint32_t);
void pinMode(uint8_t, uint8_t);
void digitalWrite(uint8_t, uint8_t);
int digitalRead(uint8_t);
void neopixelWrite(uint8_t, uint8_t, uint8_t, uint8_t);
void yield();
bool psramFound();
void *ps_malloc(size_t);
class String {
 public:
  std::string s;
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &x) : s(x) {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(float v, int d = 2) { char b[32]; snprintf(b, 32, "%.*f", d, v); s = b; }
  String(double v, int d = 2) { char b[32]; snprintf(b, 32, "%.*f", d, v); s = b; }
  const char *c_str() const { return s.c_str(); }
  unsigned length() const { return s.size(); }
  bool isEmpty() const { return s.empty(); }
  int indexOf(const String &x, unsigned from = 0) const { auto p = s.find(x.s, from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(char c, unsigned from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  String substring(unsigned a) const { return String(s.substr(a)); }
  String substring(unsigned a, unsigned b) const { return String(s.substr(a, b - a)); }
  bool startsWith(const String &x) const { return s.rfind(x.s, 0) == 0; }
  bool endsWith(const String &x) const { return s.size() >= x.s.size() && s.compare(s.size() - x.s.size(), x.s.size(), x.s) == 0; }
  long toInt() const { return atol(s.c_str()); }
  float toFloat() const { return atof(s.c_str()); }
  void trim() {}
  void reserve(unsigned) {}
  void remove(unsigned, unsigned = 1) {}
  void replace(const String &, const String &) {}
  char operator[](unsigned i) const { return s[i]; }
  String &operator+=(const String &x) { s += x.s; return *this; }
  String &operator+=(const char *x) { s += x; return *this; }
  String &operator+=(char x) { s += x; return *this; }
  bool operator==(const String &x) const { return s == x.s; }
  bool operator==(const char *x) const { return s == x; }
  bool operator!=(const String &x) const { return s != x.s; }
  friend String operator+(const String &a, const String &b) { return String(a.s + b.s); }
  friend String operator+(const String &a, const char *b) { return String(a.s + b); }
  friend String operator+(const char *a, const String &b) { return String(a + b.s); }
};
class Print {
 public:
  size_t write(uint8_t);
  size_t write(const uint8_t *, size_t);
  size_t write(const char *, size_t);
  size_t write(const char *);
  size_t printf(const char *, ...) __attribute__((format(printf, 2, 3)));
  template <typename T> size_t print(const T &) { return 0; }
  template <typename T> size_t print(const T &, int) { return 0; }
  template <typename T> size_t println(const T &) { return 0; }
  template <typename T> size_t println(const T &, int) { return 0; }
  size_t println() { return 0; }
};
class Stream : public Print {
 public:
  int available();
  int read();
  size_t read(uint8_t *, size_t);
  int peek();
  size_t readBytes(uint8_t *, size_t);
  size_t readBytes(char *, size_t);
  void setTimeout(unsigned long);
  void flush();
};
typedef enum { UART_NO_ERROR, UART_BREAK_ERROR, UART_BUFFER_FULL_ERROR, UART_FIFO_OVF_ERROR, UART_FRAME_ERROR, UART_PARITY_ERROR } hardwareSerial_error_t;
class HardwareSerial : public Stream {
 public:
  void begin(unsigned long, uint32_t = SERIAL_8N1, int8_t = -1, int8_t = -1, bool = false, unsigned long = 20000UL, uint8_t = 112);
  void end();
  void updateBaudRate(unsigned long);
  size_t setRxBufferSize(size_t);
  size_t setTxBufferSize(size_t);
  void onReceiveError(std::function<void(hardwareSerial_error_t)>);
  void onReceive(std::function<void(void)>, bool = true);
  bool setRxFIFOFull(uint8_t);
  void setRxTimeout(uint8_t);
  operator bool() const { return true; }
};
class USBCDC : public HardwareSerial {
 public:
  void setTxTimeoutMs(uint32_t);
};
extern USBCDC Serial;
extern HardwareSerial Serial1;
extern HardwareSerial Serial2;
class EspClass {
 public:
  const char *getChipModel();
  uint8_t getChipRevision();
  uint8_t getChipCores();
  uint32_t getCpuFreqMHz();
  uint64_t getEfuseMac();
  uint32_t getPsramSize();
  uint32_t getFreePsram();
  uint32_t getFlashChipSize();
  uint32_t getFreeHeap();
  void restart();
};
extern EspClass ESP;
using std::min;
using std::max;
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
