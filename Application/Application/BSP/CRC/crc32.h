/**
 * CRC32 校验（IEEE 802.3，和 zip / PNG / Ethernet 用的是同一个多项式）
 *
 * 为什么 OTA 需要它：
 *   光看"字节数对上了"不能说明 Flash 里的内容是对的。串口掉一个字节、
 *   Flash 某一位没擦干净、写入过程中被打断，都会让长度照样对上、内容却是坏的。
 *   CRC32 能把这种情况以几乎 100%% 的概率揪出来。
 *
 * 两个用法要分清：
 *   Crc32_Buf()   —— 算 RAM 里收到的数据，验证"传输过程有没有丢/错"
 *   Crc32_Flash() —— 直接把 Flash 读回来算，验证"到底写进去的是什么"
 *
 * 后者才是判断 App 能不能跳的依据。边收边算只能证明"我收到了什么"，
 * 证明不了"Flash 里存的是什么"—— 擦写失败、写保护、电压不稳都会让两者不同。
 */
#ifndef __CRC32_H
#define __CRC32_H

#include <stdint.h>

/* 标准初值。分两段算时，把上一段的结果当下一段的初值传进来。 */
#define CRC32_INIT      0xFFFFFFFFU

/* 分段累计。crc 传 CRC32_INIT 表示从头开始。 */
uint32_t Crc32_Update(uint32_t crc, const uint8_t *buf, uint32_t len);

/* 一次算完一整块 RAM。 */
uint32_t Crc32_Buf(const uint8_t *buf, uint32_t len);

/* 把 Flash 里的 len 个字节读出来算。addr 不需要 4 字节对齐，内部会处理。 */
uint32_t Crc32_Flash(uint32_t addr, uint32_t len);

#endif
