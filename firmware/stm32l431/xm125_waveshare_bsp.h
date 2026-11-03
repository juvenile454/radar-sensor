/**
 * @file xm125_waveshare_bsp.h
 * @brief Board Support Package (BSP) & exact pin/peripheral definitions for the
 *        Waveshare A121 Range Sensor Board (Acconeer XM125 = STM32L431CBT6 + A121).
 *
 * Verified by schematic and measurements on the board (with the original firmwares
 * `i2c_distance_detector.bin` & `acc_exploration_server_a121.bin`, a121-v1.12.0).
 */

#ifndef XM125_WAVESHARE_BSP_H
#define XM125_WAVESHARE_BSP_H

#include <stdint.h>

/* ============================================================================
 * 1. A121 radar transceiver <-> STM32L431CBT6 (internal to the XM125 module)
 * ============================================================================ */
#define A121_SPI_INSTANCE               SPI1                /* 0x40013000, APB2 (40 MHz SPI clock @ 80 MHz HCLK) */
#define A121_SPI_SCK_PORT               GPIOA
#define A121_SPI_SCK_PIN                (1U << 5)           /* PA5 (AF5 = SPI1_SCK) */
#define A121_SPI_MISO_PORT              GPIOA
#define A121_SPI_MISO_PIN               (1U << 6)           /* PA6 (AF5 = SPI1_MISO) */
#define A121_SPI_MOSI_PORT              GPIOA
#define A121_SPI_MOSI_PIN               (1U << 7)           /* PA7 (AF5 = SPI1_MOSI) */

#define A121_SPI_CS_PORT                GPIOB               /* 0x48000400 */
#define A121_SPI_CS_PIN                 (1U << 0)           /* PB0 (software NSS, active LOW) */

#define A121_ENABLE_PORT                GPIOB
#define A121_ENABLE_PIN                 (1U << 12)          /* PB12 (A121 ENABLE / power enable, active HIGH) */

#define A121_INT_PORT                   GPIOB
#define A121_INT_PIN                    (1U << 3)           /* PB3 (EXTI3_IRQn = IRQ 9, rising edge = data ready) */
#define A121_INT_IRQn                   9                   /* EXTI3_IRQn */

#define A121_SPI_DMA_RX_CHANNEL         DMA1_Channel2       /* 0x4002001C, Request 1 = SPI1_RX, IRQ 12 */
#define A121_SPI_DMA_TX_CHANNEL         DMA1_Channel3       /* 0x40020030, Request 1 = SPI1_TX, IRQ 13 */

/* ============================================================================
 * 2. 10-pin MX1.25 connector <-> ESP32-S3 (external host interfaces)
 * ============================================================================ */

/* High-Speed UART1 (MX1.25 pins TXD/RXD <-> ESP32-S3 GPIO5 RX / GPIO4 TX + CH342 channel B) */
#define HOST_UART1_INSTANCE             USART1              /* 0x40013800, APB2 (80 MHz PCLK2) */
#define HOST_UART1_TX_PORT              GPIOA
#define HOST_UART1_TX_PIN               (1U << 9)           /* PA9  (AF7 = USART1_TX -> ESP32 GPIO5) */
#define HOST_UART1_RX_PORT              GPIOA
#define HOST_UART1_RX_PIN               (1U << 10)          /* PA10 (AF7 = USART1_RX <- ESP32 GPIO4) */
#define HOST_UART1_DMA_TX_CHANNEL       DMA1_Channel4       /* 0x40020044, Request 2 = USART1_TX, IRQ 14 */
#define HOST_UART1_DMA_RX_CHANNEL       DMA1_Channel5       /* 0x40020058, Request 2 = USART1_RX, IRQ 15 */
#define HOST_UART1_IRQn                 37                  /* USART1_IRQn */

