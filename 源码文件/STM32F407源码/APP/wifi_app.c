#include "wifi_app.h"
#include "ui.h"
#include "soft_uart.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define WIFI_MAX_AP   10

static char    s_ssid[WIFI_MAX_AP][34];
static uint8_t s_n;
static char    s_sel[34];

/* ---------------- 发送 ---------------- */
static void send_cmd(const char *s)
{
    suart_send((const uint8_t *)s, (uint16_t)strlen(s));
    suart_send((const uint8_t *)"\n", 1);
}

/* ---------------- 新建滚轮选项 ---------------- */
static void roller_rebuild(void)
{
    static char buf[WIFI_MAX_AP * 36];
    uint16_t len = 0;
    uint8_t  i;

    buf[0] = 0;

    for (i = 0; i < s_n; i++)
    {
        len += (uint16_t)snprintf(buf + len, sizeof(buf) - len, "%s%s",
                                  (i ? "\n" : ""), s_ssid[i]);
    }

    if (s_n == 0) snprintf(buf, sizeof(buf), "未找到WiFi");

    lv_roller_set_options(ui_Roller1, buf, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(ui_Roller1, 0, LV_ANIM_OFF);
}

/* ---------------- 状态显示 ---------------- */
/* 只改文字 */
static void set_text(const char *txt)
{
    lv_label_set_text(ui_Label14, txt);
}

/* 只改图标（设置页 + 主界面） */
static void set_icon(uint8_t linked)
{
    if (linked)
    {
        lv_obj_add_state(ui_SettingPagewifiState, LV_STATE_CHECKED);
        lv_obj_add_state(ui_MainwifiState,        LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_clear_state(ui_SettingPagewifiState, LV_STATE_CHECKED);
        lv_obj_clear_state(ui_MainwifiState,        LV_STATE_CHECKED);
    }
}

/* ---------------- 事件 ---------------- */
static void on_settings_loaded(lv_event_t *e)
{
    (void)e;

    /* 不管是否已连接，都扫描；连接状态由 ESP32 扫完后回报 */
    s_n = 0;
    roller_rebuild();
    set_text("扫描中...");
    send_cmd("WSCAN");
}

/* 长按滕轮：强制重新扫描（换网络用） */
static void on_roller_long(lv_event_t *e)
{
    (void)e;

    s_n = 0;
    roller_rebuild();
    set_text("扫描中...");
    send_cmd("WSCAN");
}

static void on_btn_ok(lv_event_t *e)
{
    (void)e;

    /* 记下滕轮选中的 SSID（SquareLine 的回调会把密码面板显示出来） */
    uint16_t idx = lv_roller_get_selected(ui_Roller1);

    s_sel[0] = 0;

    if (idx < s_n) snprintf(s_sel, sizeof(s_sel), "%s", s_ssid[idx]);

    lv_textarea_set_text(ui_Password, "");
    lv_keyboard_set_textarea(ui_KeyboardPass, ui_Password);
    lv_obj_clear_flag(ui_PasswordPanel, LV_OBJ_FLAG_HIDDEN);
}

static void on_btn_pass(lv_event_t *e)
{
    (void)e;

    const char *pw = lv_textarea_get_text(ui_Password);
    char cmd[96];

    lv_obj_add_flag(ui_PasswordPanel, LV_OBJ_FLAG_HIDDEN);

    if (s_sel[0] == 0)
    {
        set_text("未选网络");   set_icon(0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "WCON %s|%s", s_sel, pw);
    send_cmd(cmd);

    set_text("连接中...");
}


/* 主界面加载：查询当前 WiFi 状态 */
static void on_main_loaded(lv_event_t *e)
{
    (void)e;
    send_cmd("WSTAT");
}

/* ---------------- 串口发来的数据 ---------------- */
static int parse_wss(char *s)
{
    /* WSS <i>|<rssi>|<ssid> */
    char *p1 = strchr(s, '|');
    if (p1 == NULL) return 1;

    char *p2 = strchr(p1 + 1, '|');
    if (p2 == NULL) return 1;

    uint8_t idx = (uint8_t)atoi(s + 4);          /* 跳过 "WSS " */

    if (idx >= 1 && idx <= WIFI_MAX_AP)
    {
        snprintf(s_ssid[idx - 1], sizeof(s_ssid[0]), "%s", p2 + 1);

        if (idx > s_n) s_n = idx;

        roller_rebuild();        /* 每收到一个就刷新滕轮 */
    }

    return 1;
}

int wifi_app_handle_line(char *s)
{
    if (strncmp(s, "WSL ", 4) == 0)
    {
        int n = atoi(s + 4);

        if (n < 0)  n = 0;
        if (n > WIFI_MAX_AP) n = WIFI_MAX_AP;

        s_n = 0;
        roller_rebuild();                        /* 先清空 */

        if (n == 0) set_text("未找到WiFi");

        return 1;
    }

    if (strncmp(s, "WSS ", 4) == 0)
    {
        return parse_wss(s);
    }

    if (strncmp(s, "WST ", 4) == 0)
    {
        if (s[4] == '1')      { set_text("已连接");     set_icon(1); }
        else if (s[4] == '2') set_text("连接中...");
        else                  { set_text("连接失败"); set_icon(0); }

        return 1;
    }

    return 0;
}

/* ---------------- 初始化 ---------------- */
void wifi_app_init(void)
{
    s_n   = 0;
    s_sel[0] = 0;

    lv_obj_add_event_cb(ui_Main, on_main_loaded, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_SettingsPage, on_settings_loaded, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_Roller1, on_roller_long, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(ui_Button1, on_btn_ok,   LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button2, on_btn_pass, LV_EVENT_CLICKED, NULL);

    roller_rebuild();
}
