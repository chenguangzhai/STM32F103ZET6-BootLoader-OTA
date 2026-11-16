/**
 * Flash 写入实现（最简版）
 *
 * 整个工程里只有这个文件碰 HAL_FLASH_*，别处要写 Flash 就调这两个函数。
 */
#include "flash_app.h"

#include "stm32f1xx_hal.h"

/**
 * 擦除一页
 *
 * HAL 的擦除接口是按「从哪页开始、擦几页」调的，它自己不做对齐检查 ——
 * 你给它 0x08004500，它就从那儿开始擦，擦到哪一页全看运气。
 * 所以这里先自己判一下：必须 2KB 对齐。
 */
int8_t Flash_ErasePage(uint32_t addr)
{
	FLASH_EraseInitTypeDef init;
	uint32_t page_err = 0U;
	HAL_StatusTypeDef st;

	/* 没对齐就直接退出，别去试 —— 擦错页是不可恢复的 */
	if ((addr & (FLASH_PAGE_BYTES - 1U)) != 0U)
	{
		return FLASH_ERR_ARG;
	}

	init.TypeErase   = FLASH_TYPEERASE_PAGES;		/* 按页擦 */
	init.Banks       = FLASH_BANK_1;				/* 单 bank，按页擦时用不到 */
	init.PageAddress = addr;						/* 从这一页开始 */
	init.NbPages     = 1U;							/* 只擦一页 */

	HAL_FLASH_Unlock();								/* 解锁，否则写不进 */
	st = HAL_FLASHEx_Erase(&init, &page_err);
	HAL_FLASH_Lock();								/* 用完立刻锁回去 */

	/* 擦完这一页全是 0xFF。page_err 是出错页的地址，调试时可以看它 */
	if (st != HAL_OK)
	{
		return FLASH_ERR_ERASE;
	}
	return FLASH_OK;
}

/**
 * 写入一段数据
 *
 * 底层一次只能写 2 字节（半字），所以循环两字节两字节地写。
 * 拼接顺序是「低字节放低地址」，写反了读回来就是字节颠倒的。
 */
int8_t Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	uint32_t i;

	if ((buf == NULL) || (len == 0U))
	{
		return FLASH_ERR_ARG;
	}
	/* 半字对齐：地址是奇数的话，硬件会报 PGERR，一个字节都写不进去 */
	if ((addr & 1U) != 0U)
	{
		return FLASH_ERR_ARG;
	}

	HAL_FLASH_Unlock();

	for (i = 0U; i < len; i += 2U)
	{
		uint16_t hw;

		if ((i + 1U) < len)
		{
			/* 完整半字：低字节在前 */
			hw = (uint16_t)((uint16_t)buf[i] | ((uint16_t)buf[i + 1U] << 8));
		}
		else
		{
			/* 只剩最后一个字节：高 8 位补 0xFF（保持"未写"状态），凑成半字 */
			hw = (uint16_t)((uint16_t)buf[i] | 0xFF00U);
		}

		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, (uint64_t)hw) != HAL_OK)
		{
			HAL_FLASH_Lock();
			return FLASH_ERR_WRITE;
		}
	}

	HAL_FLASH_Lock();
	return FLASH_OK;
}
