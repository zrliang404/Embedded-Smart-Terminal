#ifndef __UI_USER_H
#define __UI_USER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* 在 ui_init() 之后调用一次，注册自定义事件
 * 注意：本文件放在 APP 目录，不要放进 UI 目录。
 *       SquareLine 导出会清空 UI 目录，导致文件被删。 */
void ui_user_init(void);

#ifdef __cplusplus
}
#endif

#endif
