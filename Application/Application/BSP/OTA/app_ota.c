/**
 * App 侧固件接收器实现。设计说明见 app_ota.h。
 *
 * 关键约束（写代码时最容易踩的两条）：
 *   1) 写 W25Q 必须以 256 字节页为单位对齐；跨页写会被芯片回卷覆盖页首
 *   2) CRC 必须算在"真正发给 Flash 的那些字节"上，而不是算在串口收到的
 *      缓冲区上 —— 两者长度可能不同（分块边界不一样），一旦错位就永远对不上
 */
#include "app_ota.h"
#include "ota_slot.h"
#include "ota_param.h"			/* 只借类型与地址常量：App 不链接它的 .c，写不了参数页 */
#include "w25q.h"
#include "crc32.h"
#include "uart_app.h"
#include "board.h"

#include "stm32f1xx_hal.h"		/* NVIC_SystemReset / HAL_Delay */
#include <stddef.h>				/* offsetof */
#include <string.h>				/* memcpy / memcmp */

/* 小于这个长度的一律不当固件：真实固件至少几 KB，
   留 1KB 是给"误识别成一个极短固件、把槽擦掉"这种情况兜底 */
#define APP_OTA_MIN_LEN		1024UL

/* 传输中途静默超过这个时间就放弃。为什么需要：串口线被拔、上位机崩了、
   用户中途关窗口 —— 不设超时的话状态机会一直卡在 DATA，
   把之后所有的业务数据都当成固件字节往 Flash 里写 */
#define APP_OTA_TIMEOUT_MS	3000UL

/* 状态 */
#define ST_IDLE			0U
#define ST_HEADER		1U
#define ST_DATA			2U

/* 一次从串口取多少。64 字节够小，主循环响应快；也足够摊薄调用开销 */
#define RX_CHUNK		64U

static struct
{
	uint8_t  state;

	/* IDLE 状态下用 4 字节滑动窗口找 'FWUP'。
	   用窗口而不是直接比较，是因为标识可能被拆在两个读周期里 */
	uint8_t  win[4];

	/* 头部缓冲。收满 32 字节后才解析，避免边收边解析导致半成品被判有效 */
	uint8_t  hdr[sizeof(OtaSlotHeader_t)];
	uint16_t hdr_fill;

	uint32_t crc;			/* 边收边算，最终和头部里的 fw_crc 比 */
	uint32_t left;			/* 还差多少字节没收到 */
	uint32_t addr;			/* 下一个要写的 W25Q 地址 */
	uint32_t erased_end;	/* 已经擦到哪个地址（不含），惰性擦除用 */

	uint8_t  page[W25Q_PAGE_SIZE];	/* 攒满一页再写 */
	uint16_t page_fill;

	uint32_t last_act;		/* 最后一次收到数据的时刻，用于超时判断 */
} s_rx;

/* 把攒够的数据写进 Flash，并把这段算进 CRC。
 * 返回非 0 表示写失败，调用方负责中止 */
static int8_t FlushPage(uint16_t n)
{
	if(n == 0U)
	{
		return W25Q_OK;
	}

	/* 惰性擦除：只擦"这一页要落进去的那个扇区"，而不是开始接收时一次性擦光。
	   为什么必须这样 —— 算一下就知道：
	     W25Q 单扇区擦除典型 45ms，17KB 固件 = 5 个扇区 = 225ms；
	     115200 波特率约 11.5KB/s，225ms 会进来约 2600 字节，
	     而串口 DMA 环形缓冲只有 1024 字节 → 必然溢出丢包 → 最后 CRC 永远对不上。
	   拆成"写之前才擦"之后，单次阻塞约 45ms（约 520 字节），缓冲装得下。
	   固件越大收益越明显：476KB 固件按老写法要连续阻塞十几秒，必挂。 */
	while((s_rx.addr + (uint32_t)n) > s_rx.erased_end)
	{
		if(W25q_EraseSector(s_rx.erased_end) != W25Q_OK)
		{
			return W25Q_ERR_SPI;
		}
		s_rx.erased_end += OTA_SLOT_HDR_BYTES;
	}

	if(W25q_Write(s_rx.addr, s_rx.page, n) != W25Q_OK)
	{
		return W25Q_ERR_SPI;
	}

	/* CRC 在"写成功之后"才算 —— 写失败了这串字节根本不在 Flash 里，
	   算进去只会让最后的比对给出一个更迷惑的结果 */
	s_rx.crc = Crc32_Update(s_rx.crc, s_rx.page, n);
	s_rx.addr += n;
	s_rx.page_fill = 0;

	return W25Q_OK;
}

