/**
 * @file a121_server_client.h
 * @brief Voller A121 Exploration Server UART-Client & High-Speed Bridge für den ESP32-S3.
 *
 * UART und Pins übergibt main.cpp. Seit 2026-11-01 am Gerät geprüft: Serial2 über die I2C-Adern des Steckers,
 * ESP32 GPIO15 RX <- STM32 PB11 (LPUART1_TX, SDA-Ader) und GPIO16 TX -> PB10 (LPUART1_RX, SCL-Ader),
 * für eine aus dem Acconeer-SDK gebaute Server-Firmware (firmware/stm32l431/README.md).
 * Grund: PA10 (USART1_RX) wird zusätzlich vom CH342 (TXD1 über R9 = 22 Ω) getrieben.
 *
 * Stellt dem ESP32-S3 die Sparse-IQ-Funktionen des Exploration Servers bereit (Grenzen: ≈ 195 kB/s bei 2 Mbaud,
 * CSM lückenlos bis 21 kHz Sweep-Rate, s. docs/HARDWARE.md):
 *   - 1..4 unabhängige Subsweeps pro Frame (Start, Step, NumPoints, Profile 1..5, PRF 19.5..5.2 MHz,
 *     HWAAS 1..511, Receiver Gain 0..23, Enable TX, Phase Enhancement, IQ Imbalance Compensation,
 *     On-Chip Loopback)
 *   - Continuous Sweep Mode (CSM) & Double Buffering für kHz-Mikro-Doppler/Vibrationsanalyse
 *   - Inter-Frame & Inter-Sweep Idle States (DEEP_SLEEP, SLEEP, READY)
 *   - On-Chip Temperatursensor, Sättigungs- und Kalibrierungs-Flags pro Frame
 *   - Baudraten-Aushandlung nach Break-Reset (115200 -> 2000000 Baud)
 *   - Hardware-Soft-Reset des STM32L431 per gezieltem UART-Break-Puls auf der TX-Leitung.
 *
 * Protokoll (am Server a121-v1.12.0 über CH342 Kanal B geprüft, 2026-10-31):
 *   - Vor fast jeder Antwort sendet der Server eine Logzeile {"status":"log",...};
 *     readResponse()/readStreamFrame() überspringen sie.
 *   - Enum-Werte wie exptool: "profile":1..5, "prf":"15_6_MHz", Idle-States "ready"/"sleep"/"deep_sleep",
 *     freie Rate = 0.0 (nicht null).
 *   - Seit der Härtung (2026-11-01) setzt jeder UART-Fehler auf RX (Break, falsche Baudrate, Rauschen) den STM32
 *     zurück → danach 115200 Baud.
 */

#ifndef A121_SERVER_CLIENT_H
#define A121_SERVER_CLIENT_H

#include <Arduino.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a121_esp_dsp.h"

namespace a121_srv {

struct SubsweepSpec {
    int32_t     start_point;
    uint16_t    num_points;
    uint16_t    step_length;
    uint8_t     profile;                    /* 1..5 */
    uint16_t    hwaas;                      /* 1..511 */
    uint8_t     receiver_gain;              /* 0..23 */
    bool        enable_tx;
    bool        phase_enhancement;
    bool        iq_imbalance_compensation;
    bool        enable_loopback;
    const char *prf;                        /* "19_5_MHz", "15_6_MHz", "13_0_MHz", "8_7_MHz", "6_5_MHz", "5_2_MHz" */
};

struct SessionSpec {
    uint16_t     sweeps_per_frame;
    float        sweep_rate_hz;             /* 0.0f = null */
    float        frame_rate_hz;             /* 0.0f = null */
    bool         continuous_sweep_mode;
    bool         double_buffering;
    const char  *inter_frame_idle_state;    /* "deep_sleep", "sleep", "ready" */
    const char  *inter_sweep_idle_state;    /* "deep_sleep", "sleep", "ready" */
    uint8_t      num_subsweeps;             /* 1..4 */
    SubsweepSpec subsweeps[4];
};

struct FrameMeta {
    uint32_t tick;
    int16_t  temperature_c;
    uint16_t payload_bytes;
    bool     data_saturated;
    bool     frame_delayed;
    bool     calibration_needed;
};

class ExplorationServerClient {
public:
    ExplorationServerClient(HardwareSerial &uart, int rx_pin, int tx_pin)
        : uart_(uart), rx_pin_(rx_pin), tx_pin_(tx_pin) {}

