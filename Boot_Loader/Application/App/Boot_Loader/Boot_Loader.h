#ifndef __BOOT_LOADER_H
#define __BOOT_LOADER_H

#include <stdint.h>
#include "uart_app.h"

uint16_t Boot_Loader_Getlen(void);

/* 判断 app_addr 处是不是一份"看起来能跑"的固件。
 * 只看头两个 32 位字（初始 SP + 复位向量），不需要执行代码：
 *   偏移 0x00 = 初始 SP，必须落在 SRAM 里
 *   偏移 0x04 = 复位向量，必须指向 Flash，且最低位为 1（Thumb）
 * 空白 Flash 全是 0xFFFFFFFF，两项都过不了，所以这个判断能挡住"跳空白区"。
 * 返回 1 = 有效，0 = 无效。 */
uint8_t Boot_Loader_AppValid(uint32_t app_addr);

/* 跳到 app_addr 处的固件。
 * 内部会：关总中断 → 停 SysTick → 清 NVIC → 停串口 DMA → 设 VTOR → 换 MSP → 跳。
 * 地址无效时直接返回，什么都不做。
 * 正常情况下此函数不返回（进去就跑 App 了）。 */
void Boot_Loader_JumpToApp(uint32_t app_addr);

#endif
