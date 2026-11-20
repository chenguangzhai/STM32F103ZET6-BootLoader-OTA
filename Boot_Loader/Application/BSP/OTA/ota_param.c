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

int8_t Ota_Param_Load(OtaParam_t *p)
{
	uint8_t *raw = (uint8_t *)p;
	uint32_t i;
	uint8_t  blank = 1U;

	if(p == 0)
	{
		return OTA_PARM_BAD;
	}

	for(i = 0U; i < sizeof(OtaParam_t); i++)
	{
		raw[i] = *(volatile const uint8_t *)(OTA_PARAM_ADDR + i);
		if(raw[i] != 0xFFU)
		{
			blank = 0U;
		}
	}

	/* 整页全 0xFF = 从来没写过。老固件、刚量产的新片都长这样，
	   这不算"坏"，要单独告诉调用方，好让它退回兼容模式 */
	if(blank != 0U)
	{
		return OTA_PARM_EMPTY;
	}

	if(p->magic != OTA_PARAM_MAGIC)
	{
		return OTA_PARM_BAD;
	}

	/* 参数页自己也要能自证。最怕的情况是擦了一半就断电，
	   里面残留着一组"看起来像参数"的旧值 */
	if(p->self_crc != Crc32_Flash(OTA_PARAM_ADDR,
	                              sizeof(OtaParam_t) - sizeof(uint32_t)))
	{
		return OTA_PARM_BAD;
	}

	if((p->length == 0U) || (p->length > OTA_APP_MAX))
	{
		return OTA_PARM_BAD;
	}

	return OTA_PARM_OK;
}

int8_t Ota_Param_Save(uint32_t length, uint32_t crc, uint32_t version)
{
	OtaParam_t p;

	/* 长度越界就别写 —— 写进去反而会让下次上电误判 */
	if((length == 0U) || (length > OTA_APP_MAX))
	{
		return OTA_PARM_ERR;
	}

	p.magic    = OTA_PARAM_MAGIC;
	p.length   = length;
	p.crc32    = crc;
	p.version  = version;
	p.reserved = 0xFFFFFFFFU;

	/* 这里算的是 RAM 里那份结构体的 CRC，不是 Flash 里的 ——
	   所以要用 Crc32_Buf 而不是 Crc32_Flash */
	p.self_crc = Crc32_Buf((const uint8_t *)&p,
	                       sizeof(OtaParam_t) - sizeof(uint32_t));

	if(Flash_ErasePage(OTA_PARAM_ADDR) != FLASH_OK)
	{
		return OTA_PARM_ERR;
	}
	if(Flash_Write(OTA_PARAM_ADDR, (const uint8_t *)&p,
	               sizeof(OtaParam_t)) != FLASH_OK)
	{
		return OTA_PARM_ERR;
	}

	return OTA_PARM_OK;
}

int8_t Ota_Param_Erase(void)
{
	return (Flash_ErasePage(OTA_PARAM_ADDR) == FLASH_OK) ? OTA_PARM_OK
	                                                     : OTA_PARM_ERR;
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
