/**
 * Flash 写入（最简版）
 *
 * 只有两个函数：擦一页、写一段。没有校验、没有缓冲、没有 CRC。
 *
 * 三条必须知道的规则（它们不是"建议"，是硬件限制）：
 *   1) 写之前必须先擦。Flash 只能 1→0，想让位变回 1 只能整页擦掉。
 *   2) 擦除的最小单位是「一页」= 2KB（F103xE 大容量），不是 1 个字节。
 *      所以擦的时候要给页首地址（2KB 的整数倍），给中间地址会擦错页。
 *   3) 写是按「半字」= 2 字节。地址必须偶数。
 *
 * 能写哪：只有 0x08008000 ~ 0x0807FFFF（App 区）。
 * 千万别擦 0x08000000 开头那 32KB，那是 bootloader 自己，擦掉就再也起不来了。
 */
#ifndef __FLASH_APP_H
#define __FLASH_APP_H

#include <stdint.h>

/* ==================== 分区 ====================
 *   0x08000000 ~ 0x08007FFF   bootloader   32KB（16 页）
 *   0x08008000 ~ 0x0807FFFF   App          480KB（240 页）
 *
 * 分区只在下面两个宏里定义，其他地方一律用 APP_ADDR / FLASH_END，
 * 不要另写 0x08008000 这类常数 —— 以后分区再变，改这里就够了。
 */
#define FLASH_PAGE_BYTES    0x800U                      /* 一页 2KB */
#define BOOT_ADDR           0x08000000U
#define BOOT_SIZE           0x8000U                     /* 给 boot 留 32KB */
#define APP_ADDR            (BOOT_ADDR + BOOT_SIZE)     /* 0x08008000，第 16 页开头 */
#define FLASH_END           0x08080000U                 /* 512KB 的末尾 */

/* ==================== 返回值 ==================== */
#define FLASH_OK            0
#define FLASH_ERR_ARG       (-1)    /* 参数不对：地址没对齐 / 指针空 / 长度 0 */
#define FLASH_ERR_ERASE     (-2)    /* 擦除失败 */
#define FLASH_ERR_WRITE     (-3)    /* 写入失败（最常见原因：这段没擦干净） */

/* 擦除 addr 所在的这一页。addr 必须是页首地址（2KB 对齐）。
 * 例：Flash_ErasePage(0x08008000);      擦第 16 页
 *     Flash_ErasePage(0x08008800);      擦第 17 页 */
int8_t Flash_ErasePage(uint32_t addr);

/* 从 addr 开始连写 len 个字节。写完不校验。
 * 前提：这段地址已经擦过，否则写不进去（返回 FLASH_ERR_WRITE）。
 * 长度是奇数也可以，最后一个字节会自动补 0xFF 凑成半字。 */
int8_t Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len);

#endif
