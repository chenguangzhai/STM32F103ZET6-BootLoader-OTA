/**
  * W25Q128 SPI Flash 驱动实现
  *
  * 【BootLoader 侧副本，源文件在 Application/Application/BSP/W25Q/w25q.c】
  * 两份文件必须保持完全一致，原因见 w25q.h 顶部说明。
  *
  * 所有函数都靠 CS 拉低 → 先发命令 → 再收发数据 → CS 拉高 这个套路。
  * 每一步失败都先把 CS 拉回高电平再 return —— 片选一直低着，芯片会认为
  * 上一帧命令还没结束，下一帧数据直接接在后面解释，症状是"第一次对，之后全乱"。
  */
#include <stddef.h>
#include "w25q.h"

/* W25Q 指令集（只列用到的） */
#define CMD_WRITE_ENABLE    0x06U
#define CMD_READ_STATUS1    0x05U
#define CMD_READ_DATA       0x03U
#define CMD_PAGE_PROGRAM    0x02U
#define CMD_SECTOR_ERASE    0x20U
#define CMD_JEDEC_ID        0x9FU

#define SPI_TX_TIMEOUT_MS   100U
#define SPI_RX_TIMEOUT_MS   1000U

static SPI_HandleTypeDef *s_hspi    = NULL;
static GPIO_TypeDef      *s_cs_port = NULL;
static uint16_t           s_cs_pin  = 0U;

static void Cs_Low(void)
{
	HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_RESET);
}

static void Cs_High(void)
{
	HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_SET);
}

static int8_t Tx(const uint8_t *buf, uint16_t len)
{
	if(HAL_SPI_Transmit(s_hspi, (uint8_t *)buf, len, SPI_TX_TIMEOUT_MS) != HAL_OK)
	{
		return W25Q_ERR_SPI;
	}
	return W25Q_OK;
}

static int8_t Rx(uint8_t *buf, uint16_t len)
{
	if(HAL_SPI_Receive(s_hspi, buf, len, SPI_RX_TIMEOUT_MS) != HAL_OK)
	{
		return W25Q_ERR_SPI;
	}
	return W25Q_OK;
}

/* 轮询状态寄存器 BUSY 位，直到芯片内部操作结束 */
static int8_t WaitBusy(uint32_t timeout_ms)
{
	uint8_t  cmd = CMD_READ_STATUS1;
	uint8_t  sr  = 0U;
	uint32_t t0  = HAL_GetTick();

	do
	{
		Cs_Low();
		if((Tx(&cmd, 1U) != W25Q_OK) || (Rx(&sr, 1U) != W25Q_OK))
		{
			Cs_High();
			return W25Q_ERR_SPI;
		}
		Cs_High();

		if((sr & 0x01U) == 0U)		/* BUSY = 0，干完了 */
		{
			return W25Q_OK;
		}
	} while((HAL_GetTick() - t0) < timeout_ms);

	return W25Q_ERR_TIMEOUT;
}

/* 每次擦/写之前都必须发一次，否则芯片拒绝执行 */
static int8_t WriteEnable(void)
{
	uint8_t cmd = CMD_WRITE_ENABLE;

	Cs_Low();
	if(Tx(&cmd, 1U) != W25Q_OK)
	{
		Cs_High();
		return W25Q_ERR_SPI;
	}
	Cs_High();

	return W25Q_OK;
}

int8_t W25q_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
	GPIO_InitTypeDef init = {0};

	if((hspi == NULL) || (cs_port == NULL) || (cs_pin == 0U))
	{
		return W25Q_ERR_ARG;
	}

	s_hspi    = hspi;
	s_cs_port = cs_port;
	s_cs_pin  = cs_pin;

	/* 片选默认拉高：W25Q 的 CS 是低有效，上电浮空会被当成命令起始 */
	HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_SET);

	init.Pin   = cs_pin;
	init.Mode  = GPIO_MODE_OUTPUT_PP;
	init.Pull  = GPIO_PULLUP;			/* 内部上拉，防止悬空被干扰成低电平 */
	init.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(cs_port, &init);

	return W25Q_OK;
}

uint32_t W25q_ReadId(void)
{
	uint8_t cmd = CMD_JEDEC_ID;
	uint8_t id[3] = {0xFFU, 0xFFU, 0xFFU};

	if(s_hspi == NULL)
	{
		return 0xFFFFFFFFUL;
	}

	Cs_Low();
	if(Tx(&cmd, 1U) == W25Q_OK)
	{
		(void)Rx(id, 3U);
	}
	Cs_High();

	return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];
}

