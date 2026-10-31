/**
 * @file main.c
 * @brief Custom Universal STM32L431CBT6 Firmware für das Waveshare A121 Range Sensor Board (XM125).
 *
 * Stellt dem ESP32-S3 über den 10-Pin MX1.25-Stecker vollen Zugriff auf 100 % der Hardware-
 * Fähigkeiten des Acconeer A121 60-GHz Pulsed Coherent Radar (PCR) Transceivers bereit:
 *   - High-Speed USART1 (PA9 TX / PA10 RX <-> ESP32 GPIO5 RX / GPIO4 TX, DMA1_Ch4/Ch5, 921600/2000000 Baud)
 *   - I2C2 Slave @ 0x52 (PB10 SCL / PB11 SDA <-> ESP32 GPIO16 SCL / GPIO15 SDA)
 *   - Hardware-Interrupt MCU_INT (PB9 -> ESP32 GPIO18)
 *   - Wakeup & Trigger über WAKE_UP (PA11), MISC_GPIO0 (PB1 <-> GPIO6) & MISC_GPIO1 (PA4 <-> GPIO7)
 *   - Volle Subsweep-Kontrolle (1..4 Subsweeps, Start/Step/NumPoints, Profile 1..5, PRF 19.5..5.2 MHz,
 *     HWAAS 1..511, Receiver Gain 0..23, Enable TX, Phase Enhancement, IQ Imbalance Compensation,
 *     On-Chip Loopback, Continuous Sweep Mode & Double Buffering).
 *
 * Hinweis:
 * Zum Kompilieren aus dem C-Quelltext wird das Acconeer XM125 C-SDK (`libacconeer_a121.a` + Header)
 * unter `../../vendor/acconeer_sdk/` benötigt (Lizenz: nicht ins Git einchecken, siehe AGENTS.md).
 * Steht das proprietäre C-SDK lokal nicht zur Verfügung, erzeugt `make patch-server` (bzw.
 * `python3 ../../host/build_stm32_usart1_server.py`) deterministisch die funktionsgleiche
 * USART1-Server-Firmware `acc_exploration_server_a121_usart1_esp32.bin` aus dem vorhandenen
 * `acc_exploration_server_a121.bin` Image.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xm125_waveshare_bsp.h"
#include "a121_universal_protocol.h"

#if defined(HAVE_ACCONEER_A121_SDK)
#include "acc_config.h"
#include "acc_config_subsweep.h"
#include "acc_definitions_a121.h"
#include "acc_definitions_common.h"
#include "acc_hal_definitions_a121.h"
#include "acc_hal_integration_a121.h"
#include "acc_integration.h"
#include "acc_processing.h"
#include "acc_rss_a121.h"
#include "acc_sensor.h"
#include "acc_version.h"

#define SENSOR_ID                       (1U)
#define SENSOR_TIMEOUT_MS               (2000U)
#define MAX_WORK_BUFFER_BYTES           (16384U)

static uint8_t                  g_sensor_work_buffer[MAX_WORK_BUFFER_BYTES] __attribute__((aligned(4)));
static acc_config_t            *g_config     = NULL;
static acc_processing_t        *g_processing = NULL;
static acc_sensor_t            *g_sensor     = NULL;
static acc_processing_metadata_t g_proc_meta;
static acc_cal_result_t         g_cal_result;
static uint32_t                 g_frame_seq  = 0;

static acc_config_profile_t map_profile(uint8_t p)
{
    switch (p) {
        case 1:  return ACC_CONFIG_PROFILE_1;
        case 2:  return ACC_CONFIG_PROFILE_2;
        case 3:  return ACC_CONFIG_PROFILE_3;
        case 4:  return ACC_CONFIG_PROFILE_4;
        case 5:  return ACC_CONFIG_PROFILE_5;
        default: return ACC_CONFIG_PROFILE_1;
    }
}

static acc_config_prf_t map_prf(uint8_t prf)
{
    switch (prf) {
        case 0:  return ACC_CONFIG_PRF_19_5_MHZ;
        case 1:  return ACC_CONFIG_PRF_15_6_MHZ;
        case 2:  return ACC_CONFIG_PRF_13_0_MHZ;
        case 3:  return ACC_CONFIG_PRF_8_7_MHZ;
        case 4:  return ACC_CONFIG_PRF_6_5_MHZ;
        case 5:  return ACC_CONFIG_PRF_5_2_MHZ;
        default: return ACC_CONFIG_PRF_15_6_MHZ;
    }
}

static acc_config_idle_state_t map_idle_state(uint8_t s)
{
    switch (s) {
        case 0:  return ACC_CONFIG_IDLE_STATE_DEEP_SLEEP;
        case 1:  return ACC_CONFIG_IDLE_STATE_SLEEP;
        case 2:  return ACC_CONFIG_IDLE_STATE_READY;
        default: return ACC_CONFIG_IDLE_STATE_READY;
    }
}

/**
 * @brief Überträgt eine komplette `a121_session_config_t` (inkl. 1..4 Subsweeps) in das A121-Silizium.
 */
