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
#include "ota_app.h"		/* 落盘逻辑都在这里，main 只负责把数据喂进去 */
#include "ota_param.h"		/* 参数区 + CRC 校验：判断 App 到底能不能跳 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* 串口超过这么多毫秒没有新数据，就把还不足一页的尾巴也写进 Flash。
 * 否则最后一页（比如 11168 字节里的最后 928 字节）会永远留在内存里写不下去。 */
#define OTA_IDLE_FLUSH_MS   2000U

/* 上电后留给上位机的升级窗口（毫秒）。
 * 这段时间内串口收到数据 → 留在 bootloader 升级；一直没有数据 → 跳到 App。
 * 为什么必须留：无条件跳的话，App 一旦写坏或跑飞，bootloader 就再也没机会
 * 被叫醒，只能拿 ST-Link 救。
 * 调大 → 开机慢但从容；调小 → 开机快但按完复位要手快。
 * 3000 是折中：串口助手里点一次"发送"通常 1~2 秒能完成。 */
#define OTA_WAIT_MS         3000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* 从串口取数据的临时区。放静态区：512 字节压栈太靠近 1KB 栈底。
   攒页缓冲已经搬进 ota_app.c，main 里不再需要 s_page。 */
static uint8_t s_rx[512];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * 函    数：把这次复位的原因打出来，然后清掉标志
  * 说    明：RCC->CSR 里锁存着复位来源，读一次就知道是按键、上电还是看门狗。
  *           必须清标志 —— 不清的话下次读到的还是这一次的旧值，永远查不出真相。
  *
  * 怎么看结果（实测规律）：
  *     真正上电      → POR + PIN 两条（上电瞬间 NRST 引脚也会被拉低一次）
  *     只按复位键    → 只有 PIN 一条
  *     调试器 reset  → SFT（或 PIN，取决于调试器走哪条路）
  *     看门狗咬的    → IWDG
  *
 * 为什么要查这个：只有 PIN 却连续出现好几条，说明按一次键产生了多个复位脉冲
 * （机械按键抖动），不是固件在循环重启 —— 这两件事的处理方式完全不同。
 *
 * 【为什么日志一律用英文，不用中文】
 * 源码是 UTF-8 保存的，一个汉字占 3 字节，这些字节会被原样烧进固件；
 * 而串口助手默认按 GBK 解码（1 个汉字 2 字节），于是「引脚」6 个字节
 * 被拆成 3 个 GBK 汉字 → 显示成「寮曡剼」这种乱码。
 * 这不是固件的问题，是两端编码不一致。日志用 ASCII 就永远不会遇到，
 * 换个串口工具、换台电脑都一样。中文只写在注释里（注释不进二进制）。
 */
static void PrintResetCause(void)
{
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))
	{
		Uart_Printf(UART_LOG, "[rst] POR  / power-on or brown-out\r\n");
	}
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))
	{
		Uart_Printf(UART_LOG, "[rst] PIN  / NRST pin\r\n");
	}
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))
	{
		Uart_Printf(UART_LOG, "[rst] SFT  / software reset\r\n");
	}
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST))
	{
		Uart_Printf(UART_LOG, "[rst] IWDG / independent watchdog\r\n");
	}
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST))
	{
		Uart_Printf(UART_LOG, "[rst] WWDG / window watchdog\r\n");
	}
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST))
	{
		Uart_Printf(UART_LOG, "[rst] LPWR / low-power wakeup\r\n");
	}

	__HAL_RCC_CLEAR_RESET_FLAGS();		/* 清掉，下次复位才有干净的结果 */
}

/* ==================== OTA 收尾：校验 → 存参数 → 主动跳转 ==================== */

/**
  * 函    数：CheckAppBoot
  * 功    能：判断 App 到底能不能跳
  * 返 回 值：0 = 不能跳（空片 / 向量表不合法 / 参数坏 / CRC 不对）
  *           1 = 能跳（参数有效，且 Flash 读回来算的 CRC 对得上）
  *           2 = 兼容模式（参数页空的，多半是老固件；只做了向量表检查）
  *
  * 说    明：为什么要有 2 这个状态。
  *           加了参数区之后，之前刷进去的老固件没有参数，一律拒绝跳转的话
  *           用户会以为"刷个 bootloader 把板子刷砖了"。所以参数页全空时
  *           退回原来的弱检查，但日志明确写 unverified —— 提醒这份固件没校验过。
  */
