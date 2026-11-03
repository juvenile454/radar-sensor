// Bring-up-Firmware ESP32-S3 ↔ A121 (Waveshare A121 Range Sensor)
//
// Konsole über den nativen USB-Port (115200, Zeilenende \n). Befehle: help
// Pins wie in docs/HARDWARE.md (= Waveshare-ESP32-S3-Demos):
//   I2C SDA 15, SCL 16, WAKE_UP 17 (nur lesen, s. u.), MCU_INT 18 (Eingang, high = bereit)
//   UART1: RX 5 ← A121 TXD (Debug-UART des STM32, 921600 Baud); GPIO4 bleibt hochohmig, weil der CH342
//          die A121-RXD-Leitung (PA10) über R9 = 22 Ω selbst treibt.
//
// Server-Link (Modus A): Serial2 auf denselben Adern wie I2C – GPIO15 RX ← PB11 (LPUART1_TX, SDA-Ader),
// GPIO16 TX → PB10 (LPUART1_RX, SCL-Ader). Braucht eine Server-Firmware auf LPUART1 (Acconeer-SDK-Neubau,
// firmware/stm32l431/README.md). I2C und Server-UART sind nie gleichzeitig aktiv ('link i2c|srv').
//
// WAKE_UP treibt auf dem Waveshare-Board der USB-Chip CH342 (DTR0, Push-Pull) – auch ohne USB, weil er
// am 5V-Netz hängt. Der ESP32 treibt GPIO17 deshalb nur auf ausdrücklichen Befehl ('wake') und gibt die
// Leitung wieder frei, wenn sie nicht folgt.
//
// Hinweis: Die I2C-Register gibt es nur mit einer i2c_*-Firmware auf dem STM32.
// Mit der Werksfirmware (Exploration Server) findet der Scan nichts unter 0x52.

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

#include "a121_esp_dsp.h"
#include "a121_server_client.h"
#include "a121_session_console.h"

static const int PIN_SDA = 15;
static const int PIN_SCL = 16;
static const int PIN_WAKE = 17;
static const int PIN_INT = 18;
static const int PIN_UART_TX = 4;
static const int PIN_UART_RX = 5;
static const int PIN_MISC_GPIO0 = 6;  // braune Ader -> STM32 PB1 (MISC_GPIO0)
static const int PIN_MISC_GPIO1 = 7;  // graue Ader  -> STM32 PA4 (MISC_GPIO1)
static const uint32_t A121_BAUD = 921600;   // Debug-UART des STM32 (printf auf PA9 → GPIO5)
// Server-UART (LPUART1): Höchstrate des Servers. Am 2026-11-01 über die Bridge ~10 MB ohne einen UART-Fehler;
// netto ≈ 195 kB/s, bei 921 600 Baud wären es ≈ 90 kB/s.
static const uint32_t SRV_BAUD = 2000000;
static const uint8_t A121_ADDR = 0x52;

static const int PIN_SRV_RX = PIN_SDA;  // GPIO15 ← STM32 PB11 (LPUART1_TX), orange Ader
static const int PIN_SRV_TX = PIN_SCL;  // GPIO16 → STM32 PB10 (LPUART1_RX), grüne Ader
static HardwareSerial &SrvUart = Serial2;

// Stand der Firmware für '@HELLO' (App/PC erkennen damit Protokoll und Fähigkeiten, docs/ANDROID_APP.md).
// proto 1: @IQ3 mit Frame-Zähler + Sensor-tick, @RAW, @RECONNECT, @DOPPLER mit Frame-Zähler + tick.
// Mit dem Schalter A121_DSP_REACQ (V6, Umgebung esp32s3-n16r8-reacq) hängt die Kennung "-reacq" an, damit Aufnahmen
// mit und ohne Wiederfinden in @HELLO unterscheidbar sind. Ohne Schalter bleibt die Kennung unverändert.
#ifdef A121_DSP_REACQ
#define FW_VERSION_SUFFIX "-reacq"
#else
#define FW_VERSION_SUFFIX ""
#endif
static const char *FW_VERSION = "2026-11-02-vital1" FW_VERSION_SUFFIX;  // DSP-Stand Audit V0–V5 (vorher 2026-11-01-app1 = Basislinie)
static const int PROTO_VERSION = 1;
static String g_serverVersion = "-";  // rss_version der letzten erfolgreichen Server-Verbindung

static int rgbPin = 48;  // DevKitC-1 v1.0: 48, v1.1: 38 — mit "led <pin>" umstellbar
static bool uartBridge = false;
static String line;

// Wer nutzt gerade GPIO15/16? Beim Start niemand (beide hochohmig): Schon Wire.begin() erzeugt Pegelwechsel
// auf SCL, und die landen bei der LPUART1-Server-Firmware auf PB10 (RX). Danach war der Server am 2026-11-01
// zweimal taub (Neustart des ESP32), bis die RESET-Taste am A121 gedrückt wurde. I2C startet beim ersten
// I2C-Befehl, die Server-UART beim ersten Server-Befehl.
enum class Link { NONE, I2C, SRV };
static Link g_link = Link::NONE;

static const char *linkName() { return g_link == Link::I2C ? "i2c" : g_link == Link::SRV ? "srv" : "frei"; }

static a121_srv::ExplorationServerClient g_srv(SrvUart, PIN_SRV_RX, PIN_SRV_TX);
static a121_dsp::UltimateEspDsp g_dsp;
static a121_sess::SessionConsole g_sess;

static void led(uint8_t r, uint8_t g, uint8_t b) { neopixelWrite(rgbPin, r, g, b); }

static void startI2c() {
  Wire.begin(PIN_SDA, PIN_SCL, 400000);  // Pull-ups 4,7 kΩ sitzen auf dem A121-Board (Schaltplan R13/R14)
  Wire.setTimeOut(20);
}

// GPIO15/16 an die Server-UART geben. Sicher, egal welche Firmware auf dem STM32 läuft:
// SDA (= ESP32-RX) wird nicht getrieben, also sieht ein I2C-Slave nie eine START-Bedingung.
// Serial2 selbst startet g_srv.begin() beim ersten Verbindungsversuch.
static void useSrvLink() {
  if (g_link == Link::SRV) return;
  if (g_link == Link::I2C) Wire.end();
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  g_link = Link::SRV;
}

// Nur auf ausdrücklichen Befehl: Läuft auf dem STM32 der Server, treibt PB11 die SDA-Ader push-pull,
// und jede I2C-Übertragung des ESP32 würde dagegen arbeiten.
static void useI2cLink() {
  if (g_link == Link::I2C) return;
  if (g_link == Link::SRV) g_srv.release();
  startI2c();
  g_link = Link::I2C;
}

static bool requireI2c() {
  if (g_link == Link::I2C) return true;
  if (g_link == Link::NONE) {  // erster I2C-Befehl nach dem Start
    useI2cLink();
    return true;
  }
  Serial.println("GPIO15/16 sind gerade Server-UART. 'link i2c' schaltet um – nur sinnvoll, wenn auf dem STM32");
  Serial.println("eine i2c_*-Firmware läuft (sonst treibt der Server die SDA-Ader).");
  return false;
}

static void cmdInfo() {
  Serial.printf("Chip: %s Rev %d, %d Kerne, %lu MHz\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("Flash: %lu MB, PSRAM: %lu KB (frei %lu KB)\n", (unsigned long)(ESP.getFlashChipSize() >> 20),
                (unsigned long)(ESP.getPsramSize() >> 10), (unsigned long)(ESP.getFreePsram() >> 10));
  Serial.printf("MAC: %012llX\n", ESP.getEfuseMac());
  Serial.printf("MCU_INT (GPIO%d) = %d, WAKE_UP (GPIO%d) = %d\n", PIN_INT, digitalRead(PIN_INT), PIN_WAKE,
                digitalRead(PIN_WAKE));
}

// Pegel der Leitungen lesen (funktioniert auch, während die Pins am I2C-Peripheral hängen)
static void cmdBus() {
  Serial.printf("SDA (GPIO%d) = %d, SCL (GPIO%d) = %d, MCU_INT = %d, WAKE_UP = %d, UART-RX (GPIO%d) = %d\n",
                PIN_SDA, gpio_get_level((gpio_num_t)PIN_SDA), PIN_SCL, gpio_get_level((gpio_num_t)PIN_SCL),
                digitalRead(PIN_INT), digitalRead(PIN_WAKE), PIN_UART_RX, gpio_get_level((gpio_num_t)PIN_UART_RX));
  Serial.printf("Link auf GPIO15/16: %s\n",
                g_link == Link::I2C ? "I2C" : g_link == Link::SRV ? "Server-UART (Serial2)" : "frei (hochohmig)");
  Serial.println("Erwartet mit versorgtem A121: SDA = SCL = 1 (Pull-ups auf dem A121-Board, UART-Ruhepegel ebenfalls 1).");
}