    void begin(uint32_t baud = 921600) {
        current_baud_ = baud;
        stage_pos_ = stage_len_ = 0;
        if (!rx_buf_allocated_) {
            uart_.end();                     /* Puffergröße lässt sich nur bei gestoppter UART setzen */
            uart_.setRxBufferSize(32768);
            uart_.setTxBufferSize(4096);     /* sonst blockiert write(), bis im 128-Byte-FIFO Platz ist */
            rx_buf_allocated_ = true;
            uart_.begin(current_baud_, SERIAL_8N1, rx_pin_, tx_pin_);
        } else {
            uart_.updateBaudRate(current_baud_);
        }
    }

    uint32_t currentBaud() const { return current_baud_; }

    HardwareSerial &uart() { return uart_; }

    /** UART stoppen und beide Pins hochohmig machen (z. B. bevor I2C dieselben Pins nutzt). */
    void release() {
        uart_.end();
        stage_pos_ = stage_len_ = 0;
        rx_buf_allocated_ = false;
        pinMode(rx_pin_, INPUT);
        pinMode(tx_pin_, INPUT);
    }

    /**
     * @brief Soft-Reset des STM32L431: 15 ms LOW auf der TX-Leitung erzeugt einen Framing-Error.
     *        Die USART1-Server-Firmware startete daraufhin neu (danach 115200 Baud, bereit nach ~0,5 s);
     *        ob eine SDK-Neubau-Firmware das ebenso tut, ist am Gerät zu prüfen.
     */
    void resetStm32ViaBreak() {
        uart_.end();
        rx_buf_allocated_ = false;
        pinMode(tx_pin_, OUTPUT);
        digitalWrite(tx_pin_, LOW);   /* UART Break = Dauer-LOW > 1 Zeichenlänge */
        delay(15);
        digitalWrite(tx_pin_, HIGH);  /* Idle-HIGH */
        begin(115200);
        drainRx(5);
    }

    /**
     * @brief Verbindet sich mit dem Server und schaltet ihn auf `target_baud`.
     *        Immer erst Break-Reset, dann Handshake auf 115200 (bis ~2,5 s Bootzeit).
     *        Kein Versuch auf einer geratenen Baudrate: Bytes mit falscher Baudrate erzeugen am STM32
     *        Noise-/Overrun-Fehler; die HAL bricht dann den DMA-Empfang ab, und der SDK-Server startet nur
     *        bei einem Framing-Error neu. Er bleibt taub, bis RESET gedrückt wird (2026-11-01 dreimal am Gerät).
     */
    bool connectAndNegotiateBaud(uint32_t target_baud = 921600, String *out_sys_info = nullptr) {
        String resp;
        bool ok = false;
        resetStm32ViaBreak();
        delay(400);
        const uint32_t t0 = millis();
        while (!ok && (millis() - t0) < 2500) {
            ok = tryHandshake(115200U, resp);
        }
        if (!ok) return false;
        if (out_sys_info) *out_sys_info = resp;
        if (current_baud_ == target_baud) return true;

        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"set_uart_baudrate\",\"baudrate\":%lu}\n",
                 (unsigned long)target_baud);
        uart_.print(cmd);
        String ack;
        if (!readResponse(ack, 300) || ack.indexOf("\"status\":\"ok\"") < 0) return false;
        uart_.flush();
        delay(5);
        begin(target_baud);
        drainRx(5);
        return tryHandshake(target_baud, resp);
    }

    bool getSystemInfo(String &out_json) {
        drainRx(10);
        uart_.print("{\"cmd\":\"get_system_info\"}\n");
        return readResponse(out_json, 350) && out_json.indexOf("rss_version") >= 0;
    }

    bool getSensorInfo(String &out_json) {
        drainRx(10);
        uart_.print("{\"cmd\":\"get_sensor_info\"}\n");
        return readResponse(out_json, 350) && out_json.indexOf("connected") >= 0;
    }

