/**
 * OTA 双槽头部读写实现
 *
 * 这一层的职责很窄：把「槽」抽象成 头 + 数据 两段，并保证只有
 * 「magic / 结构版本 / 头 CRC / 长度范围」四项全过才返回 OTA_SLOT_OK。
 *
 * 为什么不在这里判断「版本比当前低就拒绝」：
 * 那是升级策略，不是数据完整性。策略会变（比如允许紧急降级），
 * 完整性判定不会变。混在一起以后想加"强制降级"就得改这个文件。
 */
#include "ota_slot.h"
#include "w25q.h"
#include "crc32.h"

#include <stddef.h>		/* offsetof */
#include <string.h>		/* memset / memcmp */

/* 头 CRC 覆盖的字节数：从结构体开头到 hdr_crc 之前。
   当前布局下是 24（magic4 + ver2 + hdrver2 + len4 + crc4 + time4 + flags4），
   不是 28 —— 28 是 hdr_crc 到 reserved 之后的总长，容易记混。
   用 offsetof 而不是写死数字，以后结构体加字段时这里自动跟着走，
   不会出现"加了字段但 CRC 少算 4 字节"这种查半天的问题 */
#define HDR_CRC_SPAN        ((uint32_t)offsetof(OtaSlotHeader_t, hdr_crc))

/* 读写 W25Q 的临时缓冲。放静态区而不是栈上：
   局部 256 字节 + 调用链上的缓冲有可能把 1KB 主栈挤爆，
   而这里是单线程顺序调用，静态缓冲没有并发问题 */
static uint8_t s_buf[W25Q_PAGE_SIZE];

uint32_t OtaSlot_Base(uint8_t slot)
{
	if(slot == OTA_SLOT_A)
	{
		return OTA_SLOT_A_BASE;
	}
	if(slot == OTA_SLOT_B)
	{
		return OTA_SLOT_B_BASE;
	}
	return 0xFFFFFFFFUL;
}

uint32_t OtaSlot_DataAddr(uint8_t slot)
{
	uint32_t base = OtaSlot_Base(slot);

	if(base == 0xFFFFFFFFUL)
	{
		return 0xFFFFFFFFUL;
	}
	return base + OTA_SLOT_HDR_BYTES;
}

uint32_t OtaSlot_Capacity(void)
{
	return OTA_SLOT_DATA_MAX;
}

void OtaSlot_BuildHeader(OtaSlotHeader_t *hdr, uint16_t fw_ver, uint32_t fw_len,
                         uint32_t fw_crc, uint32_t build_time, uint32_t flags)
{
	if(hdr == NULL)
	{
		return;
	}

	/* 先清零再逐字段填：reserved 必须为 0，否则两份"相同内容"的头部
	   CRC 不一样，回滚比较版本时会误判 */
	memset(hdr, 0, sizeof(*hdr));

	hdr->magic      = OTA_SLOT_HDR_MAGIC;
	hdr->fw_ver     = fw_ver;
	hdr->hdr_ver    = OTA_SLOT_HDR_VER;
	hdr->fw_len     = fw_len;
	hdr->fw_crc     = fw_crc;
	hdr->build_time = build_time;
	hdr->flags      = flags;
	hdr->hdr_crc    = Crc32_Buf((const uint8_t *)hdr, HDR_CRC_SPAN);
}