static void AbortRx(const char *why)
{
	s_rx.state = ST_IDLE;
	s_rx.hdr_fill = 0U;
	s_rx.page_fill = 0U;
	Uart_Printf(UART_LOG, "[app-ota] 中止：%s\r\n", why);
}

/* 头部通过校验，开始正式接收数据：先作废旧头，再擦数据区 */
static void StartData(const OtaSlotHeader_t *h)
{
	uint32_t data_addr = OtaSlot_DataAddr(OTA_SLOT_A);
	uint32_t sectors;

	if(data_addr == 0xFFFFFFFFUL)
	{
		AbortRx("槽地址非法");
		return;
	}

	/* 先擦头再擦数据。顺序反了就会出现一个危险窗口：
	   数据已经擦掉、旧头还在，此刻断电 → 下次上电 Boot 看到
	   "长度合法的有效头"，但数据已经没了 → 搬运进内部 Flash 一堆 0xFF */
	(void)OtaSlot_EraseHeader(OTA_SLOT_A);

	sectors = (h->fw_len + OTA_SLOT_HDR_BYTES - 1UL) / OTA_SLOT_HDR_BYTES;

	s_rx.crc        = CRC32_INIT;
	s_rx.addr       = data_addr;
	s_rx.erased_end = data_addr;	/* 数据扇区留到真正要写之前才擦（见 FlushPage） */
	s_rx.left       = h->fw_len;
	s_rx.page_fill  = 0U;
	s_rx.state      = ST_DATA;

	Uart_Printf(UART_LOG, "[app-ota] 开始接收 版本=%u 长度=%luB（%lu 个扇区）\r\n",
				(unsigned)h->fw_ver,
				(unsigned long)h->fw_len,
				(unsigned long)sectors);
}

/* 数据收齐：校验 CRC → 写头 → 复位 */
static void FinishRx(void)
{
	OtaSlotHeader_t h;
	uint32_t calc;

	if(s_rx.page_fill > 0U)
	{
		if(FlushPage(s_rx.page_fill) != W25Q_OK)
		{
			AbortRx("写尾部失败");
			return;
		}
	}

	memcpy(&h, s_rx.hdr, sizeof(h));

	/* Crc32_Update 不做最终取反，和 Crc32_Buf / 上位机 zlib.crc32 的约定对齐 */
	calc = ~s_rx.crc;

	if(calc != h.fw_crc)
	{
		Uart_Printf(UART_LOG, "[app-ota] CRC 不符：实得=0x%08lX 期望=0x%08lX\r\n",
					(unsigned long)calc, (unsigned long)h.fw_crc);
		/* 头部在 StartData 时已经作废，所以这里什么都不用做 ——
		   Boot 上电只会看到空槽，不会去搬一份错的固件 */
		s_rx.state = ST_IDLE;
		return;
	}

	/* 在复位前，先把"当前这一版"确认掉。
	 *
	 * 为什么需要这一步（5 秒确认门限带来的副作用）：
	 *   主循环要求"稳定跑满 5 秒"才确认，那么"升级成功后 5 秒内又发一版新固件"
	 *   就会带着 TRIAL 状态复位 —— 已经装上的这版其实完全正常，却会被 Boot
	 *   记一次试用失败。连续快速刷三次，板子就会莫名其妙回滚到旧版。
	 *
	 * 为什么这时候确认是有依据的（不是又退回"乐观确认"）：
	 *   能走到这一行，意味着这版固件完整跑通了一整条链路 —— 主循环没卡死、
	 *   UART DMA 没丢包、W25Q 连续擦了 5 个扇区写了 18180 字节、CRC32 与
	 *   上位机逐位相同。这个证据强度比"空跑 5 秒"高得多。
	 *
	 * 注意它确认的是参数页里那一版（正在运行的），不是刚收到的这一版；
	 * 新固件装上去之后 Boot 会重新置 TRIAL，试用期照样从零开始。 */
	(void)AppOta_Confirm();

	if(OtaSlot_WriteHeader(OTA_SLOT_A, &h) != OTA_SLOT_OK)
	{
		AbortRx("写头部失败");
		return;
	}

	Uart_Printf(UART_LOG, "[app-ota] 接收完成 %luB，CRC 通过，头部已写入\r\n",
				(unsigned long)h.fw_len);
	Uart_Printf(UART_LOG, "[app-ota] 复位回 BootLoader ...\r\n");

	/* 复位前留一点时间把上面两行发完。Boot 那次踩过"发送被复位打断"的坑，
	   串口在 115200 下发 80 字节约 7ms，100ms 足够 */
	HAL_Delay(100);

	/* 软复位回 BootLoader。用户不需要去按复位键 —— 这正是"App 升级"该有的样子 */
	NVIC_SystemReset();
}

