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
#include "crc32.h"			/* 算固件 CRC，写头部之前必须先把数据算实 */
#include "ota_slot.h"		/* 双槽布局：App 负责往槽里写，Boot 负责读出来搬运 */
#include "app_ota.h"		/* 一直监听的固件接收状态机 */
#include "wdg.h"			/* 独立看门狗：死循环 → 自动复位 → Boot 才能回滚 */
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

/* 试用期确认门限：稳定运行这么久且期间一直在喂狗，才向 Boot 承认"这版能用"。
 *
 * 为什么不能像以前那样在初始化末尾立即确认：
 *   「初始化全过」不等于「固件能用」。一个初始化写对了、主循环里死循环的固件，
 *   在旧写法下第一秒就自己签了字，试用期形同虚设，回滚永远不会发生。
 *   现在要求"进主循环后连续跑满 5 秒"，死的固件根本走不到这一行。
 *
 * 为什么是 5 秒而不是 30 秒：
 *   5 秒足够暴露绝大多数启动后立即崩的问题（空指针、时钟没配好、外设死等），
 *   又短到用户感觉不到"升级完还要等一会儿"。
 *   真需要长周期的健康检查（比如等第一帧传感器数据）应该单独写业务判据，
 *   而不是靠把这个延时调得很长 —— 那只会让人以为升级卡住了。 */
#define APP_CONFIRM_DELAY_MS    5000U

/* 确认失败（W25Q 写不进）时的重试：每 1 秒一次，最多 5 次。
 * 不无限重试是因为失败原因基本是硬件性的，多试几次能救回来的话头几次就够了；
 * 而如果一直失败，继续每秒写一次 W25Q 只是白白消耗擦写寿命。 */
#define APP_CONFIRM_RETRY_MS    1000U
#define APP_CONFIRM_RETRY_MAX   5U

/* 和 BootLoader 约定的暗号，两边必须一字不差（Boot 侧见
   Boot_Loader/Core/Src/main.c 的 BKP_JUMP_MAGIC）。
   存在 RTC 后备寄存器 BKP_DR1 里，不占 RAM/Flash，复位不掉、断电才丢。
   注意必须是 16 位：F1 的 BKP_DRx 只有低 16 位有效，写 32 位会被截断，
   比较就永远不相等（Boot 那边踩过一次，整条握手静默失效）。 */
#define BKP_JUMP_MAGIC   0xA55AU

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

/**
  * 函    数：Bkp_ClearJumpMark
  * 功    能：擦掉 Boot 留下的"我跳过来了"暗号，等于报到："main 已经跑起来了"
  * 说    明：为什么要有这个标记 —— Boot 只能把控制权交出去，交完之后
  *           它什么也看不见。固件如果死在时钟配置里、或者 HardFault 卡在
  *           默认处理函数里，看门狗都还没开，板子就那么挂着，Boot 下次
  *           上电只看到"复位键"，以为是用户手按的，于是永远不回滚。
  *           有了这个标记：Boot 写一笔 → App 一进 main 就擦掉 → 下次上电
  *           这笔还在，就说明上次跳过去根本没活过来，这才能计一次试用失败。
  *
  * 为什么放在 main 最前面：越早越准。放在 Uart_Init 之后的话，
  * 串口初始化死等的固件就会漏判 —— 而那正是最需要认出来的情况之一。
  *
  * 为什么直接写寄存器：工程 Drivers 是裁剪过的，没有 HAL 的 PWR/BKP 驱动，
  * 为一个位补一堆 HAL 代码不划算（和 WDG 那边同样的取舍）。 */
static void Bkp_ClearJumpMark(void)
{
	RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
	(void)RCC->APB1ENR;			/* 读回一次，确认时钟真的开了再动 BKP */
	PWR->CR      |= PWR_CR_DBP;	/* 解除后备区写保护 */

	if((BKP->DR1 & 0xFFFFU) == BKP_JUMP_MAGIC)
	{
		BKP->DR1 = 0U;			/* 报到完毕，把暗号擦掉 */
	}
}

