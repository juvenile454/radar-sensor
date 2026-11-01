#pragma once
#include <Arduino.h>
class TwoWire : public Stream {
 public:
  bool begin(int, int, uint32_t = 0);
  bool end();
  void setTimeOut(uint16_t);
  void beginTransmission(uint8_t);
  uint8_t endTransmission(bool = true);
  uint8_t requestFrom(uint8_t, uint8_t, bool = true);
  size_t write(uint8_t);
  size_t write(const uint8_t *, size_t);
};
extern TwoWire Wire;
