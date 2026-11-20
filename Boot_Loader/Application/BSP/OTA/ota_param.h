/**
 * App 参数区 + 完整性校验
 *
 * 解决什么问题：
 *   ota_app 只负责把字节攒成页写进 Flash，它不知道写的是不是一份完整固件。
 *   跳转前如果只看"SP 像 RAM 地址、复位向量在 Flash"，那么一份写了 8KB
 *   就断电的固件同样能通过检查 —— 跳过去就是跑飞。
 *
 * 本模块在 App 区最后留一页存「固件长度 + CRC32」，跳转前把 Flash 读回来
 * 重算一遍 CRC 比对。对不上就不跳，停在 OTA 等重传。
 *
 * 为什么是"读回来算"而不是"边收边算"：
 *   边收边算只能证明「我收到了什么」，证明不了「Flash 里存的是什么」。
 *   擦除不干净、写入被打断、电压跌落，都会让两者不一样。只有重读才是实锤。
 *
 * 存储布局：
 *   App 区   0x08008000 ~ 0x0807F7FF   固件本体（最多 476KB）
 *   参数页   0x0807F800 ~ 0x0807FFFF   本模块独占，OTA 不许写进来
 */
#ifndef __OTA_PARAM_H
#define __OTA_PARAM_H

#include <stdint.h>
#include "flash_app.h"

/* ==================== 布局 ==================== */

/* 参数页：App 区最后一页，避开固件本体 */
#define OTA_PARAM_ADDR      (FLASH_END - FLASH_PAGE_BYTES)

/* App 固件能占用的最大字节数（到参数页为止） */
#define OTA_APP_MAX         (OTA_PARAM_ADDR - APP_ADDR)

/* 参数页的识别码，写成 'OTAP' 的小端形式。全 0xFF 的空片不会命中 */
#define OTA_PARAM_MAGIC     0x4F544150U

/* ==================== 尾部记录 ====================
 * 上位机把固件发完之后，再补 12 个字节告诉 MCU「发完了、共多少、CRC 多少」：
 *
 *   偏移 0..3   'O','T','A','E'      固定标识
 *   偏移 4..7   固件长度，小端
 *   偏移 8..11  CRC32，小端
 *
 * 为什么不用一个特殊字符当结束符：串口是裸字节流，任何单字节都可能
 * 恰好出现在固件数据里。用 4 字节 magic 加上「长度必须和已收字节数相等」
 * 双重条件，误判概率低到可以不管。
 */
#define OTA_TAIL_MAGIC      0x4541544FU     /* 'O','T','A','E' 的小端值 */
#define OTA_TAIL_BYTES      12U

/* ==================== 返回值 ==================== */
#define OTA_PARM_OK         0       /* 参数有效，CRC 也对得上 */
#define OTA_PARM_EMPTY      1       /* 参数页是空的（从没写过），老固件就长这样 */
#define OTA_PARM_BAD        2       /* 参数页有内容但校验不过：写坏了或被擦了一半 */
#define OTA_PARM_ERR        (-1)    /* 底层 Flash 操作失败 */

/* 参数页内容。sizeof = 24 字节，一页 2KB 只用开头这一点 */
typedef struct
{
	uint32_t magic;         /* OTA_PARAM_MAGIC */
	uint32_t length;        /* App 固件的有效字节数 */
	uint32_t crc32;         /* App 区 [APP_ADDR, APP_ADDR+length) 的 CRC32 */
	uint32_t version;       /* 留作版本号，暂时写 0 */
	uint32_t reserved;      /* 对齐用 */
	uint32_t self_crc;      /* 上面 20 字节的 CRC32，防止参数页自己写坏 */
} OtaParam_t;

/* 尾部记录解析结果 */
typedef struct
{
	uint32_t length;
	uint32_t crc32;
} OtaTail_t;

/* 读参数页。
 * 返回 OTA_PARM_OK / OTA_PARM_EMPTY / OTA_PARM_BAD。 */
int8_t Ota_Param_Load(OtaParam_t *p);

/* 写参数页（内部先擦整页再写）。
 * 调用前请确认 length 不超过 OTA_APP_MAX。 */
int8_t Ota_Param_Save(uint32_t length, uint32_t crc, uint32_t version);

/* 擦掉参数页，让下一次上电退回"没有参数"的状态。 */
int8_t Ota_Param_Erase(void);

/* 把 Flash 里的 App 重新算一遍 CRC 和 length 比对。
 * 返回 0 表示一致，非 0 表示不一致或 Flash 读取异常。 */
int8_t Ota_VerifyFlash(uint32_t addr, uint32_t length, uint32_t crc);

/* 尝试把一段收到的数据当作尾部记录解析。
 * recv_total：目前已经写进 Flash 的字节数（Ota_Total）+ 还在 RAM 里攒着的（Ota_Fill）。
 * 返回 1 表示认出来了（结果写进 out），0 表示这段是普通固件数据。 */
uint8_t Ota_ParseTail(const uint8_t *buf, uint32_t len,
                      uint32_t recv_total, OtaTail_t *out);

#endif