int8_t OtaSlot_ReadHeader(uint8_t slot, OtaSlotHeader_t *hdr)
{
	uint32_t base;
	uint32_t i;
	uint8_t  all_ff = 1U;

	if(hdr == NULL)
	{
		return OTA_SLOT_ERR;
	}

	base = OtaSlot_Base(slot);
	if(base == 0xFFFFFFFFUL)
	{
		return OTA_SLOT_ERR;
	}

	memset(hdr, 0, sizeof(*hdr));
	if(W25q_Read(base, (uint8_t *)hdr, (uint32_t)sizeof(*hdr)) != W25Q_OK)
	{
		return OTA_SLOT_ERR;
	}

	/* 全 0xFF 表示这个扇区从没被写过。
	   先判这个再判 magic，是为了把"空槽"和"写坏了"区分开 ——
	   两者对升级流程的含义完全不同：空的可以直接用，坏的必须先擦 */
	for(i = 0U; i < (uint32_t)sizeof(*hdr); i++)
	{
		if(((const uint8_t *)hdr)[i] != 0xFFU)
		{
			all_ff = 0U;
			break;
		}
	}
	if(all_ff != 0U)
	{
		return OTA_SLOT_EMPTY;
	}

	if(hdr->magic != OTA_SLOT_HDR_MAGIC)
	{
		return OTA_SLOT_BAD;
	}

	/* 结构版本不符时不要试图兼容解析：字段含义可能全变了，
	   按老布局读出来的 length/crc 会指向完全错误的位置 */
	if(hdr->hdr_ver != (uint16_t)OTA_SLOT_HDR_VER)
	{
		return OTA_SLOT_BAD;
	}

	if(Crc32_Buf((const uint8_t *)hdr, HDR_CRC_SPAN) != hdr->hdr_crc)
	{
		return OTA_SLOT_BAD;
	}

	/* 长度必须落在 (0, 数据区容量] 内。
	   上界不查的话，一个被写坏的 length 会让搬运阶段去读越过槽尾的地址，
	   可能读到槽 B 甚至 15MB 之外的空白区，表现成"搬进去的固件跑飞" */
	if((hdr->fw_len == 0UL) || (hdr->fw_len > OTA_SLOT_DATA_MAX))
	{
		return OTA_SLOT_BAD;
	}

	return OTA_SLOT_OK;
}

int8_t OtaSlot_WriteHeader(uint8_t slot, const OtaSlotHeader_t *hdr)
{
	uint32_t base;

	if(hdr == NULL)
	{
		return OTA_SLOT_ERR;
	}

	base = OtaSlot_Base(slot);
	if(base == 0xFFFFFFFFUL)
	{
		return OTA_SLOT_ERR;
	}

	/* 写之前先在 RAM 里自检一遍，避免把半成品头部写进 Flash。
	   尤其是 flags 传了 SELFTEST 的假头 —— 那必须由调用方显式构造，
	   这里只保证"结构完整"，不判断业务含义 */
	if((hdr->magic != OTA_SLOT_HDR_MAGIC) ||
	   (hdr->hdr_ver != (uint16_t)OTA_SLOT_HDR_VER) ||
	   (hdr->fw_len == 0UL) || (hdr->fw_len > OTA_SLOT_DATA_MAX) ||
	   (Crc32_Buf((const uint8_t *)hdr, HDR_CRC_SPAN) != hdr->hdr_crc))
	{
		return OTA_SLOT_ERR;
	}

	/* 头独占扇区，所以每次都是"整扇区擦掉再写 32 字节"。
	   擦除期间 CS 一直有效，不会被别的 SPI 设备打断 */
	if(W25q_EraseSector(base) != W25Q_OK)
	{
		return OTA_SLOT_ERR;
	}

	if(W25q_Write(base, (const uint8_t *)hdr, (uint32_t)sizeof(*hdr)) != W25Q_OK)
	{
		return OTA_SLOT_ERR;
	}

	return OTA_SLOT_OK;
}

int8_t OtaSlot_EraseHeader(uint8_t slot)
{
	uint32_t base = OtaSlot_Base(slot);

	if(base == 0xFFFFFFFFUL)
	{
		return OTA_SLOT_ERR;
	}

	return (W25q_EraseSector(base) == W25Q_OK) ? OTA_SLOT_OK : OTA_SLOT_ERR;
}