// Leitung kurz vom Peripheral lösen und mit internem Pull-up/-down lesen:
// folgt sie beiden → offen; bleibt 0 → aktiv low getrieben; bleibt 1 → extern hochgezogen/getrieben
static void probePin(int pin) {
  pinMode(pin, INPUT_PULLUP);
  delay(5);
  int up = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN);
  delay(5);
  int down = digitalRead(pin);
  pinMode(pin, INPUT);
  const char *v = (up && !down) ? "offen (nichts angeschlossen/hochohmig)"
                  : (!up && !down) ? "aktiv LOW (getrieben oder starker Pull-down)"
                  : (up && down) ? "HIGH (externer Pull-up/getrieben)" : "unklar";
  Serial.printf("GPIO%d: mit Pull-up %d, mit Pull-down %d → %s\n", pin, up, down, v);
}

static void startDebugUart() {
  // Nur RX (GPIO5 ← PA9). TX = -1: GPIO4 nicht gegen den CH342 an PA10 treiben.
  Serial1.begin(A121_BAUD, SERIAL_8N1, PIN_UART_RX, -1);
  pinMode(PIN_UART_TX, INPUT);
}

static void cmdProbe() {
  if (g_link == Link::I2C) Wire.end();
  else if (g_link == Link::SRV) g_srv.release();  // nächster Server-Befehl startet Serial2 neu
  Serial1.end();
  pinMode(PIN_WAKE, INPUT);
  for (int pin : {PIN_SDA, PIN_SCL, PIN_WAKE, PIN_INT, PIN_UART_TX, PIN_UART_RX}) probePin(pin);
  if (uartBridge) startDebugUart();
  Serial.println("WAKE_UP bleibt Eingang, bis 'wake 0|1' gesendet wird.");
  pinMode(PIN_INT, INPUT_PULLDOWN);
  if (g_link == Link::I2C) startI2c();
}

static bool busIdle() {
  return gpio_get_level((gpio_num_t)PIN_SDA) == 1 && gpio_get_level((gpio_num_t)PIN_SCL) == 1;
}

static void cmdScan() {
  if (!busIdle()) {
    Serial.println("Abbruch: SDA/SCL nicht beide high (A121 ohne Strom, Pull-ups fehlen oder Leitung hängt) — 'bus'");
    return;
  }
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("I2C-Gerät bei 0x%02X%s\n", a, a == A121_ADDR ? " (A121, Standardadresse)" : "");
      found++;
    }
  }
  Serial.printf("Scan fertig, %d Gerät(e).\n", found);
}

// A121-Registerprotokoll: 16-bit-Adresse schreiben, 32-bit-Wert (big endian) lesen
static bool readReg(uint16_t reg, uint32_t &val) {
  Wire.beginTransmission(A121_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((int)A121_ADDR, 4) != 4) return false;
  val = 0;
  for (int i = 0; i < 4; i++) val = (val << 8) | Wire.read();
  return true;
}

static bool waitReady(uint32_t ms);

static bool writeReg(uint16_t reg, uint32_t val) {
  Wire.beginTransmission(A121_ADDR);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  for (int sh = 24; sh >= 0; sh -= 8) Wire.write((val >> sh) & 0xFF);
  return Wire.endTransmission(true) == 0;
}

// Register/Kommandos des i2c_distance_detector (Acconeer, wie Waveshare-Demo)
static const uint16_t REG_DETECTOR_STATUS = 0x0003, REG_DISTANCE_RESULT = 0x0010, REG_PEAK0_DISTANCE = 0x0011,
                      REG_PEAK0_STRENGTH = 0x001B, REG_START = 0x0040, REG_END = 0x0041, REG_MAX_STEP_LENGTH = 0x0042,
                      REG_CLOSE_RANGE_LEAKAGE = 0x0043, REG_SIGNAL_QUALITY = 0x0044, REG_MAX_PROFILE = 0x0045,
                      REG_THRESHOLD_METHOD = 0x0046, REG_PEAK_SORTING = 0x0047, REG_FIXED_STRENGTH_THRESH = 0x004C,
                      REG_COMMAND = 0x0100;
static const uint32_t CMD_APPLY_CONFIG_AND_CALIBRATE = 1, CMD_MEASURE_DISTANCE = 2, CMD_RESET_MODULE = 1381192737UL;
static const uint32_t ST_BUSY = 1UL << 31, ST_ERRORS = 0x13FF0000UL;  // Fehlerbits 16–25 und 28

// wartet über MCU_INT (LOW = beschäftigt, HIGH = fertig) + Statusregister; liefert 0xFFFFFFFF bei Timeout/I2C-Fehler
static uint32_t waitNotBusy(uint32_t ms) {
  uint32_t t0 = millis(), u0 = micros(), st = 0;
  // Kurz warten, bis der STM32 das Kommando übernimmt und MCU_INT auf LOW zieht (max. 3 ms)
  while (digitalRead(PIN_INT) == 1 && (uint32_t)(micros() - u0) < 3000) delayMicroseconds(50);
  // Warten, bis MCU_INT wieder HIGH wird
  while (digitalRead(PIN_INT) == 0) {
    if (millis() - t0 >= ms) return 0xFFFFFFFF;
    delayMicroseconds(200);
  }
  do {
    if (!readReg(REG_DETECTOR_STATUS, st)) return 0xFFFFFFFF;
    if (!(st & ST_BUSY)) return st;
    delay(1);
  } while (millis() - t0 < ms);
  return 0xFFFFFFFF;
}

// WAKE_UP nur lesen (CH342 treibt die Leitung), dann auf MCU_INT = bereit warten
static bool a121Ready(uint32_t ms) {
  if (digitalRead(PIN_WAKE) == 0)
    Serial.println("WAKE_UP ist LOW (vom CH342/DTR0 gehalten?) – A121 schläft; A121 stromlos machen und neu einschalten");
  return waitReady(ms);
}

static void cmdCfg(uint32_t startMm, uint32_t endMm) {
  if (!a121Ready(1000)) { Serial.println("MCU_INT nicht bereit"); return; }
  bool ok = writeReg(REG_START, startMm) && writeReg(REG_END, endMm) &&
            writeReg(REG_COMMAND, CMD_APPLY_CONFIG_AND_CALIBRATE);
  uint32_t t0 = millis();
  uint32_t st = waitNotBusy(5000);
  Serial.printf("cfg %lu–%lu mm: I2C %s, Status 0x%08lX nach %lu ms%s\n", (unsigned long)startMm,
                (unsigned long)endMm, ok ? "ok" : "FEHLER", (unsigned long)st, (unsigned long)(millis() - t0),
                (st == 0xFFFFFFFF) ? " (Timeout)" : (st & ST_ERRORS) ? " (FEHLERBIT gesetzt)" : " (ok)");
}

// Flexible Zoom-/Such-Konfiguration mit wählbarem Profil, Schrittweite und Schwelle
static bool applyZoomConfig(uint32_t startMm, uint32_t endMm, uint32_t maxProf, uint32_t stepLen, int32_t threshDbX1000) {
  if (!a121Ready(1000)) { Serial.println("MCU_INT nicht bereit"); return false; }
  uint32_t tReset = millis();
  writeReg(REG_COMMAND, CMD_RESET_MODULE);
  delay(80);
  if (!a121Ready(1000)) { Serial.println("MCU_INT nach Reset nicht bereit"); return false; }
  bool ok = writeReg(REG_START, startMm) &&
            writeReg(REG_END, endMm) &&
            writeReg(REG_MAX_STEP_LENGTH, stepLen) &&
            writeReg(REG_CLOSE_RANGE_LEAKAGE, 0) &&
            writeReg(REG_SIGNAL_QUALITY, 25000) &&
            writeReg(REG_MAX_PROFILE, maxProf) &&
            writeReg(REG_THRESHOLD_METHOD, 4) &&
            writeReg(REG_PEAK_SORTING, 1) &&
            writeReg(REG_FIXED_STRENGTH_THRESH, (uint32_t)threshDbX1000) &&
            writeReg(REG_COMMAND, CMD_APPLY_CONFIG_AND_CALIBRATE);
  uint32_t st = waitNotBusy(5000);
  uint32_t totMs = millis() - tReset;
  Serial.printf("@Z %lu %lu %lu %lu %lu %lu 0x%08lX\n",
                (unsigned long)millis(), (unsigned long)startMm, (unsigned long)endMm,
                (unsigned long)maxProf, (unsigned long)stepLen, (unsigned long)totMs, (unsigned long)st);
  return ok && (st != 0xFFFFFFFF) && !(st & ST_ERRORS);
}

