/**
  * 串口收发封装：环形缓冲 + DMA 循环接收，DMA 永不停止
  *
  * 设计目标：一套代码管所有串口。将来接 WiFi / LoRa / NB-IoT / 4G 都直接调这里，
  *           换模块只改协议层，收发层一个字不用动。
  *
  * 和「收一帧停一次」的区别：
  *   旧方案一帧收完 DMA 就停，主循环来不及取，这段时间的字节全丢；
  *   本方案 DMA 一直在跑，收和取彻底解耦，主循环慢一点只是数据在缓冲里多待一会儿。
  *
  * 用法：
  *   Uart_Init();                              // 一次性绑定所有通道并启动接收
  *   uint8_t buf[64];
  *   uint16_t n = Uart_Read(UART_AT, buf, sizeof(buf));   // 按需取 N 字节
  *
  * 为什么是「通道号」而不是「传 huart」：
  *   传 &huart2 的话，调用方必须看见 UART_HandleTypeDef，就得 include usart.h，
  *   usart.h 又拖进 stm32f1xx_hal.h —— 一个协议层的 .c 就这样拿到了整个 HAL。
  *   改成通道号后，本头文件只依赖 stdint.h，谁 include 它都不会被迫认识 HAL。
  *   具体哪个通道对应哪个 huart，只写在 uart_app.c 的 s_huart[] 里。
  */
#ifndef __UART_APP_H
#define __UART_APP_H

#include <stdint.h>

/* 逻辑通道。加新串口（LoRa / 4G）就在末尾插一个，然后去 uart_app.c 的 s_huart[] 补一行
 *
 * ⚠ 2026-11-08 两个口的角色对调了，枚举名没跟着改，别再被名字误导：
 *
 *     枚举名       物理口      现在干什么
 *     UART_LOG  →  USART1     跑 Modbus RTU（原日志口，9600 8N1）
 *     UART_AT   →  USART2    当日志口（原 ESP8266 口，115200，只发不收）
 *     UART_MB   →  （未绑定） 传感器口预留，现场确认挂哪一路再说
 *
 *   为什么不改名：UART_AT 这个名字在 ESP8266.c / mqtt_client.c 里出现了十几次，
 *   改一趟要动三个不到万不得已不动的文件，而那两个文件现在整体停用。
 *   等 ESP8266 链路恢复、口子再切回去时，这里自然就不别扭了。
 *   现在要 Modbus 就认准一句话：UART_LOG 就是 USART1，就是 Modbus。 */
typedef enum
{
	UART_LOG = 0,		/* USART1：Modbus RTU 业务口（原日志口） */
	// UART_AT  = 1,		/* USART2：日志输出口（原 ESP8266 业务口） */
	// UART_MB  = 2,		/* 预留：现场确认传感器挂哪个 huart 前保持 NULL */
	UART_CH_NUM			/* 通道总数，必须放最后 */
} uart_ch_t;

/* 每个串口接收环形缓冲的字节数。
   必须是 2 的幂（内部用 & 取模，比 % 快）。
   1024 足够放一条几百字节的 MQTT 报文，RAM 紧张时可以降到 512。 */
#define UART_RX_BUF_SIZE    1024

/* 发送超时（毫秒） */
#define UART_TX_TIMEOUT     1000

/* ==================== 调试回显 ====================
   1 = 把 Uart_Read() 取到的数据、以及 Uart_Write() 发出的数据，
       按可读形式转发到 UART_ECHO_CH（默认 USART2）
   0 = 关闭，这段代码在编译期整段消失，零运行时开销

   当前默认 0，原因是 Modbus 阶段开不起这个开销：
   回显走的是 HAL_UART_Transmit，阻塞发送。USART2 若没接 USB-TTL，
   一帧 8 字节请求展开成 HEX 文本约 40 个字符，115200 下也要 3.5ms 白等。
   而 Modbus 的 T3.5 判帧只有 20ms 余量，每次收发都挖掉几毫秒不值。

   要看回显也不吃亏：串口助手上本来就能同时看到 MCU 发的请求和
   从站回的应答（都在 USART1 上），USART2 这路是重复信息。
   真要单独抓一路日志时再改成 1，并确认 UART_ECHO_CH 指向的不是
   Modbus 那个口（指向了就等着收 ASCII 垃圾吧）。 */
#define UART_ECHO_DEBUG     0

/* 回显输出到哪个通道。
   ⚠ 必须指向「不跑 Modbus 的那一路」，否则 Uart_Read 每次取到帧都会往
   Modbus 线上打一串 "[RX 8] 01 03 ..."，从站收到的是 ASCII 垃圾，
   帧直接对不上 CRC —— 而且因为回显是自己发的，看起来像是「主站发错帧了」，
   排查方向会被带偏。当前 Modbus 在 USART1(UART_LOG)，所以回显走 UART_AT。 */
#define UART_ECHO_CH        UART_AT

/* 函数返回值 */
#define UART_OK             0
#define UART_ERR_PARAM      (-1)    /* 参数为空 */
#define UART_ERR_STATE      (-3)    /* HAL 状态异常，启动接收失败 */

int8_t   Uart_Init(void);
void     Uart_DeInit(uart_ch_t ch);

uint16_t Uart_Available(uart_ch_t ch);
uint8_t  Uart_FrameReady(uart_ch_t ch);
uint16_t Uart_Read(uart_ch_t ch, uint8_t *buf, uint16_t len);
void     Uart_FlushRx(uart_ch_t ch);
uint32_t Uart_GetOverflow(uart_ch_t ch);

void     Uart_Write(uart_ch_t ch, const uint8_t *buf, uint16_t len);
void     Uart_Printf(uart_ch_t ch, const char *format, ...);

#endif
