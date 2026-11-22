#include "link.h"
#include "config.h"
#include "ime.h"
#include "net.h"

#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

/* ============================================================
 * 内部缓冲
 * ============================================================ */
static char     s_rx[LINE_MAX];
static uint16_t s_rx_len = 0;

static char     s_dbg[LINE_MAX];
static uint16_t s_dbg_len = 0;

static const int kMaxAp = 10;      /* 滚轮最多显示 10 个 */

/* ============================================================
 * 发送
 * ============================================================ */
static void send_raw(const char *s)
{
    Serial1.print(s);
}

static void sendf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    Serial1.print(buf);
}

/* ============================================================
 * 网络层回调实现（网络事件 → 串口应答）
 * ============================================================ */
static void on_ai_chunk(const char *text)
{
    sendf("\nAIS %s\n", text);
}

static void on_ai_done(void)
{
    send_raw("\nAIE\n");
}

static void on_ai_busy(void)
{
    send_raw("\nAIS 上一个问题还没回答完，请等一下\n\nAIE\n");
}

static void on_wifi_state(int st, const char *ip)
{
    if (st == 1)      sendf("\nWST 1 %s\n", (ip && ip[0]) ? ip : net_ip());
    else if (st == 2) send_raw("\nWST 2\n");
    else              send_raw("\nWST 0\n");
}

/* 天气获取完成（在天气任务中调用） */
static void on_weather_done(const struct net_weather_s *w)
{
    if (w == NULL)
    {
        send_raw("\nWTE\n");
        Serial.println("[link] weather -> WTE");
        return;
    }

    /* WT <天气>|<温度>|<湿度>|<城市>|<低>~<高>|<更新时间> */
    sendf("\nWT %s|%s|%s|%s|%s~%s|%s\n",
          w->text, w->temp, w->humidity, w->city, w->low, w->high, w->update);

    /* WFD <序号>|<日期>|<白天天气>|<低>|<高> */
    for (int i = 0; i < w->days; i++)
    {
        sendf("\nWFD %d|%s|%s|%s|%s\n", i + 1,
              w->fc[i].date, w->fc[i].text_day, w->fc[i].low, w->fc[i].high);
        delay(15);
    }

    Serial.printf("[link] weather -> WT %s|%s|%s|%s|%s~%s|%s\n",
                  w->text, w->temp, w->humidity, w->city, w->low, w->high, w->update);

    for (int i = 0; i < w->days; i++)
        Serial.printf("[link] forecast %d|%s|%s|%s|%s\n", i + 1,
                      w->fc[i].date, w->fc[i].text_day, w->fc[i].low, w->fc[i].high);
}

static const net_hooks_t kHooks = {
    on_ai_chunk,
    on_ai_done,
    on_ai_busy,
    on_weather_done,
    on_wifi_state
};

const net_hooks_t *link_hooks(void)
{
    return &kHooks;
}

/* ============================================================
 * 命令处理
 * ============================================================ */
static void do_scan(void)
{
    net_ap_t aps[kMaxAp];
    int      n = net_scan(aps, kMaxAp);

    sendf("\nWSL %d\n", n);

    for (int i = 0; i < n; i++)
    {
        Serial.printf("[link] ap%d %s (%d)\n", i + 1, aps[i].ssid, aps[i].rssi);
        sendf("\nWSS %d|%d|%s\n", i + 1, aps[i].rssi, aps[i].ssid);
        delay(15);
    }
}

static void do_time(void)
{
    struct tm tmv;

    if (net_time(&tmv))
    {
        sendf("\nTIM %04d-%02d-%02d %02d:%02d:%02d\n",
              tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
              tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    }
    else
    {
        send_raw("\nTIE\n");
    }
}

static void do_wstat(void)
{
    int st = net_status();

    if (st == 1)
    {
        sendf("\nWST 1 %s\n", net_ip());
    }
    else if (st == 2)
    {
        send_raw("\nWST 2\n");
    }
    else
    {
        send_raw("\nWST 0\n");
    }
}


static void do_weather(void)
{
    /* 异步：立即返回，结果走 on_weather_done 回调 */
    net_weather_async();
}

static void do_ime(const String &arg)
{
    String py   = arg;
    int    page = 1;
    int    sp   = arg.lastIndexOf(' ');

    if (sp > 0)
    {
        page = arg.substring(sp + 1).toInt();
        if (page < 1) page = 1;
        py = arg.substring(0, sp);
    }

    String cand = ime_lookup(py, page);
    if (cand.length() == 0) cand = "NA";

    Serial.printf("[link] ime %s p%d -> %s\n", py.c_str(), page, cand.c_str());
    sendf("\nPYC %s\n", cand.c_str());
}

static void handle_line(String cmd)
{
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.printf("[f407] %s\n", cmd.c_str());

    if (cmd == "RST")
    {
        send_raw("\nRSTACK\n");
        Serial1.flush();
        delay(150);
        Serial.println("[link] restart by F407");
        ESP.restart();
    }
    else if (cmd == "PING")
    {
        send_raw("\nPONG\n");
    }
    else if (cmd == "TIME")
    {
        do_time();
    }
    else if (cmd == "WSTAT")
    {
        do_wstat();
    }
    else if (cmd == "TQ")
    {
        do_weather();
    }
    else if (cmd.startsWith("WCITY "))
    {
        net_set_city(cmd.substring(6).c_str());
        send_raw("\nWCT OK\n");
    }
    else if (cmd == "WSCAN")
    {
        do_scan();
    }
    else if (cmd.startsWith("WCON "))
    {
        String arg  = cmd.substring(5);
        int    bar  = arg.indexOf('|');
        String ssid = (bar > 0) ? arg.substring(0, bar) : arg;
        String pass = (bar > 0) ? arg.substring(bar + 1) : String("");

        net_connect(ssid, pass);
    }
    else if (cmd.startsWith("PY "))
    {
        do_ime(cmd.substring(3));
    }
    else if (cmd.startsWith("AI "))
    {
        net_ai_ask(cmd.substring(3));
    }
    else
    {
        send_raw("ERR\n");
    }
}

/* ============================================================
 * 收字节
 * ============================================================ */
static void feed(char c, char *buf, uint16_t *len)
{
    if (c == '\n' || c == '\r')
    {
        if (*len > 0)
        {
            buf[*len] = 0;
            handle_line(String(buf));
            *len = 0;
        }
    }
    else if (*len < LINE_MAX - 1)
    {
        buf[(*len)++] = c;
    }
    else
    {
        *len = 0;          /* 超长丢弃整行 */
    }
}

void link_init(void)
{
    Serial1.begin(LINK_BAUD, SERIAL_8N1, ESP_RX, ESP_TX);
    Serial1.setTimeout(100);

    s_rx_len  = 0;
    s_dbg_len = 0;

    send_raw("\nWST 0\n");       /* 开机上报：未连接 */
    send_raw("\nBOOT\n");        /* 告诉 F407：我已重启完毕 */
}

void link_poll(void)
{
    while (Serial1.available())
    {
        feed((char)Serial1.read(), s_rx, &s_rx_len);
    }

    while (Serial.available())
    {
        feed((char)Serial.read(), s_dbg, &s_dbg_len);
    }
}