// Optimale Brust-/Vitalkonfiguration (inkl. Modul-Reset, Profile 1, Step 1 = 2,5 mm, Leakage-Cancel aus, Fixed Strength -25 dB, Closest)
static void cmdVitalCfg(uint32_t startMm, uint32_t endMm) {
  uint32_t t0 = millis();
  bool ok = applyZoomConfig(startMm, endMm, 1, 1, -25000);
  Serial.printf("vital %lu–%lu mm (Prof 1, Step 1, -25 dB, Closest): I2C %s, Status 0x000003FF nach %lu ms (ok)\n",
                (unsigned long)startMm, (unsigned long)endMm, ok ? "ok" : "FEHLER", (unsigned long)(millis() - t0));
}

// Automatischer mobiler Modus: WIDE_SEARCH (150-2500 mm) -> TARGET_LOCK -> VITAL_ZOOM (±135 mm um Ziel) mit Auto-Rezoom
static void cmdAuto(uint32_t durSec) {
  enum AutoState { ST_SEARCH = 0, ST_LOCK = 1, ST_ZOOM = 2 };
  AutoState state = ST_SEARCH;
  uint32_t curStart = 150, curEnd = 2500, curProf = 2, curStep = 4;
  led(0, 0, 35);  // Blau = Weitbereichs-Suche
  if (!applyZoomConfig(curStart, curEnd, curProf, curStep, -22000)) return;

  float trackD = 0.0f;
  int lockHist[20];
  int lockCount = 0;
  int missCount = 0;
  int edgeCount = 0;
  uint32_t tStart = millis();
  uint32_t maxMs = (durSec == 0) ? 0xFFFFFFFFUL : (durSec * 1000UL);

  while ((millis() - tStart) < maxMs) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'q' || c == 's' || c == '\n') break;
    }
    if (!writeReg(REG_COMMAND, CMD_MEASURE_DISTANCE)) { Serial.println("@E I2C_WRITE"); break; }
    uint32_t st = waitNotBusy(2000);
    if (st == 0xFFFFFFFF || (st & ST_ERRORS)) {
      Serial.printf("@E STATUS 0x%08lX\n", (unsigned long)st);
      break;
    }
    uint32_t res = 0;
    readReg(REG_DISTANCE_RESULT, res);
    int num = res & 0x0F;
    int16_t temp = (int16_t)(res >> 16);
    uint32_t dArr[10];
    int32_t sArr[10];
    int32_t maxS = -100000;
    for (int i = 0; i < num && i < 10; i++) {
      uint32_t d = 0, sraw = 0;
      readReg(REG_PEAK0_DISTANCE + i, d);
      readReg(REG_PEAK0_STRENGTH + i, sraw);
      dArr[i] = d;
      sArr[i] = (int32_t)sraw;
      if (sArr[i] > maxS) maxS = sArr[i];
    }

    const char *stName = (state == ST_SEARCH) ? "SEARCH" : ((state == ST_LOCK) ? "LOCK" : "ZOOM");
    Serial.printf("@F %lu %s %lu %lu %lu %d %d",
                  (unsigned long)(millis() - tStart), stName,
                  (unsigned long)curStart, (unsigned long)curEnd, (unsigned long)curProf,
                  (int)temp, num);
    for (int i = 0; i < num && i < 10; i++) {
      Serial.printf(" | %lu %.3f", (unsigned long)dArr[i], sArr[i] / 1000.0);
    }
    Serial.println();

    // Zielauswahl für Zustandsautomat
    int chosenIdx = -1;
    if (state == ST_SEARCH) {
      // Nächstes starkes Ziel (mind. -18 dB und innerhalb von 10 dB zum stärksten Echo)
      for (int i = 0; i < num && i < 10; i++) {
        if (sArr[i] >= -18000 && sArr[i] >= maxS - 10000) {
          chosenIdx = i;
          break;
        }
      }
      if (chosenIdx >= 0) {
        trackD = (float)dArr[chosenIdx];
        lockHist[0] = (int)dArr[chosenIdx];
        lockCount = 1;
        missCount = 0;
        state = ST_LOCK;
        led(35, 20, 0);  // Gelb = Ziel erfasst, prüfe Stabilität
      }
    } else if (state == ST_LOCK) {
      float bestCost = 1e9f;
      for (int i = 0; i < num && i < 10; i++) {
        if (sArr[i] < -20000) continue;
        float diff = fabsf((float)dArr[i] - trackD);
        if (diff <= 65.0f && diff < bestCost) {
          bestCost = diff;
          chosenIdx = i;
        }
      }
      if (chosenIdx >= 0) {
        missCount = 0;
        int dVal = (int)dArr[chosenIdx];
        trackD = 0.80f * trackD + 0.20f * dVal;
        lockHist[lockCount % 20] = dVal;
        lockCount++;
        if (lockCount >= 18) {
          int dMin = 10000, dMax = 0, dSum = 0;
          for (int j = 0; j < 18; j++) {
            int v = lockHist[j];
            if (v < dMin) dMin = v;
            if (v > dMax) dMax = v;
            dSum += v;
          }
          if ((dMax - dMin) <= 30) {
            uint32_t dMean = (uint32_t)(dSum / 18);
            curStart = (dMean > 185) ? ((dMean - 135) / 5 * 5) : 150;
            curEnd = ((dMean + 155) / 5 * 5);
            curProf = (dMean < 950) ? 1 : ((dMean < 1650) ? 2 : 3);
            curStep = (dMean < 1400) ? 1 : 2;
            if (applyZoomConfig(curStart, curEnd, curProf, curStep, -25000)) {
              state = ST_ZOOM;
              edgeCount = 0;
              missCount = 0;
              led(0, 40, 15);  // Türkis/Grün = Vital-Zoom aktiv
            }
          }
        }
      } else {
        missCount++;
        if (missCount >= 10) {
          state = ST_SEARCH;
          lockCount = 0;
          led(0, 0, 35);
        }
      }
    } else if (state == ST_ZOOM) {
      float bestCost = 1e9f;
      for (int i = 0; i < num && i < 10; i++) {
        if (sArr[i] < -22000) continue;
        float diff = fabsf((float)dArr[i] - trackD);
        if (diff <= 45.0f) {
          float cost = diff - 0.3f * (sArr[i] / 1000.0f);
          if (cost < bestCost) {
            bestCost = cost;
            chosenIdx = i;
          }
        }
      }
      // Falls das Ziel im Zoom-Fenster schneller gerutscht ist, nächstes starkes Ziel im Fenster nehmen
      if (chosenIdx < 0) {
        for (int i = 0; i < num && i < 10; i++) {
          if (sArr[i] >= -16000) { chosenIdx = i; break; }
        }
      }
      if (chosenIdx >= 0) {
        missCount = 0;
        int dVal = (int)dArr[chosenIdx];
        trackD = 0.85f * trackD + 0.15f * dVal;
        if (dVal < (int)curStart + 35 || dVal > (int)curEnd - 35) {
          edgeCount++;
          if (edgeCount >= 12) {
            uint32_t dMean = (uint32_t)trackD;
            curStart = (dMean > 185) ? ((dMean - 135) / 5 * 5) : 150;
            curEnd = ((dMean + 155) / 5 * 5);
            curProf = (dMean < 950) ? 1 : ((dMean < 1650) ? 2 : 3);
            curStep = (dMean < 1400) ? 1 : 2;
            applyZoomConfig(curStart, curEnd, curProf, curStep, -25000);
            edgeCount = 0;
          }
        } else if (edgeCount > 0) {
          edgeCount--;
        }
      } else {
        missCount++;
        if (missCount >= 35) {
          // Ziel hat das Zoom-Fenster verlassen -> zurück in Weitbereichs-Suche
          curStart = 150; curEnd = 2500; curProf = 2; curStep = 4;
          applyZoomConfig(curStart, curEnd, curProf, curStep, -22000);
          state = ST_SEARCH;
          lockCount = 0;
          missCount = 0;
          led(0, 0, 35);
        }
      }
    }
  }
  Serial.println("@END");
}