bool a121_apply_universal_config(const a121_session_config_t *cfg)
{
    if (cfg == NULL || cfg->num_subsweeps == 0U || cfg->num_subsweeps > A121_MAX_SUBSWEEPS) {
        return false;
    }

    if (g_config == NULL) {
        g_config = acc_config_create();
        if (g_config == NULL) {
            return false;
        }
    }

    acc_config_sweeps_per_frame_set(g_config, cfg->sweeps_per_frame > 0 ? cfg->sweeps_per_frame : 1U);
    acc_config_sweep_rate_set(g_config, cfg->sweep_rate_hz);
    acc_config_frame_rate_set(g_config, cfg->frame_rate_hz);
    acc_config_continuous_sweep_mode_set(g_config, cfg->continuous_sweep_mode != 0U);
    acc_config_double_buffering_set(g_config, cfg->double_buffering != 0U);
    acc_config_inter_frame_idle_state_set(g_config, map_idle_state(cfg->inter_frame_idle_state));
    acc_config_inter_sweep_idle_state_set(g_config, map_idle_state(cfg->inter_sweep_idle_state));

    acc_config_num_subsweeps_set(g_config, cfg->num_subsweeps);

    for (uint8_t i = 0; i < cfg->num_subsweeps; i++) {
        const a121_subsweep_config_t *sub = &cfg->subsweeps[i];
        acc_config_subsweep_start_point_set(g_config, sub->start_point, i);
        acc_config_subsweep_num_points_set(g_config, sub->num_points, i);
        acc_config_subsweep_step_length_set(g_config, sub->step_length, i);
        acc_config_subsweep_profile_set(g_config, map_profile(sub->profile), i);
        acc_config_subsweep_hwaas_set(g_config, sub->hwaas, i);
        acc_config_subsweep_receiver_gain_set(g_config, sub->receiver_gain, i);
        acc_config_subsweep_enable_tx_set(g_config, sub->enable_tx != 0U, i);
        acc_config_subsweep_phase_enhancement_set(g_config, sub->phase_enhancement != 0U, i);
        acc_config_subsweep_iq_imbalance_compensation_set(g_config, sub->iq_imbalance_compensation != 0U, i);
        acc_config_subsweep_enable_loopback_set(g_config, sub->enable_loopback != 0U, i);
        acc_config_subsweep_prf_set(g_config, map_prf(sub->prf), i);
    }

    uint32_t needed_buffer_size = 0U;
    if (!acc_rss_get_buffer_size(g_config, &needed_buffer_size) || needed_buffer_size > MAX_WORK_BUFFER_BYTES) {
        return false;
    }

    if (g_processing != NULL) {
        acc_processing_destroy(g_processing);
        g_processing = NULL;
    }

    g_processing = acc_processing_create(g_config, &g_proc_meta);
    if (g_processing == NULL) {
        return false;
    }

    if (!acc_sensor_prepare(g_sensor, g_config, &g_cal_result, g_sensor_work_buffer, needed_buffer_size)) {
        return false;
    }

    return true;
}

