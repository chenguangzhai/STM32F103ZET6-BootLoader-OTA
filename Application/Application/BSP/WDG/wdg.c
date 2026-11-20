/**
 * IWDG 实现。设计说明见 wdg.h。
 *
 * 为什么不用 HAL_IWDG 驱动：
 *   本工程的 Drivers/STM32F1xx_HAL_Driver 是裁剪过的，只保留了实际用到的
 *   模块（gpio/spi/uart/flash/dma/rcc…），没有 iwdg。为看门狗补两个 HAL 文件
 *   （约 300 行 + 一堆 typedef）不划算 —— IWDG 是 STM32 上寄存器最少的
 *   外设之一，总共 4 个：KR / PR / RLR / SR，全部语义就是
 *   "解锁 → 配分频 → 配重装 → 等生效 → 启动 → 周期性重装"。
 *
 * 为什么也不放 Core/Src 里让 CubeMX 生成：
 *   生成区会被 .ioc 重写。本项目已经因为 spi.c 手写版与生成版同时参与编译
 *   踩过一次链接冲突，把狗放在 BSP 下可以彻底避开这条线。
 */
#include "wdg.h"
#include "stm32f1xx.h"		/* CMSIS 设备头：IWDG / RCC 寄存器定义，不拉 HAL */

/* IWDG_KR 的三个魔数。参考手册 RM0008 第 19 章，这三个值没有宏 */
#define IWDG_KEY_ENABLE     0xCCCCU		/* 启动看门狗（启动后无法关闭） */
#define IWDG_KEY_RELOAD     0xAAAAU		/* 重装计数器 = 喂狗 */
#define IWDG_KEY_ACCESS     0x5555U		/* 解锁 PR / RLR，允许写入 */

static uint8_t s_on;

void Wdg_Init(void)
{
	/* 0) 先手动打开 LSI 并等它稳定，放在 0xCCCC 之前。
	 *    IWDG 启动时硬件也会强制开 LSI，但"强制打开"到频率稳定有一段爬升期，
	 *    这段时间的计数周期不准。提前几十微秒等 LSIRDY，换一个确定的起点。 */
	RCC->CSR |= RCC_CSR_LSION;
	while((RCC->CSR & RCC_CSR_LSIRDY) == 0U)
	{
	}

	/* 1) 必须先启动（0xCCCC），再配置 PR/RLR。
	 *
	 * 这个顺序是踩坑换来的，写反了会死机：
	 *   只手动开 LSI 并等 LSIRDY，只证明"LSI 振荡了"，并没有把 LSI 时钟
	 *   路由给 IWDG 这个外设。此时写 PR/RLR 会让 SR 的 PVU/RVU 置位，
	 *   但 IWDG 没有时钟，更新永远完不成 → 卡在下面的 while 里。
	 *   现象很有迷惑性：日志停在 W25Q 自检最后一行，之后一片空白，
	 *   而且永远不会复位（看门狗根本没启动，没人咬它）。实测 PC 停在
	 *   等 SR 的循环里（0x08008f08），靠 SWD 读 PC 才定位到。
	 *
	 * 0xCCCC 之后到配置生效之间，狗按复位值 PR=0（4 分频）、
	 * RLR=0xFFF 在跑，超时约 0.41s。下面几行是纯寄存器写，微秒级完成。 */
	IWDG->KR = IWDG_KEY_ENABLE;

	/* 2) 解锁 PR / RLR。不解锁的话下面两步写进去会被硬件忽略，
	 *    表现是"狗开了但超时不是你要的那个值"，非常难查 */
	IWDG->KR = IWDG_KEY_ACCESS;

	/* 3) 分频 64（PR = 0b100）。周期 = (RLR + 1) × 64 / f_LSI
	 *    = 1251 × 64 / 40000 ≈ 2.0 s（LSI 30~60kHz 时实测 1.33~2.67s） */
	IWDG->PR  = 0x04U;
	IWDG->RLR = 1250U;

	/* 4) 等 PR / RLR 真正生效。
	 *    PVU（分频更新中）/ RVU（重装值更新中）清零才算写完；
	 *    不等就往下走，启动时用的可能还是旧值（复位值 PR=0 即 4 分频，
	 *    那会变成 0.125s 超时 —— 板子会疯狂复位，看起来像完全跑不起来） */
	while((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U)
	{
	}

	/* 5) 按新参数重装一次，把计数器拉回满值 */
	IWDG->KR = IWDG_KEY_RELOAD;

	s_on = 1U;
}

void Wdg_Feed(void)
{
	if(s_on != 0U)
	{
		IWDG->KR = IWDG_KEY_RELOAD;
	}
}

uint8_t Wdg_IsRunning(void)
{
	return s_on;
}