uint8_t W25q_ReadStatus(void)
{
	uint8_t cmd = CMD_READ_STATUS1;
	uint8_t sr  = 0xFFU;

	if(s_hspi == NULL)
	{
		return 0xFFU;
	}

	Cs_Low();
	if(Tx(&cmd, 1U) == W25Q_OK)
	{
		(void)Rx(&sr, 1U);
	}
	Cs_High();

	return sr;
}

int8_t W25q_EraseSector(uint32_t addr)
{
	uint8_t cmd[4];

	if((addr % W25Q_SECTOR_SIZE) != 0U)		/* 必须扇区对齐，否则会擦错 4KB */
	{
		return W25Q_ERR_ADDR;
	}
	if(addr >= W25Q_SIZE)
	{
		return W25Q_ERR_ADDR;
	}

	if(WriteEnable() != W25Q_OK)
	{
		return W25Q_ERR_SPI;
	}

	cmd[0] = CMD_SECTOR_ERASE;
	cmd[1] = (uint8_t)(addr >> 16);
	cmd[2] = (uint8_t)(addr >> 8);
	cmd[3] = (uint8_t)(addr);

	Cs_Low();
	if(Tx(cmd, 4U) != W25Q_OK)
	{
		Cs_High();
		return W25Q_ERR_SPI;
	}
	Cs_High();

	return WaitBusy(1000U);					/* 擦除典型 45ms，最坏 400ms，给足余量 */
}

int8_t W25q_WritePage(uint32_t addr, const uint8_t *buf, uint16_t len)
{
	uint8_t cmd[4];

	if((buf == NULL) || (len == 0U) || (len > W25Q_PAGE_SIZE))
	{
		return W25Q_ERR_ARG;
	}
	/* 不能跨页：当前页内偏移 + 长度不能超过 256，否则芯片会回卷覆盖页首 */
	if(((addr & (W25Q_PAGE_SIZE - 1U)) + len) > W25Q_PAGE_SIZE)
	{
		return W25Q_ERR_ADDR;
	}
	if((addr + len) > W25Q_SIZE)
	{
		return W25Q_ERR_ADDR;
	}

	if(WriteEnable() != W25Q_OK)
	{
		return W25Q_ERR_SPI;
	}

	cmd[0] = CMD_PAGE_PROGRAM;
	cmd[1] = (uint8_t)(addr >> 16);
	cmd[2] = (uint8_t)(addr >> 8);
	cmd[3] = (uint8_t)(addr);

	Cs_Low();
	if((Tx(cmd, 4U) != W25Q_OK) || (Tx(buf, len) != W25Q_OK))
	{
		Cs_High();
		return W25Q_ERR_SPI;
	}
	Cs_High();

	return WaitBusy(100U);					/* 页编程典型 0.7ms，最坏 3ms */
}

int8_t W25q_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	uint32_t done = 0U;
	int8_t   st;

	if((buf == NULL) || (len == 0U))
	{
		return W25Q_ERR_ARG;
	}
	if((addr + len) > W25Q_SIZE)
	{
		return W25Q_ERR_ADDR;
	}

	while(done < len)
	{
		uint32_t cur   = addr + done;
		uint32_t left  = len - done;
		uint32_t space = W25Q_PAGE_SIZE - (cur & (W25Q_PAGE_SIZE - 1U));
		uint32_t n     = (left < space) ? left : space;

		st = W25q_WritePage(cur, &buf[done], (uint16_t)n);
		if(st != W25Q_OK)
		{
			return st;
		}
		done += n;
	}

	return W25Q_OK;
}

int8_t W25q_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
	uint8_t  cmd[4];
	uint32_t done = 0U;

	if((buf == NULL) || (len == 0U))
	{
		return W25Q_ERR_ARG;
	}
	if((addr + len) > W25Q_SIZE)
	{
		return W25Q_ERR_ADDR;
	}

	cmd[0] = CMD_READ_DATA;
	cmd[1] = (uint8_t)(addr >> 16);
	cmd[2] = (uint8_t)(addr >> 8);
	cmd[3] = (uint8_t)(addr);

	Cs_Low();
	if(Tx(cmd, 4U) != W25Q_OK)
	{
		Cs_High();
		return W25Q_ERR_SPI;
	}

	/* HAL 的 Size 是 uint16，一次最多 65535，这里按 1KB 分块更稳 */
	while(done < len)
	{
		uint32_t rest = len - done;
		uint16_t n    = (rest > 1024U) ? 1024U : (uint16_t)rest;

		if(Rx(&buf[done], n) != W25Q_OK)
		{
			Cs_High();
			return W25Q_ERR_SPI;
		}
		done += n;
	}
	Cs_High();

	return W25Q_OK;
}