    /**
     * @brief Konfiguriert eine beliebige 1..4 Subsweep Sparse-IQ Session auf dem A121.
     */
    bool setupSession(const SessionSpec &spec, String *out_setup_ack = nullptr, String *out_logs = nullptr) {
        if (spec.num_subsweeps == 0 || spec.num_subsweeps > 4) return false;

        String json;
        json.reserve(1536);
        json += "{\"cmd\":\"setup\",\"groups\":[[{\"sensor_id\":1,\"config\":{";
        json += "\"sweeps_per_frame\":";
        json += String(spec.sweeps_per_frame);
        json += ",\"sweep_rate\":";
        json += String(spec.sweep_rate_hz > 0.0f ? spec.sweep_rate_hz : 0.0f, 1);   /* 0.0 = frei */
        json += ",\"frame_rate\":";
        json += String(spec.frame_rate_hz > 0.0f ? spec.frame_rate_hz : 0.0f, 1);
        json += ",\"continuous_sweep_mode\":";
        json += spec.continuous_sweep_mode ? "true" : "false";
        json += ",\"double_buffering\":";
        json += spec.double_buffering ? "true" : "false";
        json += ",\"inter_frame_idle_state\":\"";
        json += spec.inter_frame_idle_state ? spec.inter_frame_idle_state : "ready";
        json += "\",\"inter_sweep_idle_state\":\"";
        json += spec.inter_sweep_idle_state ? spec.inter_sweep_idle_state : "ready";
        json += "\",\"subsweeps\":[";

        for (uint8_t i = 0; i < spec.num_subsweeps; i++) {
            const SubsweepSpec &s = spec.subsweeps[i];
            if (i > 0) json += ",";
            json += "{\"start_point\":";
            json += String(s.start_point);
            json += ",\"num_points\":";
            json += String(s.num_points);
            json += ",\"step_length\":";
            json += String(s.step_length);
            json += ",\"profile\":";
            json += String(s.profile);
            json += ",\"hwaas\":";
            json += String(s.hwaas);
            json += ",\"receiver_gain\":";
            json += String(s.receiver_gain);
            json += ",\"enable_tx\":";
            json += s.enable_tx ? "true" : "false";
            json += ",\"enable_loopback\":";
            json += s.enable_loopback ? "true" : "false";
            json += ",\"phase_enhancement\":";
            json += s.phase_enhancement ? "true" : "false";
            json += ",\"iq_imbalance_compensation\":";
            json += s.iq_imbalance_compensation ? "true" : "false";
            json += ",\"prf\":\"";
            json += s.prf ? s.prf : "15_6_MHz";
            json += "\"}";
        }
        json += "]}}]]}\n";

        drainRx(10);
        uart_.print(json);

        String resp;
        if (!readResponse(resp, 1500, out_logs)) {
            return false;
        }
        if (out_setup_ack) *out_setup_ack = resp;
        return resp.indexOf("\"status\":\"ok\"") >= 0;
    }

    /**
     * @brief Baut das 3-Subsweep Ultimate Vital + Raum-Tracking Preset:
     *        - Subsweep 0: On-Chip Loopback (1 Punkt, Profil 1, HWAAS 8, Loopback=true)
     *        - Subsweep 1: 2.5-mm Vital-Zoom (24 Punkte = 60 mm, Step 1, Profil 1, HWAAS 32, Phase Enhancement)
     *        - Subsweep 2: Full-Room Scan (80 Punkte, 0.30..2.67 m, Step 12 = 30 mm, Profil 3, HWAAS 16)
     */
    static SessionSpec make3SubsweepPreset(int32_t zoom_start_pt = 200, float frame_rate_hz = 60.0f) {
        SessionSpec spec = {};
        spec.sweeps_per_frame       = 1;
        spec.sweep_rate_hz          = 0.0f;
        spec.frame_rate_hz          = frame_rate_hz;
        spec.continuous_sweep_mode  = false;
        spec.double_buffering       = false;
        spec.inter_frame_idle_state = "ready";
        spec.inter_sweep_idle_state = "ready";
        spec.num_subsweeps          = 3;

        /* Subsweep 0: Interner On-Chip TX->RX Loopback */
        spec.subsweeps[0] = {
            0, 1, 1, 1, 8, 16,
            true, false, true, true, "19_5_MHz"
        };
        /* Subsweep 1: Kohärenter 2.5-mm Vital-Zoom */
        spec.subsweeps[1] = {
            zoom_start_pt, a121_dsp::kZoomPoints, 1, 1, 32, 16,
            true, true, true, false, "15_6_MHz"
        };
        /* Subsweep 2: Gleichzeitiger Raum-Scan (0.30..2.67 m, 30-mm Raster) */
        spec.subsweeps[2] = {
            120, a121_dsp::kRoomPoints, a121_dsp::kRoomStep, 3, 16, 16,
            true, true, true, false, "15_6_MHz"
        };
        return spec;
    }

