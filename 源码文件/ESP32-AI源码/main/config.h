#ifndef __CONFIG_H
#define __CONFIG_H

/* ================= AI 服务 ================= */
#define AI_HOST     "api.deepseek.com"
#define AI_PATH     "/chat/completions"  /* 可在我->DeepSeek->API 文档中查看 */
#define AI_KEY      ""   /* 替换为你的 DeepSeek API Key */
#define AI_MODEL    "deepseek-chat"

/* ================= 心知天气 ================= */
#define SEN_KEY      ""   /* 替换为你的心知天气 API Key */
#define SEN_CITY     "beijing"
#define SEN_HOST     "api.seniverse.com"
#define SEN_PATH     "/v3/weather/now.json"  /* 可在我->心知天气->API 文档中查看 */


/* ================= 与 F407 的软件串口 =================
 * GPIO1 = RX（接 F407 的 TX）
 * GPIO0 = TX（接 F407 的 RX）
 */
#define ESP_RX        1
#define ESP_TX        0

#define LINK_BAUD     38400
#define LINE_MAX      320

/* 单条 AI 回答分片大小（字节，含 UTF-8 边界回退） */
#define AI_CHUNK      45

/* WiFi 连接超时（毫秒） */
#define WIFI_CONNECT_TIMEOUT  15000

/* NTP 服务器 */
#define NTP_SERVER1   "ntp.aliyun.com"
#define NTP_SERVER2   "ntp.ntsc.ac.cn"
#define NTP_SERVER3   "pool.ntp.org"
#define NTP_TZ_OFFSET (8 * 3600)


#endif
