#include "Boot_Loader.h"

uint16_t Boot_Loader_Getlen(void)
{
    uint8_t  rx_buf[512] = {0};		/* 取数据用的落地缓冲 */
    uint16_t rx_len = 0;			/* 本帧累计收到的字节数 */
    if(Uart_FrameReady(UART_LOG) == 1)
    {
        rx_len = Uart_Read(UART_LOG, rx_buf, sizeof(rx_buf));
        if(rx_len > 0)
        {
            return rx_len;
        }
    }
    return 0;
}