    /**
     * @brief Baut ein High-Speed Continuous-Sweep-Mode (CSM) + Double-Buffering Preset
     *        für hochfrequente Vibrations- und Mikro-Doppler-Analyse (z.B. 64 Sweeps/Frame @ 2000 Hz Sweep-Rate).
     */
    static SessionSpec makeContinuousDopplerPreset(int32_t target_pt = 200,
                                                   uint16_t sweeps_per_frame = 64,
                                                   float sweep_rate_hz = 2000.0f) {
        SessionSpec spec = {};
        spec.sweeps_per_frame       = sweeps_per_frame;
        spec.sweep_rate_hz          = sweep_rate_hz;
        spec.frame_rate_hz          = 0.0f; /* Host-driven / nahtlos durch CSM */
        spec.continuous_sweep_mode  = true;
        spec.double_buffering       = true;
        spec.inter_frame_idle_state = "ready";
        spec.inter_sweep_idle_state = "ready";
        spec.num_subsweeps          = 1;

        spec.subsweeps[0] = {
            target_pt, 4, 1, 1, 16, 16,
            true, true, true, false, "15_6_MHz"
        };
        return spec;
    }

    bool startStreaming() {
        uart_.print("{\"cmd\":\"start_streaming\"}\n");
        String resp;
        if (!readResponse(resp, 500)) return false;
        return resp.indexOf("\"status\":\"start\"") >= 0;
    }

    bool stopStreaming() {
        uart_.print("{\"cmd\":\"stop_streaming\"}\n");
        uint32_t t0 = millis();
        String line;
        while ((millis() - t0) < 400) {
            if (readJsonLine(line, 100)) {
                int p_idx = line.indexOf("\"payload_size\":");
                if (p_idx >= 0) {
                    int p_sz = atoi(line.c_str() + p_idx + 15);
                    if (p_sz > 0) discardBytes((size_t)p_sz, 100);
                }
                /* Frame-Header tragen ebenfalls "status":"ok" — nur "stop" beendet. */
                if (line.indexOf("\"status\":\"stop\"") >= 0) {
                    drainRx(20);
                    return true;
                }
            }
        }
        drainRx(30);
        return false;
    }

    /**
     * @brief Liest genau einen Streaming-Frame (JSON-Headerzeile + `payload_size` Binär-Bytes).
     */
    bool readStreamFrame(a121_dsp::Complex16 *out_pts, uint16_t max_pts, FrameMeta &meta, uint32_t timeout_ms = 500) {
        String hdr;
        const uint32_t t0 = millis();
        int p_idx = -1;
        while (true) {
            const uint32_t el = millis() - t0;
            if (el >= timeout_ms || !readJsonLine(hdr, timeout_ms - el)) return false;
            if (hdr.indexOf("\"result_info\"") < 0) continue;   /* Log-/Statuszeilen überspringen */
            p_idx = hdr.indexOf("\"payload_size\":");
            if (p_idx >= 0) break;
        }
        const int payload_bytes = atoi(hdr.c_str() + p_idx + 15);
        meta.payload_bytes      = (uint16_t)payload_bytes;

        int t_idx = hdr.indexOf("\"tick\":");
        meta.tick = (t_idx >= 0) ? (uint32_t)strtoul(hdr.c_str() + t_idx + 7, nullptr, 10) : 0U;

        int temp_idx = hdr.indexOf("\"temperature\":");
        meta.temperature_c = (temp_idx >= 0) ? (int16_t)atoi(hdr.c_str() + temp_idx + 14) : 0;

        meta.data_saturated     = (hdr.indexOf("\"data_saturated\":true") >= 0);
        meta.frame_delayed      = (hdr.indexOf("\"frame_delayed\":true") >= 0);
        meta.calibration_needed = (hdr.indexOf("\"calibration_needed\":true") >= 0);

        if (payload_bytes <= 0) {
            return false;
        }

        const size_t max_bytes = (size_t)max_pts * sizeof(a121_dsp::Complex16);
        const size_t to_read   = ((size_t)payload_bytes <= max_bytes) ? (size_t)payload_bytes : max_bytes;
        if (!readExactBytes((uint8_t *)out_pts, to_read, timeout_ms)) {
            return false;
        }
        if ((size_t)payload_bytes > to_read) {
            discardBytes((size_t)payload_bytes - to_read, timeout_ms);
        }
        return true;
    }

private:
    bool tryHandshake(uint32_t baud, String &resp) {
        begin(baud);
        drainRx(5);
        uart_.print("{\"cmd\":\"get_system_info\"}\n");
        return readResponse(resp, 150) && resp.indexOf("rss_version") >= 0;
    }

