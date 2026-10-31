/**
 * @file xm125_waveshare_bsp.h
 * @brief Board Support Package (BSP) & exakte Pin-/Peripherie-Definitionen für das
 *        Waveshare A121 Range Sensor Board (Acconeer XM125 = STM32L431CBT6 + A121).
 *
 * Verifiziert durch Schaltplan, Hardware-Probing und Disassembly der Original-Firmwares
 * (`i2c_distance_detector.bin` & `acc_exploration_server_a121.bin`, a121-v1.12.0).
 */

#ifndef XM125_WAVESHARE_BSP_H
#define XM125_WAVESHARE_BSP_H

#include <stdint.h>

/* ============================================================================
 * 1. A121 Radar-Transceiver <-> STM32L431CBT6 (intern im XM125-Modul)
 * ============================================================================ */
#define A121_SPI_INSTANCE               SPI1                /* 0x40013000, APB2 (40 MHz SPI-Takt @ 80 MHz HCLK) */
#define A121_SPI_SCK_PORT               GPIOA
#define A121_SPI_SCK_PIN                (1U << 5)           /* PA5 (AF5 = SPI1_SCK) */
#define A121_SPI_MISO_PORT              GPIOA
#define A121_SPI_MISO_PIN               (1U << 6)           /* PA6 (AF5 = SPI1_MISO) */
#define A121_SPI_MOSI_PORT              GPIOA
#define A121_SPI_MOSI_PIN               (1U << 7)           /* PA7 (AF5 = SPI1_MOSI) */

#define A121_SPI_CS_PORT                GPIOB               /* 0x48000400 */
#define A121_SPI_CS_PIN                 (1U << 0)           /* PB0 (Software-NSS, aktiv LOW) */

#define A121_ENABLE_PORT                GPIOB
#define A121_ENABLE_PIN                 (1U << 12)          /* PB12 (A121 ENABLE / Power-Enable, aktiv HIGH) */

#define A121_INT_PORT                   GPIOB
#define A121_INT_PIN                    (1U << 3)           /* PB3 (EXTI3_IRQn = IRQ 9, steigende Flanke = Daten bereit) */
#define A121_INT_IRQn                   9                   /* EXTI3_IRQn */

#define A121_SPI_DMA_RX_CHANNEL         DMA1_Channel2       /* 0x4002001C, Request 1 = SPI1_RX, IRQ 12 */
#define A121_SPI_DMA_TX_CHANNEL         DMA1_Channel3       /* 0x40020030, Request 1 = SPI1_TX, IRQ 13 */

/* ============================================================================
 * 2. 10-Pin MX1.25 Stecker <-> ESP32-S3 (externe Host-Schnittstellen)
 * ============================================================================ */

/* High-Speed UART1 (MX1.25 Pins TXD/RXD <-> ESP32-S3 GPIO5 RX / GPIO4 TX + CH342 Kanal B) */
#define HOST_UART1_INSTANCE             USART1              /* 0x40013800, APB2 (80 MHz PCLK2) */
#define HOST_UART1_TX_PORT              GPIOA
#define HOST_UART1_TX_PIN               (1U << 9)           /* PA9  (AF7 = USART1_TX -> ESP32 GPIO5) */
#define HOST_UART1_RX_PORT              GPIOA
#define HOST_UART1_RX_PIN               (1U << 10)          /* PA10 (AF7 = USART1_RX <- ESP32 GPIO4) */
#define HOST_UART1_DMA_TX_CHANNEL       DMA1_Channel4       /* 0x40020044, Request 2 = USART1_TX, IRQ 14 */
#define HOST_UART1_DMA_RX_CHANNEL       DMA1_Channel5       /* 0x40020058, Request 2 = USART1_RX, IRQ 15 */
#define HOST_UART1_IRQn                 37                  /* USART1_IRQn */

/* USB-Only UART2 (CH342 Kanal A -if00, NICHT am 10-Pin MX1.25 Stecker!) */
#define USB_UART2_INSTANCE              USART2              /* 0x40004400, APB1 (80 MHz PCLK1) */
#define USB_UART2_CTS_PIN               (1U << 0)           /* PA0 (AF7 = USART2_CTS) */
#define USB_UART2_RTS_PIN               (1U << 1)           /* PA1 (AF7 = USART2_RTS) */
#define USB_UART2_TX_PIN                (1U << 2)           /* PA2 (AF7 = USART2_TX) */
#define USB_UART2_RX_PIN                (1U << 3)           /* PA3 (AF7 = USART2_RX) */
#define USB_UART2_DMA_RX_CHANNEL        DMA1_Channel6       /* 0x4002006C, Request 2 = USART2_RX, IRQ 16 */
#define USB_UART2_DMA_TX_CHANNEL        DMA1_Channel7       /* 0x40020080, Request 2 = USART2_TX, IRQ 17 */
#define USB_UART2_IRQn                  38                  /* USART2_IRQn */

