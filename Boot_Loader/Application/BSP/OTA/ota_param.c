/**
 * App 参数区 + 完整性校验
 *
 * 判断"App 能不能跳"分三步，任何一步不过都不跳：
 *   1) 向量表合法性      SP 在 RAM、复位向量在 Flash（由 Boot_Loader_AppValid 做）
 *   2) 参数页有效性      magic 对、自校验 CRC 对
 *   3) 固件内容一致性    把 Flash 读回来重算 CRC，和参数里记的比
 *
 * 第 1 步只挡得住空片；第 2、3 步才能挡住"写了一半"和"写坏了"。
 */
#include "ota_param.h"
#include "crc32.h"

/* 小端拼 4 字节。不用 memcpy 是为了不依赖对齐 —— buf 可能从任意地址开始 */
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0]
	     | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[3] << 24);
}

/**
 * 读一页参数页，判断它是什么成色。
 *
 * 返回 OTA_PARM_OK / OTA_PARM_EMPTY / OTA_PARM_BAD，有效时把 seq 带出去。
 *
 * 这里要同时认两种格式：
 *   新格式 28 字节：self_crc 在偏移 24，覆盖前 24 字节，带 seq
 *   老格式 24 字节：self_crc 在偏移 20，覆盖前 20 字节，没有 seq
 *
 * 为什么必须认老格式：双备份是后来加的，板子上本来就有老格式的记录。
 * 不认的话新 Boot 第一次上电就把它判成「坏」，一块好板子当场停在 OTA。
 * 老记录一律当 seq = 0，下一次保存自然写成新格式，迁移是自动完成的。
 */
static int8_t Page_Load(uint32_t addr, OtaParam_t *p, uint32_t *seq)
{
	uint8_t *raw = (uint8_t *)p;
	uint32_t i;
	uint8_t  blank = 1U;

	for(i = 0U; i < (uint32_t)sizeof(OtaParam_t); i++)
	{
		raw[i] = *(volatile const uint8_t *)(addr + i);
		if(raw[i] != 0xFFU)
		{
			blank = 0U;
		}
	}

	if(blank != 0U)
	{
		return OTA_PARM_EMPTY;		/* 整页没写过 */
	}

	if(p->magic != OTA_PARAM_MAGIC)
	{
		return OTA_PARM_BAD;
	}

	/* 先按新格式验：self_crc 在末尾 4 字节 */
	if(p->self_crc == Crc32_Flash(addr,
	                              (uint32_t)sizeof(OtaParam_t) - sizeof(uint32_t)))
	{
		if((p->length == 0U) || (p->length > OTA_APP_MAX))
		{
			return OTA_PARM_BAD;
		}
		*seq = p->seq;
		return OTA_PARM_OK;
	}

	/* 再按老格式验：偏移 20 藏着的那个值就是老 self_crc，覆盖前 20 字节。
	   注意此时 p->seq 读到的是老 self_crc，是垃圾值，必须显式清 0，
	   否则两页比大小时会拿这个垃圾去比，选错页 */
	if(le32(&raw[20]) == Crc32_Flash(addr, 20U))
	{
		if((p->length == 0U) || (p->length > OTA_APP_MAX))
		{
			return OTA_PARM_BAD;
		}
		p->seq = 0U;
		*seq  = 0U;
		return OTA_PARM_OK;
	}

	return OTA_PARM_BAD;
}

/* 两页一起看，挑出有效且序号最新的那条。
 * 顺带把这条记录所在的地址带出来，写的时候要写另一页。 */