/* W25Q128 自检：读 ID → 擦一个扇区 → 写 16 字节 → 读回打印。
 *
 * 擦的是 OTA_SCRATCH_BASE（W25Q 最后一个扇区 0x00FFF000），不是地址 0。
 *
 * 这个改动是必须的：地址 0 现在是「槽 A 的头部扇区」，而 Boot 靠这个头
 * 判断"有没有待装固件"。如果 App 每次上电都把它擦掉，就等于每次开机
 * 都悄悄取消了刚下载好的升级 —— 而且下一个字节写进去还会让头部变成 BAD。
 * 症状会是"下载完一复位就不升了"，极难联想到是自检代码干的。
 *
 * 验通之后把 USER CODE BEGIN 2 里那行调用删掉即可。 */
static void W25q_SelfTest(void)
{
	static uint8_t wbuf[16];
	static uint8_t rbuf[16];
	uint32_t id;
	uint8_t  i;

	/* 片选 PB12。SPI2 本身（Mode 0 / 8bit / 9MHz）已在 MX_SPI2_Init() 里配好。
	   用 CubeMX 生成的宏而不是写死 GPIOB/PIN_12，换脚时这里自动跟随 */
	W25q_Init(&hspi2, W25Q128_CS_GPIO_Port, W25Q128_CS_Pin);

	id = W25q_ReadId();
	Uart_Printf(UART_LOG, "[w25q] 芯片ID=0x%06lX %s\r\n", (unsigned long)id,
				(id == W25Q_JEDEC_ID) ? "正常" : "异常，应为 0xEF4018");

	if(id != W25Q_JEDEC_ID)
	{
		/* 读不到 ID 说明片选或接线有问题，后面擦写必然失败，不浪费时间 */
		return;
	}

	for(i = 0U; i < 16U; i++)
	{
		wbuf[i] = (uint8_t)(0xA0U + i);
	}

	Uart_Printf(UART_LOG, "[w25q] 擦临时扇区 ... %s\r\n",
				(W25q_EraseSector(OTA_SCRATCH_BASE) == W25Q_OK) ? "成功" : "失败");

	Uart_Printf(UART_LOG, "[w25q] 写 16 字节 ... %s\r\n",
				(W25q_Write(OTA_SCRATCH_BASE, wbuf, 16U) == W25Q_OK) ? "成功" : "失败");

	(void)W25q_Read(OTA_SCRATCH_BASE, rbuf, 16U);
	Uart_Printf(UART_LOG, "[w25q] 读回：");
	for(i = 0U; i < 16U; i++)
	{
		Uart_Printf(UART_LOG, "%02X ", rbuf[i]);
	}
	Uart_Printf(UART_LOG, "\r\n");
}

/* 阶段 1 自检：往槽 A 写一份「假固件」（16 字节数据 + 头），再读回来校验。
 *
 * 验的是跨工程一致性 —— App 用 ota_slot.c 写、Boot 用另一份副本读。
 * 两份文件只要有一处布局、字节序或 CRC 约定不一致，Boot 就会打印 bad。
 * 这正是"两份副本必须同步"这条约定的自动化检查。
 *
 * 顺序固定为「先写数据、最后写头」：
 * 头部存在就等于数据已经在那儿且校验过，Boot 不需要再猜。
 *
 * flags 打 OTA_SLOT_FLAG_SELFTEST：这不是真固件，Boot 阶段 3 的搬运逻辑
 * 必须跳过带这个标志的槽，否则会把这份假数据搬进内部 Flash 当固件跑。 */
/* 阶段 1 已验证通过，调用点已注释掉。加 unused 属性是为了保留这段自测代码
   又不触发 -Wunused-function：下次改 ota_slot 布局时把它放开跑一遍，
   是成本最低的一致性回归测试 */