int8_t OtaSlot_VerifyData(uint8_t slot, const OtaSlotHeader_t *hdr)
{
	uint32_t addr;
	uint32_t left;
	uint32_t chunk;
	uint32_t crc = CRC32_INIT;

	if(hdr == NULL)
	{
		return OTA_SLOT_ERR;
	}

	if((hdr->fw_len == 0UL) || (hdr->fw_len > OTA_SLOT_DATA_MAX))
	{
		return OTA_SLOT_ERR;
	}

	addr = OtaSlot_DataAddr(slot);
	if(addr == 0xFFFFFFFFUL)
	{
		return OTA_SLOT_ERR;
	}

	/* 分块读、分块累计：不能一次读完 508KB，F103 只有 64KB RAM。
	   这里刻意用 W25q_Read 而不是把地址映射过来直接访问 ——
	   W25Q 不支持 XIP，必须走 SPI 命令读 */
	left = hdr->fw_len;
	while(left > 0UL)
	{
		chunk = (left > W25Q_PAGE_SIZE) ? W25Q_PAGE_SIZE : left;

		if(W25q_Read(addr, s_buf, chunk) != W25Q_OK)
		{
			return OTA_SLOT_ERR;
		}

		crc   = Crc32_Update(crc, s_buf, chunk);
		addr += chunk;
		left -= chunk;
	}

	/* Crc32_Update 不做最终取反，和 Crc32_Buf 的约定要保持一致 */
	return (~crc == hdr->fw_crc) ? 1 : 0;
}

/* ==================== 试用确认标记 ====================
 *
 * 16 字节，放在 0x100000 那个独立扇区里。
 * 校验方式和槽头一样分四步：magic → 长度范围 → 自带 CRC。
 * 少了"自带 CRC"这一步，写一半断电会留下一个"看起来像确认"的残片，
 * Boot 会误以为新固件跑起来了 —— 那比不确认更糟。
 */
int8_t OtaSlot_ReadConfirm(OtaConfirm_t *c)
{
	uint8_t k;
	uint8_t blank = 1U;

	if(c == NULL)
	{
		return OTA_SLOT_ERR;
	}

	if(W25q_Read(OTA_CONFIRM_BASE, (uint8_t *)c, sizeof(OtaConfirm_t)) != W25Q_OK)
	{
		return OTA_SLOT_ERR;
	}

	for(k = 0U; k < (uint8_t)sizeof(OtaConfirm_t); k++)
	{
		if(((const uint8_t *)c)[k] != 0xFFU)
		{
			blank = 0U;
			break;
		}
	}

	if(blank != 0U)
	{
		return OTA_SLOT_EMPTY;
	}

	if(c->magic != OTA_CONFIRM_MAGIC)
	{
		return OTA_SLOT_BAD;
	}

	if(c->crc != Crc32_Buf((const uint8_t *)c,
	                       (uint32_t)offsetof(OtaConfirm_t, crc)))
	{
		return OTA_SLOT_BAD;
	}

	if((c->fw_len == 0UL) || (c->fw_len > OTA_SLOT_DATA_MAX))
	{
		return OTA_SLOT_BAD;
	}

	return OTA_SLOT_OK;
}

int8_t OtaSlot_WriteConfirm(uint32_t fw_len, uint32_t fw_crc)
{
	OtaConfirm_t c;

	if((fw_len == 0UL) || (fw_len > OTA_SLOT_DATA_MAX))
	{
		return OTA_SLOT_ERR;
	}

	c.magic  = OTA_CONFIRM_MAGIC;
	c.fw_len = fw_len;
	c.fw_crc = fw_crc;
	c.crc    = Crc32_Buf((const uint8_t *)&c,
	                     (uint32_t)offsetof(OtaConfirm_t, crc));

	/* 顺序不能反：先擦再写。这个扇区只放 16 字节，但要整扇区擦掉 */
	if(W25q_EraseSector(OTA_CONFIRM_BASE) != W25Q_OK)
	{
		return OTA_SLOT_ERR;
	}

	if(W25q_Write(OTA_CONFIRM_BASE, (const uint8_t *)&c,
	              sizeof(OtaConfirm_t)) != W25Q_OK)
	{
		return OTA_SLOT_ERR;
	}

	return OTA_SLOT_OK;
}