static int8_t LoadBest(OtaParam_t *p, uint32_t *seq, uint32_t *addr)
{
	OtaParam_t pa;
	OtaParam_t pb;
	uint32_t   sa = 0U;
	uint32_t   sb = 0U;
	int8_t     ra;
	int8_t     rb;

	ra = Page_Load(OTA_PARAM_ADDR_A, &pa, &sa);
	rb = Page_Load(OTA_PARAM_ADDR_B, &pb, &sb);

	if((ra == OTA_PARM_OK) && (rb == OTA_PARM_OK))
	{
		/* 正常轮转状态：两页都有效（旧页还没被下一次写入覆盖）。
		   取序号大的；序号相同时取 A —— 这只可能出现在两页都是老格式
		   的极端情况下，实际不会发生，但两种写法都必须有确定结果 */
		if(sa >= sb)
		{
			*p    = pa;
			*seq  = sa;
			*addr = OTA_PARAM_ADDR_A;
		}
		else
		{
			*p    = pb;
			*seq  = sb;
			*addr = OTA_PARAM_ADDR_B;
		}
		return OTA_PARM_OK;
	}

	if(ra == OTA_PARM_OK)
	{
		*p = pa;  *seq = sa;  *addr = OTA_PARAM_ADDR_A;
		return OTA_PARM_OK;
	}

	if(rb == OTA_PARM_OK)
	{
		*p = pb;  *seq = sb;  *addr = OTA_PARAM_ADDR_B;
		return OTA_PARM_OK;
	}

	if((ra == OTA_PARM_EMPTY) && (rb == OTA_PARM_EMPTY))
	{
		return OTA_PARM_EMPTY;		/* 从没写过，老固件、新片都长这样 */
	}

	return OTA_PARM_BAD;			/* 至少一页写过但都对不上 */
}

int8_t Ota_Param_Load(OtaParam_t *p)
{
	OtaParam_t tmp;
	uint32_t   seq;
	uint32_t   addr;
	int8_t     r;

	if(p == 0)
	{
		return OTA_PARM_BAD;
	}

	r = LoadBest(&tmp, &seq, &addr);
	if(r != OTA_PARM_OK)
	{
		return r;			/* EMPTY 还是 BAD，原样告诉调用方 */
	}

	*p = tmp;
	return OTA_PARM_OK;
}

uint8_t Ota_Param_State(uint32_t reserved)
{
	return (uint8_t)(reserved & OTA_PARM_STATE_MASK);
}

uint8_t Ota_Param_TrialCnt(uint32_t reserved)
{
	return (uint8_t)((reserved & OTA_PARM_CNT_MASK) >> OTA_PARM_CNT_SHIFT);
}

int8_t Ota_Param_SaveEx(uint32_t length, uint32_t crc, uint32_t version,
                        uint8_t state, uint8_t cnt)
{
	OtaParam_t p;
	OtaParam_t chk;
	OtaParam_t cur;
	uint32_t   seq;
	uint32_t   active;
	uint32_t   target;
	uint32_t   dummy;
	int8_t     r;

	/* 长度越界就别写 —— 写进去反而会让下次上电误判 */
	if((length == 0U) || (length > OTA_APP_MAX))
	{
		return OTA_PARM_ERR;
	}

	/* 新记录写到「当前没在用的那一页」，序号接在最大序号之后。
	   这样无论擦/写在哪一步断电，另一页始终是完整的旧账本 */
	r = LoadBest(&cur, &seq, &active);
	if(r == OTA_PARM_OK)
	{
		seq    = seq + 1U;
		target = (active == OTA_PARAM_ADDR_A) ? OTA_PARAM_ADDR_B
		                                      : OTA_PARAM_ADDR_A;
	}
	else
	{
		/* 两页都没有有效记录（空片 / 两页都坏）：从 A 页、序号 1 起步 */
		seq    = 1U;
		target = OTA_PARAM_ADDR_A;
	}

	p.magic   = OTA_PARAM_MAGIC;
	p.length  = length;
	p.crc32   = crc;
	p.version = version;
	p.seq     = seq;

	/* 先把高 16 位置 1（没用到），**不能**从 bit8 开始填 1：
	   位 8..15 就是试用计数，预先置成 1 的话读出来是 255，
	   加一次就回绕成 0，计数永远到不了上限 —— 坏固件会无限重启。
	   这个坑上板第一天就踩了，日志表现是"[trial] 等 App 确认 0/3"。 */
	p.reserved = 0xFFFF0000UL
	           | (((uint32_t)cnt << OTA_PARM_CNT_SHIFT) & OTA_PARM_CNT_MASK)
	           | ((uint32_t)state & OTA_PARM_STATE_MASK);

	/* 这里算的是 RAM 里那份结构体的 CRC，不是 Flash 里的 ——
	   所以要用 Crc32_Buf 而不是 Crc32_Flash */
	p.self_crc = Crc32_Buf((const uint8_t *)&p,
	                       (uint32_t)sizeof(OtaParam_t) - sizeof(uint32_t));

	if(Flash_ErasePage(target) != FLASH_OK)
	{
		return OTA_PARM_ERR;
	}
	if(Flash_Write(target, (const uint8_t *)&p,
	               (uint32_t)sizeof(OtaParam_t)) != FLASH_OK)
	{
		return OTA_PARM_ERR;
	}

	/* 回读确认。擦到一半、写到一半、电压跌落，都会在这一步暴露，
	   而不是等到下次上电才发现账本对不上。
	   失败时**不去补写另一页** —— 那页是此刻唯一的有效记录，动它就是自毁 */
	if(Page_Load(target, &chk, &dummy) != OTA_PARM_OK)
	{
		return OTA_PARM_ERR;
	}
	if((chk.seq != p.seq) || (chk.crc32 != p.crc32) || (chk.length != p.length))
	{
		return OTA_PARM_ERR;
	}

	return OTA_PARM_OK;
}