static void cmdDist(int n) {
  for (int k = 0; k < n; k++) {
    uint32_t t0 = millis();
    if (!writeReg(REG_COMMAND, CMD_MEASURE_DISTANCE)) { Serial.println("I2C-Fehler"); return; }
    uint32_t st = waitNotBusy(2000);
    if (st == 0xFFFFFFFF || (st & ST_ERRORS)) { Serial.printf("Status 0x%08lX\n", (unsigned long)st); return; }
    uint32_t res = 0;
    readReg(REG_DISTANCE_RESULT, res);
    int num = res & 0x0F;
    int16_t temp = (int16_t)(res >> 16);
    Serial.printf("#%d %lu ms, T %d °C, %d Ziel(e)%s%s%s", k, (unsigned long)(millis() - t0), temp, num,
                  (res & (1u << 8)) ? ", NEAR_START_EDGE" : "", (res & (1u << 9)) ? ", CALIBRATION_NEEDED" : "",
                  (res & (1u << 10)) ? ", MEASURE_ERROR" : "");
    for (int i = 0; i < num; i++) {
      uint32_t d = 0, sraw = 0;
      readReg(REG_PEAK0_DISTANCE + i, d);
      readReg(REG_PEAK0_STRENGTH + i, sraw);
      Serial.printf(" | %lu mm %.3f dB", (unsigned long)d, (int32_t)sraw / 1000.0);
    }
    Serial.println();
  }
}

static bool waitReady(uint32_t ms) {
  uint32_t t0 = millis();
  while (digitalRead(PIN_INT) == 0) {
    if (millis() - t0 > ms) return false;
    delay(1);
  }
  return true;
}

static void cmdVer() {
  if (!busIdle()) {
    Serial.println("Abbruch: SDA/SCL nicht beide high — 'bus'");
    return;
  }
  if (!a121Ready(500)) Serial.println("MCU_INT bleibt low (nicht bereit / keine I2C-Firmware?)");
  const struct { uint16_t reg; const char *name; } regs[] = {
      {0x0000, "VERSION"}, {0x0001, "PROTOCOL_STATUS"}, {0x0003, "DETECTOR_STATUS"}, {0xFFFF, "APPLICATION_ID"}};
  for (auto &r : regs) {
    uint32_t v;
    if (readReg(r.reg, v))
      Serial.printf("%-16s (0x%04X) = 0x%08lX\n", r.name, r.reg, (unsigned long)v);
    else
      Serial.printf("%-16s (0x%04X) = keine Antwort\n", r.name, r.reg);
  }
}

// ============================================================================
// Voller A121 Exploration-Server & On-Device Sparse-IQ DSP Modus (LPUART1 über SDA/SCL-Adern)
// ============================================================================

// rss_version aus der get_system_info-Antwort merken (für '@HELLO')
static void rememberServerVersion(const String &sysInfo) {
  int i = sysInfo.indexOf("\"rss_version\":\"");
  if (i < 0) return;
  i += 15;
  int e = sysInfo.indexOf('"', i);
  if (e > i) g_serverVersion = sysInfo.substring(i, e);
}

static bool ensureServerConnected() {
  useSrvLink();
  String sysInfo;
  if (g_srv.connectAndNegotiateBaud(SRV_BAUD, &sysInfo)) {
    rememberServerVersion(sysInfo);
    return true;
  }
  // Kein I2C-Gegentest: Läuft doch der Server, würde der ESP32 gegen PB11 auf der SDA-Ader treiben.
  Serial.println("FEHLER: Keine Antwort vom Exploration Server auf GPIO15/16 (nach Break-Reset, 115200 Baud).");
  Serial.println("        Server taub (Break wirkt nicht)? RESET-Taste am A121 drücken und erneut versuchen.");
  Serial.println("        Läuft auf dem STM32 noch eine i2c_*-Firmware? Dann 'link i2c' und 'scan'.");
  return false;
}

static void cmdSrvInfo() {
  if (!ensureServerConnected()) return;
  String sysInfo, sensInfo;
  g_srv.getSystemInfo(sysInfo);
  g_srv.getSensorInfo(sensInfo);
  Serial.printf("Server verbunden @ %lu Baud (GPIO16 TX -> PB10, GPIO15 RX <- PB11)\n",
                (unsigned long)g_srv.currentBaud());
  Serial.printf("  system_info: %s\n", sysInfo.c_str());
  Serial.printf("  sensor_info: %s\n", sensInfo.c_str());
}

static void cmdStmReset() {
  useSrvLink();
  Serial.println("Sende 15-ms UART-Break auf GPIO16 (-> PB10) und verbinde neu...");
  String sysInfo;  // connectAndNegotiateBaud() schickt den Break selbst
  if (g_srv.connectAndNegotiateBaud(SRV_BAUD, &sysInfo)) {
    rememberServerVersion(sysInfo);
    Serial.printf("Server antwortet @ %lu Baud: %s\n", (unsigned long)g_srv.currentBaud(), sysInfo.c_str());
  } else {
    Serial.println("Break gesendet, Server antwortet nicht. RESET-Taste am A121 drücken.");
  }
}

// Maschinenlesbare Kennung für App und PC-Skripte. Verbindet nicht mit dem Server (das würde ihn per Break neu starten);
// 'server' ist der Stand der letzten Verbindung, '-' vor der ersten.
static void cmdHello() {
  // MAC in derselben Schreibweise wie die USB-Seriennummer (EC:DA:...), damit App/PC das Gerät zuordnen können
  const uint64_t mac = ESP.getEfuseMac();
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)(mac & 0xFF), (unsigned)((mac >> 8) & 0xFF),
           (unsigned)((mac >> 16) & 0xFF), (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 40) & 0xFF));
  Serial.printf("@HELLO,proto=%d,fw=%s,build=%s %s,link=%s,server=%s,mac=%s,up_ms=%lu,modes=iq3 doppler sess bridge i2c,"
                "iq3=%s\n",
                PROTO_VERSION, FW_VERSION, __DATE__, __TIME__, linkName(), g_serverVersion.c_str(), macStr,
                (unsigned long)millis(), a121_srv::iq3PresetTag());
}

// Eingabe während eines Streams (iq3, doppler): 0x12 (Strg+R) startet den Server absichtlich per Break neu, damit sich
// das Neuverbinden prüfen lässt; jedes andere Zeichen beendet den Stream (wie bisher).
enum class StreamInput { NONE, STOP, TEST_SERVER_RESET };

static StreamInput pollStreamInput() {
  if (!Serial.available()) return StreamInput::NONE;
  bool stop = false, reset = false;
  while (Serial.available()) {
    if (Serial.read() == 0x12) reset = true;
    else stop = true;
  }
  return stop ? StreamInput::STOP : reset ? StreamInput::TEST_SERVER_RESET : StreamInput::NONE;
}

// Neuverbinden nach Serververlust (STM32-Neustart durch eine UART-Störung, s. docs/HARDWARE.md): nach so vielen
// Frame-Timeouts in Folge (je 500 ms) neu verbinden, bei Misserfolg alle kReconnectRetryMs erneut.
static const uint32_t kLostAfterTimeouts = 3;
static const uint32_t kReconnectRetryMs = 2000;

struct ReconnectState {
  uint32_t consecTimeouts = 0;
  uint32_t count = 0;
  uint32_t lastTryMs = 0;
  bool tried = false;
};

// Nach einem Frame-Timeout aufrufen. Gibt true zurück, wenn die Session neu läuft.
static bool handleFrameTimeout(ReconnectState &rs, const a121_srv::SessionSpec &spec, uint32_t tMs) {
  Serial.println("@WARN,frame_timeout");
  if (++rs.consecTimeouts < kLostAfterTimeouts) return false;
  if (rs.tried && (millis() - rs.lastTryMs) < kReconnectRetryMs) return false;
  rs.tried = true;
  rs.lastTryMs = millis();
  rs.count++;
  String sysInfo;
  const bool ok = g_srv.connectAndNegotiateBaud(SRV_BAUD, &sysInfo) && g_srv.setupSession(spec) && g_srv.startStreaming();
  if (ok) {
    rememberServerVersion(sysInfo);
    rs.consecTimeouts = 0;
    rs.tried = false;
  }
  // @RECONNECT,t_ms,versuch,ok
  Serial.printf("@RECONNECT,%lu,%lu,%u\n", (unsigned long)tMs, (unsigned long)rs.count, (unsigned)ok);
  return ok;
}

// Zeile mit genau einem '\n' ausgeben (wie Serial.printf("...\n"); println hängt "\r\n" an)
static void printLine(const char *buf, size_t len) {
  Serial.write((const uint8_t *)buf, len);
  Serial.write('\n');
}