/**
 * @brief Standard-Preset: 3-Subsweep Ultimate Vital + Raum-Tracking @ 60 Hz:
 *   - Subsweep 0: On-Chip Loopback (1 Punkt, Profil 1, HWAAS 8, TX an, Loopback an)
 *   - Subsweep 1: 2.5-mm Vital-Zoom (24 Punkte = 60 mm Fenster, Step 1, Profil 1, HWAAS 32, Phase Enhancement)
 *   - Subsweep 2: Full-Room Scan (60 Punkte, 0.30..2.70 m, Step 16 = 40 mm, Profil 3, HWAAS 16)
 */
void a121_load_default_3subsweep_preset(a121_session_config_t *cfg, int32_t zoom_start_point)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sweeps_per_frame         = 1U;
    cfg->sweep_rate_hz            = 0.0f;
    cfg->frame_rate_hz            = 60.0f;
    cfg->continuous_sweep_mode    = 0U;
    cfg->double_buffering         = 0U;
    cfg->inter_frame_idle_state   = 2U; /* READY */
    cfg->inter_sweep_idle_state   = 2U; /* READY */
    cfg->num_subsweeps            = 3U;

    /* Subsweep 0: Interner On-Chip Loopback (Jittersubtraktion) */
    cfg->subsweeps[0].start_point               = 0;
    cfg->subsweeps[0].num_points                = 1U;
    cfg->subsweeps[0].step_length               = 1U;
    cfg->subsweeps[0].profile                   = 1U;
    cfg->subsweeps[0].hwaas                     = 8U;
    cfg->subsweeps[0].receiver_gain             = 16U;
    cfg->subsweeps[0].enable_tx                 = 1U;
    cfg->subsweeps[0].phase_enhancement         = 0U;
    cfg->subsweeps[0].iq_imbalance_compensation = 1U;
    cfg->subsweeps[0].enable_loopback           = 1U;
    cfg->subsweeps[0].prf                       = 0U; /* 19.5 MHz */

    /* Subsweep 1: Kohärenter 2.5-mm Vital-Zoom auf den Thorax */
    cfg->subsweeps[1].start_point               = zoom_start_point;
    cfg->subsweeps[1].num_points                = 24U;
    cfg->subsweeps[1].step_length               = 1U;
    cfg->subsweeps[1].profile                   = 1U;
    cfg->subsweeps[1].hwaas                     = 32U;
    cfg->subsweeps[1].receiver_gain             = 16U;
    cfg->subsweeps[1].enable_tx                 = 1U;
    cfg->subsweeps[1].phase_enhancement         = 1U;
    cfg->subsweeps[1].iq_imbalance_compensation = 1U;
    cfg->subsweeps[1].enable_loopback           = 0U;
    cfg->subsweeps[1].prf                       = 1U; /* 15.6 MHz */

    /* Subsweep 2: Gleichzeitiger Raum-Scan (0.30..2.70 m, 40-mm Raster) */
    cfg->subsweeps[2].start_point               = 120; /* 0.30 m */
    cfg->subsweeps[2].num_points                = 60U;
    cfg->subsweeps[2].step_length               = 16U; /* 40 mm */
    cfg->subsweeps[2].profile                   = 3U;
    cfg->subsweeps[2].hwaas                     = 16U;
    cfg->subsweeps[2].receiver_gain             = 16U;
    cfg->subsweeps[2].enable_tx                 = 1U;
    cfg->subsweeps[2].phase_enhancement         = 1U;
    cfg->subsweeps[2].iq_imbalance_compensation = 1U;
    cfg->subsweeps[2].enable_loopback           = 0U;
    cfg->subsweeps[2].prf                       = 1U; /* 15.6 MHz */
}
#endif /* HAVE_ACCONEER_A121_SDK */
