#include "ota_app.h"
#include "uart_app.h"		/* 进度日志。不想让这层打印就把 OTA_LOG_EN 改成 0 */
#include <string.h>

/* 1 = 每落盘一页/一次尾巴就打一行日志，0 = 全静音 */
#define OTA_LOG_EN      1

#if (OTA_LOG_EN != 0)
#define OTA_LOG(...)    Uart_Printf(UART_LOG, __VA_ARGS__)
#else
#define OTA_LOG(...)    do { } while (0)
#endif


/* 攒页缓冲：整整一页。放静态区，不进栈。 */
static uint8_t  s_page[OTA_PAGE_BYTES];

static uint32_t s_addr;			/* 当前这一页要写到哪个页首地址 */
static uint32_t s_fill;			/* s_page 里已经攒了多少字节 */
static uint32_t s_total;		/* 已经真正落进 Flash 的字节数 */
static uint8_t  s_inited;		/* 没调过 Ota_Init 就不许写 */


/**
  * 函    数：把 buf 的 len 个字节从 addr 开始落盘（先擦后写）
  * 说    明：addr 必须是页首地址，len 不能超过一页
  * 返 回 值：OTA_OK / OTA_ERR_ERASE / OTA_ERR_WRITE
  */
static int8_t Ota_PushPage(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	if(Flash_ErasePage(addr) != FLASH_OK)
	{
		return OTA_ERR_ERASE;
	}
	if(Flash_Write(addr, buf, len) != FLASH_OK)
	{
		return OTA_ERR_WRITE;
	}
	return OTA_OK;
}


/**
  * 函    数：设定写入起点并清空内部状态
  * 参    数：start_addr 第一个页首地址，必须 2KB 对齐
  */
void Ota_Init(uint32_t start_addr)
{
	s_addr   = start_addr;
	s_fill   = 0U;
	s_total  = 0U;
	s_inited = 1U;
}


/**
  * 函    数：喂数据，攒满一页就落盘
  * 参    数：buf 数据首地址；len 字节数（可以是任意值，不用自己对齐）
  * 返 回 值：OTA_OK 全部处理完；负数表示某一页失败了
  * 说    明：失败时已写的页不回滚。调用方看到负数应该停手，
  *           否则后面的数据会落在错误的位置上。
  */
int8_t Ota_Feed(const uint8_t *buf, uint32_t len)
{
	uint32_t i = 0U;

	if((s_inited == 0U) || (buf == NULL) || (len == 0U))
	{
		return OTA_ERR_ARG;
	}

	while(i < len)
	{
		uint32_t space = OTA_PAGE_BYTES - s_fill;					/* 这一页还剩几个空位 */
		uint32_t n     = ((len - i) < space) ? (len - i) : space;	/* 这次实际能塞多少 */

		memcpy(&s_page[s_fill], &buf[i], n);
		s_fill += n;
		i      += n;

		if(s_fill >= OTA_PAGE_BYTES)		/* 满一页，落盘 */
		{
			if(Ota_PushPage(s_addr, s_page, OTA_PAGE_BYTES) != OTA_OK)
			{
				/* 不清 s_fill：把攒着的数据留着，方便调试时看现场 */
				OTA_LOG("[ota] FAIL at 0x%08lX\r\n", (unsigned long)s_addr);
				return OTA_ERR_WRITE;
			}

			s_total += OTA_PAGE_BYTES;
			OTA_LOG("[ota] page 0x%08lX ok, sum: %lu\r\n",
					(unsigned long)s_addr, (unsigned long)s_total);

			s_addr  += OTA_PAGE_BYTES;
			s_fill   = 0U;
		}
	}

	return OTA_OK;
}


/**
  * 函    数：把不满一页的尾巴写出去
  * 返 回 值：OTA_OK / 负数
  * 说    明：只写 s_fill 个字节，这一页剩下部分保持擦除态 0xFF。
  *           写完地址照样前进一整页 —— 因为「这一页已经结束了」，
  *           后面再来数据必须落到下一页，否则会覆盖刚写的内容。
  */
int8_t Ota_Flush(void)
{
	if(s_inited == 0U)
	{
		return OTA_ERR_ARG;
	}
	if(s_fill == 0U)			/* 没尾巴，什么都不用做 */
	{
		return OTA_OK;
	}

	if(Ota_PushPage(s_addr, s_page, s_fill) != OTA_OK)
	{
		OTA_LOG("[ota] tail FAIL at 0x%08lX %luB\r\n",
				(unsigned long)s_addr, (unsigned long)s_fill);
		return OTA_ERR_WRITE;
	}

	s_total += s_fill;
	OTA_LOG("[ota] tail 0x%08lX %luB ok, sum: %lu\r\n",
			(unsigned long)s_addr, (unsigned long)s_fill, (unsigned long)s_total);

	s_addr  += OTA_PAGE_BYTES;
	s_fill   = 0U;

	return OTA_OK;
}


/* ==================== 状态查询 ==================== */

uint32_t Ota_Total(void)
{
	return s_total;
}

uint32_t Ota_Fill(void)
{
	return s_fill;
}

uint32_t Ota_Addr(void)
{
	return s_addr;
}