static uint8_t CheckAppBoot(void)
{
	OtaParam_t p;
	int8_t     st;

	if(Boot_Loader_AppValid(APP_ADDR) == 0U)
	{
		Uart_Printf(UART_LOG, "[boot] app invalid: vector table\r\n");
		return 0U;
	}

	st = Ota_Param_Load(&p);
	if(st == OTA_PARM_EMPTY)
	{
		Uart_Printf(UART_LOG, "[boot] no param, unverified app\r\n");
		return 2U;
	}
	if(st != OTA_PARM_OK)
	{
		/* 参数页有内容却校验不过：多半是上次写参数时断了电 */
		Uart_Printf(UART_LOG, "[boot] param broken, stay in ota\r\n");
		return 0U;
	}

	Uart_Printf(UART_LOG, "[boot] param: app=%luB crc=0x%08lX\r\n",
				(unsigned long)p.length, (unsigned long)p.crc32);

	if(Ota_VerifyFlash(APP_ADDR, p.length, p.crc32) != 0)
	{
		/* 参数说该是多少，Flash 里却不是 —— 跳过去必跑飞 */
		Uart_Printf(UART_LOG, "[boot] crc mismatch, stay in ota\r\n");
		return 0U;
	}

	Uart_Printf(UART_LOG, "[boot] crc ok\r\n");
	return 1U;
}

/**
  * 函    数：FinishOta
  * 功    能：收到尾部记录后的完整收尾：落盘 → 校验 → 存参数 → 跳转
  * 说    明：任何一步失败都 return，把板子留在 OTA 里等重传。
  *           宁可让用户再发一次，也不能跳进一份坏固件。
  */
static void FinishOta(const OtaTail_t *tail)
{
	Uart_Printf(UART_LOG, "[ota] tail: len=%lu crc=0x%08lX\r\n",
				(unsigned long)tail->length, (unsigned long)tail->crc32);

	/* 先把 RAM 里攒着的最后一页写出去。不 Flush 的话 Flash 里缺结尾一段，
	   下面读回来算的 CRC 必然对不上 */
	if(Ota_Flush() != OTA_OK)
	{
		Uart_Printf(UART_LOG, "[ota] flush failed\r\n");
		return;
	}

	if(Ota_Total() != tail->length)
	{
		Uart_Printf(UART_LOG, "[ota] size mismatch: got %lu want %lu\r\n",
					(unsigned long)Ota_Total(),
					(unsigned long)tail->length);
		return;
	}

	if(Ota_VerifyFlash(APP_ADDR, tail->length, tail->crc32) != 0)
	{
		Uart_Printf(UART_LOG, "[ota] verify FAIL\r\n");
		return;
	}
	Uart_Printf(UART_LOG, "[ota] verify OK %luB\r\n",
				(unsigned long)tail->length);

	/* 参数最后写：它写成功之后，下次上电就以它为准了 */
	if(Ota_Param_Save(tail->length, tail->crc32, 0U) != OTA_PARM_OK)
	{
		Uart_Printf(UART_LOG, "[ota] param save failed\r\n");
		return;
	}

	Uart_Printf(UART_LOG, "[boot] jump to app 0x%08lX\r\n",
				(unsigned long)APP_ADDR);
	Boot_Loader_JumpToApp(APP_ADDR);
	Uart_Printf(UART_LOG, "[boot] jump aborted\r\n");
}

/**
  * 函    数：HandleRx
  * 功    能：统一处理收到的一包数据
  * 返 回 值：1 = 这包是尾部记录（收尾流程已接管，可能已经跳走不返回）
  *           0 = 普通固件数据，已喂给 ota_app
  */
