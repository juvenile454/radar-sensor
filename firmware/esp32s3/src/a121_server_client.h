/**
 * @file a121_server_client.h
 * @brief Full A121 Exploration Server UART client & high-speed bridge for the ESP32-S3.
 *
 * main.cpp passes the UART and pins. Verified on the device since 2026-11-01: Serial2 over the I2C wires of the connector,
 * ESP32 GPIO15 RX <- STM32 PB11 (LPUART1_TX, SDA wire) and GPIO16 TX -> PB10 (LPUART1_RX, SCL wire),
 * for a server firmware built from the Acconeer SDK (firmware/stm32l431/README.md).
 * Reason: PA10 (USART1_RX) is additionally driven by the CH342 (TXD1 via R9 = 22 Ω).
 *
 * Provides the ESP32-S3 with the sparse IQ functions of the Exploration Server (limits: ≈ 195 kB/s at 2 Mbaud,
 * CSM gapless up to 21 kHz sweep rate, see docs/HARDWARE.md):
 *   - 1..4 independent subsweeps per frame (Start, Step, NumPoints, Profile 1..5, PRF 19.5..5.2 MHz,
 *     HWAAS 1..511, Receiver Gain 0..23, Enable TX, Phase Enhancement, IQ Imbalance Compensation,
 *     On-Chip Loopback)
 *   - Continuous Sweep Mode (CSM) & double buffering for kHz micro-Doppler/vibration analysis
 *   - Inter-Frame & Inter-Sweep Idle States (DEEP_SLEEP, SLEEP, READY)
 *   - On-chip temperature sensor, saturation and calibration flags per frame
 *   - Baud rate negotiation after break reset (115200 -> 2000000 baud)
 *   - Hardware soft reset of the STM32L431 via a targeted UART break pulse on the TX line.
 *
 * Protocol (checked on server a121-v1.12.0 over CH342 channel B, 2026-10-31):
 *   - Before almost every reply the server sends a log line {"status":"log",...};
 *     readResponse()/readStreamFrame() skip it.
 *   - Enum values like exptool: "profile":1..5, "prf":"15_6_MHz", idle states "ready"/"sleep"/"deep_sleep",
 *     free rate = 0.0 (not null).
 *   - Since the hardening (2026-11-01) every UART error on RX (break, wrong baud rate, noise) resets the STM32
 *     → afterwards 115200 baud.
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

/* iq3 preset (V4, measurement V4.1/V4.2 in docs/EXPERIMENTS.md): zoom and loopback settings as compile switches,
 * default as before. The DSP only works at 60 Hz (filter), so the frame rate is not a switch. */
#ifndef A121_IQ3_ZOOM_PROFILE
#define A121_IQ3_ZOOM_PROFILE 1
#endif
#ifndef A121_IQ3_ZOOM_HWAAS
#define A121_IQ3_ZOOM_HWAAS 32
#endif
#ifndef A121_IQ3_LOOP_PROFILE
#define A121_IQ3_LOOP_PROFILE 1
#endif
#ifndef A121_IQ3_LOOP_GAIN
#define A121_IQ3_LOOP_GAIN 16
#endif
#ifndef A121_IQ3_LOOP_PRF
#define A121_IQ3_LOOP_PRF "19_5_MHz"  /* 19.5 MHz only with profile 1; 15.6 MHz works because the zoom has 15.6 MHz */
#endif
static_assert(A121_IQ3_ZOOM_PROFILE >= 1 && A121_IQ3_ZOOM_PROFILE <= 5, "A121_IQ3_ZOOM_PROFILE: 1..5");
static_assert(A121_IQ3_ZOOM_HWAAS >= 1 && A121_IQ3_ZOOM_HWAAS <= 511, "A121_IQ3_ZOOM_HWAAS: 1..511");
static_assert(A121_IQ3_LOOP_PROFILE >= 1 && A121_IQ3_LOOP_PROFILE <= 5 && A121_IQ3_LOOP_PROFILE != 2,
              "A121_IQ3_LOOP_PROFILE: Loopback geht nicht mit Profil 2");
