/**
 * @file a121_universal_protocol.h
 * @brief Shared protocol and data structures between STM32L431 (XM125) and ESP32-S3.
 *
 * Supports two complementary access paths over the 10-pin MX1.25 connector:
 *   1. Acconeer Exploration Server protocol over USART1 (PA9 TX / PA10 RX <-> ESP32 GPIO5 RX / GPIO4 TX):
 *      - JSON commands (newline-terminated):
 *          {"cmd":"get_system_info"}
 *          {"cmd":"get_sensor_info"}
 *          {"cmd":"set_uart_baudrate","baudrate":921600}
 *          {"cmd":"setup","groups":[[{"sensor_id":1,"config":{...}}]]}
 *          {"cmd":"start_streaming"}
 *          {"cmd":"stop_streaming"}
 *          {"cmd":"recalibrate"}
 *      - Streaming packets:
 *          1 line of JSON header (`{"status":"stream","payload_size":N,"result_info":[[{"tick":...,"temperature":T,...}]]}\n`)
 *          immediately followed by `N` bytes of raw `acc_int16_complex_t` IQ samples (`int16_t real, int16_t imag`, little-endian).
 *
 *   2. Compact binary frame header format (`a121_raw_iq_frame_header_t`) for low-latency
 *      dual-core streaming and PSRAM recording on the ESP32-S3.
 */

#ifndef A121_UNIVERSAL_PROTOCOL_H
#define A121_UNIVERSAL_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A121_MAX_SUBSWEEPS              4U
#define A121_FRAME_MAGIC                0x49513231UL  /* "IQ21" in little-endian */

/**
 * @brief Complex 16-bit IQ data point of the A121 (4 bytes per distance point / sweep).
 */
typedef struct __attribute__((packed)) {
    int16_t real;
    int16_t imag;
} a121_complex_int16_t;

/**
 * @brief Configuration of a single A121 subsweep (100 % coverage of acc_config_subsweep_*).
 */
typedef struct __attribute__((packed)) {
    int32_t  start_point;               /* Start point in 2.5 mm units (e.g. 180 = 0.450 m, also <= 0 for loopback) */
    uint16_t num_points;                /* Number of distance points in this subsweep */
    uint16_t step_length;               /* Step length in 2.5 mm units (1, 2, 4, 8, 12, 16, 24, ...) */
    uint8_t  profile;                   /* Pulse profile 1..5 (1 = shortest pulse / highest resolution, 5 = highest energy) */
    uint16_t hwaas;                     /* Hardware Accelerated Average Samples (1..511) */
    uint8_t  receiver_gain;             /* LNA/TIA receiver gain (0..23, default 16) */
    uint8_t  enable_tx;                 /* 1 = transmitter output stage active, 0 = purely passive noise/EMC measurement */
    uint8_t  phase_enhancement;         /* 1 = on-chip phase stabilization active */
    uint8_t  iq_imbalance_compensation; /* 1 = on-chip IQ amplitude/phase correction active */
    uint8_t  enable_loopback;           /* 1 = internal on-chip TX->RX loopback (no antenna radiation) */
    uint8_t  prf;                       /* Pulse Repetition Frequency: 0=19.5MHz, 1=15.6MHz, 2=13.0MHz, 3=8.7MHz, 4=6.5MHz, 5=5.2MHz */
} a121_subsweep_config_t;

/**
 * @brief Complete A121 frame configuration (up to 4 independent subsweeps per frame).
 */
typedef struct __attribute__((packed)) {
    uint16_t sweeps_per_frame;          /* Number of sweeps per frame (1..4095) */
    float    sweep_rate_hz;             /* Repetition rate of individual sweeps within a frame (0.0 = as fast as possible) */
    float    frame_rate_hz;             /* Frame rate in Hz (0.0 = host-driven / unlimited) */
    uint8_t  continuous_sweep_mode;     /* 1 = gapless coherent sweep sequence across frame boundaries (CSM) */
    uint8_t  double_buffering;          /* 1 = halve the A121 memory for simultaneous measuring + SPI readout */
    uint8_t  inter_frame_idle_state;    /* 0 = DEEP_SLEEP, 1 = SLEEP, 2 = READY */
    uint8_t  inter_sweep_idle_state;    /* 0 = DEEP_SLEEP, 1 = SLEEP, 2 = READY */
    uint8_t  num_subsweeps;             /* 1..4 */
    a121_subsweep_config_t subsweeps[A121_MAX_SUBSWEEPS];
} a121_session_config_t;

/**
 * @brief Status flags from `acc_processing_metadata_t` / `acc_processing_result_t`.
 */
#define A121_RESULT_FLAG_DATA_SATURATED     (1U << 0)
#define A121_RESULT_FLAG_FRAME_DELAYED      (1U << 1)
#define A121_RESULT_FLAG_CALIBRATION_NEEDED (1U << 2)

/**
 * @brief Compact binary frame metadata packet for a received A121 sparse IQ frame.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;                     /* A121_FRAME_MAGIC (0x49513231) */
    uint32_t frame_seq;                 /* Running frame counter */
    uint32_t stm32_tick_ms;             /* Tick timestamp of the STM32L431 */
    uint32_t esp32_us;                  /* Microsecond timestamp of the ESP32-S3 (`esp_timer_get_time()`) */
    int16_t  temperature_c;             /* A121 on-chip temperature in °C */
    uint8_t  result_flags;              /* Bit mask of A121_RESULT_FLAG_* */
    uint8_t  num_subsweeps;             /* Number of active subsweeps (1..4) */
    uint16_t total_points;              /* Total number of complex IQ points in the frame */
    uint16_t payload_bytes;             /* total_points * 4 */
} a121_raw_iq_frame_header_t;

#ifdef __cplusplus
}
#endif

#endif /* A121_UNIVERSAL_PROTOCOL_H */