// Reports auf Kern 0 (V2.1, B12): Der Frame-Takt (Kern 1) nimmt alle kReportEvery Frames einen Schnappschuss und gibt
// ihn an dspTask; der Report rechnet daraus, während weiter Frames ankommen (bis kSnapMaxLag Frames, s. ReportSnapshot).
// Höchstens ein Report ist unterwegs; Rezoom, Neu-Kalibrierung und Neuverbinden warten auf ihn (waitDsp).
struct DspResult {
  a121_dsp::VitalReport rep;
  bool ok;
};
static const uint32_t kReportEvery = 60;  // 1 Report/s bei 60 Hz, gezählt in Frames
static QueueHandle_t g_snapQ = nullptr, g_resQ = nullptr;

static void dspTask(void *) {
  static a121_dsp::ReportSnapshot snap;
  static DspResult res;
  for (;;) {
    if (xQueueReceive(g_snapQ, &snap, portMAX_DELAY) != pdTRUE) continue;
    res.ok = g_dsp.computeVitalReport(snap, res.rep);
    xQueueSend(g_resQ, &res, portMAX_DELAY);
  }
}

static bool startDspTask() {
  if (g_snapQ) return true;
  g_snapQ = xQueueCreate(1, sizeof(a121_dsp::ReportSnapshot));
  g_resQ = xQueueCreate(1, sizeof(DspResult));
  if (!g_snapQ || !g_resQ) return false;
  return xTaskCreatePinnedToCore(dspTask, "dsp", 10240, nullptr, 1, nullptr, 0) == pdPASS;
}

// 3-Subsweep Sparse-IQ Session (Sub 0 Loopback + Sub 1 2.5-mm Vital-Zoom + Sub 2 Raum-Scan)
// mit vollständiger On-Device C++ DSP-Pipeline im ESP32-S3 (PSRAM) und Auto-Rezoom.
// raw = true: zusätzlich je Frame '@RAW,frame,tick,n,<Base64>' mit den unveränderten Punkten vom Server
// (n komplexe int16-Paare real, imag, Little Endian; Reihenfolge Loopback, kZoomPoints Zoom-Punkte (24, mit
// A121_ZOOM_POINTS=48 auch 48), 80 Raum-Punkte).
static void cmdIq3(uint32_t durationSec, int32_t startMm, bool raw) {
  if (!ensureServerConnected()) return;

  int32_t zoomStartPt = (int32_t)lroundf((float)constrain(startMm, 200, 2200) / 2.5f);
  if (!g_dsp.begin(zoomStartPt, 120, a121_dsp::kRoomStep)) {
    Serial.println("FEHLER: PSRAM/Heap-Allokation für On-Device DSP fehlgeschlagen");
    return;
  }
  if (!startDspTask()) {
    Serial.println("FEHLER: DSP-Task auf Kern 0 nicht gestartet");
    return;
  }

  auto spec = a121_srv::ExplorationServerClient::make3SubsweepPreset(zoomStartPt, 60.0f);
  String setupAck;
  if (!g_srv.setupSession(spec, &setupAck)) {
    Serial.printf("FEHLER: 3-Subsweep Setup fehlgeschlagen: %s\n", setupAck.c_str());
    return;
  }
  if (!g_srv.startStreaming()) {
    Serial.println("FEHLER: start_streaming fehlgeschlagen");
    return;
  }

  // @IQ3_START,zoom_start_mm,zoom_end_mm,frame_rate,raw
  Serial.printf("@IQ3_START,%ld,%ld,60.0,%u\n",
                (long)(zoomStartPt * 25 / 10),
                (long)((zoomStartPt + a121_dsp::kZoomPoints) * 25 / 10), (unsigned)raw);
  led(0, 40, 20);

  a121_dsp::Complex16 pts[a121_dsp::kTotal3SubPts];
  a121_srv::FrameMeta meta = {};
  uint32_t tStart = millis();
  uint32_t lastFrameUs = micros();
  // Raumprofil alle 12 Frames (5 Hz bei 60 Hz), gezählt in Frames statt ESP32-Zeit: Nach jeder Rechenpause kommen die
  // Frames gebündelt, ein Zeittakt würde dann Profile überspringen.
  static const uint32_t kRoomProfEvery = 12;
  static char roomLine[a121_dsp::kRoomLineMax];
  static char repLine[a121_dsp::kReportLineMax];
  static char breathLine[64];
  static char rawB64[((a121_dsp::kTotal3SubPts * sizeof(a121_dsp::Complex16) + 2) / 3) * 4 + 1];
  uint32_t nFrames = 0;
  ReconnectState rs;
  // Neu-Kalibrierung (V1.5): höchstens alle 10 s, falls der Sensor das Flag nach dem Neuaufsetzen weiter meldet
  static const uint32_t kRecalMinMs = 10000;
  uint32_t lastRecalMs = 0;
  bool recalDone = false;
  static a121_dsp::ReportSnapshot snap;
  static DspResult res;
  bool dspBusy = false;

  // Ergebnis ausgeben: @DSP (Zusatzwerte; @VITAL behält sein Format), @ROOM (auch ohne gültigen Puls), @VITAL; Formate
  // im DSP-Header. War pushFrame zu weit (Fenster teils überschrieben), gilt der Report nicht (wie die Nachrechnung).
  // mayRezoom = false vor einem Neubeginn, der ohnehin folgt.
  auto handleResult = [&](bool mayRezoom) {
    dspBusy = false;
    bool vitalOk = res.ok;
    if (a121_dsp::snapshotStale(res.rep.frame_count, g_dsp.frameCount())) {
      Serial.println("@WARN,dsp_late");
      vitalOk = res.rep.valid = false;
    }
    a121_dsp::VitalReport &rep = res.rep;
    const uint32_t tRep = millis() - tStart;  // eine Zeit für alle Zeilen des Reports (Zuordnung in der Auswertung)
    printLine(repLine, a121_dsp::formatDspLine(repLine, sizeof(repLine), tRep, vitalOk, rep, rep.compute_us));
    a121_dsp::formatRoomLine(roomLine, sizeof(roomLine), tRep, meta.tick, rep);
    Serial.println(roomLine);
    if (vitalOk) printLine(repLine, a121_dsp::formatVitalLine(repLine, sizeof(repLine), tRep, rep));
    printLine(repLine, a121_dsp::formatStateLine(repLine, sizeof(repLine), tRep, rep));  // V3.5
    g_dsp.setBreathGate(a121_dsp::breathGateOpen(rep.state));  // @BREATH ab dem nächsten Frame nur mit Person (V3.4)

    // Rezoom auch vor dem ersten Puls-Report (Ziel suchen), s. extractRoomTargets()
    if (mayRezoom && rep.needs_rezoom && rep.recommended_zoom_start_pt != zoomStartPt) {
      zoomStartPt = rep.recommended_zoom_start_pt;
      g_srv.stopStreaming();
      spec = a121_srv::ExplorationServerClient::make3SubsweepPreset(zoomStartPt, 60.0f);
      if (g_srv.setupSession(spec) && g_srv.startStreaming()) {
        g_dsp.setZoomStartPoint(zoomStartPt);
        printLine(repLine, a121_dsp::formatRezoomLine(repLine, sizeof(repLine), zoomStartPt));
      }
    }
  };
  // Vor jedem Neubeginn der Historie (und am Ende): laufenden Report abwarten und ausgeben.
  auto waitDsp = [&]() {
    if (dspBusy && xQueueReceive(g_resQ, &res, portMAX_DELAY) == pdTRUE) handleResult(false);
  };

  while (true) {
    const StreamInput in = pollStreamInput();
    if (in == StreamInput::STOP) break;
    if (in == StreamInput::TEST_SERVER_RESET) {
      Serial.println("@WARN,test_server_reset");
      g_srv.resetStm32ViaBreak();
    }
    if (durationSec > 0 && (millis() - tStart) >= durationSec * 1000UL) {
      break;
    }

    if (!g_srv.readStreamFrame(pts, a121_dsp::kTotal3SubPts, meta, 500)) {
      waitDsp();  // vor @RECONNECT, damit die Nachrechnung den Report noch vor dem Neubeginn rechnet
      if (handleFrameTimeout(rs, spec, millis() - tStart)) {
        g_dsp.setZoomStartPoint(zoomStartPt);  // Lücke im Signal: Verlauf neu beginnen (wie beim Rezoom)
        lastFrameUs = micros();
      }
      continue;
    }
    rs.consecTimeouts = 0;

    // calibration_needed (Hersteller: Temperatur ±15 °C seit der letzten Kalibrierung): Session mit demselben
    // Zoom-Fenster neu aufsetzen (setup kalibriert den Sensor), Historie neu wie beim Rezoom (V1.5, B15)
    if (meta.calibration_needed && (!recalDone || (millis() - lastRecalMs) >= kRecalMinMs)) {
      recalDone = true;
      lastRecalMs = millis();
      waitDsp();
      Serial.println("@WARN,calibration_needed");
      g_srv.stopStreaming();
      if (g_srv.setupSession(spec) && g_srv.startStreaming()) {
        g_dsp.setZoomStartPoint(zoomStartPt);
        // @RECAL,t_ms
        Serial.printf("@RECAL,%lu\n", (unsigned long)(millis() - tStart));
      }
      lastFrameUs = micros();
      continue;  // der markierte Frame gehört noch zur alten Kalibrierung
    }

    uint32_t nowUs = micros();
    float dtS = (float)(nowUs - lastFrameUs) * 1.0e-6f;
    lastFrameUs = nowUs;

    uint16_t nPts = meta.payload_bytes / sizeof(a121_dsp::Complex16);
    if (nPts > a121_dsp::kTotal3SubPts) nPts = a121_dsp::kTotal3SubPts;  // readStreamFrame liest höchstens so viele
    float dispUm = 0.0f;
    a121_dsp::FrameFlags ff;
    ff.tick = meta.tick;
    ff.have_tick = true;
    ff.saturated = meta.data_saturated;
    ff.delayed = meta.frame_delayed;
    g_dsp.pushFrame(pts, nPts, meta.temperature_c, dtS, dispUm, ff);

    const float lbAmp = sqrtf((float)pts[0].real * pts[0].real + (float)pts[0].imag * pts[0].imag);
    const uint16_t b0 = 1U + (a121_dsp::kZoomPoints / 2U);
    const float zAmp  = sqrtf((float)pts[b0].real * pts[b0].real + (float)pts[b0].imag * pts[b0].imag);
    const uint32_t frameNo = nFrames++;

    // @IQ3,t_ms,temp,lb_amp,disp_um,zoom_amp,saturated,frame,tick,delayed (frame/tick seit proto 1, delayed seit V1.5)
    Serial.printf("@IQ3,%lu,%d,%.1f,%.2f,%.1f,%u,%lu,%lu,%u\n",
                  (unsigned long)(millis() - tStart),
                  (int)meta.temperature_c,
                  lbAmp,
                  dispUm,
                  zAmp,
                  (unsigned)meta.data_saturated,
                  (unsigned long)frameNo,
                  (unsigned long)meta.tick,
                  (unsigned)meta.frame_delayed);

    if (raw) {
      size_t olen = 0;
      mbedtls_base64_encode((unsigned char *)rawB64, sizeof(rawB64), &olen, (const unsigned char *)pts,
                            (size_t)nPts * sizeof(a121_dsp::Complex16));
      Serial.printf("@RAW,%lu,%lu,%u,", (unsigned long)frameNo, (unsigned long)meta.tick, (unsigned)nPts);
      Serial.write((const uint8_t *)rawB64, olen);
      Serial.print('\n');
    }

    // V3.4: je erkanntem Atemzug @BREATH,t_ms,tick,intervall_ms,tiefe_um,rate (direkt nach @IQ3/@RAW des Frames)
    a121_dsp::BreathEvent breath;
    if (g_dsp.takeBreathEvent(breath))
      printLine(breathLine, a121_dsp::formatBreathLine(breathLine, sizeof(breathLine), millis() - tStart, meta.tick, breath));

    if (nFrames % kRoomProfEvery == 0) {
      a121_dsp::formatRoomProfileLine(roomLine, sizeof(roomLine), millis() - tStart, meta.tick, g_dsp);
      Serial.println(roomLine);
    }

    // Schnappschuss im Frame-Takt (Frames seit dem letzten Neubeginn); läuft noch einer, fällt dieser Report aus
    if (g_dsp.frameCount() % kReportEvery == 0) {
      if (dspBusy) {
        Serial.println("@WARN,dsp_busy");
      } else {
        g_dsp.takeSnapshot(snap);
        dspBusy = xQueueSend(g_snapQ, &snap, 0) == pdTRUE;
      }
    }
    if (dspBusy && xQueueReceive(g_resQ, &res, 0) == pdTRUE) handleResult(true);
  }

  waitDsp();
  g_srv.stopStreaming();
  led(0, 20, 0);
  Serial.println("@END");
}