static_assert(A121_IQ3_LOOP_GAIN >= 0 && A121_IQ3_LOOP_GAIN <= 23, "A121_IQ3_LOOP_GAIN: 0..23");

#define A121_IQ3_STR2(x) #x
#define A121_IQ3_STR(x) A121_IQ3_STR2(x)
/** Identifier of the iq3 preset for @HELLO, e.g. "p1h32/lp1g16/19_5_MHz" (zoom profile/HWAAS, loopback profile/gain/PRF). */
inline const char *iq3PresetTag() {
    return "p" A121_IQ3_STR(A121_IQ3_ZOOM_PROFILE) "h" A121_IQ3_STR(A121_IQ3_ZOOM_HWAAS) "/lp" A121_IQ3_STR(
        A121_IQ3_LOOP_PROFILE) "g" A121_IQ3_STR(A121_IQ3_LOOP_GAIN) "/" A121_IQ3_LOOP_PRF;
}

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
            uart_.end();                     /* buffer size can only be set while the UART is stopped */
            uart_.setRxBufferSize(32768);
            uart_.setTxBufferSize(4096);     /* otherwise write() blocks until there is room in the 128-byte FIFO */
            rx_buf_allocated_ = true;
            uart_.begin(current_baud_, SERIAL_8N1, rx_pin_, tx_pin_);
        } else {
            uart_.updateBaudRate(current_baud_);
        }
    }

    uint32_t currentBaud() const { return current_baud_; }

    HardwareSerial &uart() { return uart_; }

    /** Stop the UART and set both pins to high impedance (e.g. before I2C uses the same pins). */
    void release() {
        uart_.end();
        stage_pos_ = stage_len_ = 0;
        rx_buf_allocated_ = false;
        pinMode(rx_pin_, INPUT);
        pinMode(tx_pin_, INPUT);
    }

    /**
     * @brief Soft reset of the STM32L431: 15 ms LOW on the TX line produces a framing error.
     *        The USART1 server firmware then restarted (afterwards 115200 baud, ready after ~0.5 s);
     *        whether a firmware rebuilt from the SDK does the same must be checked on the device.
     */
    void resetStm32ViaBreak() {
        uart_.end();
        rx_buf_allocated_ = false;
        pinMode(tx_pin_, OUTPUT);
        digitalWrite(tx_pin_, LOW);   /* UART break = constant LOW > 1 character length */
        delay(15);
        digitalWrite(tx_pin_, HIGH);  /* idle HIGH */
        begin(115200);
        drainRx(5);
    }

    /**
     * @brief Connects to the server and switches it to `target_baud`.
     *        Always break reset first, then handshake at 115200 (up to ~2.5 s boot time).
     *        No attempt at a guessed baud rate: bytes at the wrong baud rate produce noise/overrun errors
     *        on the STM32; the HAL then aborts DMA reception, and the SDK server only restarts
     *        on a framing error. It stays deaf until RESET is pressed (three times on the device on 2026-11-01).
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
     * @brief Configures an arbitrary 1..4 subsweep sparse IQ session on the A121.
     */
    bool setupSession(const SessionSpec &spec, String *out_setup_ack = nullptr, String *out_logs = nullptr) {
        if (spec.num_subsweeps == 0 || spec.num_subsweeps > 4) return false;

        String json;
        json.reserve(1536);
        json += "{\"cmd\":\"setup\",\"groups\":[[{\"sensor_id\":1,\"config\":{";
        json += "\"sweeps_per_frame\":";
        json += String(spec.sweeps_per_frame);
        json += ",\"sweep_rate\":";
        json += String(spec.sweep_rate_hz > 0.0f ? spec.sweep_rate_hz : 0.0f, 1);   /* 0.0 = free */
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
     * @brief Builds the 3-subsweep Ultimate Vital + room tracking preset:
     *        - Subsweep 0: on-chip loopback (1 point, profile 1, HWAAS 8, gain 16, 19.5 MHz, loopback=true)
     *        - Subsweep 1: 2.5 mm vital zoom (24 points = 60 mm, step 1, profile 1, HWAAS 32, 15.6 MHz, phase enhancement)
     *        Zoom profile/HWAAS and loopback profile/gain/PRF via A121_IQ3_* (above, default as here).
     *        - Subsweep 2: full-room scan (80 points, 0.30..2.67 m, step 12 = 30 mm, profile 3, HWAAS 16)
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

        /* Subsweep 0: internal on-chip TX->RX loopback */
        spec.subsweeps[0] = {
            0, 1, 1, A121_IQ3_LOOP_PROFILE, 8, A121_IQ3_LOOP_GAIN,
            true, false, true, true, A121_IQ3_LOOP_PRF
        };
        /* Subsweep 1: coherent 2.5 mm vital zoom */
        spec.subsweeps[1] = {
            zoom_start_pt, a121_dsp::kZoomPoints, 1, A121_IQ3_ZOOM_PROFILE, A121_IQ3_ZOOM_HWAAS, 16,
            true, true, true, false, "15_6_MHz"
        };
        /* Subsweep 2: simultaneous room scan (0.30..2.67 m, 30 mm grid) */
        spec.subsweeps[2] = {
            120, a121_dsp::kRoomPoints, a121_dsp::kRoomStep, 3, 16, 16,
            true, true, true, false, "15_6_MHz"
        };
        return spec;
    }

    /**
     * @brief Builds a high-speed continuous sweep mode (CSM) + double buffering preset
     *        for high-frequency vibration and micro-Doppler analysis (e.g. 64 sweeps/frame @ 2000 Hz sweep rate).
     */
    static SessionSpec makeContinuousDopplerPreset(int32_t target_pt = 200,
                                                   uint16_t sweeps_per_frame = 64,
                                                   float sweep_rate_hz = 2000.0f) {
        SessionSpec spec = {};
        spec.sweeps_per_frame       = sweeps_per_frame;
        spec.sweep_rate_hz          = sweep_rate_hz;
        spec.frame_rate_hz          = 0.0f; /* host-driven / seamless through CSM */
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
                /* Frame headers also carry "status":"ok" — only "stop" ends it. */
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
     * @brief Reads exactly one streaming frame (JSON header line + `payload_size` binary bytes).
     */
    bool readStreamFrame(a121_dsp::Complex16 *out_pts, uint16_t max_pts, FrameMeta &meta, uint32_t timeout_ms = 500) {
        String hdr;
        const uint32_t t0 = millis();
        int p_idx = -1;
        while (true) {
            const uint32_t el = millis() - t0;
            if (el >= timeout_ms || !readJsonLine(hdr, timeout_ms - el)) return false;
            if (hdr.indexOf("\"result_info\"") < 0) continue;   /* skip log/status lines */
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
     * Next reply line; server log lines ({"status":"log",...}, payload_size 0) are skipped.
     * With `logs`, log lines other than INFO are collected as "LEVEL: text\n". The server gives the exact reason for a
     * rejected configuration (e.g. "Invalid step_length") only there.
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

    /** Value of a simple string field ("key":"value"), without escape handling except for \". */
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
     * Staging buffer: fetch bytes from the UART driver in blocks instead of one by one. Every read()/available() call
     * costs a driver lock in the Arduino core. Byte by byte, the ESP32 only managed ~140 frame headers per second;
     * at 375 frames/s the 32 KB buffer overflowed (2026-11-01 on the device, `sess`).
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
                } else if (c != '\r' && out.length() < 2048) {  // protection against binary garbage without a line end
                    out += c;
                }
            }
        }
        return false;
    }

    bool readExactBytes(uint8_t *dst, size_t count, uint32_t timeout_ms) {
        size_t got = 0;
        if (stage_pos_ < stage_len_) {  // empty the staging buffer first
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
