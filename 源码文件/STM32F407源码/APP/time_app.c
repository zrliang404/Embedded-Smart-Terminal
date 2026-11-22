#include "time_app.h"
#include "ui.h"
#include "soft_uart.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* 已同步的本地时间（东八区），单位：秒 */
static uint8_t  s_synced = 0;
static int64_t  s_epoch  = 0;
static uint32_t s_tick0  = 0;
static uint8_t  s_tries  = 0;
static uint32_t s_last_req = 0;

/* ---------- 日期 <-> 天数 ---------- */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)(yoe + (unsigned)era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp + (mp < 10 ? 3 : -9);

    *y = yy + (mm <= 2 ? 1 : 0);
    *m = (int)mm;
    *d = (int)dd;
}

/* ---------- 刷新界面 ---------- */
static void time_refresh(void)
{
    static const char *wk[] = { "星期日", "星期一", "星期二", "星期三",
                            "星期四", "星期五", "星期六" };

    int64_t t, days;
    int y, m, d, hh, mm, ss;
    int64_t sec_of_day;

    if (!s_synced) return;

    t = s_epoch + (int64_t)((lv_tick_get() - s_tick0) / 1000u);

    days       = t / 86400;
    sec_of_day = t % 86400;
    if (sec_of_day < 0) { sec_of_day += 86400; days--; }

    civil_from_days(days, &y, &m, &d);

    hh = (int)(sec_of_day / 3600);
    mm = (int)((sec_of_day % 3600) / 60);
    ss = (int)(sec_of_day % 60);

    lv_label_set_text_fmt(ui_TimeLabel, "%02d:%02d:%02d", hh, mm, ss);

    {
        int wd = (int)((days % 7 + 11) % 7);   /* 1970-01-01 是星期四 */
        lv_label_set_text_fmt(ui_Label4, "%04d/%02d/%02d %s", y, m, d, wk[wd]);
    }
}

/* ---------- 每秒一次 ---------- */
static void time_timer_cb(lv_timer_t *tmr)
{
    (void)tmr;

    if (s_synced)
    {
        time_refresh();
    }
    else if (s_tries && (lv_tick_get() - s_last_req > 5000))
    {
        s_last_req = lv_tick_get();
        s_tries--;
        suart_send((const uint8_t *)"TIME\n", 5);
    }
}

/* ---------- 进入主界面 ---------- */
static void on_main_loaded(lv_event_t *e)
{
    (void)e;

    s_tries    = 6;                     /* 最多试 6 次（约 30 秒） */
    s_last_req = lv_tick_get();
    suart_send((const uint8_t *)"TIME\n", 5);
}

/* ---------- 解析 TIM ---------- */
int time_app_handle_line(char *s)
{
    int y, mo, d, h, mi, se;

    if (strncmp(s, "TIM ", 4) == 0)
    {
        char *p = s + 4;

        if (strchr(p, '-') != NULL)
        {
            /* 旧格式：TIM 2026-11-09 13:20:45 */
            if (sscanf(p, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) == 6)
            {
                s_epoch = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
            }
            else return 1;
        }
        else
        {
            /* 新格式：TIM <epoch>（东八区秒数） */
            s_epoch = (int64_t)atoll(p);
            if (s_epoch < 1000000000LL) return 1;      /* 明显不合法 */
        }

        s_tick0  = lv_tick_get();
        s_synced = 1;
        s_tries  = 0;
        time_refresh();
        return 1;
    }

    if (strncmp(s, "TIE", 3) == 0)
    {
        return 1;                        /* 未联网：保持未同步 */
    }

    return 0;
}

void time_app_init(void)
{
    /* 时间标签："20:00:00" 最宽约 279 px，原 271 px 会换行导致秒位下跑 */
    lv_obj_set_width(ui_TimeLabel, 288);
    lv_obj_set_height(ui_TimeLabel, 70);
    lv_label_set_long_mode(ui_TimeLabel, LV_LABEL_LONG_CLIP);

    lv_timer_create(time_timer_cb, 1000, NULL);
    lv_obj_add_event_cb(ui_Main, on_main_loaded, LV_EVENT_SCREEN_LOADED, NULL);
}
