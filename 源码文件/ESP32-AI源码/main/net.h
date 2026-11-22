#ifndef __NET_H
#define __NET_H

#include <Arduino.h>
#include <time.h>

/* ============================================================
 * 网络层：WiFi / NTP / 云端 AI
 * 本层【不】直接操作 Serial1，所有发送都通过下面的回调交给通信层
 * ============================================================ */

typedef struct
{
    /* AI 回答：每来一片调用一次，参数是一段 UTF-8 文本 */
    void (*ai_chunk)(const char *text);

    /* AI 回答结束（成功或失败都会调用一次） */
    void (*ai_done)(void);

    /* 上一条 AI 问题还没答完 */
    void (*ai_busy)(void);

    /* 天气获取完成（异步）；w == NULL 表示失败 */
    void (*weather_done)(const struct net_weather_s *w);

    /* WiFi 状态：0=未连接 1=已连接 2=连接中；ip 仅在 1 时有效 */
    void (*wifi_state)(int st, const char *ip);
} net_hooks_t;

typedef struct
{
    char ssid[33];
    int  rssi;
} net_ap_t;

#define NET_FC_DAYS  3

typedef struct
{
    char date[12];      /* 2026-11-13 */
    char text_day[24];  /* 白天天气 */
    char high[8];
    char low[8];
} net_forecast_t;

typedef struct net_weather_s
{
    char text[24];      /* 当前天气：晴 / 多云 / 阴 ... */
    char temp[8];       /* 当前温度（摄氏） */
    char city[24];      /* 城市名 */
    char update[32];    /* 数据更新时间（ISO8601） */

    char humidity[8];   /* 湿度 % */
    char high[8];       /* 最高温 */
    char low[8];        /* 最低温 */
    char text_day[24];  /* 白天天气 */
    char text_night[24];/* 夜间天气 */

    int            days;                 /* 预报天数（最多 NET_FC_DAYS） */
    net_forecast_t fc[NET_FC_DAYS];
} net_weather_t;

/* 初始化：注册回调、创建 AI 后台任务 */
void net_init(const net_hooks_t *hooks);

/* 主循环里调用：处理连接超时、连接成功等异步事件 */
void net_poll(void);

/* WiFi 状态：0=未连接 1=已连接 2=连接中 */
int  net_status(void);

/* 是否已连接 */
bool net_connected(void);

/* 当前 IP（未连接时返回 "0.0.0.0"） */
const char *net_ip(void);

/* 扫描周围 WiFi，返回数量（按信号从强到弱，最多 max 个） */
int  net_scan(net_ap_t *out, int max);

/* 发起连接（非阻塞，结果通过 wifi_state 回调上报） */
void net_connect(const String &ssid, const String &pass);

/* 读取当前时间（东八区）；未联网/NTP 未同步返回 false */
bool net_time(struct tm *out);

/* 设置查询城市（汉字或拼音） */
void net_set_city(const char *city);

/* 同步获取天气，成功返回 true */
bool net_weather(net_weather_t *out);

/* 异步获取：立即返回，结果通过 weather_done 回调 */
void net_weather_async(void);

/* 发起 AI 提问（非阻塞，结果通过 ai_chunk/ai_done 回调上报） */
void net_ai_ask(const String &question);

#endif
