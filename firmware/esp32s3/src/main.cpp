// Bring-up-Firmware ESP32-S3 ↔ A121 (Waveshare A121 Range Sensor)
//
// Konsole über den nativen USB-Port (115200, Zeilenende \n). Befehle: help
// Pins wie in docs/HARDWARE.md (= Waveshare-ESP32-S3-Demos):
//   I2C SDA 15, SCL 16, WAKE_UP 17 (Ausgang), MCU_INT 18 (Eingang, high = bereit)
//   UART1: TX 4 → A121 RXD, RX 5 ← A121 TXD (Debug-UART des STM32, 921600 Baud)
//
// Hinweis: Die I2C-Register gibt es nur mit einer i2c_*-Firmware auf dem STM32.
// Mit der Werksfirmware (Exploration Server) findet der Scan nichts unter 0x52.

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

static const int PIN_SDA = 15;
static const int PIN_SCL = 16;
static const int PIN_WAKE = 17;
static const int PIN_INT = 18;
static const int PIN_UART_TX = 4;
static const int PIN_UART_RX = 5;
static const uint32_t A121_BAUD = 921600;
static const uint8_t A121_ADDR = 0x52;

static int rgbPin = 48;  // DevKitC-1 v1.0: 48, v1.1: 38 — mit "led <pin>" umstellbar
static bool uartBridge = false;
static String line;

static void led(uint8_t r, uint8_t g, uint8_t b) { neopixelWrite(rgbPin, r, g, b); }

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
  Serial.println("Erwartet mit versorgtem A121: SDA = SCL = 1 (Pull-ups auf dem A121-Board).");
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

static void cmdProbe() {
  Wire.end();
  Serial1.end();
  pinMode(PIN_WAKE, INPUT);
  for (int pin : {PIN_SDA, PIN_SCL, PIN_WAKE, PIN_INT, PIN_UART_TX, PIN_UART_RX}) probePin(pin);
  Serial1.begin(A121_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  Serial.println("WAKE_UP bleibt Eingang, bis 'wake 0|1' gesendet wird.");
  pinMode(PIN_INT, INPUT_PULLDOWN);
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Wire.setTimeOut(20);
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
  pinMode(PIN_WAKE, OUTPUT);
  digitalWrite(PIN_WAKE, HIGH);
  if (!waitReady(500)) Serial.println("MCU_INT bleibt low (nicht bereit / keine I2C-Firmware?)");
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

static void help() {
  Serial.println(
      "Befehle:\n"
      "  info        Chip/Flash/PSRAM, Pegel MCU_INT/WAKE_UP\n"
      "  bus         Pegel SDA/SCL/MCU_INT/WAKE_UP/UART-RX\n"
      "  probe       SDA/SCL/WAKE/MCU_INT/UART-TX/RX mit internem Pull-up/-down prüfen (offen/low/high)\n"
      "  scan        I2C-Bus scannen (SDA 15, SCL 16)\n"
      "  ver         A121-Register VERSION/STATUS/APPLICATION_ID lesen (nur mit i2c_*-Firmware)\n"
      "  wake 0|1    WAKE_UP setzen\n"
      "  uart        A121-Debug-UART (921600) an USB durchreichen an/aus\n"
      "  led <pin>   RGB-LED-Pin wählen (48 oder 38) und testen");
}

static void handle(String cmd) {
  cmd.trim();
  if (cmd == "help" || cmd.isEmpty()) help();
  else if (cmd == "info") cmdInfo();
  else if (cmd == "bus") cmdBus();
  else if (cmd == "probe") cmdProbe();
  else if (cmd == "scan") cmdScan();
  else if (cmd == "ver") cmdVer();
  else if (cmd.startsWith("wake ")) {
    digitalWrite(PIN_WAKE, cmd.endsWith("1") ? HIGH : LOW);
    Serial.printf("WAKE_UP = %d\n", digitalRead(PIN_WAKE));
  } else if (cmd == "uart") {
    uartBridge = !uartBridge;
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
  Serial.begin(115200);
  pinMode(PIN_WAKE, INPUT);  // erst bei 'wake'/'ver' treiben — schützt bei Verdrahtungsfehlern
  pinMode(PIN_INT, INPUT_PULLDOWN);
  Wire.begin(PIN_SDA, PIN_SCL, 100000);  // Pull-ups 4,7 kΩ sitzen auf dem A121-Board (Schaltplan R13/R14)
  Wire.setTimeOut(20);
  Serial1.begin(A121_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
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
  } else {
    while (Serial1.available()) Serial1.read();  // verwerfen, solange Durchreichung aus
  }
  static uint32_t tBlink = 0;
  if (millis() - tBlink > 1000) {  // Lebenszeichen: grün blinkend
    tBlink = millis();
    static bool on = false;
    on = !on;
    led(0, on ? 20 : 0, 0);
  }
}
