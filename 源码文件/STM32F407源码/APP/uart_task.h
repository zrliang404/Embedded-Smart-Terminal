#ifndef __UART_TASK_H
#define __UART_TASK_H

#include <stdint.h>

/* 与 ESP32 交互的独立任务（优先级 1）
 * 只做“收字节 + 拼行”，不碰 LVGL；
 * UI 更新由 LVGL 任务通过 uart_task_pop() 取行后处理 */
void uart_task_start(void);

/* LVGL 任务调用：取一行（不阻塞），返回 1 表示取到 */
int  uart_task_pop(char *out, int max_len);

#endif
