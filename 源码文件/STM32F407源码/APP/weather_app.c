#include "weather_app.h"
#include "ui.h"
#include "soft_uart.h"
#include "ime_kb.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FC_MAX   3

/* 三天预报，每行已排好格式："11/12 雾 1~15" */
static char    s_fc[FC_MAX][40];
static uint8_t s_fc_n = 0;

/* ---------------- 发送刷新命令 ---------------- */
static void request_weather(void)
{
    suart_send((const uint8_t *)"TQ\n", 3);

    lv_label_set_text(ui_Weather_Label, "刷新中...");
    /* 城市框不清空：保留用户输入的内容 */
    lv_label_set_text(ui_TemperatureLabel, "--");
    lv_label_set_text(ui_HumidityLabel, "--");
    lv_label_set_text(ui_LastTimeUpdataLabel, "更新中");
}

/* ---------------- 事件 ---------------- */

static void on_page_unloaded(lv_event_t *e)
{
    (void)e;
    ime_kb_close();
}

static void on_refresh(lv_event_t *e)
{
    (void)e;

    s_fc_n = 0;
    lv_roller_set_options(ui_WeatherForecastRoller, "暂无数据", LV_ROLLER_MODE_NORMAL);

    request_weather();
}


/* ---------------- 城市输入 ---------------- */
static void city_on_send(const char *text)
{
    char cmd[96];

    if (text == NULL || text[0] == 0) return;

    snprintf(cmd, sizeof(cmd), "WCITY %s", text);
    suart_send((const uint8_t *)cmd, (uint16_t)strlen(cmd));
    suart_send((const uint8_t *)"\n", 1);

    request_weather();          /* 换城市后立即查一次 */
}

/* ---------------- 工具 ---------------- */
/* 从 "2026-11-12" 取 "11/12" */
static void date_to_md(const char *date, char *out, int n)
{
    const char *p = date;

    out[0] = 0;

    while (*p && *p != '-') p++;      /* 跳年 */
    if (!*p) return;
    p++;                              /* 月 */

    out[0] = p[0];
    out[1] = p[1];
    out[2] = '/';

    p += 2;
    if (*p == '-') p++;

    out[3] = p[0];
    out[4] = p[1];
    out[5] = 0;

    (void)n;
}

/* 从 ISO 时间取 "20:44" */
static const char *time_hhmm(const char *iso)
{
    const char *t = strchr(iso, 'T');

    if (t == NULL) return iso;
    return t + 1;
}

/* 重建滕轮 */
static void roller_rebuild(void)
{
    static char buf[FC_MAX * 44];
    uint16_t len = 0;
    uint8_t  i;

    buf[0] = 0;

    for (i = 0; i < s_fc_n; i++)
    {
        if (s_fc[i][0] == 0) continue;          /* 跳过没收到的那一天 */

        len += (uint16_t)snprintf(buf + len, sizeof(buf) - len, "%s%s",
                                  (len ? "\n" : ""), s_fc[i]);
    }

    if (len == 0) snprintf(buf, sizeof(buf), "暂无数据");

    lv_roller_set_options(ui_WeatherForecastRoller, buf, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(ui_WeatherForecastRoller, 0, LV_ANIM_OFF);
}

/* ---------------- 解析 ---------------- */
/* WT <天气>|<温度>|<湿度>|<城市>|<低>~<高>|<更新时间> */
static int parse_wt(char *s)
{
    char *f[6];
    int   n = 0;
    char *p = s + 3;              /* 跳过 "WT " */
    char  temp[48];

    f[n++] = p;

    while (*p && n < 6)
    {
        if (*p == '|')
        {
            *p = 0;
            f[n++] = p + 1;
        }
        p++;
    }

    if (n < 5) return 1;

    lv_label_set_text(ui_Weather_Label, f[0]);              /* 天气 */
    snprintf(temp, sizeof(temp), "%s度", f[1]);
    lv_label_set_text(ui_TemperatureLabel, temp);           /* 温度 */
    snprintf(temp, sizeof(temp), "%s%%", f[2]);
    lv_label_set_text(ui_HumidityLabel, temp);              /* 湿度 */
    lv_textarea_set_text(ui_CityArea, f[3]);                /* 城市 */

    /* 主界面：温度：5度 天气：雾 */
    {
        char MainPage_Weathertxt[64];

        snprintf(MainPage_Weathertxt, sizeof(MainPage_Weathertxt), "温度：%s度",f[1]);
        lv_label_set_text(ui_WeatherLabel1, MainPage_Weathertxt);
        snprintf(MainPage_Weathertxt, sizeof(MainPage_Weathertxt), "天气：%s",f[0]);
        lv_label_set_text(ui_WeatherLabel2, MainPage_Weathertxt);
    }

    if (n >= 6)                                             /* 更新时间 */
    {
        snprintf(temp, sizeof(temp), "上一次更新时间 %.5s", time_hhmm(f[5]));   /* 更新 20:44 */
        lv_label_set_text(ui_LastTimeUpdataLabel, temp);
    }

    return 1;
}

/* WFD <序号>|<日期>|<白天天气>|<低>|<高> */
static int parse_wfd(char *s)
{
    char *f[5];
    int   n = 0;
    char *p = s + 4;              /* 跳过 "WFD " */
    char  md[8];

    f[n++] = p;

    while (*p && n < 5)
    {
        if (*p == '|')
        {
            *p = 0;
            f[n++] = p + 1;
        }
        p++;
    }

    if (n < 5) return 1;

    {
        int idx = atoi(f[0]) - 1;

        if (idx < 0 || idx >= FC_MAX) return 1;

        date_to_md(f[1], md, sizeof(md));

        snprintf(s_fc[idx], sizeof(s_fc[0]), "%s %s %s~%s", md, f[2], f[3], f[4]);

        if (idx + 1 > s_fc_n) s_fc_n = (uint8_t)(idx + 1);

        roller_rebuild();
    }

    return 1;
}

int weather_app_handle_line(char *s)
{
    if (strncmp(s, "WT ", 3) == 0)
    {
        return parse_wt(s);
    }

    if (strncmp(s, "WFD ", 4) == 0)
    {
        return parse_wfd(s);
    }

    if (strncmp(s, "WTE", 3) == 0)
    {
        lv_label_set_text(ui_Weather_Label, "获取失败");
        lv_label_set_text(ui_TemperatureLabel, "--");
        lv_label_set_text(ui_HumidityLabel, "--");
        lv_label_set_text(ui_LastTimeUpdataLabel, "请检查网络");
        lv_label_set_text(ui_WeatherLabel1, "天气：获取失败");
        return 1;
    }

    return 0;
}

/* ---------------- 初始化 ---------------- */
void weather_app_init(void)
{
    s_fc_n = 0;

    /* 进入页面不自动刷新，只有点「更新」图标才刷新 */
    lv_obj_add_event_cb(ui_Weather_Updata, on_refresh, LV_EVENT_CLICKED, NULL);

    /* 点城市框弹键盘；离开页面时收起 */
    ime_kb_bind(ui_CityArea, city_on_send, NULL);
    lv_obj_add_event_cb(ui_Weather_Page, on_page_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);

    roller_rebuild();
}
