#ifndef __AI_APP_H
#define __AI_APP_H

#include "lvgl.h"

/* AI 助手页逻辑：键盘输入拼音 -> 转汉字 -> 发送给 ESP32 -> 显示回答 */
void ai_app_init(lv_obj_t *kb, lv_obj_t *input, lv_obj_t *output);
void ai_app_poll(void);

#endif
