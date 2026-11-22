#ifndef __IME_KB_H
#define __IME_KB_H

#include "lvgl.h"

/* ============================================================
 * 拼音键盘（AI 页 / 天气页共用）
 *
 * 用法：
 *   ime_kb_bind(输入框, on_send, on_close);
 *   点击输入框自动弹层；点「发」回调 on_send(文本)
 * ============================================================ */

typedef void (*ime_kb_cb_t)(const char *text);

/* 绑定一个输入框（可重复调用，切换当前目标） */
void ime_kb_bind(lv_obj_t *input, ime_kb_cb_t on_send, ime_kb_cb_t on_close);

void ime_kb_open(void);
void ime_kb_close(void);
int  ime_kb_is_open(void);

/* 处理 "PYC ..." 行，返回 1 表示已消费 */
int  ime_kb_handle_line(char *s);

#endif
