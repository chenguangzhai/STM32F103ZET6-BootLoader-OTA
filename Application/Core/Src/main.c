/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "board.h"
#include "uart_app.h"
#include "w25q.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* App 镜像在 Flash 里的起始地址，必须和 STM32F103XX_FLASH.ld 的 FLASH ORIGIN
   一字不差。BootLoader 跳过来时也是往这个地址取初始 SP 和复位向量。
   BootLoader 占 32KB，所以 App 从 0x08008000 开始。
   F103xE 页大小 2KB，所以这个值必须 2KB 对齐（0x08008000 = 第 16 页）。 */
#define APP_ADDR    0x08008000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* W25Q128 自检：读 ID → 擦扇区 0 → 写 16 字节 → 读回打印。
 *
 * 只擦地址 0 这一个 4KB 扇区，不影响其他区域。
 * 但如果你板上这颗 W25Q 里存了别的东西（出厂参数、字库等），
 * 就把下面的 0x000000 换成你确认空白的地址，比如 0x00F00000。
 *
 * 验通之后把 USER CODE BEGIN 2 里那行调用删掉即可。 */
static void W25q_SelfTest(void)
{
	static uint8_t wbuf[16];
	static uint8_t rbuf[16];
	uint32_t id;
	uint8_t  i;

	/* 片选 PB12。SPI2 本身（Mode 0 / 8bit / 9MHz）已在 MX_SPI2_Init() 里配好 */
	W25q_Init(&hspi2, GPIOB, GPIO_PIN_12);

	id = W25q_ReadId();
	Uart_Printf(UART_LOG, "[w25q] id=0x%06lX %s\r\n", (unsigned long)id,
				(id == W25Q_JEDEC_ID) ? "OK" : "FAIL, expect 0xEF4018");

	if(id != W25Q_JEDEC_ID)
	{
		/* 读不到 ID 说明片选或接线有问题，后面擦写必然失败，不浪费时间 */
		return;
	}

	for(i = 0U; i < 16U; i++)
	{
		wbuf[i] = (uint8_t)(0xA0U + i);
	}

	Uart_Printf(UART_LOG, "[w25q] erase sector 0 ... %s\r\n",
				(W25q_EraseSector(0U) == W25Q_OK) ? "ok" : "FAIL");

	Uart_Printf(UART_LOG, "[w25q] write 16B ... %s\r\n",
				(W25q_Write(0U, wbuf, 16U) == W25Q_OK) ? "ok" : "FAIL");

	(void)W25q_Read(0U, rbuf, 16U);
	Uart_Printf(UART_LOG, "[w25q] read back: ");
	for(i = 0U; i < 16U; i++)
	{
		Uart_Printf(UART_LOG, "%02X ", rbuf[i]);
	}
	Uart_Printf(UART_LOG, "\r\n");
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* 中断向量表重定位，必须放在 HAL_Init() 之前。
     HAL_Init() 里会打开 SysTick，而 SysTick_Handler 的入口是从 VTOR
     指向的那张表里取的：VTOR 还指着 0x08000000 时，SysTick 会跑到
     BootLoader 的向量表里去 —— 能跑，但跑的是别人的中断。
     后面开 USART1 / DMA 中断同理。 */
  SCB->VTOR = APP_ADDR;

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_SPI2_Init();
  /* USER CODE BEGIN 2 */
  Uart_Init();
  Uart_Printf(UART_LOG, "Hello, world!\r\n");

  W25q_SelfTest();			/* W25Q128 通不通，看这一行打印 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    Led_Fan(1);
    HAL_Delay(500);
    Led_Fan(0);
    HAL_Delay(500);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
