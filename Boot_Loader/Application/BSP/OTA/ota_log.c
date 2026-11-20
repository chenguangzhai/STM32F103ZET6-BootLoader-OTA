/**
 * OTA 黑匣子实现
 *
 * 写入策略：扇区内顺序追加，写满整扇区后擦掉重来（序号继续递增）。
 *
 * 为什么不做"逐条擦"或者"多扇区轮转"：
 *   W25Q 的最小擦除单位是 4KB 扇区。想在已有记录后面追加，只能先擦掉整扇区
 *   —— 没有"只擦 32 字节"这种操作。所以环形覆盖是唯一简单可行的做法，
 *   代价是写满时丢掉最老的记录，这正好是我们要的语义（保留最近 128 条）。
 *
 * 为什么每条都带自校验 CRC：
 *   掉电可能把一条记录写成半截（前 16 字节写进去了、后 16 字节还是 0xFF）。
 *   只看 magic 的话这半截记录会被当成有效，后面所有的扫描位置全错一位。
 *   带上 CRC 就能明确区分"完整记录"和"写了一半"。
 */
#include "ota_log.h"
#include "w25q.h"
#include "crc32.h"
#include "uart_app.h"

/* 当前状态。惰性初始化：第一次用到时才扫描扇区，
   这样 Boot 若因为别的原因很早就退出，不会白白多花几毫秒读 Flash */
static uint8_t  s_ready = 0U;
static uint8_t  s_next  = 0U;		/* 下一条要写的位置（0..OTA_LOG_MAX） */
static uint32_t s_seq   = 1U;		/* 下一条的序号 */

static const char *Evt_Name(uint32_t evt)
{
	switch(evt)
	{
	case OTA_LOG_EVT_RST_POR:    return "上电复位";
	case OTA_LOG_EVT_RST_PIN:    return "复位键";
	case OTA_LOG_EVT_RST_SFT:    return "软件复位";
	case OTA_LOG_EVT_RST_IWDG:   return "看门狗复位";
	case OTA_LOG_EVT_RST_OTHER:  return "其它复位";
	case OTA_LOG_EVT_APPLY:      return "搬运固件";
	case OTA_LOG_EVT_ALREADY:    return "已是最新";
	case OTA_LOG_EVT_TRIAL:      return "试用计数";
	case OTA_LOG_EVT_CONFIRM:    return "试用转正";
	case OTA_LOG_EVT_TRIAL_FAIL: return "试用失败";
	case OTA_LOG_EVT_ROLLBACK:   return "回滚";
	case OTA_LOG_EVT_DOWNGRADE:  return "拒绝降级";
	case OTA_LOG_EVT_RECOVER:    return "参数重建";
	case OTA_LOG_EVT_OTA_MODE:   return "进入OTA";
	case OTA_LOG_EVT_STUCK:      return "跳转未响应";
	case OTA_LOG_EVT_NOAPP:      return "无可用App";
	default:                     return "未知事件";
	}
}

/* 一条记录是否完整可信 */
static uint8_t Entry_Valid(const OtaLogEntry_t *e)
{
	if(e->magic != OTA_LOG_MAGIC)
	{
		return 0U;
	}
	return (e->crc == Crc32_Buf((const uint8_t *)e,
	                            (uint32_t)sizeof(OtaLogEntry_t) - sizeof(uint32_t)))
	       ? 1U : 0U;
}

/* 这块地方是不是还从没写过（全 0xFF） */
static uint8_t Entry_Blank(const OtaLogEntry_t *e)
{
	const uint8_t *p = (const uint8_t *)e;
	uint32_t i;

	for(i = 0U; i < (uint32_t)sizeof(OtaLogEntry_t); i++)
	{
		if(p[i] != 0xFFU)
		{
			return 0U;
		}
	}
	return 1U;
}

/* 扫描扇区，数出前面连续有多少条有效记录，并接上序号。
 * 记录是顺序追加的，所以"第一条无效"就是写指针该在的位置。 */
