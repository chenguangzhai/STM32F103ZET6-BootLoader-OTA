#include "Boot_Loader.h"
#include "usart.h"			/* huart1，以及 main.h → stm32f1xx_hal.h 里的 SysTick / NVIC / SCB */

/* ==================== App 有效性判据 ====================
 * 固件的头两个 32 位字是链接器写死的，不执行代码就能读：
 *   偏移 0x00：初始 SP（栈顶），必须落在 SRAM 里     → 0x2000xxxx
 *   偏移 0x04：复位向量，必须指向 Flash              → 0x08xxxxxx
 * 空白 Flash 是 0xFFFFFFFF，这两项都过不了，所以这个判断
 * 同时也挡住了"跳到一片空白区然后跑飞"。
 */
#define APP_SP_BASE		0x20000000U
#define APP_SP_MASK		0xFFFE0000U		/* 放宽到 128KB，足够覆盖 64KB RAM 的任意栈顶 */
#define APP_VEC_BASE	0x08000000U
#define APP_VEC_MASK	0xFF000000U


uint16_t Boot_Loader_Getlen(void)
{
    uint8_t  rx_buf[512] = {0};		/* 取数据用的落地缓冲 */
    uint16_t rx_len = 0;			/* 本帧累计收到的字节数 */
    if(Uart_FrameReady(UART_LOG) == 1)
    {
        rx_len = Uart_Read(UART_LOG, rx_buf, sizeof(rx_buf));
        if(rx_len > 0)
        {
            return rx_len;
        }
    }
    return 0;
}


/**
  * 函    数：判断指定地址处有没有一份能跑的固件
  * 参    数：app_addr 固件起始地址（应为页首，如 0x08008000）
  * 返 回 值：1 = 有效，0 = 无效
  * 说    明：只读两个 32 位字，不执行任何代码，所以对空白区也安全。
  */
uint8_t Boot_Loader_AppValid(uint32_t app_addr)
{
	uint32_t sp = *(volatile uint32_t *)(app_addr);
	uint32_t rv = *(volatile uint32_t *)(app_addr + 4U);

	if((sp & APP_SP_MASK) != APP_SP_BASE)		return 0U;	/* SP 不像 RAM 地址：空片或数据错位 */
	if((rv & APP_VEC_MASK) != APP_VEC_BASE)		return 0U;	/* 复位向量不在 Flash */
	if((rv & 0x1U) == 0U)						return 0U;	/* 缺 Thumb 位，跳过去第一个取指就 HardFault */

	return 1U;
}


/**
  * 函    数：跳转到 App 运行
  * 参    数：app_addr 固件起始地址
  * 说    明：正常不返回。
  *
  * 为什么跳之前要做这么多清理 —— 换位想一下：CPU 只是把 PC 指过去，
  * 但 bootloader 留下的东西全都还在：
  *   · SysTick 还在数数，跳过去 ms 级就会进 bootloader 的中断
  *   · USART1 / DMA1_Channel5 的中断使能还开着
  *   · DMA 是脱离 CPU 独立搬运的，光关中断拦不住它继续往内存写
  *   · VTOR 还指着 0x08000000
  *   · MSP 还是 bootloader 的栈
  * 少清一项，症状都是"跳过去能跑几秒，然后莫名其妙死"。
  */
void Boot_Loader_JumpToApp(uint32_t app_addr)
{
	uint32_t sp;
	uint32_t rv;
	void (*entry)(void);

	if(Boot_Loader_AppValid(app_addr) == 0U)
	{
		return;								/* 地址无效，什么都不动，让调用方自己处理 */
	}

	sp    = *(volatile uint32_t *)(app_addr);
	rv    = *(volatile uint32_t *)(app_addr + 4U);
	entry = (void (*)(void))rv;

	/* 1) 关总中断。从这行起，任何外设都插不进 CPU */
	__disable_irq();

	/* 2) 停 SysTick。不清它，App 里第一次 HAL_Delay 就会立刻超时，
	      因为 tick 带着 bootloader 的计数进来了 */
	SysTick->CTRL = 0U;
	SysTick->LOAD = 0U;
	SysTick->VAL  = 0U;

	/* 3) 关掉所有 NVIC 中断使能，并清掉挂起标志。
	      只关 SysTick 不够：USART1、DMA1_Channel5 的中断还挂着 */
	NVIC->ICER[0] = 0xFFFFFFFFU;
	NVIC->ICPR[0] = 0xFFFFFFFFU;

	/* 4) 停掉串口 DMA 接收。HAL_UART_AbortReceive 会顺带 Abort 掉
	      huart1 的接收 DMA，DMA 停了才不会继续往 s_port 的缓冲里灌数据 */
	HAL_UART_AbortReceive(&huart1);

	/* 5) 提前把 VTOR 指过去。App 的 main 里还会再设一次，
	      但那要执行到 main 才行 —— 在这之前如果来了异常，
	      没有这一句就会跑进 bootloader 的向量表 */
	SCB->VTOR = app_addr;

	/* 6) 换栈顶。这一步之后就不能再调用任何函数了 ——
	      栈已经变成 App 的，bootloader 的栈帧全失效 */
	__set_MSP(sp);

	/* 7) 开中断再跳。不开的话 App 的 SysTick 中断会一直挂着，
	      HAL_Delay 会死等 */
	__enable_irq();

	entry();

	/* 理论上到不了这里。真到了说明 App 一进去就异常返回了 */
	while(1)
	{
	}
}
