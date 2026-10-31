/**
 * @file a121_universal_protocol.h
 * @brief Gemeinsame Protokoll- und Datenstrukturen zwischen STM32L431 (XM125) und ESP32-S3.
 *
 * Unterstützt zwei komplementäre Zugriffswege über den 10-Pin MX1.25 Stecker:
 *   1. Acconeer Exploration Server Protokoll über USART1 (PA9 TX / PA10 RX <-> ESP32 GPIO5 RX / GPIO4 TX):
 *      - JSON-Kommandos (newline-terminiert):
 *          {"cmd":"get_system_info"}
 *          {"cmd":"get_sensor_info"}
 *          {"cmd":"set_uart_baudrate","baudrate":921600}
 *          {"cmd":"setup","groups":[[{"sensor_id":1,"config":{...}}]]}
 *          {"cmd":"start_streaming"}
 *          {"cmd":"stop_streaming"}
 *          {"cmd":"recalibrate"}
 *      - Streaming-Pakete:
 *          1 Zeile JSON-Header (`{"status":"stream","payload_size":N,"result_info":[[{"tick":...,"temperature":T,...}]]}\n`)
 *          unmittelbar gefolgt von `N` Bytes rohen `acc_int16_complex_t` IQ-Samples (`int16_t real, int16_t imag`, Little-Endian).
 *
 *   2. Kompaktes Binär-Frame-Header-Format (`a121_raw_iq_frame_header_t`) für latenzarmes
 *      Dual-Core-Streaming und PSRAM-Aufzeichnung auf dem ESP32-S3.
 */

#ifndef A121_UNIVERSAL_PROTOCOL_H
#define A121_UNIVERSAL_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A121_MAX_SUBSWEEPS              4U
#define A121_FRAME_MAGIC                0x49513231UL  /* "IQ21" in Little-Endian */

/**
 * @brief Komplexer 16-Bit IQ-Datenpunkt des A121 (4 Bytes pro Distanzpunkt / Sweep).
 */
typedef struct __attribute__((packed)) {
    int16_t real;
    int16_t imag;
} a121_complex_int16_t;

/**
 * @brief Konfiguration eines einzelnen A121-Subsweeps (100 % Abdeckung von acc_config_subsweep_*).
 */
typedef struct __attribute__((packed)) {
    int32_t  start_point;               /* Startpunkt in 2.5-mm-Einheiten (z.B. 180 = 0.450 m, auch <= 0 bei Loopback) */
    uint16_t num_points;                /* Anzahl Distanzpunkte in diesem Subsweep */
    uint16_t step_length;               /* Schrittweite in 2.5-mm-Einheiten (1, 2, 4, 8, 12, 16, 24, ...) */
    uint8_t  profile;                   /* Impulsprofil 1..5 (1 = kürzester Puls / höchste Auflösung, 5 = höchste Energie) */
    uint16_t hwaas;                     /* Hardware Accelerated Average Samples (1..511) */
    uint8_t  receiver_gain;             /* LNA/TIA Empfänger-Verstärkung (0..23, Standard 16) */
    uint8_t  enable_tx;                 /* 1 = Sendeendstufe aktiv, 0 = rein passives Rausch-/EMV-Messen */
    uint8_t  phase_enhancement;         /* 1 = On-Chip Phasen-Stabilisierung aktiv */
    uint8_t  iq_imbalance_compensation; /* 1 = On-Chip IQ-Amplituden/Phasen-Korrektur aktiv */
    uint8_t  enable_loopback;           /* 1 = Interner On-Chip TX->RX Loopback (ohne Antennenabstrahlung) */
    uint8_t  prf;                       /* Pulse Repetition Frequency: 0=19.5MHz, 1=15.6MHz, 2=13.0MHz, 3=8.7MHz, 4=6.5MHz, 5=5.2MHz */
} a121_subsweep_config_t;

/**
 * @brief Gesamte A121-Frame-Konfiguration (bis zu 4 unabhängige Subsweeps pro Frame).
 */
typedef struct __attribute__((packed)) {
    uint16_t sweeps_per_frame;          /* Anzahl Sweeps pro Frame (1..4095) */
    float    sweep_rate_hz;             /* Wiederholrate einzelner Sweeps innerhalb eines Frames (0.0 = as fast as possible) */
    float    frame_rate_hz;             /* Frame-Rate in Hz (0.0 = host-driven / unbegrenzt) */
    uint8_t  continuous_sweep_mode;     /* 1 = lückenlose kohärente Sweep-Folge über Frame-Grenzen hinweg (CSM) */
    uint8_t  double_buffering;          /* 1 = A121-Speicher halbieren für gleichzeitiges Messen + SPI-Auslesen */
    uint8_t  inter_frame_idle_state;    /* 0 = DEEP_SLEEP, 1 = SLEEP, 2 = READY */
    uint8_t  inter_sweep_idle_state;    /* 0 = DEEP_SLEEP, 1 = SLEEP, 2 = READY */
    uint8_t  num_subsweeps;             /* 1..4 */
    a121_subsweep_config_t subsweeps[A121_MAX_SUBSWEEPS];
} a121_session_config_t;

/**
 * @brief Status-Flags aus `acc_processing_metadata_t` / `acc_processing_result_t`.
 */
#define A121_RESULT_FLAG_DATA_SATURATED     (1U << 0)
#define A121_RESULT_FLAG_FRAME_DELAYED      (1U << 1)
#define A121_RESULT_FLAG_CALIBRATION_NEEDED (1U << 2)

/**
 * @brief Kompaktes Binär-Frame-Metadaten-Paket für einen empfangenen A121 Sparse-IQ Frame.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;                     /* A121_FRAME_MAGIC (0x49513231) */
    uint32_t frame_seq;                 /* Fortlaufender Frame-Zähler */
    uint32_t stm32_tick_ms;             /* Tick-Zeitstempel des STM32L431 */
    uint32_t esp32_us;                  /* Mikrosekunden-Zeitstempel des ESP32-S3 (`esp_timer_get_time()`) */
    int16_t  temperature_c;             /* A121 On-Chip Temperatur in °C */
    uint8_t  result_flags;              /* Bitmaske aus A121_RESULT_FLAG_* */
    uint8_t  num_subsweeps;             /* Anzahl aktiver Subsweeps (1..4) */
    uint16_t total_points;              /* Gesamtzahl komplexer IQ-Punkte im Frame */
    uint16_t payload_bytes;             /* total_points * 4 */
} a121_raw_iq_frame_header_t;

#ifdef __cplusplus
}
#endif

#endif /* A121_UNIVERSAL_PROTOCOL_H */