static void Log_Scan(void)
{
	OtaLogEntry_t e;
	uint8_t i;
	uint32_t last_seq = 0U;

	for(i = 0U; i < (uint8_t)OTA_LOG_MAX; i++)
	{
		if(W25q_Read(OTA_LOG_BASE + ((uint32_t)i * OTA_LOG_ENTRY_BYTES),
		             (uint8_t *)&e, (uint32_t)sizeof(e)) != W25Q_OK)
		{
			break;						/* 读失败：当作到此为止，不覆盖后面 */
		}
		if(Entry_Valid(&e) == 0U)
		{
			/* 不是有效记录，而且这块地方也不是空的 —— 说明扇区里躺着
			   别的东西（换新片子时常见：旧工程或别的工具在里面留过数据）。
			   不擦的话这里永远写不进去（Flash 只能 1→0），整个黑匣子
			   会静默失效，日志里一条记录都没有，还看不出为什么。
			   踩过一次：0x101000 上是 08 4E 02 00，append 一律返回失败。

			   擦掉整扇区重新开始，序号接着 last_seq 往上走 —— 这样
			   "中间清过场"从序号跳跃上看得出来，不会和连续记录混淆。 */
			if(Entry_Blank(&e) == 0U)
			{
				if(W25q_EraseSector(OTA_LOG_BASE) == W25Q_OK)
				{
					i = 0U;				/* 擦干净了，从头开始写 */
				}
			}
			break;
		}
		last_seq = e.seq;
	}

	s_next  = i;
	s_seq   = last_seq + 1U;
	s_ready = 1U;
}

static void Log_EnsureReady(void)
{
	if(s_ready == 0U)
	{
		Log_Scan();
	}
}

uint8_t OtaLog_Append(uint32_t evt, uint32_t ver, uint32_t len, uint32_t arg)
{
	OtaLogEntry_t e;
	OtaLogEntry_t chk;
	uint32_t      addr;

	Log_EnsureReady();

	/* 写满了：擦整扇区从头来。序号不重置 —— 它是唯一能跨擦除
	   排序的依据，重置了就分不清"擦除后的新记录"和"上一轮的老记录" */
	if(s_next >= (uint8_t)OTA_LOG_MAX)
	{
		if(W25q_EraseSector(OTA_LOG_BASE) != W25Q_OK)
		{
			return 0U;
		}
		s_next = 0U;
	}

	e.magic    = OTA_LOG_MAGIC;
	e.seq      = s_seq;
	e.evt      = evt;
	e.ver      = ver;
	e.fw_len   = len;
	e.arg      = arg;
	e.reserved = 0U;
	e.crc      = Crc32_Buf((const uint8_t *)&e,
	                       (uint32_t)sizeof(e) - sizeof(uint32_t));

	addr = OTA_LOG_BASE + ((uint32_t)s_next * OTA_LOG_ENTRY_BYTES);

	if(W25q_Write(addr, (const uint8_t *)&e, (uint32_t)sizeof(e)) != W25Q_OK)
	{
		return 0U;
	}

	/* 回读校验。W25Q 写完不校验的话，"写成功但内容错"这种情况
	   会让下一条记录覆盖到错误位置，后面的记录全跟着错位 */
	if(W25q_Read(addr, (uint8_t *)&chk, (uint32_t)sizeof(chk)) != W25Q_OK)
	{
		return 0U;
	}
	if((Entry_Valid(&chk) == 0U) || (chk.seq != e.seq) || (chk.evt != e.evt))
	{
		return 0U;
	}

	s_next = (uint8_t)(s_next + 1U);
	s_seq  = s_seq + 1U;
	return 1U;
}

uint8_t OtaLog_Count(void)
{
	Log_EnsureReady();
	return s_next;
}

uint8_t OtaLog_DumpRecent(uint8_t n)
{
	OtaLogEntry_t e;
	uint8_t start;
	uint8_t i;
	uint8_t shown = 0U;

	Log_EnsureReady();

	if((s_next == 0U) || (n == 0U))
	{
		return 0U;
	}
	if(n > s_next)
	{
		n = s_next;
	}
	start = (uint8_t)(s_next - n);

	Uart_Printf(UART_LOG, "[log] 最近 %u 条（共 %u 条）：\r\n",
	            (unsigned)n, (unsigned)s_next);

	for(i = start; i < s_next; i++)
	{
		if(W25q_Read(OTA_LOG_BASE + ((uint32_t)i * OTA_LOG_ENTRY_BYTES),
		             (uint8_t *)&e, (uint32_t)sizeof(e)) != W25Q_OK)
		{
			break;
		}
		if(Entry_Valid(&e) == 0U)
		{
			continue;
		}

		/* 一行控制在 80 字符内：Uart_Printf 的内部缓冲只有 100 字节，
		   超了会被截断，截掉的恰好是行尾的 \r\n —— 打印出来就是两条
		   记录粘在一起 */
		Uart_Printf(UART_LOG, "[log] #%lu %s v=%lu %luB a=%lu\r\n",
		            (unsigned long)e.seq,
		            Evt_Name(e.evt),
		            (unsigned long)e.ver,
		            (unsigned long)e.fw_len,
		            (unsigned long)e.arg);
		shown++;
	}

	return shown;
}