/* I2C2 Slave (MX1.25 Pins SCL/SDA <-> ESP32-S3 GPIO16 SCL / GPIO15 SDA)
 * PB10/PB11 geprüft 2026-10-31: Schaltplan-Netze I2C2_SCL = PB10, I2C2_SDA = PB11, und
 * i2c_distance_detector.bin initialisiert GPIOB-Maske 0x0C00 als AF4 Open-Drain mit Pull-up.
 * (Frühere Angabe PB13/PB14 war falsch.) Auf der Leitung sitzen nur R13/R14 (4,7 kΩ nach 3V3). */
#define HOST_I2C2_INSTANCE              I2C2                /* 0x40005800, APB1 */
#define HOST_I2C2_SCL_PORT              GPIOB
#define HOST_I2C2_SCL_PIN               (1U << 10)          /* PB10 (AF4 = I2C2_SCL) */
#define HOST_I2C2_SDA_PORT              GPIOB
#define HOST_I2C2_SDA_PIN               (1U << 11)          /* PB11 (AF4 = I2C2_SDA) */
#define HOST_I2C2_DEFAULT_ADDR          0x52U               /* 7-Bit Slave-Adresse */

/* Server-UART über dieselben Adern (geplant für den SDK-Neubau, nicht gleichzeitig mit I2C2):
 * LPUART1 AF8: PB11 = LPUART1_TX -> ESP32 GPIO15, PB10 = LPUART1_RX <- ESP32 GPIO16.
 * DMA2 Kanal 6 (TX) / 7 (RX), Request 4 laut RM0394 – beim Einrichten im SDK gegenprüfen.
 * USART3 (AF7) ginge auch, bräuchte aber DMA1 Kanal 2/3, die der Server für SPI1 (A121) belegt. */
#define HOST_LPUART1_INSTANCE           LPUART1             /* 0x40008000, APB1 */
#define HOST_LPUART1_TX_PORT            GPIOB
#define HOST_LPUART1_TX_PIN             (1U << 11)          /* PB11 (AF8 = LPUART1_TX) */
#define HOST_LPUART1_RX_PORT            GPIOB
#define HOST_LPUART1_RX_PIN             (1U << 10)          /* PB10 (AF8 = LPUART1_RX) */
#define HOST_LPUART1_IRQn               70                  /* LPUART1_IRQn */

/* Steuer- und Interrupt-Pins am 10-Pin MX1.25 Stecker */
#define HOST_MCU_INT_PORT               GPIOB
#define HOST_MCU_INT_PIN                (1U << 9)           /* PB9  (MCU_INT -> ESP32 GPIO18, Push-Pull Ausgang) */

#define HOST_WAKE_UP_PORT               GPIOA
#define HOST_WAKE_UP_PIN                (1U << 11)          /* PA11 (WAKE_UP, EXTI11, getrieben von CH342 DTR0) */

#define HOST_MISC_GPIO0_PORT            GPIOB
#define HOST_MISC_GPIO0_PIN             (1U << 1)           /* PB1  (MISC_GPIO0 <-> ESP32 GPIO6, braune Ader) */

#define HOST_MISC_GPIO1_PORT            GPIOA
#define HOST_MISC_GPIO1_PIN             (1U << 4)           /* PA4  (MISC_GPIO1 <-> ESP32 GPIO7, graue Ader) */

#define HOST_MISC_GPIO2_PORT            GPIOA
#define HOST_MISC_GPIO2_PIN             (1U << 12)          /* PA12 (MISC_GPIO2, intern für I2C-Addr Pull-Up) */

#define HOST_DEBUG_UART_DISABLE_PORT    GPIOB
#define HOST_DEBUG_UART_DISABLE_PIN     (1U << 8)           /* PB8  (DEBUG_UART_DISABLE, Input Pull-Up) */

#endif /* XM125_WAVESHARE_BSP_H */