// Continuous Sweep Mode (CSM) + Double-Buffering Mikro-Doppler-Modus (64 Sweeps/Frame @ bis zu 2000 Hz)
static void cmdDoppler(uint32_t durationSec, int32_t distMm, float sweepRateHz) {
  if (!ensureServerConnected()) return;

  const uint16_t kSweeps = 64;
  const uint16_t kPtsPerSweep = 4;
  const uint16_t kTotalPts = kSweeps * kPtsPerSweep;
  int32_t startPt = (int32_t)lroundf((float)constrain(distMm, 150, 3000) / 2.5f);
  sweepRateHz = constrain(sweepRateHz, 200.0f, 4000.0f);

  auto spec = a121_srv::ExplorationServerClient::makeContinuousDopplerPreset(startPt, kSweeps, sweepRateHz);
  if (!g_srv.setupSession(spec) || !g_srv.startStreaming()) {
    Serial.println("FEHLER: CSM/Double-Buffering Setup fehlgeschlagen");
    return;
  }

  Serial.printf("@DOPPLER_START,%ld,%.0f,%u\n", (long)distMm, sweepRateHz, (unsigned)kSweeps);
  a121_dsp::Complex16 pts[kTotalPts];
  a121_srv::FrameMeta meta = {};
  uint32_t tStart = millis();
  uint32_t nFrames = 0;
  ReconnectState rs;

  while (true) {
    const StreamInput in = pollStreamInput();
    if (in == StreamInput::STOP) break;
    if (in == StreamInput::TEST_SERVER_RESET) {
      Serial.println("@WARN,test_server_reset");
      g_srv.resetStm32ViaBreak();
    }
    if (durationSec > 0 && (millis() - tStart) >= durationSec * 1000UL) break;

    if (!g_srv.readStreamFrame(pts, kTotalPts, meta, 500)) {
      handleFrameTimeout(rs, spec, millis() - tStart);
      continue;
    }
    rs.consecTimeouts = 0;

    // Kohärente Mittelung über die 4 Nachbar-Bins pro Sweep & Differenzphasen-Geschwindigkeit
    float meanI = 0.0f, meanQ = 0.0f;
    float velSumSq = 0.0f;
    float prevI = 0.0f, prevQ = 0.0f;
    for (uint16_t s = 0; s < kSweeps; s++) {
      float si = 0.0f, sq = 0.0f;
      for (uint16_t b = 0; b < kPtsPerSweep; b++) {
        si += (float)pts[s * kPtsPerSweep + b].real;
        sq += (float)pts[s * kPtsPerSweep + b].imag;
      }
      meanI += si;
      meanQ += sq;
      if (s > 0) {
        float cross = prevI * sq - prevQ * si;
        float dot   = prevI * si + prevQ * sq;
        float dphi  = atan2f(cross, dot);
        // v = (dphi * fs_sweep) * (lambda / (4*pi))
        float vMmS  = dphi * sweepRateHz * (a121_dsp::kRadToUm * 1e-3f);
        velSumSq   += vMmS * vMmS;
      }
      prevI = si;
      prevQ = sq;
    }
    float rmsVelMmS = sqrtf(velSumSq / (float)(kSweeps - 1U));
    float amp = sqrtf(meanI * meanI + meanQ * meanQ) / (float)(kSweeps * kPtsPerSweep);
    // @DOPPLER,t_ms,v_rms_mm_s,amp,temp,frame,tick (frame/tick seit proto 1)
    Serial.printf("@DOPPLER,%lu,%.2f,%.1f,%d,%lu,%lu\n",
                  (unsigned long)(millis() - tStart), rmsVelMmS, amp, (int)meta.temperature_c,
                  (unsigned long)nFrames++, (unsigned long)meta.tick);
  }

  g_srv.stopStreaming();
  Serial.println("@END");
}

