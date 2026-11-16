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
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include "board.h"
#include "uart_app.h"
#include "boot_loader.h"
#include "flash_app.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* 串口超过这么多毫秒没有新数据，就把还不足一页的尾巴也写进 Flash。
 * 否则最后一页（比如 11156 字节里的最后 916 字节）会永远留在内存里写不下去。 */
#define FLASH_IDLE_FLUSH_MS 2000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* 从串口取数据的临时区。放静态区：512 字节压栈太靠近 1KB 栈底 */
static uint8_t s_rx[512];

/* 攒页缓冲：收够 2KB 才擦一次、写一次。
   为什么不收一包写一包：擦一页要 20~40ms，写一页才 1~2ms。
   512 字节写一次的话，同一页要被擦 4 次，慢 4 倍、寿命也砍到 1/4。 */
static uint8_t s_page[FLASH_PAGE_BYTES];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

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
  /* USER CODE BEGIN 2 */
  Uart_Init();
  Led_Fan(1);

	uint16_t len;						/* 这次从串口取到多少字节 */
	uint32_t wr_addr = APP_ADDR;		/* 下一个要写的页首地址 */
	uint32_t fill    = 0U;				/* s_page 里已经攒了多少字节 */
	uint32_t total   = 0U;				/* 累计收到的字节数 */
	uint32_t last_rx = Board_Millis();	/* 最后一次收到数据的时刻 */

	Uart_Printf(UART_LOG, "\r\n[ota] ready, app=0x%08lX, page=%luB\r\n",
				(unsigned long)APP_ADDR, (unsigned long)FLASH_PAGE_BYTES);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	len = Uart_Read(UART_LOG, s_rx, sizeof(s_rx));

	if(len > 0U)
	{
		uint16_t i = 0U;

		total  += len;
		last_rx = Board_Millis();

		/* 把这次收到的字节塞进页缓冲，攒满 2KB 就落盘一次 */
		while(i < len)
		{
			uint32_t space = FLASH_PAGE_BYTES - fill;									/* 这一页还剩多少空位 */
			uint32_t n     = ((uint32_t)(len - i) < space) ? (uint32_t)(len - i) : space;	/* 这次能塞多少 */

			memcpy(&s_page[fill], &s_rx[i], n);
			fill += n;
			i    += (uint16_t)n;

			if(fill == FLASH_PAGE_BYTES)		/* 攒满一页才动手 */
			{
				Flash_ErasePage(wr_addr);			/* 1. 先擦这一页 */
				Flash_Write(wr_addr, s_page, fill);	/* 2. 再整页写进去 */

				Uart_Printf(UART_LOG, "[ota] page 0x%08lX ok, sum: %lu\r\n",
							(unsigned long)wr_addr, (unsigned long)total);

				wr_addr += FLASH_PAGE_BYTES;		/* 3. 挪到下一页 */
				fill = 0U;
			}
		}
	}
	else if((fill > 0U) && (Timer_Timeout(&last_rx, FLASH_IDLE_FLUSH_MS) != 0U))
	{
		/* 2 秒没有新数据：说明发完了，把不足一页的尾巴也写进去。
		   这一页刚擦过，所以只写 fill 个字节就够，后面的自动还是 0xFF。 */
		Flash_ErasePage(wr_addr);
		Flash_Write(wr_addr, s_page, fill);

		Uart_Printf(UART_LOG, "[ota] tail 0x%08lX %luB ok, sum: %lu\r\n",
					(unsigned long)wr_addr, (unsigned long)fill, (unsigned long)total);

		wr_addr += FLASH_PAGE_BYTES;
		fill = 0U;
	}
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
