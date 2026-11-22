#ifndef __BACKLIGHT_H
#define __BACKLIGHT_H

#include <stdint.h>

/* 屏幕背光：PD12 = TIM4_CH1，硬件 PWM */
void backlight_init(void);
void backlight_set(uint8_t pct);      /* 0~100 */

#endif
