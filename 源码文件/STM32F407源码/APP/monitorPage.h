#ifndef __MONITORPAGE_H
#define __MONITORPAGE_H

#include <stdint.h>

/* 启动监控：初始化 ADC + 建立 500ms 刷新定时器（只调一次） */
void monitor_init(void);

/* 四个指标（供调试或其他页面读取） */
uint8_t  monitor_cpu_pct(void);     /* 0~100   */
uint8_t  monitor_ram_pct(void);     /* 0~100   */
uint8_t  monitor_flash_pct(void);   /* 0~100   */
int16_t  monitor_temp_c(void);      /* 摄氏度  */

#endif
