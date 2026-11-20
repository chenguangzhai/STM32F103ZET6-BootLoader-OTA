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
#include <string.h>
#include "board.h"
#include "uart_app.h"
#include "boot_loader.h"
#include "ota_app.h"		/* 落盘逻辑都在这里，main 只负责把数据喂进去 */
#include "ota_param.h"		/* 参数区 + CRC 校验：判断 App 到底能不能跳 */
#include "w25q.h"			/* 外部 Flash：新固件先落脚在这里。spi.h 已被上方 CubeMX 生成区包含，勿重复 */
#include "ota_slot.h"		/* 双槽布局 + 头部校验：App 把固件写进槽里，Boot 从这里读 */
#include "ota_apply.h"		/* 阶段 3：把槽里的待装固件搬进内部 App 区 */
#include "ota_log.h"		/* 黑匣子：把启动/升级/回滚的关键决策落到 W25Q */
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

/* ==================== 复位原因快照 ====================
 * RCC->CSR 里的复位标志读一次就得清（不清的话下次读到的还是这一次的旧值），
 * 而"这次是怎么起来的"在 TrialGuard 里还要再用一次 —— 用来决定
 * "要不要给这一版记一次试用失败"。所以开机先抄进 RAM，后面谁问都能答。 */
#define RST_CAUSE_POR    0x01U      /* 上电 / 欠压 */
#define RST_CAUSE_PIN    0x02U      /* NRST 引脚：人按的复位键 */
#define RST_CAUSE_SFT    0x04U      /* 软件复位：App 收完固件主动回 Boot */
#define RST_CAUSE_IWDG   0x08U      /* 独立看门狗 */
#define RST_CAUSE_WWDG   0x10U      /* 窗口看门狗 */
#define RST_CAUSE_LPWR   0x20U      /* 低功耗唤醒 */

/* ==================== "跳出去没回来"标记 ====================
 * 放在 RTC 后备寄存器 BKP_DR1：不占 RAM 也不占 Flash，复位不掉，
 * 只有断电才丢（板上没装纽扣电池，断电即清 —— 正好当"没有证据"处理）。
 *
 * 用法：Boot 交控制权之前记一笔，App 进 main 的第一件事把它擦掉。
 * 下次上电这笔还在，就说明上次跳过去的那个 App 连 main 都没进 ——
 * 例如时钟配置死等、HardFault 卡在默认处理函数里，**这时候看门狗救不了**
 * （狗还没开），只能靠这个标记把它认出来。
 *
 * 为什么是 16 位：F1 的 BKP_DRx 实际只有低 16 位有效！写 32 位常量进去
 * 高位会被丢掉（踩过：写 0x4A4D5031，回读只有 0x5031，比较永远不相等，
 * 整条握手静默失效，日志上看不出任何异常）。所以魔术字必须是 16 位的。 */
#define BKP_JUMP_MAGIC   0xA55AU         /* 只占低 16 位，正好放得下 */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* 从串口取数据的临时区。放静态区：512 字节压栈太靠近 1KB 栈底。
   攒页缓冲已经搬进 ota_app.c，main 里不再需要 s_page。 */
static uint8_t s_rx[512];

/* 这次复位的原因（RST_CAUSE_xxx 的位或），ResetCause_Snapshot 填一次 */
static uint8_t s_rst_cause = 0U;
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
/**
  * 函    数：ResetCause_Snapshot
  * 功    能：把 RCC->CSR 里的复位标志抄进 RAM，然后清掉标志
  * 说    明：必须在 main 最开头调用，早于任何可能触发第二次复位的操作。
  *           抄而不是每次现读，是因为标志只能读一次 —— 清完就没了，
  *           而 TrialGuard 是几百毫秒后才需要这个信息。
  *
  * 怎么看结果（实测规律）：
  *     真正上电      → POR + PIN 两条（上电瞬间 NRST 引脚也会被拉低一次）
  *     只按复位键    → 只有 PIN 一条
  *     调试器 reset  → SFT（或 PIN，取决于调试器走哪条路）
  *     看门狗咬的    → IWDG
  */
static void ResetCause_Snapshot(void)
{
	uint8_t c = 0U;

	if(__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))  { c |= RST_CAUSE_POR;  }
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))  { c |= RST_CAUSE_PIN;  }
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))  { c |= RST_CAUSE_SFT;  }
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) { c |= RST_CAUSE_IWDG; }
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST)) { c |= RST_CAUSE_WWDG; }
	if(__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST)) { c |= RST_CAUSE_LPWR; }

	s_rst_cause = c;
	__HAL_RCC_CLEAR_RESET_FLAGS();		/* 清掉，下次复位才有干净的结果 */
}