/* USB-only UART2 (CH342 channel A -if00, NOT on the 10-pin MX1.25 connector!) */
#define USB_UART2_INSTANCE              USART2              /* 0x40004400, APB1 (80 MHz PCLK1) */
#define USB_UART2_CTS_PIN               (1U << 0)           /* PA0 (AF7 = USART2_CTS) */
#define USB_UART2_RTS_PIN               (1U << 1)           /* PA1 (AF7 = USART2_RTS) */
#define USB_UART2_TX_PIN                (1U << 2)           /* PA2 (AF7 = USART2_TX) */
#define USB_UART2_RX_PIN                (1U << 3)           /* PA3 (AF7 = USART2_RX) */
#define USB_UART2_DMA_RX_CHANNEL        DMA1_Channel6       /* 0x4002006C, Request 2 = USART2_RX, IRQ 16 */
#define USB_UART2_DMA_TX_CHANNEL        DMA1_Channel7       /* 0x40020080, Request 2 = USART2_TX, IRQ 17 */
#define USB_UART2_IRQn                  38                  /* USART2_IRQn */

/* I2C2 slave (MX1.25 pins SCL/SDA <-> ESP32-S3 GPIO16 SCL / GPIO15 SDA)
 * PB10/PB11 checked 2026-10-31: schematic nets I2C2_SCL = PB10, I2C2_SDA = PB11, and
 * i2c_distance_detector.bin initializes GPIOB mask 0x0C00 as AF4 open-drain with pull-up.
 * (The earlier PB13/PB14 entry was wrong.) Only R13/R14 (4.7 kΩ to 3V3) sit on the line. */
#define HOST_I2C2_INSTANCE              I2C2                /* 0x40005800, APB1 */
#define HOST_I2C2_SCL_PORT              GPIOB
#define HOST_I2C2_SCL_PIN               (1U << 10)          /* PB10 (AF4 = I2C2_SCL) */
#define HOST_I2C2_SDA_PORT              GPIOB
#define HOST_I2C2_SDA_PIN               (1U << 11)          /* PB11 (AF4 = I2C2_SDA) */
#define HOST_I2C2_DEFAULT_ADDR          0x52U               /* 7-bit slave address */

/* Server UART over the same wires (planned for the SDK rebuild, not simultaneously with I2C2):
 * LPUART1 AF8: PB11 = LPUART1_TX -> ESP32 GPIO15, PB10 = LPUART1_RX <- ESP32 GPIO16.
 * DMA2 channel 6 (TX) / 7 (RX), request 4 per RM0394 – cross-check when setting up in the SDK.
 * USART3 (AF7) would also work, but would need DMA1 channel 2/3, which the server uses for SPI1 (A121). */
#define HOST_LPUART1_INSTANCE           LPUART1             /* 0x40008000, APB1 */
#define HOST_LPUART1_TX_PORT            GPIOB
#define HOST_LPUART1_TX_PIN             (1U << 11)          /* PB11 (AF8 = LPUART1_TX) */
#define HOST_LPUART1_RX_PORT            GPIOB
#define HOST_LPUART1_RX_PIN             (1U << 10)          /* PB10 (AF8 = LPUART1_RX) */
#define HOST_LPUART1_IRQn               70                  /* LPUART1_IRQn */

/* Control and interrupt pins on the 10-pin MX1.25 connector */
#define HOST_MCU_INT_PORT               GPIOB
#define HOST_MCU_INT_PIN                (1U << 9)           /* PB9  (MCU_INT -> ESP32 GPIO18, push-pull output) */

#define HOST_WAKE_UP_PORT               GPIOA
#define HOST_WAKE_UP_PIN                (1U << 11)          /* PA11 (WAKE_UP, EXTI11, driven by CH342 DTR0) */

#define HOST_MISC_GPIO0_PORT            GPIOB
#define HOST_MISC_GPIO0_PIN             (1U << 1)           /* PB1  (MISC_GPIO0 <-> ESP32 GPIO6, brown wire) */

#define HOST_MISC_GPIO1_PORT            GPIOA
#define HOST_MISC_GPIO1_PIN             (1U << 4)           /* PA4  (MISC_GPIO1 <-> ESP32 GPIO7, gray wire) */

#define HOST_MISC_GPIO2_PORT            GPIOA
#define HOST_MISC_GPIO2_PIN             (1U << 12)          /* PA12 (MISC_GPIO2, internal, for I2C address pull-up) */

#define HOST_DEBUG_UART_DISABLE_PORT    GPIOB
#define HOST_DEBUG_UART_DISABLE_PIN     (1U << 8)           /* PB8  (DEBUG_UART_DISABLE, input pull-up) */

#endif /* XM125_WAVESHARE_BSP_H */
