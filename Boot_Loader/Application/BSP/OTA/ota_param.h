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

/* ==================== 布局 ====================
 *
 * 参数页做双备份：App 区最后两页，A 在低地址。
 *
 * 为什么必须两页：写参数页的过程是「擦 2KB 页 → 写 28 字节」，那段擦除
 * 要 20~40ms。而升级恰恰是最容易断电的时刻（用户看进度条卡住就去拔电源）。
 * 单页时这一下掉电，参数页就变成「有内容但 CRC 不对」—— 自愈故意不碰这种
 * 状态（不猜），于是 Boot 停在 OTA 等串口，现场没串口就是一块砖。
 *
 * 双页轮转后，每次只写「当前没在用的那一页」，写完整页回读校验才认；
 * 任何时刻至少有一页是完整的。掉电最坏结果是丢一次状态更新，不会丢账本。
 * 附带好处：单页 1 万次的擦写寿命变成两页轮流各 1 万次。
 */
#define OTA_PARAM_ADDR_A    (FLASH_END - (2U * FLASH_PAGE_BYTES))   /* 0x0807F000 */
#define OTA_PARAM_ADDR_B    (FLASH_END - (1U * FLASH_PAGE_BYTES))   /* 0x0807F800 */

/* 老代码仍用这个名字，指向 A 页（也是新记录的默认落点） */
#define OTA_PARAM_ADDR      OTA_PARAM_ADDR_A

/* App 固件能占用的最大字节数（到参数页 A 为止）。
   比双备份之前少了一页 2KB，对 18KB 的固件没有实际影响 */
#define OTA_APP_MAX         (OTA_PARAM_ADDR_A - APP_ADDR)

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

/* 参数页内容。sizeof = 28 字节，一页 2KB 只用开头这一点。
 *
 * 布局前后不能改：前 24 字节和老版本一模一样，只在 self_crc 前面插了 seq。
 * 老记录（无 seq）由 Ota_Param_Load 按老格式单独识别，见 .c 里的说明。 */
typedef struct
{
	uint32_t magic;         /* OTA_PARAM_MAGIC */
	uint32_t length;        /* App 固件的有效字节数 */
	uint32_t crc32;         /* App 区 [APP_ADDR, APP_ADDR+length) 的 CRC32 */
	uint32_t version;       /* 固件版本号，降级拦截靠它比较 */
	uint32_t reserved;      /* bit0..3 状态，bit8..15 试用计数 */
	uint32_t seq;           /* 递增序号：两页比大小，谁大谁新 */
	uint32_t self_crc;      /* 上面 24 字节的 CRC32，防止参数页自己写坏 */
} OtaParam_t;

/* 尾部记录解析结果 */
typedef struct
{
	uint32_t length;
	uint32_t crc32;
} OtaTail_t;

/* ==================== 试用状态（阶段 4） ====================
 *
 * 存在 OtaParam_t.reserved 里，不改结构体大小 —— 这样老参数页不用重刷，
 * self_crc 也照样覆盖得到（它算的是前 20 字节，reserved 在其中）。
 *
 *   bit0..3   状态
 *   bit8..15  已经试着启动过几次
 *   其余位    全 1（Flash 擦完就是 1，少写几个 0 等于少几次电位翻转）
 *
 * 兼容性：老固件写参数时 reserved = 0xFFFFFFFF，状态读出来是 0xF，
 * 按"已确认"处理。否则升级后第一次上电会把老固件当成"试用中"，
 * 而老固件根本不会去确认，三次之后就误回滚。
 */
#define OTA_PARM_STATE_MASK      0x0000000FUL
#define OTA_PARM_CNT_MASK        0x0000FF00UL
#define OTA_PARM_CNT_SHIFT       8U

#define OTA_PARM_STATE_CONFIRMED 0x0U    /* 这一版跑起来过，App 亲自确认了 */
#define OTA_PARM_STATE_TRIAL     0x1U    /* 刚装上，还在试用期 */
#define OTA_PARM_STATE_LEGACY    0xFU    /* 老参数（reserved=0xFFFFFFFF），按已确认对待 */

/* 试用上限：到这个次数还没被确认就回滚。
   3 次能容忍 1~2 次上电抖动（插拔电源、按复位），又不至于让坏固件
   在场上跑太多轮才暴露 */
#define OTA_PARM_TRIAL_MAX       3U

/* 从 reserved 里取状态/计数 */
uint8_t Ota_Param_State(uint32_t reserved);
uint8_t Ota_Param_TrialCnt(uint32_t reserved);

/* 读参数页。
 * 返回 OTA_PARM_OK / OTA_PARM_EMPTY / OTA_PARM_BAD。 */
int8_t Ota_Param_Load(OtaParam_t *p);

/* 写参数页（内部先擦整页再写）。
 * 调用前请确认 length 不超过 OTA_APP_MAX。 */
int8_t Ota_Param_Save(uint32_t length, uint32_t crc, uint32_t version);

/* 写参数页，同时指定试用状态与已试次数。
 * 新固件搬进来之后要用这个（state=TRIAL, cnt=0）——
 * 用上面的 Ota_Param_Save 会把状态写成"已确认"，试用期就被跳过了。 */
int8_t Ota_Param_SaveEx(uint32_t length, uint32_t crc, uint32_t version,
                        uint8_t state, uint8_t cnt);

/* 只改试用状态，长度/CRC/版本原样保留（读-改-写）。
 * 试用计数递增、确认转正都走这里，免得每次都要把 len/crc 再传一遍、
 * 也避免调用方传错长度把参数页写坏。 */
int8_t Ota_Param_SetState(uint8_t state, uint8_t cnt);

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