int8_t Ota_Param_Save(uint32_t length, uint32_t crc, uint32_t version)
{
	/* 老协议的升级路径（Boot 串口直收）走这里：它能成功就说明固件已经
	   完整躺在内部 Flash 且 CRC 校验过了，没有"试用"的必要，直接算确认。
	   这样老流程的行为和阶段 3 之前完全一致，不会因为加了试用计数
	   而让一块本来正常的板子开始回滚。 */
	return Ota_Param_SaveEx(length, crc, version, OTA_PARM_STATE_CONFIRMED, 0U);
}

int8_t Ota_Param_SetState(uint8_t state, uint8_t cnt)
{
	OtaParam_t p;

	if(Ota_Param_Load(&p) != OTA_PARM_OK)
	{
		return OTA_PARM_ERR;
	}

	return Ota_Param_SaveEx(p.length, p.crc32, p.version, state, cnt);
}

int8_t Ota_Param_Erase(void)
{
	/* 两页都擦，才算真的"没有参数"。只擦一页的话另一页的旧记录
	   会在下次上电被当成有效账本读出来 —— 看起来像擦除失败 */
	if(Flash_ErasePage(OTA_PARAM_ADDR_A) != FLASH_OK)
	{
		return OTA_PARM_ERR;
	}
	if(Flash_ErasePage(OTA_PARAM_ADDR_B) != FLASH_OK)
	{
		return OTA_PARM_ERR;
	}
	return OTA_PARM_OK;
}

int8_t Ota_VerifyFlash(uint32_t addr, uint32_t length, uint32_t crc)
{
	uint32_t calc;

	if((length == 0U) || (length > OTA_APP_MAX))
	{
		return -1;
	}

	calc = Crc32_Flash(addr, length);
	if(calc != crc)
	{
		return -2;
	}
	return 0;
}

uint8_t Ota_ParseTail(const uint8_t *buf, uint32_t len,
                      uint32_t recv_total, OtaTail_t *out)
{
	uint32_t magic;
	uint32_t length;
	uint32_t crc;

	if((buf == 0) || (out == 0) || (len < OTA_TAIL_BYTES))
	{
		return 0U;
	}

	magic = le32(&buf[0]);
	if(magic != OTA_TAIL_MAGIC)
	{
		return 0U;
	}

	length = le32(&buf[4]);
	crc    = le32(&buf[8]);

	/* 第二道闸：声明的长度必须和已经收下的字节数完全相等。
	   固件数据里恰好出现 'OTAE' 的概率约 2^-32，再加上这一条，
	   实际误判概率可以忽略 */
	if(length != recv_total)
	{
		return 0U;
	}

	out->length = length;
	out->crc32  = crc;
	return 1U;
}