static void FeedByte(uint8_t b)
{
	if(s_rx.state == ST_IDLE)
	{
		/* 4 字节滑动窗口找标识 */
		s_rx.win[0] = s_rx.win[1];
		s_rx.win[1] = s_rx.win[2];
		s_rx.win[2] = s_rx.win[3];
		s_rx.win[3] = b;

		if(memcmp(s_rx.win, "FWUP", 4U) == 0)
		{
			memcpy(s_rx.hdr, s_rx.win, 4U);
			s_rx.hdr_fill = 4U;
			s_rx.state    = ST_HEADER;
		}
		return;
	}

	if(s_rx.state == ST_HEADER)
	{
		s_rx.hdr[s_rx.hdr_fill] = b;
		s_rx.hdr_fill++;

		if(s_rx.hdr_fill < (uint16_t)sizeof(OtaSlotHeader_t))
		{
			return;
		}

		{
			OtaSlotHeader_t h;

			memcpy(&h, s_rx.hdr, sizeof(h));

			if(h.magic != OTA_SLOT_HDR_MAGIC)
			{
				AbortRx("固件标识不符");
			}
			else if(Crc32_Buf(s_rx.hdr, (uint32_t)offsetof(OtaSlotHeader_t, hdr_crc)) != h.hdr_crc)
			{
				/* CRC 放在版本号之前验，是为了让误判的诊断准确：
				   固件镜像里本身就含 "FWUP" 这 4 个 ASCII 字节（就是本文件的
				   memcmp 常量），所以只要在"已经放弃本次接收、但仍然在扫流"
				   的状态下扫到那 4 个字节，就能拼出一个 magic 正确的假头部，
				   接着必然报一个看起来很吓人的 "hdr ver"。
				   先算 CRC 的话，这种假头会被准确地归为 hdr crc，不会误导排查方向 ——
				   而且它只有 24 字节的 CRC32 计算量，比误判后重新走一遍流程便宜。 */
				AbortRx("头部 CRC 不符");
			}
			else if(h.hdr_ver != (uint16_t)OTA_SLOT_HDR_VER)
			{
				AbortRx("头部版本不支持");
			}
			else if((h.fw_len < APP_OTA_MIN_LEN) || (h.fw_len > OTA_SLOT_DATA_MAX))
			{
				AbortRx("长度越界");
			}
			else if((h.flags & OTA_SLOT_FLAG_SELFTEST) != 0UL)
			{
				/* 自测假头不允许从串口进来：一旦生效，Boot 会把
				   16 字节垃圾当固件搬到内部 Flash 然后跳过去跑飞 */
				AbortRx("自测标志未清");
			}
			else
			{
				StartData(&h);
			}
		}
		return;
	}

	if(s_rx.state == ST_DATA)
	{
		if(s_rx.left == 0UL)
		{
			return;
		}

		s_rx.page[s_rx.page_fill] = b;
		s_rx.page_fill++;
		s_rx.left--;

		if(s_rx.page_fill >= (uint16_t)W25Q_PAGE_SIZE)
		{
			if(FlushPage((uint16_t)W25Q_PAGE_SIZE) != W25Q_OK)
			{
				AbortRx("写 Flash 失败");
				return;
			}
		}

		if(s_rx.left == 0UL)
		{
			FinishRx();
		}
	}
}

void AppOta_Init(void)
{
	memset(&s_rx, 0, sizeof(s_rx));
	s_rx.state    = ST_IDLE;
	s_rx.last_act = Board_Millis();
}

/* ==================== 读参数页（双备份） ====================
 *
 * App 不写参数页，但必须能**读懂**它 —— 确认标记里写的 len/crc 要和参数页
 * 完全一致才算数，所以读错了就等于永远确认不了。
 *
 * 参数页有两页轮转，还要认双备份之前的老格式（24 字节、self_crc 在偏移 20）。
 * 这段逻辑和 BootLoader 侧 ota_param.c 的 Page_Load / LoadBest 是同一套规则，
 * 改一边必须同步另一边。
 */

