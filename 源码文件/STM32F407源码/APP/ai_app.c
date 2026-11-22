#include "ai_app.h"
#include "ui.h"
#include "soft_uart.h"
#include "wifi_app.h"
#include "time_app.h"
#include "weather_app.h"
#include "uart_task.h"
#include "ime_kb.h"

#include <string.h>
#include <stdio.h>

/* ============================================================
 * AI 页：输出框 + 串口协议
 * 键盘（拼音输入）已抽到 ime_kb 模块，AI 页与天气页共用
 * ============================================================ */

extern lv_font_t *g_bin_font;

static lv_obj_t *s_out;
static lv_obj_t *s_in;
static uint8_t   s_wait_ai;

static const lv_font_t *s_font;

/* ================= 串口 ================= */
static void send_line(const char *s)
{
    suart_send((const uint8_t *)s, (uint16_t)strlen(s));
    suart_send((const uint8_t *)"\n", 1);
}

/* ================= 键盘「发」回调 ================= */
static void ai_on_send(const char *text)
{
    char cmd[768];

    if (text == NULL || text[0] == 0) return;

    snprintf(cmd, sizeof(cmd), "AI %s", text);

    lv_textarea_set_text(s_out, "");
    s_wait_ai = 1;

    send_line(cmd);
}

/* ================= 协议 ================= */
static void handle_line(char *s)
{
    char *m;

    if (wifi_app_handle_line(s))    return;
    if (time_app_handle_line(s))    return;
    if (weather_app_handle_line(s)) return;
    if (ime_kb_handle_line(s))      return;      /* PYC 交给键盘模块 */

    /* 容忍行首垃圾：从任何位置找到标记 */
    m = strstr(s, "AIS ");
    if (m == NULL) m = strstr(s, "AIE");
    if (m == NULL) return;

    s = m;

    if (strncmp(s, "AIS ", 4) == 0)
    {
        if (s_wait_ai == 0 || lv_textarea_get_text(s_out)[0] == 0)
        {
            lv_textarea_set_text(s_out, "");
            s_wait_ai = 1;
        }

        lv_textarea_add_text(s_out, s + 4);
        lv_textarea_add_text(s_out, "\n");

        lv_textarea_set_cursor_pos(s_out, LV_TEXTAREA_CURSOR_LAST);
        lv_obj_scroll_to_y(s_out, lv_obj_get_scroll_bottom(s_out), LV_ANIM_OFF);
    }
    else if (strncmp(s, "AIE", 3) == 0)
    {
        s_wait_ai = 0;
    }
}

static void on_page_unloaded(lv_event_t *e)
{
    (void)e;
    ime_kb_close();
}

/* ================= 初始化 ================= */
void ai_app_init(lv_obj_t *kb_old, lv_obj_t *input, lv_obj_t *output)
{
    lv_font_glyph_dsc_t d;

    s_in  = input;
    s_out = output;

    if (s_in == NULL || s_out == NULL) return;

    if (kb_old) lv_obj_del(kb_old);          /* 旧页面的 lv_keyboard 删掉 */

    s_font = g_bin_font;
    if (!(s_font && lv_font_get_glyph_dsc(s_font, &d, 0x62FC, 0))) s_font = NULL;

    if (s_font)
    {
        lv_obj_set_style_text_font(s_out, s_font, LV_PART_MAIN);
    }

    /* 输出框 */
    lv_textarea_set_text(s_out, "");
    lv_textarea_set_placeholder_text(s_out, "点下面输入框开始对话");
    lv_obj_set_style_anim_time(s_out, 0, 0);

    /* 输入框：交给键盘模块统一配置（字体/单行/滚动/最大长度） */
    ime_kb_bind(s_in, ai_on_send, NULL);
    lv_obj_add_event_cb(ui_AIPage, on_page_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);

    s_wait_ai = 0;
}

/* ================= 轮询 ================= */
void ai_app_poll(void)
{
    char line[128];

    while (uart_task_pop(line, (int)sizeof(line)))
    {
        handle_line(line);
    }
}