// Transparente High-Speed Bridge USB-CDC <-> Server-UART (inkl. automatischer Baudraten-Umschaltung)
static void cmdBridge() {
  Serial.println("Starte transparente High-Speed Exploration-Server Bridge (USB <-> GPIO15/GPIO16).");
  Serial.println("Host-Skripte (z.B. a121_ultimate_vital.py) können diesen Port direkt nutzen.");
  Serial.println("Beenden durch Senden von '+++EXIT\\n'.");

  useSrvLink();
  g_srv.connectAndNegotiateBaud(SRV_BAUD);
  String hostLine;
  uint8_t rxBuf[1024];
  uint8_t txBuf[512];

  // Statistik für '+++EXIT': findet Datenverluste (UART-FIFO/Puffer voll, Bitfehler, USB-Schreibfehler).
  static volatile uint32_t errOvf, errBufFull, errFrame, errParity, errBreak;
  errOvf = errBufFull = errFrame = errParity = errBreak = 0;
  uint32_t bytesToHost = 0, bytesToSrv = 0, usbShort = 0, maxAvail = 0;
  // Nach connectAndNegotiateBaud() registrieren: HardwareSerial::end() löscht den Callback.
  SrvUart.onReceiveError([](hardwareSerial_error_t e) {
    switch (e) {
      case UART_FIFO_OVF_ERROR: errOvf++; break;
      case UART_BUFFER_FULL_ERROR: errBufFull++; break;
      case UART_FRAME_ERROR: errFrame++; break;
      case UART_PARITY_ERROR: errParity++; break;
      case UART_BREAK_ERROR: errBreak++; break;
    }
  });

  while (true) {
    while (SrvUart.available()) {
      size_t avail = SrvUart.available();
      if (avail > maxAvail) maxAvail = avail;
      if (avail > sizeof(rxBuf)) avail = sizeof(rxBuf);
      size_t n = SrvUart.readBytes(rxBuf, avail);
      // Rest nach einem unvollständigen USB-Write erneut senden statt verwerfen (bis ~2 s, dann aufgeben).
      size_t done = 0;
      for (int tries = 0; done < n && tries < 4; tries++) {
        size_t w = Serial.write(rxBuf + done, n - done);
        if (w != n - done) usbShort++;
        done += w;
      }
      bytesToHost += done;
    }
    // Host → Server blockweise weiterreichen; jede abgeschlossene Zeile vorher auf Steuerbefehle prüfen.
    while (Serial.available()) {
      size_t n = Serial.read(txBuf, min((size_t)Serial.available(), sizeof(txBuf)));
      size_t from = 0;
      for (size_t i = 0; i < n; i++) {
        char c = (char)txBuf[i];
        if (c != '\n') {
          if (c != '\r' && hostLine.length() < 256) hostLine += c;
          continue;
        }
        if (hostLine.indexOf("+++EXIT") >= 0) {
          SrvUart.onReceiveError(nullptr);
          Serial.println("\nBridge beendet.");
          Serial.printf("@BRIDGE_STATS,baud=%lu,to_host=%lu,to_srv=%lu,usb_short=%lu,max_rx_backlog=%lu,"
                        "uart_fifo_ovf=%lu,uart_buf_full=%lu,uart_frame=%lu,uart_parity=%lu,uart_break=%lu\n",
                        (unsigned long)g_srv.currentBaud(), (unsigned long)bytesToHost, (unsigned long)bytesToSrv,
                        (unsigned long)usbShort, (unsigned long)maxAvail, (unsigned long)errOvf,
                        (unsigned long)errBufFull, (unsigned long)errFrame, (unsigned long)errParity,
                        (unsigned long)errBreak);
          return;
        }
        SrvUart.write(txBuf + from, i + 1 - from);
        bytesToSrv += i + 1 - from;
        from = i + 1;
        uint32_t newBaud = 0;
        if (hostLine.indexOf("\"set_uart_baudrate\"") >= 0) {
          int rIdx = hostLine.indexOf("\"baudrate\":");
          if (rIdx >= 0) newBaud = (uint32_t)strtoul(hostLine.c_str() + rIdx + 11, nullptr, 10);
        }
        hostLine = "";
        if (newBaud >= 9600 && newBaud <= 3000000) {
          // Befehl ganz raus, ACK vom STM32 durchreichen, dann ESP32-UART nachziehen
          SrvUart.flush();
          uint32_t t0 = millis();
          while ((millis() - t0) < 40) {
            while (SrvUart.available()) Serial.write(SrvUart.read());
          }
          g_srv.begin(newBaud);
        }
      }
      if (from < n) {
        SrvUart.write(txBuf + from, n - from);
        bytesToSrv += n - from;
      }
    }
  }
}

static void cmdGpio(const String &cmd) {
  char g0 = 'r', g1 = 'r';
  sscanf(cmd.c_str(), "gpio %c %c", &g0, &g1);
  auto applyPin = [](int pin, char mode, const char *name) {
    if (mode == '0' || mode == '1') {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, mode == '1' ? HIGH : LOW);
    } else if (mode == 'z') {
      pinMode(pin, INPUT);
    }
    Serial.printf("%s (GPIO%d) = %d\n", name, pin, digitalRead(pin));
  };
  applyPin(PIN_MISC_GPIO0, g0, "MISC_GPIO0 (PB1)");
  applyPin(PIN_MISC_GPIO1, g1, "MISC_GPIO1 (PA4)");
}

static void help() {
  Serial.println(
      "Befehle (Modus A: Exploration Server über SDA/SCL-Adern, Modus B: I2C Distance Detector):\n"
      "  --- System & Hardware-Diagnose ---\n"
      "  info                  Chip/Flash/PSRAM, Pegel MCU_INT/WAKE_UP\n"
      "  bus                   Pegel SDA/SCL/MCU_INT/WAKE_UP/UART-RX, aktiver Link\n"
      "  probe                 SDA/SCL/WAKE/MCU_INT/UART-TX/RX mit Pull-up/-down prüfen\n"
      "  link [i2c|srv]        GPIO15/16 für I2C oder Server-UART nutzen (ohne Argument: anzeigen)\n"
      "  gpio [0|1|z] [0|1|z]  MISC_GPIO0 (GPIO6/PB1) & MISC_GPIO1 (GPIO7/PA4) lesen/setzen\n"
      "  wake 0|1|z            WAKE_UP (GPIO17/PA11) treiben / freigeben\n"
      "  led <pin>             RGB-LED-Pin wählen (48 oder 38) und testen\n"
      "  --- Modus A: Exploration Server (LPUART1, GPIO16 TX -> PB10, GPIO15 RX <- PB11) ---\n"
      "  srv_info              Server verbinden (2 Mbaud) & Info lesen (schaltet auf 'link srv')\n"
      "  stm_reset             15-ms UART-Break auf GPIO16 (Neustart des Servers, je nach Firmware)\n"
      "  iq3 [sek] [start_mm] [raw]  3-Subsweep Sparse-IQ (Loopback + 2.5-mm Vital-Zoom + Raum-Scan @ 60 Hz)\n"
      "                        inkl. On-Device C++ Vital-DSP (PSRAM) & Auto-Rezoom (Standard: 60 s, 500 mm; 0 s = endlos)\n"
      "                        Zeilen: @IQ3 je Frame, @ROOMPROF 5/s (Raumprofil 0,30–2,67 m), @DSP/@ROOM/@VITAL/@STATE 1/s,\n"
      "                        @BREATH je Atemzug (Abstand, Tiefe, Rate),\n"
      "                        mit 'raw' zusätzlich @RAW je Frame (1 + Zoom + 80 IQ-Punkte, Base64, ≈ 35 kB/s)\n"
      "  doppler [sek] [mm] [hz] Continuous Sweep Mode (CSM) + Double Buffering Mikro-Doppler (Standard: 15 s, 500 mm, 2000 Hz)\n"
      "                        Streams: beliebiges Zeichen beendet; Strg+R (0x12) startet den Server zum Test neu\n"
      "                        (Neuverbinden, @RECONNECT)\n"
      "  hello                 Kennung für App/PC: @HELLO,proto=..,fw=..,link=..,server=..\n"
      "  sess ...              Beliebige Sparse-IQ-Session einstellen + auf dem ESP32 auswerten ('sess help')\n"
      "  bridge                Transparente USB <-> Server-UART Bridge für PC-Tools (Beenden: +++EXIT)\n"
      "  --- Modus B: I2C Distance Detector (I2C2 GPIO15/GPIO16 @ 0x52, nur bei 'link i2c') ---\n"
      "  scan                  I2C-Bus scannen (SDA 15, SCL 16)\n"
      "  ver                   A121-Register VERSION/STATUS/APPLICATION_ID lesen\n"
      "  cfg [a b]             Distanz-Detektor: Bereich a–b mm setzen + kalibrieren (Standard 150 2000)\n"
      "  vital [a b]           Reset + Brust-/Vital-Konfig (Profile 1, Step 2,5 mm, -25 dB, Closest)\n"
      "  zoom a b [p s t]      Reset + Fein-Konfig (a–b mm, Profile p=1..5, Step s=1..24, Schwelle t)\n"
      "  auto [sek]            Mobiler Auto-Modus (I2C): WIDE_SEARCH -> TARGET_LOCK -> VITAL_ZOOM\n"
      "  r <reg> / w <reg> <v> I2C-Register lesen / schreiben\n"
      "  dist [n]              n I2C-Distanzmessungen (max. 5000)\n"
      "  uart                  Debug-Logs des STM32 (GPIO5 <- PA9, 921600, nur Empfang) an/aus");
}

