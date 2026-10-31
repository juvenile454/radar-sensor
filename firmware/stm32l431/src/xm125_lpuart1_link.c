// radar-sensor: Exploration Server auf LPUART1 über die SDA/SCL-Adern des Waveshare-A121-Steckers.
//
// build_server.py legt diese Datei in eine Kopie des Acconeer-XM125-SDK (Src/integration/).
// Pins laut ST-Pindaten (STM32_open_pin_data, STM32L431C(B-C)Tx) und Board-Schaltplan:
//   PB11 = LPUART1_TX (AF8) -> SDA-Ader -> ESP32 GPIO15 (RX)
//   PB10 = LPUART1_RX (AF8) <- SCL-Ader <- ESP32 GPIO16 (TX)
// DMA (STM32L431, ohne DMAMUX): DMA2 Kanal 6 = LPUART1_TX, Kanal 7 = LPUART1_RX, jeweils Request 4.
// An den Adern hängen auf dem Board nur R13/R14 (4,7 kOhm nach 3V3), kein CH342.

#include "main.h"

UART_HandleTypeDef hlpuart1;
DMA_HandleTypeDef  hdma_lpuart1_rx;
DMA_HandleTypeDef  hdma_lpuart1_tx;

extern I2C_HandleTypeDef hi2c2;

void LPUART1_IRQHandler(void);
void DMA2_Channel6_IRQHandler(void);
void DMA2_Channel7_IRQHandler(void);

// Aufruf in main() direkt vor acconeer_main(), nach den CubeMX-Inits des SDK.
void xm125_lpuart1_init(void)
{
	// PB10/PB11 gehören in diesem Build der UART: I2C2 aus MX_I2C2_Init() wieder freigeben
	// (HAL_I2C_MspDeInit des SDK schaltet Takt, Pins und I2C2_EV_IRQn ab).
	if (HAL_I2C_DeInit(&hi2c2) != HAL_OK)
	{
		Error_Handler();
	}

	__HAL_RCC_DMA2_CLK_ENABLE();
	HAL_NVIC_SetPriority(DMA2_Channel6_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(DMA2_Channel6_IRQn);
	HAL_NVIC_SetPriority(DMA2_Channel7_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(DMA2_Channel7_IRQn);

	// Wie MX_USART2_UART_Init() des SDK, aber ohne RTS/CTS (die Adern gibt es nicht).
	hlpuart1.Instance                    = LPUART1;
	hlpuart1.Init.BaudRate               = 115200;
	hlpuart1.Init.WordLength             = UART_WORDLENGTH_8B;
	hlpuart1.Init.StopBits               = UART_STOPBITS_1;
	hlpuart1.Init.Parity                 = UART_PARITY_NONE;
	hlpuart1.Init.Mode                   = UART_MODE_TX_RX;
	hlpuart1.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
	hlpuart1.Init.OverSampling           = UART_OVERSAMPLING_16;
	hlpuart1.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
	hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
	if (HAL_UART_Init(&hlpuart1) != HAL_OK)
	{
		Error_Handler();
	}
}

// Die SDK-Versionen heißen im Build xm125_sdk_HAL_UART_Msp(De)Init (build_server.py benennt sie um);
// alle anderen UARTs laufen unverändert dorthin.
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
	if (huart->Instance != LPUART1)
	{
		xm125_sdk_HAL_UART_MspInit(huart);
		return;
	}

	RCC_PeriphCLKInitTypeDef clk = {0};
	clk.PeriphClockSelection  = RCC_PERIPHCLK_LPUART1;
	clk.Lpuart1ClockSelection = RCC_LPUART1CLKSOURCE_PCLK1;  // 80 MHz
	if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
	{
		Error_Handler();
	}

	__HAL_RCC_LPUART1_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	GPIO_InitTypeDef gpio = {0};
	gpio.Pin       = SERVER_UART_RX_Pin | SERVER_UART_TX_Pin;
	gpio.Mode      = GPIO_MODE_AF_PP;
	gpio.Pull      = GPIO_PULLUP;  // Ruhepegel HIGH halten, auch ohne ESP32
	gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
	gpio.Alternate = GPIO_AF8_LPUART1;
	HAL_GPIO_Init(SERVER_UART_RX_GPIO_Port, &gpio);

	hdma_lpuart1_rx.Instance                 = DMA2_Channel7;
	hdma_lpuart1_rx.Init.Request             = DMA_REQUEST_4;
	hdma_lpuart1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
	hdma_lpuart1_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
	hdma_lpuart1_rx.Init.MemInc              = DMA_MINC_ENABLE;
	hdma_lpuart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
	hdma_lpuart1_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
	hdma_lpuart1_rx.Init.Mode                = DMA_NORMAL;
	hdma_lpuart1_rx.Init.Priority            = DMA_PRIORITY_LOW;
	if (HAL_DMA_Init(&hdma_lpuart1_rx) != HAL_OK)
	{
		Error_Handler();
	}
	__HAL_LINKDMA(huart, hdmarx, hdma_lpuart1_rx);

	hdma_lpuart1_tx.Instance                 = DMA2_Channel6;
	hdma_lpuart1_tx.Init.Request             = DMA_REQUEST_4;
	hdma_lpuart1_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
	hdma_lpuart1_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
	hdma_lpuart1_tx.Init.MemInc              = DMA_MINC_ENABLE;
	hdma_lpuart1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
	hdma_lpuart1_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
	hdma_lpuart1_tx.Init.Mode                = DMA_NORMAL;
	hdma_lpuart1_tx.Init.Priority            = DMA_PRIORITY_LOW;
	if (HAL_DMA_Init(&hdma_lpuart1_tx) != HAL_OK)
	{
		Error_Handler();
	}
	__HAL_LINKDMA(huart, hdmatx, hdma_lpuart1_tx);

	HAL_NVIC_SetPriority(LPUART1_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(LPUART1_IRQn);
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *huart)
{
	if (huart->Instance != LPUART1)
	{
		xm125_sdk_HAL_UART_MspDeInit(huart);
		return;
	}

	__HAL_RCC_LPUART1_CLK_DISABLE();
	HAL_GPIO_DeInit(SERVER_UART_RX_GPIO_Port, SERVER_UART_RX_Pin | SERVER_UART_TX_Pin);
	HAL_DMA_DeInit(huart->hdmarx);
	HAL_DMA_DeInit(huart->hdmatx);
	HAL_NVIC_DisableIRQ(LPUART1_IRQn);
}

void LPUART1_IRQHandler(void)
{
	HAL_UART_IRQHandler(&hlpuart1);
}

void DMA2_Channel6_IRQHandler(void)
{
	HAL_DMA_IRQHandler(&hdma_lpuart1_tx);
}

void DMA2_Channel7_IRQHandler(void)
{
	HAL_DMA_IRQHandler(&hdma_lpuart1_rx);
}