/* 读一页。返回 1 = 有效（*seq 带回序号），0 = 空 / 坏 */
static uint8_t ReadParamPage(uint32_t addr, OtaParam_t *p, uint32_t *seq)
{
	const uint8_t *raw = (const uint8_t *)p;
	uint32_t i;
	uint32_t legacy_crc;
	uint8_t  blank = 1U;

	for(i = 0U; i < (uint32_t)sizeof(OtaParam_t); i++)
	{
		((uint8_t *)p)[i] = *(volatile const uint8_t *)(addr + i);
		if(((const uint8_t *)p)[i] != 0xFFU)
		{
			blank = 0U;
		}
	}

	if((blank != 0U) || (p->magic != OTA_PARAM_MAGIC) || (p->length == 0UL))
	{
		return 0U;
	}

	/* 新格式：self_crc 在末尾，覆盖前 24 字节 */
	if(p->self_crc == Crc32_Buf(raw, (uint32_t)sizeof(OtaParam_t) - 4U))
	{
		*seq = p->seq;
		return 1U;
	}

	/* 老格式：偏移 20 那个值是老 self_crc，覆盖前 20 字节。
	   此时 p->seq 读到的是垃圾，必须清掉，否则两页比大小时会选错 */
	legacy_crc = (uint32_t)raw[20]
	           | ((uint32_t)raw[21] << 8)
	           | ((uint32_t)raw[22] << 16)
	           | ((uint32_t)raw[23] << 24);
	if(legacy_crc == Crc32_Buf(raw, 20U))
	{
		p->seq = 0U;
		*seq   = 0U;
		return 1U;
	}

	return 0U;
}

/* 两页里取序号最新的那条。返回 1 = 有可用参数并已写入 *p */
static uint8_t ReadParamBest(OtaParam_t *p)
{
	OtaParam_t pa;
	OtaParam_t pb;
	uint32_t   sa = 0U;
	uint32_t   sb = 0U;
	uint8_t    va;
	uint8_t    vb;

	va = ReadParamPage(OTA_PARAM_ADDR_A, &pa, &sa);
	vb = ReadParamPage(OTA_PARAM_ADDR_B, &pb, &sb);

	if((va != 0U) && (vb != 0U))
	{
		*p = (sa >= sb) ? pa : pb;		/* 两页都有效，取新的一页 */
		return 1U;
	}
	if(va != 0U)
	{
		*p = pa;
		return 1U;
	}
	if(vb != 0U)
	{
		*p = pb;
		return 1U;
	}
	return 0U;
}

uint8_t AppOta_Confirm(void)
{
	OtaParam_t   p;
	OtaConfirm_t c;

	/* 参数页按内存映射直接读，不擦不写 —— App 手里没有 flash_app.c，
	   就算想写也写不了。这是刻意保留的边界：参数页的写权只在 Boot 手里，
	   App 有 bug 也改不坏"能不能跳"的判断。 */
	if(ReadParamBest(&p) == 0U)
	{
		/* 老固件（没有参数页）或者参数页从没写过：没有"向谁确认"这回事。
		   静默返回，不打印 —— 这是正常状态，不是故障 */
		return 0U;
	}

	if((OtaSlot_ReadConfirm(&c) == OTA_SLOT_OK)
	   && (c.fw_len == p.length) && (c.fw_crc == p.crc32))
	{
		return 1U;			/* 已经确认过这一版，别重复擦扇区 */
	}

	/* 只写一次。Boot 下次上电看到它和自己参数页里的 len/crc 一致，
	   就把状态从"试用中"改成"已确认" */
	if(OtaSlot_WriteConfirm(p.length, p.crc32) != OTA_SLOT_OK)
	{
		Uart_Printf(UART_LOG, "[app-ota] 确认标记写入失败\r\n");
		return 0U;
	}

	Uart_Printf(UART_LOG, "[app-ota] 自确认 长度=%lu CRC=0x%08lX\r\n",
	            (unsigned long)p.length, (unsigned long)p.crc32);
	return 1U;
}

void AppOta_Poll(void)
{
	static uint8_t buf[RX_CHUNK];
	uint16_t n;
	uint16_t i;

	n = Uart_Read(UART_LOG, buf, sizeof(buf));

	/* 只要本轮取到了字节就必须刷新时间戳，不管当前是什么状态。
	   这里原来写成"state != ST_IDLE 时才刷新"，有一个只有在上板时才会暴露的洞：
	   状态是从 IDLE 内部（FeedByte 扫到 'FWUP'）切过去的，而那次 Poll 顶部
	   state 还是 IDLE → 时间戳不刷新 → 紧接着下一轮若恰好没数据，
	   Timer_Timeout 拿的是"上次有数据的时刻"，可能是十几秒前，
	   于是刚进 HEADER 立刻被判超时。真实表现就是：
	   [app-ota] abort: timeout，固件一个字节都还没开始收。 */
	if(n > 0U)
	{
		s_rx.last_act = Board_Millis();
	}
	else if((s_rx.state != ST_IDLE) && (Timer_Timeout(&s_rx.last_act, APP_OTA_TIMEOUT_MS) != 0U))
	{
		/* 静默太久。此时头部已作废、数据区被擦了一半，
		   但 Boot 只会看到"空槽"，不会去搬，所以是安全的中止 */
		AbortRx("接收超时");
	}

	for(i = 0U; i < n; i++)
	{
		FeedByte(buf[i]);
	}
}