static void __attribute__((unused)) OtaSlot_SelfTest(void)
{
	static uint8_t buf[16];
	OtaSlotHeader_t hdr;
	OtaSlotHeader_t back;
	uint32_t crc;
	uint32_t data_addr = OtaSlot_DataAddr(OTA_SLOT_A);
	uint8_t  i;
	int8_t   st;

	for(i = 0U; i < 16U; i++)
	{
		buf[i] = (uint8_t)(0xB0U + i);
	}

	/* CRC 必须算在真正要写进去的那串字节上，不能用长度代替 */
	crc = Crc32_Buf(buf, 16U);

	if(W25q_EraseSector(data_addr) != W25Q_OK)
	{
		Uart_Printf(UART_LOG, "[slot] 擦 A 数据区 ... 失败\r\n");
		return;
	}

	if(W25q_Write(data_addr, buf, 16U) != W25Q_OK)
	{
		Uart_Printf(UART_LOG, "[slot] 写 A 数据区 ... 失败\r\n");
		return;
	}

	OtaSlot_BuildHeader(&hdr, 3U, 16U, crc, 0U, OTA_SLOT_FLAG_SELFTEST);

	st = OtaSlot_WriteHeader(OTA_SLOT_A, &hdr);
	if(st != OTA_SLOT_OK)
	{
		Uart_Printf(UART_LOG, "[slot] 写 A 头部 ... 失败(%d)\r\n", (int)st);
		return;
	}

	st = OtaSlot_ReadHeader(OTA_SLOT_A, &back);
	if(st != OTA_SLOT_OK)
	{
		/* 写完立刻读不回来，说明头部扇区擦/写有问题，或者两份代码布局不一致 */
		Uart_Printf(UART_LOG, "[slot] 读 A 头部 ... 失败(%d)\r\n", (int)st);
		return;
	}

	Uart_Printf(UART_LOG, "[slot] A 头部正常 版本=%u 长度=%lu CRC=0x%08lX\r\n",
				(unsigned)back.fw_ver,
				(unsigned long)back.fw_len,
				(unsigned long)back.fw_crc);

	/* 读回来重算数据 CRC —— 只有这一步能证明"落盘的就是收到的" */
	st = OtaSlot_VerifyData(OTA_SLOT_A, &back);
	Uart_Printf(UART_LOG, "[slot] A 数据 CRC ... %s\r\n",
				(st == 1) ? "一致" : ((st == 0) ? "不一致" : "读失败"));
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* 报到：告诉 BootLoader"这次真的进 main 了"。
     必须是 main 的第一条语句 —— 这条路过不去，说明固件连入口都没跑起来，
     Boot 下次上电就该把它算成一次试用失败。 */
  Bkp_ClearJumpMark();

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
  Uart_Printf(UART_LOG, "[app] 上电启动\r\n");

  /* 构建戳：OTA 系统里"现在跑的到底是哪一版"必须有一行能直接看出来。
     用 __DATE__/__TIME__ 而不是手写版本号，是因为手写的那个十有八九会
     忘记改 —— 一旦忘了，日志里新旧固件长得一模一样，
     排查"到底刷进去没有"就变成了猜。 */
  Uart_Printf(UART_LOG, "[app] 编译时间 " __DATE__ " " __TIME__ "\r\n");

  W25q_SelfTest();			/* W25Q128 通不通，看这一行打印 */

  /* 阶段 1 的自测已通过（Boot 能读出 App 写的头），现在关掉：
     它每次上电都会往槽 A 写一份 SELFTEST 假头、并擦两个扇区，
     留着只会制造无用擦写、还会把真实升级的现场搅乱。函数保留备用。 */
  /* OtaSlot_SelfTest(); */

  /* 启动"一直监听"的固件接收状态机。
     注意它不占串口独占权：只从 Uart 的 DMA 环形缓冲里取数据，
     有业务数据时照样能被业务代码读走（顺序取决于谁先调 Uart_Read） */
  AppOta_Init();

  /* 开狗。位置在所有初始化之后 —— 狗一开就开始倒计时，
     从这里到进主循环之间不允许再有任何超过 2 秒的操作。
     W25q_SelfTest 之所以留在前面，就是因为它的扇区擦除最长 45ms，
     放前面不受狗约束，放后面反而要额外算余量。
     超时和参数含义见 wdg.h 顶部说明。 */
  Wdg_Init();

  /* 把两个时间常量打出来。升级出问题时第一个要看的就是这两行：
     "狗到底开没开、开到多少秒"决定了卡死后板子会不会自己活过来，
     而"确认门限"决定了它到底算不算通过了试用期。 */
  Uart_Printf(UART_LOG, "[app] 看门狗 %ums，确认门限 %ums\r\n",
              (unsigned)WDG_TIMEOUT_MS, (unsigned)APP_CONFIRM_DELAY_MS);

  /* 试用确认挪进了主循环（见下面的 while(1)）。
     原来在这里直接调 AppOta_Confirm() 属于"乐观确认"：
     只要初始化跑完就签字，主循环里死了也不回滚。
     现在必须"稳定跑满 APP_CONFIRM_DELAY_MS 且一直在喂狗"才认。 */
  Uart_Printf(UART_LOG, "[app-ota] 监听中，槽 A 位于 0x%08lX\r\n",
              (unsigned long)OtaSlot_DataAddr(OTA_SLOT_A));
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* 主循环必须非阻塞：以前这里用 HAL_Delay(500) 闪灯，整个 500ms 里
       程序什么都不看，串口收到的字节全堆在 DMA 缓冲里，超过 200 字节的
       缓冲深度就会丢包。现在改成"到点才翻转"的写法，轮询间隔是微秒级。
       顺带一提，非阻塞也正好是看门狗能工作的前提：HAL_Delay 那种写法下
       一轮 500ms，狗虽然不会被误触发，但主循环卡住时也分辨不出来。 */
    static uint32_t t_led = 0U;
    static uint8_t  led_on = 0U;
    static uint32_t t_next_confirm = APP_CONFIRM_DELAY_MS;	/* 下次尝试确认的绝对时刻 */
    static uint8_t  confirmed = 0U;		/* 0=未确认 1=已确认 2=放弃 */
    static uint8_t  retry = 0U;
    uint32_t now;

    /* 喂狗放在循环体第一行：它必须在"任何可能阻塞的操作之前"执行，
       这样上一轮里最长的那次阻塞（OTA 接收时 45ms 的扇区擦除）才被
       完整地算进下一轮的 2 秒预算里。 */
    Wdg_Feed();

    now = Board_Millis();

    AppOta_Poll();			/* 只在有数据时才干活，没数据立刻返回 */

    /* 试用确认（实证版）：跑满 APP_CONFIRM_DELAY_MS 且期间一直喂狗，才签字。
       用"绝对时刻 + 有符号差值"比较而不是 Timer_Timeout，是为了让重试
       可以不重置计时器 —— 差值法在 32 位回绕（49.7 天）时依然正确。 */
    if(confirmed == 0U)
    {
      if((int32_t)(now - t_next_confirm) >= 0)
      {
        if(AppOta_Confirm() != 0U)
        {
          confirmed = 1U;			/* 内部已打印 self-confirm，这里不重复 */
        }
        else if(retry < APP_CONFIRM_RETRY_MAX)
        {
          retry++;
          t_next_confirm = now + APP_CONFIRM_RETRY_MS;
        }
        else
        {
          /* 走到这里说明确认标记一次都没写成功。固件本身是好的（狗还在被喂），
             所以不自杀、也不放弃运行，只是把状态留在 TRIAL ——
             下次上电 Boot 会多算一次试用失败，三次之后退回旧版。
             这比"一直在这里死循环等 W25Q 恢复"要安全。 */
          confirmed = 2U;
          Uart_Printf(UART_LOG, "[app] 确认放弃（重试 %u 次），保持试用态\r\n",
                      (unsigned)retry);
        }
      }
    }

    if((now - t_led) >= 500U)
    {
      t_led  = now;
      led_on = (uint8_t)((led_on != 0U) ? 0U : 1U);
      Led_Fan(led_on);
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