static uint8_t HandleRx(const uint8_t *buf, uint32_t len)
{
	OtaTail_t tail;
	uint32_t  i;
	uint32_t  total = Ota_Total() + Ota_Fill();

	/* 在整个数据块里找尾部记录，而不是只看块首。
	   原因是串口是流式的：上位机发完最后一块会紧接着发尾部，
	   MCU 的一次 Uart_Read 完全可能把「固件末尾若干字节 + 12 字节尾部」
	   合并成一块。只看块首的话，magic 根本不在开头，检测必然失败，
	   于是这 12 字节就被当成固件写进 Flash —— 实测踩过。
	   扫到之后，把尾部前面的部分正常落盘，再走校验流程。 */
	for(i = 0U; (i + OTA_TAIL_BYTES) <= len; i++)
	{
		/* recv_total 传 total + i：扫到第 i 字节时，这 i 字节还没落盘 */
		if(Ota_ParseTail(&buf[i], OTA_TAIL_BYTES, total + i, &tail) != 0U)
		{
			if(i > 0U)
			{
				if(Ota_Feed(buf, i) != OTA_OK)
				{
					Uart_Printf(UART_LOG, "[ota] write failed before tail\r\n");
					return 1U;
				}
			}
			FinishOta(&tail);
			return 1U;
		}
	}

	/* 别写进参数页 */
	if((Ota_Total() + Ota_Fill() + len) > OTA_APP_MAX)
	{
		Uart_Printf(UART_LOG, "[ota] overflow, %lu > %lu\r\n",
					(unsigned long)(Ota_Total() + Ota_Fill() + len),
					(unsigned long)OTA_APP_MAX);
		return 1U;
	}

	if(Ota_Feed(buf, len) != OTA_OK)
	{
		Uart_Printf(UART_LOG, "[ota] write failed at 0x%08lX, sum: %lu\r\n",
					(unsigned long)Ota_Addr(), (unsigned long)Ota_Total());
	}
	return 0U;
}

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
  // Led_Fan(1);

  PrintResetCause();				/* 这次是被什么复位起来的：POR / PIN / SFT / IWDG */

	uint16_t len;						/* 这次从串口取到多少字节 */
	uint32_t last_rx  = Board_Millis();	/* 最后一次收到数据的时刻 */
	uint32_t t_boot   = Board_Millis();	/* 上电时刻，用来算等待窗口 */
	uint8_t  ota_mode = 0U;				/* 1 = 已经决定留在 bootloader 收固件 */

	Ota_Init(APP_ADDR);					/* 写入起点：App 区第 16 页开头，同时清空内部状态 */

	/* proto=2：告诉上位机"我支持尾部记录 + CRC 校验，发完请补 12 字节尾部"。
	   老版本没有这个字段，上位机会自动退回"等 sum 然后复位"的老流程，
	   所以新工具配旧 bootloader 也不会多发字节。 */
	Uart_Printf(UART_LOG, "\r\n[ota] ready, app=0x%08lX, page=%luB, proto=2\r\n",
				(unsigned long)APP_ADDR, (unsigned long)OTA_PAGE_BYTES);

	/* ---------- 上电窗口 ----------
	   App 校验通过时等 OTA_WAIT_MS 毫秒：这期间上位机发数据就留在 bootloader
	   升级，一直没数据就跳过去跑 App。
	   校验不过（空片 / 参数坏 / CRC 错）就不用等 —— 反正没得跳，直接进下面的循环。

	   注意这里比原来严了：以前只看向量表"像不像样"，现在要求 Flash 读回来
	   算的 CRC 和参数里记的一致。宁可停在 OTA 等重传，也不跳进坏固件。 */
	{
		uint8_t boot_st = CheckAppBoot();

		if(boot_st != 0U)					/* 1 = 校验通过；2 = 老固件，没参数 */
		{
			if(boot_st == 2U)
			{
				Uart_Printf(UART_LOG, "[boot] legacy mode: not verified\r\n");
			}

			while((ota_mode == 0U) && (Timer_Timeout(&t_boot, OTA_WAIT_MS) == 0U))
			{
				len = Uart_Read(UART_LOG, s_rx, sizeof(s_rx));
				if(len > 0U)
				{
					last_rx  = Board_Millis();
					ota_mode = 1U;
					Uart_Printf(UART_LOG, "[boot] enter OTA\r\n");
					HandleRx(s_rx, len);	/* 这一包别丢；万一是尾部也能认出来 */
				}
			}

			if(ota_mode == 0U)				/* 窗口内没人理，跳 App */
			{
				Uart_Printf(UART_LOG, "[boot] jump to app 0x%08lX\r\n", (unsigned long)APP_ADDR);
				Boot_Loader_JumpToApp(APP_ADDR);
				Uart_Printf(UART_LOG, "[boot] jump aborted\r\n");
			}
		}
		else
		{
			Uart_Printf(UART_LOG, "[boot] no usable app, waiting for firmware\r\n");
		}
	}
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	len = Uart_Read(UART_LOG, s_rx, sizeof(s_rx));	/* 取到 0 表示这段时间没有新数据 */

	if(len > 0U)
	{
		last_rx = Board_Millis();					/* 每收到一次就刷新"最后收到"的时刻 */

		/* 攒页 + 落盘藏在 Ota_Feed 里；但先要分辨这包是不是「尾部记录」。
		   HandleRx 返回 1 表示已按尾部处理（校验通过就直接跳走了，不回来） */
		HandleRx(s_rx, len);
	}
	else if((Ota_Fill() > 0U) && (Timer_Timeout(&last_rx, OTA_IDLE_FLUSH_MS) != 0U))
	{
		/* 2 秒没有新数据：说明发完了，把不足一页的尾巴也写出去。
		   新的上位机会明确发尾部记录，这条只是兜底 —— 老版本工具、
		   或者尾部包丢了的时候，至少数据不会烂在 RAM 里。 */
		Ota_Flush();
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