/**
  * 函    数：ResetCause_Name
  * 功    能：把快照翻成一个词，日志里直接用
  * 说    明：判序有讲究 —— 真上电是 POR+PIN 两条，先说"上电"才符合直觉；
  *           看门狗优先于其它，因为它是唯一能证明"固件自己没撑住"的那个。
  */
static const char *ResetCause_Name(void)
{
	if((s_rst_cause & (RST_CAUSE_IWDG | RST_CAUSE_WWDG)) != 0U) { return "看门狗"; }
	if((s_rst_cause & RST_CAUSE_POR)  != 0U) { return "上电"; }
	if((s_rst_cause & RST_CAUSE_PIN)  != 0U) { return "复位键"; }
	if((s_rst_cause & RST_CAUSE_SFT)  != 0U) { return "软件复位"; }
	return "未知";
}

/**
  * 函    数：PrintResetCause
  * 功    能：把这次复位的原因打出来
  * 说    明：只打印，不读也不清标志 —— 那是 ResetCause_Snapshot 的活儿，
  *           在这里再读一次只会读到已经被清空的 0。
  */
static void PrintResetCause(void)
{
	if((s_rst_cause & RST_CAUSE_POR)  != 0U)
	{
		Uart_Printf(UART_LOG, "[rst] POR  / 上电或欠压复位\r\n");
	}
	if((s_rst_cause & RST_CAUSE_PIN)  != 0U)
	{
		Uart_Printf(UART_LOG, "[rst] PIN  / 复位引脚\r\n");
	}
	if((s_rst_cause & RST_CAUSE_SFT)  != 0U)
	{
		Uart_Printf(UART_LOG, "[rst] SFT  / 软件复位\r\n");
	}
	if((s_rst_cause & RST_CAUSE_IWDG) != 0U)
	{
		Uart_Printf(UART_LOG, "[rst] IWDG / 独立看门狗复位\r\n");
	}
	if((s_rst_cause & RST_CAUSE_WWDG) != 0U)
	{
		Uart_Printf(UART_LOG, "[rst] WWDG / 窗口看门狗复位\r\n");
	}
	if((s_rst_cause & RST_CAUSE_LPWR) != 0U)
	{
		Uart_Printf(UART_LOG, "[rst] LPWR / 低功耗唤醒\r\n");
	}
}

/* ==================== 后备寄存器握手 ====================
 * 为什么不用全局变量：Boot 和 App 是两个独立工程，各编各的链接脚本，
 * 一个普通的 .noinit 变量在两边地址未必相同，而且 App 的栈和它可能撞上。
 * BKP_DR1 是芯片自带的一块独立小存储，和 RAM/Flash 布局完全无关，
 * 两个工程只要约定同一个"魔术字"就能对话 —— 和 ota_slot.h 里那套
 * 双槽布局是同一个思路。
 *
 * 为什么直接写寄存器：工程 Drivers 是裁剪过的，没有 HAL 的 PWR/BKP 驱动，
 * 为了两三个位补一堆 HAL 代码不划算（和 WDG 那边同样的取舍）。 */
static void Bkp_Init(void)
{
	RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
	(void)RCC->APB1ENR;					/* 读回一次，确认时钟真的开了再动 BKP */
	PWR->CR      |= PWR_CR_DBP;			/* 解除后备区写保护 */
}

static void Bkp_MarkJump(void)
{
	BKP->DR1 = BKP_JUMP_MAGIC;			/* 交控制权了，App 还没回话 */
}