    /**
     * Nächste Antwortzeile, Server-Logzeilen ({"status":"log",...}, payload_size 0) werden übersprungen.
     * Mit `logs` werden Logzeilen außer INFO als "LEVEL: Text\n" gesammelt. Den genauen Grund einer abgelehnten
     * Konfiguration (z. B. "Invalid step_length") nennt der Server nur dort.
     */
    bool readResponse(String &out, uint32_t timeout_ms, String *logs = nullptr) {
        const uint32_t t0 = millis();
        while (true) {
            const uint32_t el = millis() - t0;
            if (el >= timeout_ms || !readJsonLine(out, timeout_ms - el)) return false;
            if (out.indexOf("\"status\":\"log\"") < 0) return true;
            if (logs && out.indexOf("\"level\":\"INFO\"") < 0) {
                *logs += jsonString(out, "level");
                *logs += ": ";
                *logs += jsonString(out, "log");
                *logs += "\n";
            }
        }
    }

    /** Wert eines einfachen String-Felds ("key":"wert"), ohne Escape-Behandlung bis auf \". */
    static String jsonString(const String &js, const char *key) {
        String k = String("\"") + key + "\":\"";
        int i = js.indexOf(k);
        if (i < 0) return String();
        i += k.length();
        String v;
        for (int j = i; j < (int)js.length(); j++) {
            char c = js[j];
            if (c == '\\' && j + 1 < (int)js.length()) { v += js[++j]; continue; }
            if (c == '"') break;
            v += c;
        }
        return v;
    }

    void drainRx(uint32_t wait_ms) {
        stage_pos_ = stage_len_ = 0;
        uint8_t tmp[256];
        uint32_t t0 = millis();
        while ((millis() - t0) < wait_ms) {
            int avail;
            while ((avail = uart_.available()) > 0) {
                (void)uart_.readBytes(tmp, min((size_t)avail, sizeof(tmp)));
            }
            delay(1);
        }
    }

    /**
     * Zwischenpuffer: Bytes blockweise aus dem UART-Treiber holen statt einzeln. Jeder read()/available()-Aufruf
     * kostet im Arduino-Core eine Treiber-Sperre. Byteweise schaffte der ESP32 nur ~140 Frame-Header pro Sekunde;
     * bei 375 Frames/s lief der 32-KB-Puffer über (2026-11-01 am Gerät, `sess`).
     */
    bool fillStage() {
        if (stage_pos_ < stage_len_) return true;
        int avail = uart_.available();
        if (avail <= 0) return false;
        stage_len_ = uart_.readBytes(stage_, min((size_t)avail, sizeof(stage_)));
        stage_pos_ = 0;
        return stage_len_ > 0;
    }

    bool readJsonLine(String &out, uint32_t timeout_ms) {
        out = "";
        out.reserve(256);
        uint32_t t0 = millis();
        while ((millis() - t0) < timeout_ms) {
            if (!fillStage()) {
                delayMicroseconds(50);
                continue;
            }
            while (stage_pos_ < stage_len_) {
                char c = (char)stage_[stage_pos_++];
                if (c == '\n') {
                    out.trim();
                    if (out.length() > 0) return true;
                } else if (c != '\r' && out.length() < 2048) {  // Schutz bei Binärmüll ohne Zeilenende
                    out += c;
                }
            }
        }
        return false;
    }

    bool readExactBytes(uint8_t *dst, size_t count, uint32_t timeout_ms) {
        size_t got = 0;
        if (stage_pos_ < stage_len_) {  // erst den Zwischenpuffer leeren
            size_t n = min(count, stage_len_ - stage_pos_);
            memcpy(dst, stage_ + stage_pos_, n);
            stage_pos_ += n;
            got = n;
        }
        uint32_t t0 = millis();
        while (got < count && (millis() - t0) < timeout_ms) {
            int avail = uart_.available();
            if (avail > 0) {
                size_t chunk = count - got;
                if ((size_t)avail < chunk) chunk = (size_t)avail;
                size_t r = uart_.readBytes(dst + got, chunk);
                got += r;
            } else {
                delayMicroseconds(50);
            }
        }
        return (got == count);
    }

    void discardBytes(size_t count, uint32_t timeout_ms) {
        uint8_t tmp[64];
        size_t left = count;
        while (left > 0) {
            size_t chunk = (left > sizeof(tmp)) ? sizeof(tmp) : left;
            if (!readExactBytes(tmp, chunk, timeout_ms)) break;
            left -= chunk;
        }
    }

    HardwareSerial &uart_;
    int             rx_pin_;
    int             tx_pin_;
    uint32_t        current_baud_     = 921600;
    bool            rx_buf_allocated_ = false;
    uint8_t         stage_[1024];
    size_t          stage_pos_ = 0, stage_len_ = 0;
};

} /* namespace a121_srv */

#endif /* A121_SERVER_CLIENT_H */
