/**
 * OTA 落盘：把串口收到的字节流，攒成整页后写进 Flash
 *
 * 这一层只管「攒」和「落盘」，不关心数据从哪来、也不做任何校验。
 * 底层擦写由 flash_app 负责，本模块只是把 2KB 边界这件事挡住：
 *
 *     Ota_Init(APP_ADDR);      上电设一次写入起点
 *     Ota_Feed(buf, len);      每收到一段就喂进来，攒满 2KB 自动擦写一页
 *     Ota_Flush();             数据发完了，把不足一页的尾巴也写出去
 *
 * 为什么要攒满一页再写：擦一页要 20~40ms，写一页才 1~2ms。
 * 如果每收 256 字节就"擦+写"，同一页要被擦 8 次，慢 8 倍、寿命也砍到 1/8。
 *
 * 三个必须知道的边界：
 *   1) 一页 = 2KB（F103xE 大容量），起点地址必须 2KB 对齐
 *   2) Ota_Flush() 之后，下一页从「当前页 + 2KB」开始，中间不会留缝
 *   3) 写满 APP 区不会自动停 —— 调用方自己盯着 Ota_Addr() 不要越过 0x08080000
 */
#ifndef __OTA_APP_H
#define __OTA_APP_H

#include <stdint.h>
#include "flash_app.h"		/* 分区宏（APP_ADDR / FLASH_PAGE_BYTES）从这里来 */

/* 一页多大，和 Flash 擦除单位保持一致。
   不要在别处另写 2048，改这个宏就够了。 */
#define OTA_PAGE_BYTES      FLASH_PAGE_BYTES

/* ==================== 返回值 ==================== */
#define OTA_OK              0
#define OTA_ERR_ARG         (-1)	/* 没调 Ota_Init / 传了空指针 */
#define OTA_ERR_ERASE       (-2)	/* 擦除失败（多半是地址没对齐或越界） */
#define OTA_ERR_WRITE       (-3)	/* 写入失败（多半是这一页没擦干净） */

/* 设定写入起点，同时把内部状态清零。
 * start_addr 必须是页首地址（2KB 的整数倍），例：APP_ADDR = 0x08004000。
 * 重复调用等于"从头再来"，之前攒了没写的数据会被丢掉。 */
void Ota_Init(uint32_t start_addr);

/* 喂一段数据。内部自动按 2KB 攒页，攒满就擦一页写一页。
 * 一次喂多少都行：len 可以是 1 字节，也可以是 5000 字节（会连续写两页半）。
 * 返回 OTA_OK 表示这一批全部处理完；返回负数表示中途某一页失败了，
 * 失败时已写进去的页不会回滚，剩余数据会被丢弃。 */
int8_t Ota_Feed(const uint8_t *buf, uint32_t len);

/* 把当前这一页里"不满 2KB"的尾巴写出去。
 * 不发这个，最后一页会一直留在 RAM 里 —— 现象就是"固件总是少最后一段"。
 * 没有待写数据时调用它无害，直接返回 OTA_OK。 */
int8_t Ota_Flush(void);

/* 已经真正落进 Flash 的字节数（不含还在 RAM 里攒着的那部分）。 */
uint32_t Ota_Total(void);

/* 当前这一页攒了多少字节。0 表示没有待写数据。 */
uint32_t Ota_Fill(void);

/* 下一个要写的页首地址。用它判断有没有写越界。 */
uint32_t Ota_Addr(void);

#endif
