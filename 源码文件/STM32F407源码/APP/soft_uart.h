#ifndef __SOFT_UART_H
#define __SOFT_UART_H

#include <stdint.h>

/* 引脚：PA11 = TX，PA12 = RX，8N1，半双工 */
void suart_init(uint32_t baud);

/* 发送（非阻塞，数据先拷进内部缓冲） */
void suart_send(const uint8_t *buf, uint16_t len);
void suart_send_str(const char *s);

/* 接收：返回 1 表示读到 1 字节 */
int  suart_read_byte(uint8_t *b);

/* 接收缓冲里还有多少字节 */
int  suart_available(void);

/* 轮询处理（放到主循环里调用，把收到的数据打印出来） */
void suart_poll(void);

#endif