static uint8_t Bkp_JumpUnacked(void)
{
	return ((BKP->DR1 & 0xFFFFU) == BKP_JUMP_MAGIC) ? 1U : 0U;
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
		Uart_Printf(UART_LOG, "[boot] App 无效：向量表不对\r\n");
		return 0U;
	}

	st = Ota_Param_Load(&p);
	if(st == OTA_PARM_EMPTY)
	{
		Uart_Printf(UART_LOG, "[boot] 无参数页，App 未经验证\r\n");
		return 2U;
	}
	if(st != OTA_PARM_OK)
	{
		/* 参数页有内容却校验不过：多半是上次写参数时断了电 */
		Uart_Printf(UART_LOG, "[boot] 参数页损坏，停在 OTA\r\n");
		return 0U;
	}

	Uart_Printf(UART_LOG, "[boot] 参数：App=%luB CRC=0x%08lX\r\n",
				(unsigned long)p.length, (unsigned long)p.crc32);

	if(Ota_VerifyFlash(APP_ADDR, p.length, p.crc32) != 0)
	{
		/* 参数说该是多少，Flash 里却不是 —— 跳过去必跑飞 */
		Uart_Printf(UART_LOG, "[boot] CRC 不符，停在 OTA\r\n");
		return 0U;
	}

	Uart_Printf(UART_LOG, "[boot] CRC 校验通过\r\n");
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
	Uart_Printf(UART_LOG, "[ota] 尾部：长度=%lu CRC=0x%08lX\r\n",
				(unsigned long)tail->length, (unsigned long)tail->crc32);

	/* 先把 RAM 里攒着的最后一页写出去。不 Flush 的话 Flash 里缺结尾一段，
	   下面读回来算的 CRC 必然对不上 */
	if(Ota_Flush() != OTA_OK)
	{
		Uart_Printf(UART_LOG, "[ota] 落盘失败\r\n");
		return;
	}

	if(Ota_Total() != tail->length)
	{
		Uart_Printf(UART_LOG, "[ota] 长度不符：收到 %lu，应为 %lu\r\n",
					(unsigned long)Ota_Total(),
					(unsigned long)tail->length);
		return;
	}

	if(Ota_VerifyFlash(APP_ADDR, tail->length, tail->crc32) != 0)
	{
		Uart_Printf(UART_LOG, "[ota] 校验失败\r\n");
		return;
	}
	Uart_Printf(UART_LOG, "[ota] 校验通过 %luB\r\n",
				(unsigned long)tail->length);

	/* 参数最后写：它写成功之后，下次上电就以它为准了 */
	if(Ota_Param_Save(tail->length, tail->crc32, 0U) != OTA_PARM_OK)
	{
		Uart_Printf(UART_LOG, "[ota] 参数页保存失败\r\n");
		return;
	}

	Uart_Printf(UART_LOG, "[boot] 跳转到 App 0x%08lX\r\n",
				(unsigned long)APP_ADDR);
	Bkp_MarkJump();					/* 交控制权，等 App 进门后自己擦掉 */
	Boot_Loader_JumpToApp(APP_ADDR);
	Uart_Printf(UART_LOG, "[boot] 跳转中止\r\n");
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
					Uart_Printf(UART_LOG, "[ota] 尾部之前写入失败\r\n");
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
		Uart_Printf(UART_LOG, "[ota] 溢出：%lu > %lu\r\n",
					(unsigned long)(Ota_Total() + Ota_Fill() + len),
					(unsigned long)OTA_APP_MAX);
		return 1U;
	}

	if(Ota_Feed(buf, len) != OTA_OK)
	{
		Uart_Printf(UART_LOG, "[ota] 写 0x%08lX 失败，累计 %lu\r\n",
					(unsigned long)Ota_Addr(), (unsigned long)Ota_Total());
	}
	return 0U;
}

/* USER CODE END 0 */

/**
  * 函    数：TrialGuard
  * 功    能：试用计数 / 确认核对 / 超限回滚（阶段 4）
  * 返 回 值：1 = 不要跳 App（回滚失败，跳过去只会无限重启）
  *           0 = 正常，让后面的 CheckAppBoot 决定
  * 说    明：必须放在 OtaApply_FromSlot 之后、CheckAppBoot 之前。
  *           放前面：本次刚搬进来的固件还没被计过数，读到的是上一版状态。
  *           放后面：CheckAppBoot 已经决定能跳，回滚就来不及了。
  */