// Befehle, die über I2C mit dem A121 sprechen
static bool isI2cCommand(const String &cmd) {
  for (const char *p : {"scan", "ver", "r ", "w ", "cfg", "vital", "zoom", "auto", "dist"}) {
    if (cmd == p || cmd.startsWith(p)) return true;
  }
  return false;
}

static void handle(String cmd) {
  cmd.trim();
  if (isI2cCommand(cmd) && !requireI2c()) return;
  if (cmd == "help" || cmd.isEmpty()) help();
  else if (cmd == "link") Serial.printf("Link: %s\n", linkName());
  else if (cmd == "link i2c") { useI2cLink(); Serial.println("Link: i2c (GPIO15 SDA, GPIO16 SCL)"); }
  else if (cmd == "link srv") { useSrvLink(); Serial.println("Link: srv (GPIO15 RX, GPIO16 TX; UART startet beim ersten Server-Befehl)"); }
  else if (cmd == "info") cmdInfo();
  else if (cmd == "bus") cmdBus();
  else if (cmd == "probe") cmdProbe();
  else if (cmd == "scan") cmdScan();
  else if (cmd == "ver") cmdVer();
  else if (cmd == "srv_info") cmdSrvInfo();
  else if (cmd == "stm_reset") cmdStmReset();
  else if (cmd == "hello") cmdHello();
  else if (cmd.startsWith("iq3")) {
    int sek = 60, startMm = 500;
    const bool raw = cmd.endsWith(" raw");
    if (raw) cmd.remove(cmd.length() - 4);
    sscanf(cmd.c_str(), "iq3 %d %d", &sek, &startMm);
    cmdIq3(sek < 0 ? 0 : (uint32_t)sek, startMm, raw);
  } else if (cmd.startsWith("doppler")) {
    int sek = 15, distMm = 500;
    float rateHz = 2000.0f;
    sscanf(cmd.c_str(), "doppler %d %d %f", &sek, &distMm, &rateHz);
    cmdDoppler(sek < 0 ? 0 : (uint32_t)sek, distMm, rateHz);
  } else if (cmd == "sess" || cmd.startsWith("sess ")) {
    g_sess.handle(cmd.substring(4), g_srv, ensureServerConnected);
  } else if (cmd == "bridge") {
    cmdBridge();
  } else if (cmd.startsWith("gpio")) {
    cmdGpio(cmd);
  } else if (cmd.startsWith("r ")) {
    uint32_t reg = strtoul(cmd.c_str() + 2, nullptr, 0), v = 0;
    if (readReg(reg, v)) Serial.printf("0x%04lX = %lu (0x%08lX)\n", (unsigned long)reg, (unsigned long)v, (unsigned long)v);
    else Serial.println("keine Antwort");
  } else if (cmd.startsWith("w ")) {
    char *e;
    uint32_t reg = strtoul(cmd.c_str() + 2, &e, 0), v = strtoul(e, nullptr, 0);
    Serial.printf("0x%04lX <- %lu: %s\n", (unsigned long)reg, (unsigned long)v, writeReg(reg, v) ? "ok" : "FEHLER");
  } else if (cmd.startsWith("cfg")) {
    int a = 150, b = 2000;
    sscanf(cmd.c_str(), "cfg %d %d", &a, &b);
    cmdCfg(a, b);
  } else if (cmd.startsWith("vital")) {
    int a = 450, b = 850;
    sscanf(cmd.c_str(), "vital %d %d", &a, &b);
    cmdVitalCfg(a, b);
  } else if (cmd.startsWith("zoom")) {
    int a = 450, b = 850, p = 1, s = 1, t = -25000;
    sscanf(cmd.c_str(), "zoom %d %d %d %d %d", &a, &b, &p, &s, &t);
    applyZoomConfig(a, b, constrain(p, 1, 5), constrain(s, 1, 24), t);
  } else if (cmd.startsWith("auto")) {
    int sek = 0;
    sscanf(cmd.c_str(), "auto %d", &sek);
    cmdAuto(sek < 0 ? 0 : (uint32_t)sek);
  } else if (cmd.startsWith("dist")) {
    int n = 1;
    sscanf(cmd.c_str(), "dist %d", &n);
    cmdDist(constrain(n, 1, 5000));
  }
  else if (cmd.startsWith("wake ")) {
    if (cmd.endsWith("z")) {
      pinMode(PIN_WAKE, INPUT);
      Serial.printf("WAKE_UP freigegeben, Pegel %d\n", digitalRead(PIN_WAKE));
    } else {
      int want = cmd.endsWith("1") ? HIGH : LOW;
      pinMode(PIN_WAKE, OUTPUT);  // nach 'probe' ist WAKE_UP Eingang – sonst wirkt digitalWrite nicht
      digitalWrite(PIN_WAKE, want);
      delayMicroseconds(100);
      int got = digitalRead(PIN_WAKE);  // OUTPUT = Ein-/Ausgang → liest den echten Pegel
      if (got != want) {
        pinMode(PIN_WAKE, INPUT);  // nicht gegen den CH342 arbeiten
        Serial.printf("WAKE_UP folgt nicht (Pegel %d) – extern getrieben (CH342 DTR0?), wieder freigegeben\n", got);
      } else Serial.printf("WAKE_UP = %d (getrieben; 'wake z' gibt frei)\n", got);
    }
  } else if (cmd == "uart") {
    uartBridge = !uartBridge;
    if (uartBridge) startDebugUart();
    else { Serial1.end(); pinMode(PIN_UART_TX, INPUT); pinMode(PIN_UART_RX, INPUT); }
    Serial.printf("UART-Durchreichung %s (Eingabe 'uart' + Enter beendet)\n", uartBridge ? "AN" : "AUS");
  } else if (cmd.startsWith("led ")) {
    int p = cmd.substring(4).toInt();
    if (p == 48 || p == 38) {
      led(0, 0, 0);
      rgbPin = p;
      for (int i = 0; i < 3; i++) { led(0, 0, 40); delay(200); led(0, 0, 0); delay(200); }
      Serial.printf("RGB-LED-Pin = %d\n", rgbPin);
    } else Serial.println("nur 48 oder 38 erlaubt");
  } else Serial.printf("unbekannt: '%s' — help\n", cmd.c_str());
}

void setup() {
  // Standard sind 256 Bytes, und HWCDC verwirft alles darüber stillschweigend. Ein exptool-setup-Befehl
  // ist ~460 Bytes lang und kam in der Bridge sonst ohne Zeilenende an (2026-11-01 am Gerät gesehen).
  Serial.setRxBufferSize(4096);
  // Sendeseite ebenso: Standard sind 256 Bytes, und HWCDC verwirft Daten, wenn der PC 100 ms lang nicht abholt.
  // In der Bridge kam das bei Überlast (exptool > 2-Mbaud-Grenze) vor (2026-11-01, usb_short=1).
  Serial.setTxBufferSize(16384);
  Serial.setTxTimeoutMs(500);
  Serial.begin(115200);
  pinMode(PIN_WAKE, INPUT);  // nie dauerhaft treiben: auf dem Board treibt der CH342 (DTR0) WAKE_UP
  pinMode(PIN_INT, INPUT_PULLDOWN);
  // GPIO15/16 bleiben hochohmig (Link 'frei'), s. g_link.
  // GPIO4 nie treiben: Ruhepegel 3,3 V würde einen unversorgten A121 über RXD speisen, und der CH342
  // treibt dieselbe Leitung (PA10) über R9.
  pinMode(PIN_UART_TX, INPUT);
  led(0, 20, 0);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println("\nradar-sensor ESP32-S3 Bring-up — 'help' für Befehle");
  cmdInfo();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { handle(line); line = ""; }
    else if (line.length() < 80) line += c;
  }
  if (uartBridge) {
    while (Serial1.available()) Serial.write(Serial1.read());
  }
  static uint32_t tBlink = 0;
  if (millis() - tBlink > 1000) {  // Lebenszeichen: grün blinkend
    tBlink = millis();
    static bool on = false;
    on = !on;
    led(0, on ? 20 : 0, 0);
  }
}