static uint8_t TrialGuard(void)
{
	OtaParam_t   p;
	OtaConfirm_t c;
	uint8_t      state;
	uint8_t      cnt;

	if(Ota_Param_Load(&p) != OTA_PARM_OK)
	{
		return 0U;			/* 没参数 = 老固件或空片，没有试用语义 */
	}

	state = Ota_Param_State(p.reserved);
	cnt   = Ota_Param_TrialCnt(p.reserved);

	/* 确认标记必须和"当前这一版"对得上才算数。只比 magic 不够：
	   升级之后标记还是上一版留下的，会立刻把新固件误判成"跑起来过" */
	if((OtaSlot_ReadConfirm(&c) == OTA_SLOT_OK)
	   && (c.fw_len == p.length) && (c.fw_crc == p.crc32))
	{
		if(state == OTA_PARM_STATE_TRIAL)
		{
			(void)Ota_Param_SetState(OTA_PARM_STATE_CONFIRMED, 0U);
			Uart_Printf(UART_LOG, "[trial] App 已确认 长度=%lu，试用转正\r\n",
						(unsigned long)p.length);
			/* 记一笔"这一版活下来了"。将来比对"装了几次才转正"能看出
			   某些固件是不是启动偏慢、总在门限边缘晃 */
			(void)OtaLog_Append(OTA_LOG_EVT_CONFIRM, p.version, p.length, 0U);
		}
		return 0U;
	}

	if(state != OTA_PARM_STATE_TRIAL)
	{
		return 0U;			/* 已确认（含老参数 0xF），什么都不用做 */
	}

	/* ---------- 试用机制的判定核心：什么才算"这一版没撑住" ----------
	 * 以前这里只看"状态是试用 + 没有确认标记"就直接 +1，不问复位原因。
	 * 后果很现实：新固件装好后，在 5 秒确认门限内连按三次复位键，
	 * Boot 会认定它三次都没撑住，把一份完全正常的固件退回旧版。
	 * 这种"假失败"比真失败更坑人 —— 日志里看不出任何异常。
	 *
	 * 能当证据的只有两种：
	 *   1) 看门狗复位：主循环没喂狗，铁证
	 *   2) BKP 标记还在：上次控制权交出去，App 连 main 都没进
	 *      （时钟死等、HardFault 卡在默认处理函数里，狗还没开就得靠它）
	 * 不算证据的一律放行，而且不写参数页（省一次 2KB 页擦写）：
	 *   PIN   人按的复位键
	 *   SFT   软件复位：App 收完固件主动回 Boot，属正常流程
	 *   POR   纯上电 / 插拔电源，用户行为，不是固件行为
	 */
	{
		uint8_t by_wdg = (uint8_t)((s_rst_cause & (RST_CAUSE_IWDG | RST_CAUSE_WWDG)) != 0U);
		uint8_t by_app = Bkp_JumpUnacked();

		if((by_wdg == 0U) && (by_app == 0U))
		{
			Uart_Printf(UART_LOG, "[trial] 本次是%s复位，不计试用失败（%u/%u）\r\n",
						ResetCause_Name(), (unsigned)cnt,
						(unsigned)OTA_PARM_TRIAL_MAX);
			return 0U;
		}

		Uart_Printf(UART_LOG, "[trial] 判定失败：%s\r\n",
					(by_wdg != 0U) ? "看门狗复位" : "上次跳转后没进 App 的 main");

		/* 两种失败原因分开记：看门狗说明固件进了主循环才死，
		   "跳转未响应"说明连 main 都没活到。排查方向完全不同 */
		if(by_wdg != 0U)
		{
			(void)OtaLog_Append(OTA_LOG_EVT_TRIAL_FAIL, p.version, p.length, cnt);
		}
		else
		{
			(void)OtaLog_Append(OTA_LOG_EVT_STUCK, p.version, p.length, cnt);
		}
	}

	cnt = (uint8_t)(cnt + 1U);

	if(cnt >= OTA_PARM_TRIAL_MAX)
	{
		Uart_Printf(UART_LOG, "[trial] 连续 %u 次没等到确认，开始回滚\r\n",
					(unsigned)cnt);

		if(OtaApply_RestoreFromB() == OTA_APPLY_OK)
		{
			return 0U;		/* 回滚成功，参数页已写成"已确认"，可以跳 */
		}

		/* 回滚不了就别跳：这份固件已经证明自己跑不起来，跳过去只会
		   让人以为"一上电就重启"，比停在串口更难查 */
		Uart_Printf(UART_LOG, "[trial] 回滚不可用，停在 bootloader 等重传\r\n");
		return 1U;
	}

	/* 计数必须落盘。只在 RAM 里加的话每次上电都从 0 开始，永远到不了上限，
	   坏固件就会无限重启 —— 这正是要防的那个故障 */
	if(Ota_Param_SetState(OTA_PARM_STATE_TRIAL, cnt) == OTA_PARM_OK)
	{
		Uart_Printf(UART_LOG, "[trial] 等 App 确认 %u/%u\r\n",
					(unsigned)cnt, (unsigned)OTA_PARM_TRIAL_MAX);
		(void)OtaLog_Append(OTA_LOG_EVT_TRIAL, p.version, p.length, (uint32_t)cnt);
	}

	return 0U;
}


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

  /* 第一件事：把复位原因抄下来。必须在任何可能引发复位或清标志的操作之前
     —— RCC->CSR 的复位标志只能读一次，读晚了就被下一条复位覆盖了 */
  ResetCause_Snapshot();

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
  // Led_Fan(1);

  PrintResetCause();				/* 这次是被什么复位起来的：POR / PIN / SFT / IWDG */
  Bkp_Init();						/* 后备寄存器：和 App 对"跳过去有没有活过来"的暗号 */

	/* ---------- 阶段 0 自检：Boot 到底能不能碰到外部 Flash ----------
	   OTA 新方案是「App 下载固件到 W25Q → 复位 → Boot 搬进内部 Flash」，
	   所以 Boot 自己必须能驱动 SPI2 和 W25Q。这一步不通，后面所有搬运逻辑
	   都是空中楼阁，所以放在最前面打一行 ID：看到 0xEF4018 就说明链路通了。

	   失败时优先查三处（按踩坑概率排序）：
	     1. HAL_SPI_MODULE_ENABLED 没打开 —— HAL_SPI_Init 变成空函数，读回全 0xFF
	     2. PB12 片选被别的外设抢了，或者 GPIOB 时钟没开
	     3. SPI2 引脚被 CubeMX 配成了别的功能 */
	{
		uint32_t w25q_id = 0xFFFFFFFFUL;

		/* 片选用 CubeMX 生成的宏（main.h: W25Q128_CS_GPIO_Port/Pin），不再写死 GPIOB/PIN_12：
		   PB12 现在由 gpio.c 统一配置，以后在 CubeMX 里换片选脚，这里会自动跟随，
		   避免「.ioc 改了、代码还指着老引脚」这种不报错的静默不一致 */
		if(W25q_Init(&hspi2, W25Q128_CS_GPIO_Port, W25Q128_CS_Pin) == W25Q_OK)
		{
			w25q_id = W25q_ReadId();
		}
		Uart_Printf(UART_LOG, "[boot] W25Q 芯片ID=0x%06lX %s\r\n",
					(unsigned long)w25q_id,
					(w25q_id == W25Q_JEDEC_ID) ? "正常" : "异常，应为 0xEF4018");
	}

	/* ---------- 黑匣子：先把"上次及更早发生了什么"倒出来 ----------
	   放在槽扫描和搬运之前。下面几步自己会往日志里追加记录，
	   先打印再追加，看到的就是"本次上电之前"的历史，不会和本次混在一起。

	   只打最近 5 条 —— 多了会淹没正常的启动日志，而真正要查的问题
	   几乎总在最上面几条里。要看全量改这个数字即可（上限 128）。 */
	(void)OtaLog_DumpRecent(5U);

	/* 记下这次是怎么起来的 */
	{
		uint32_t ev;

		if((s_rst_cause & RST_CAUSE_IWDG) != 0U)     { ev = OTA_LOG_EVT_RST_IWDG;  }
		else if((s_rst_cause & RST_CAUSE_PIN) != 0U) { ev = OTA_LOG_EVT_RST_PIN;   }
		else if((s_rst_cause & RST_CAUSE_SFT) != 0U) { ev = OTA_LOG_EVT_RST_SFT;   }
		else if((s_rst_cause & RST_CAUSE_POR) != 0U) { ev = OTA_LOG_EVT_RST_POR;   }
		else                                         { ev = OTA_LOG_EVT_RST_OTHER; }

		(void)OtaLog_Append(ev, 0U, 0U, (uint32_t)s_rst_cause);
	}

	/* ---------- 阶段 1 自检：双槽头部读得出来吗 ----------
	   槽 A/B 各占 W25Q 前 1MB 里的 512KB：头 4KB + 数据 508KB。
	   头部独占扇区的理由见 ota_slot.h —— 它保证写固件时永远碰不到头。

	   这里只读不写，所以刷完固件上电看到的就是槽里真实状态：
	     empty  = 这个槽从没被写过（出厂状态）
	     bad    = 有内容但对不上：magic / 结构版本 / 头 CRC / 长度 任一项挂了
	     ok     = 头部可信，里面确实标记着一份长度合法的固件 */
	{
		static const uint8_t s_slots[2] = { OTA_SLOT_A, OTA_SLOT_B };

		for(uint8_t i = 0U; i < 2U; i++)
		{
			OtaSlotHeader_t hdr;
			const char     *name = (s_slots[i] == OTA_SLOT_A) ? "A" : "B";
			int8_t          st   = OtaSlot_ReadHeader(s_slots[i], &hdr);

			if(st == OTA_SLOT_OK)
			{
				Uart_Printf(UART_LOG, "[slot] %s：正常 版本=%u 长度=%lu CRC=0x%08lX\r\n",
							name,
							(unsigned)hdr.fw_ver,
							(unsigned long)hdr.fw_len,
							(unsigned long)hdr.fw_crc);
			}
			else if(st == OTA_SLOT_EMPTY)
			{
				Uart_Printf(UART_LOG, "[slot] %s：空\r\n", name);
			}
			else if(st == OTA_SLOT_BAD)
			{
				/* 判 BAD 只说明"这 32 字节不构成合法头部"，但完全没说它是
				   空白以外的什么。把原始字节打出来，才能区分三种常见情况：
				     全 0x00       → MISO 一直低，SPI 根本没读到东西
				     乱码/部分数据 → 这个槽以前被别的程序用过，需要先擦
				     像头部但 CRC 错 → 两份 ota_slot.h 布局不一致，或写一半断电 */
				const uint8_t *raw = (const uint8_t *)&hdr;
				uint8_t k;
				uint8_t m;

				Uart_Printf(UART_LOG, "[slot] %s：不可信（标识/版本/CRC/长度）\r\n", name);

				/* 16 字节一行：一行 3*16+13 = 61 字符，不会撞上 Uart_Printf 的 100 字节缓冲 */
				for(k = 0U; k < (uint8_t)sizeof(hdr); k = (uint8_t)(k + 16U))
				{
					Uart_Printf(UART_LOG, "[slot]   +%02X:", (unsigned)k);
					for(m = 0U; (m < 16U) && ((uint8_t)(k + m) < (uint8_t)sizeof(hdr)); m++)
					{
						Uart_Printf(UART_LOG, " %02X", raw[k + m]);
					}
					Uart_Printf(UART_LOG, "\r\n");
				}
			}
			else
			{
				Uart_Printf(UART_LOG, "[slot] %s：读取错误\r\n", name);
			}
		}
	}

	/* ---------- 阶段 3：把槽里的待装固件搬进内部 Flash ----------
	   放在"上电窗口"之前，是因为搬运完成之后参数页才是最新的，
	   后面 CheckAppBoot() 才能一次性判断"这份固件能不能跳"。
	   如果放到跳转判断之后，就会先判"参数对不上 → 不可跳"，
	   然后才搬，逻辑上永远慢一拍。

	   这一步可能耗时几百毫秒到十几秒（看固件大小），期间串口会卡住，
	   属于正常现象：F103 单 bank，擦写 Flash 时 CPU 停止取指。 */
	(void)OtaApply_FromSlot(OTA_SLOT_A);

	/* 参数页丢失后的自愈：账本（参数页）被整片擦除带走过一次，
	   表现是"板子还能跑，但每次上电都说未经验证"，试用和回滚
	   静默失效。真相在槽头和固件本身，重新对一次账就能恢复。
	   放在 FromSlot 之后：刚搬完的固件已经写好参数页，这里不会误动 */
	(void)OtaApply_RecoverParam();

	/* 试用期检查（阶段 4）：刚搬进来的固件还没被 App 亲口确认过。
	   这里决定"再给一次机会（计数+1）"还是"退回上一版"。
	   返回值 1 表示回滚失败、不能跳，下面会跳过跳转直接进收固件循环 */
	uint8_t trial_block = TrialGuard();

	uint16_t len;						/* 这次从串口取到多少字节 */
	uint32_t last_rx  = Board_Millis();	/* 最后一次收到数据的时刻 */
	uint32_t t_boot   = Board_Millis();	/* 上电时刻，用来算等待窗口 */
	uint8_t  ota_mode = 0U;				/* 1 = 已经决定留在 bootloader 收固件 */

	Ota_Init(APP_ADDR);					/* 写入起点：App 区第 16 页开头，同时清空内部状态 */

	/* proto=2：告诉上位机"我支持尾部记录 + CRC 校验，发完请补 12 字节尾部"。
	   老版本没有这个字段，上位机会自动退回"等 sum 然后复位"的老流程，
	   所以新工具配旧 bootloader 也不会多发字节。 */
	Uart_Printf(UART_LOG, "\r\n[ota] 就绪，App=0x%08lX，页大小=%luB，协议=2\r\n",
				(unsigned long)APP_ADDR, (unsigned long)OTA_PAGE_BYTES);

	/* ---------- 上电窗口 ----------
	   App 校验通过时等 OTA_WAIT_MS 毫秒：这期间上位机发数据就留在 bootloader
	   升级，一直没数据就跳过去跑 App。
	   校验不过（空片 / 参数坏 / CRC 错）就不用等 —— 反正没得跳，直接进下面的循环。

	   注意这里比原来严了：以前只看向量表"像不像样"，现在要求 Flash 读回来
	   算的 CRC 和参数里记的一致。宁可停在 OTA 等重传，也不跳进坏固件。 */
	{
		uint8_t boot_st = CheckAppBoot();

		/* ---------- 搬运中途断电的自救 ----------
		   掉电发生在"旧固件已擦、新固件写一半"那一刻时：参数页还是旧的
		   （它要等校验通过才更新），所以 CheckAppBoot 必然失败，
		   而槽 B 里恰好躺着上一版完整固件。
		   以前这种情况只能插串口重传；现在直接搬回来，设备自己就活了。

		   只在 boot_st == 0（真的坏了）时触发：老固件走的 2 不能碰，
		   那是"能用只是没参数"的正常状态。 */
		if((boot_st == 0U) && (trial_block == 0U))
		{
			OtaSlotHeader_t hb;

			if(OtaSlot_ReadHeader(OTA_SLOT_B, &hb) == OTA_SLOT_OK)
			{
				Uart_Printf(UART_LOG, "[boot] 内部 Flash 校验不过，用槽 B 自救\r\n");

				if(OtaApply_RestoreFromB() == OTA_APPLY_OK)
				{
					boot_st = CheckAppBoot();	/* 搬完再判一次，通过就正常跳 */
				}
			}
		}

		if(trial_block != 0U)					/* 回滚失败：绝不跳 */
		{
			/* 试用失败且没有可回滚的备份：不跳。跳到一份已经证明
			   跑不起来的固件，现场只会看到"一直在重启" */
			Uart_Printf(UART_LOG, "[boot] 固件未通过试用，等待串口重传\r\n");
		}
		else if(boot_st != 0U)					/* 1 = 校验通过；2 = 老固件，没参数 */
		{
			if(boot_st == 2U)
			{
				Uart_Printf(UART_LOG, "[boot] 兼容模式：未经验证\r\n");
			}

			while((ota_mode == 0U) && (Timer_Timeout(&t_boot, OTA_WAIT_MS) == 0U))
			{
				len = Uart_Read(UART_LOG, s_rx, sizeof(s_rx));
				if(len > 0U)
				{
					last_rx  = Board_Millis();
					ota_mode = 1U;
					Uart_Printf(UART_LOG, "[boot] 进入 OTA\r\n");
					/* arg 记收到多少字节：偶尔会有"上电就被外部噪声触发进 OTA"
					   的现象，一看字节数（1~2 字节）就能判断是噪声而不是上位机 */
					(void)OtaLog_Append(OTA_LOG_EVT_OTA_MODE, 0U, 0U, (uint32_t)len);
					HandleRx(s_rx, len);	/* 这一包别丢；万一是尾部也能认出来 */
				}
			}

			if(ota_mode == 0U)				/* 窗口内没人理，跳 App */
			{
				Uart_Printf(UART_LOG, "[boot] 跳转到 App 0x%08lX\r\n", (unsigned long)APP_ADDR);
				Bkp_MarkJump();				/* 交控制权，等 App 进门后自己擦掉 */
				Boot_Loader_JumpToApp(APP_ADDR);
				Uart_Printf(UART_LOG, "[boot] 跳转中止\r\n");
			}
		}
		else
		{
			Uart_Printf(UART_LOG, "[boot] 没有可用 App，等待串口下载\r\n");
			/* 这条经常和"回滚不可用"连着出现，是设备停在串口的直接证据 */
			(void)OtaLog_Append(OTA_LOG_EVT_NOAPP, 0U, 0U, 0U);
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
