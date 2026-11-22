#include "net.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <string.h>

/* ============================================================
 * 内部状态
 * ============================================================ */
static const net_hooks_t *s_hooks = NULL;

static String   s_ssid;                 /* 当前连接的 SSID（仅 RAM） */
static String   s_pass;                 /* 当前连接的密码（仅 RAM） */
static bool     g_connecting = false;
static uint32_t g_conn_t0    = 0;

static bool     g_ntp_started = false;

static volatile bool g_ai_busy = false;
static TaskHandle_t  g_ai_task = NULL;
static String        g_ai_q;

/* ============================================================
 * 回调封装（防止空指针）
 * ============================================================ */
static void hook_ai_chunk(const char *t)
{
    if (s_hooks && s_hooks->ai_chunk) s_hooks->ai_chunk(t);
}

static void hook_ai_done(void)
{
    if (s_hooks && s_hooks->ai_done) s_hooks->ai_done();
}

static void hook_ai_busy(void)
{
    if (s_hooks && s_hooks->ai_busy) s_hooks->ai_busy();
}

static void hook_wifi_state(int st, const char *ip)
{
    if (s_hooks && s_hooks->wifi_state) s_hooks->wifi_state(st, ip);
}

/* ============================================================
 * NTP
 * ============================================================ */
static void start_ntp(void)
{
    if (g_ntp_started) return;

    configTime(NTP_TZ_OFFSET, 0, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
    g_ntp_started = true;
    Serial.println("[net] ntp config done");
}

/* ============================================================
 * AI：后台任务 + 任务通知
 * ============================================================ */
static void json_escape(const String &in, String &out)
{
    out = "";
    for (size_t i = 0; i < in.length(); i++)
    {
        char c = in[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
}

static void ai_request(const String &question)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[net] ai: wifi not connected");
        hook_ai_done();
        return;
    }

    String esc;
    json_escape(question, esc);

    String body = "{\"model\":\"" AI_MODEL "\",\"messages\":["
                  "{\"role\":\"system\",\"content\":\"你是AI助手，回答简洁，控制在80字以内，不要用markdown。\"},"
                  "{\"role\":\"user\",\"content\":\"" + esc + "\"}],"
                  "\"stream\":false,\"max_tokens\":300}";

    Serial.printf("[net] ai ask: %s\n", question.c_str());

    int    code = -1;
    String resp = "";

    for (int attempt = 0; attempt < 2; attempt++)
    {
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(8000);

        HTTPClient http;
        http.setTimeout(10000);

        if (!http.begin(client, String("https://") + AI_HOST + AI_PATH))
        {
            Serial.println("[net] ai begin failed");
            delay(300);
            continue;
        }

        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", String("Bearer ") + AI_KEY);

        code = http.POST(body);

        int size = http.getSize();
        if (code > 0) resp = http.getString();
        http.end();

        Serial.printf("[net] ai try%d code=%d size=%d len=%u heap=%u\n",
                      attempt + 1, code, size, (unsigned)resp.length(),
                      (unsigned)ESP.getFreeHeap());

        if (resp.length() > 0) break;

        delay(400);
    }

    if (code != 200)
    {
        Serial.printf("[net] ai body: %s\n", resp.c_str());
        hook_ai_done();
        return;
    }

    /* 取 choices[0].message.content */
    String answer;
    int i = resp.indexOf("\"content\":\"");
    if (i >= 0)
    {
        i += 11;
        while (i < (int)resp.length())
        {
            char c = resp[i++];
            if (c == '\\')
            {
                char n = resp[i++];
                if (n == 'n') answer += '\n';
                else if (n == 't') answer += ' ';
                else if (n == 'r') {}
                else answer += n;
            }
            else if (c == '"') break;
            else answer += c;
        }
    }

    if (answer.length() == 0)
    {
        Serial.printf("[net] ai EMPTY. resp len=%u\n", (unsigned)resp.length());
        Serial.println(resp.substring(0, 300));
    }

    /* 分片回调：每片 ≤ AI_CHUNK 字节，且不切断 UTF-8 字符 */
    int pos = 0;

    while (pos < (int)answer.length())
    {
        int end = pos + AI_CHUNK;

        while (end < (int)answer.length() && (answer[end] & 0xC0) == 0x80) end--;
        if (end <= pos) end = pos + AI_CHUNK;

        String piece = answer.substring(pos, end);
        hook_ai_chunk(piece.c_str());

        pos = end;
        delay(20);
    }

    hook_ai_done();
}

static void ai_worker(void *arg)
{
    (void)arg;

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (g_ai_q.length() == 0) { g_ai_busy = false; continue; }

        String q = g_ai_q;
        g_ai_q = "";

        ai_request(q);

        g_ai_busy = false;
    }
}

void net_ai_ask(const String &question)
{
    if (g_ai_busy)
    {
        hook_ai_busy();
        return;
    }

    g_ai_busy = true;
    g_ai_q    = question;

    if (g_ai_task) xTaskNotifyGive(g_ai_task);
}

/* ============================================================
 * WiFi
 * ============================================================ */
int net_status(void)
{
    if (WiFi.status() == WL_CONNECTED) return 1;
    if (g_connecting)                  return 2;
    return 0;
}

bool net_connected(void)
{
    return WiFi.status() == WL_CONNECTED;
}

const char *net_ip(void)
{
    static String ip;

    if (WiFi.status() != WL_CONNECTED) return "0.0.0.0";

    ip = WiFi.localIP().toString();
    return ip.c_str();
}

void net_connect(const String &ssid, const String &pass)
{
    Serial.printf("[net] connect %s (pass %u chars)\n",
                  ssid.c_str(), (unsigned)pass.length());

    WiFi.disconnect(true);
    delay(50);
    WiFi.begin(ssid.c_str(), pass.c_str());

    s_ssid = ssid;
    s_pass = pass;

    g_connecting = true;
    g_conn_t0    = millis();

    hook_wifi_state(2, NULL);
}

int net_scan(net_ap_t *out, int max)
{
    if (out == NULL || max <= 0) return 0;

    Serial.println("[net] scan start ...");

    int n = WiFi.scanNetworks(false, false);      /* 同步扫描，约 2 秒 */
    Serial.printf("[net] scan found %d\n", n);

    int cnt = 0;

    if (n > 0)
    {
        int idx[32];
        int k = 0;

        for (int i = 0; i < n && k < 32; i++)
        {
            if (WiFi.SSID(i).length() == 0) continue;   /* 隐藏 SSID 跳过 */
            idx[k++] = i;
        }

        /* 按信号强度排序 */
        for (int a = 0; a < k - 1; a++)
            for (int b = a + 1; b < k; b++)
                if (WiFi.RSSI(idx[b]) > WiFi.RSSI(idx[a]))
                {
                    int tmp = idx[a]; idx[a] = idx[b]; idx[b] = tmp;
                }

        if (k > max) k = max;

        for (int i = 0; i < k; i++)
        {
            strncpy(out[i].ssid, WiFi.SSID(idx[i]).c_str(), sizeof(out[i].ssid) - 1);
            out[i].ssid[sizeof(out[i].ssid) - 1] = 0;
            out[i].rssi = WiFi.RSSI(idx[i]);
        }

        cnt = k;
    }

    WiFi.scanDelete();

    /* 扫描可能短暂中断连接：扫完根据现状上报 */
    if (WiFi.status() == WL_CONNECTED)
    {
        hook_wifi_state(1, WiFi.localIP().toString().c_str());
    }
    else if (s_ssid.length())
    {
        Serial.println("[net] scan: link lost, reconnecting ...");
        WiFi.begin(s_ssid.c_str(), s_pass.c_str());

        g_connecting = true;
        g_conn_t0    = millis();

        hook_wifi_state(2, NULL);
    }
    else
    {
        hook_wifi_state(0, NULL);
    }

    return cnt;
}

void net_poll(void)
{
    if (!g_connecting) return;

    if (WiFi.status() == WL_CONNECTED)
    {
        g_connecting = false;

        start_ntp();

        Serial.printf("[net] wifi OK, IP=%s\n", WiFi.localIP().toString().c_str());
        hook_wifi_state(1, WiFi.localIP().toString().c_str());
    }
    else if (millis() - g_conn_t0 > WIFI_CONNECT_TIMEOUT)
    {
        g_connecting = false;
        Serial.println("[net] wifi connect FAILED");
        hook_wifi_state(0, NULL);
    }
}


/* ============================================================
 * 天气（心知天气）
 * ============================================================ */
static bool json_field(const String &s, const char *key, char *out, size_t n)
{
    String pat = String("\"") + key + "\":\"";
    int i = s.indexOf(pat);

    if (i < 0) return false;

    i += pat.length();

    int j = s.indexOf('"', i);
    if (j < 0) return false;

    String v = s.substring(i, j);

    strncpy(out, v.c_str(), n - 1);
    out[n - 1] = 0;

    return true;
}

/* 发一次 GET，成功返回 true 并把 body 存入 out */
static bool sen_get(const String &path_query, String &body)
{
    String url = String("http://") + SEN_HOST + path_query;

    WiFiClient client;
    HTTPClient http;

    http.setTimeout(8000);

    if (!http.begin(client, url))
    {
        Serial.println("[net] sen: begin failed");
        return false;
    }

    int code = http.GET();

    if (code == 200) body = http.getString();
    http.end();

    Serial.printf("[net] sen code=%d len=%u\n", code, (unsigned)body.length());

    if (code != 200 || body.length() == 0)
    {
        Serial.printf("[net] sen body: %s\n", body.substring(0, 160).c_str());
        return false;
    }

    return true;
}

/* 当前查询城市（默认 config.h 里的 SEN_CITY） */
static String s_city = SEN_CITY;

void net_set_city(const char *city)
{
    if (city == NULL || city[0] == 0) return;

    s_city = city;
    Serial.printf("[net] city -> %s\n", s_city.c_str());
}

/* UTF-8 → 百分号编码（中文城市名必须编码） */
static String url_encode(const String &in)
{
    static const char *hex = "0123456789ABCDEF";
    String out = "";

    for (size_t i = 0; i < in.length(); i++)
    {
        unsigned char c = (unsigned char)in[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += (char)c;
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }

    return out;
}

bool net_weather(net_weather_t *out)
{
    String body;

    if (out == NULL) return false;

    memset(out, 0, sizeof(*out));

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[net] weather: wifi not connected");
        return false;
    }

    /* ---------- 1) 实时天气：天气现象 + 温度 ---------- */
    body = "";

    if (!sen_get(String(SEN_PATH) + "?key=" + SEN_KEY +
                 "&location=" + url_encode(s_city) + "&language=zh-Hans&unit=c", body))
        return false;

    if (!json_field(body, "text", out->text, sizeof(out->text)))
    {
        Serial.println("[net] weather: no text field");
        return false;
    }

    json_field(body, "temperature", out->temp,     sizeof(out->temp));
    json_field(body, "name",        out->city,     sizeof(out->city));
    json_field(body, "last_update", out->update,   sizeof(out->update));

    /* ---------- 2) 逐日天气：湿度 + 高低温 + 未来 3 天 ---------- */
    body = "";

    if (sen_get(String("/v3/weather/daily.json?key=") + SEN_KEY +
                "&location=" + url_encode(s_city) + "&language=zh-Hans&unit=c&start=0&days=3", body))
    {
        /* 拿到 "daily":[ 之后的内容 */
        int p0 = body.indexOf("\"daily\":[");

        if (p0 >= 0)
        {
            p0 += 9;

            for (int i = 0; i < NET_FC_DAYS; i++)
            {
                /* 统一用 "date":" 作分隔（不带前面的 { ），
                 * 否则第二轮会跳过下一天 */
                int d = body.indexOf("\"date\":\"", p0);

                if (d < 0) break;

                int end = body.indexOf("\"date\":\"", d + 8);
                if (end < 0) end = body.length();

                String day = body.substring(d, end);

                if (i == 0)
                {
                    /* 第 1 天就是当天：湿度 / 高低温 / 白天天气 */
                    json_field(day, "humidity",   out->humidity,   sizeof(out->humidity));
                    json_field(day, "high",       out->high,       sizeof(out->high));
                    json_field(day, "low",        out->low,        sizeof(out->low));
                    json_field(day, "text_day",   out->text_day,   sizeof(out->text_day));
                    json_field(day, "text_night", out->text_night, sizeof(out->text_night));
                }

                json_field(day, "date",     out->fc[i].date,     sizeof(out->fc[i].date));
                json_field(day, "text_day", out->fc[i].text_day, sizeof(out->fc[i].text_day));
                json_field(day, "high",     out->fc[i].high,     sizeof(out->fc[i].high));
                json_field(day, "low",      out->fc[i].low,      sizeof(out->fc[i].low));

                out->days = i + 1;
                p0 = end;
            }
        }
    }
    else
    {
        Serial.println("[net] weather: daily failed, no humidity");
    }

    Serial.printf("[net] weather: %s %s C  %s  hum=%s%%  %s~%s  days=%d\n",
                  out->city, out->temp, out->text,
                  out->humidity, out->low, out->high, out->days);

    for (int i = 0; i < out->days; i++)
        Serial.printf("[net]   fc%d %s %s %s~%s\n", i + 1,
                      out->fc[i].date, out->fc[i].text_day,
                      out->fc[i].low, out->fc[i].high);

    return true;
}


/* ============================================================
 * 天气：异步获取（不阻塞主循环，不影响串口）
 * ============================================================ */
static TaskHandle_t  g_wx_task = NULL;
static volatile bool g_wx_busy = false;

static void weather_worker(void *arg)
{
    (void)arg;

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        {
            net_weather_t w;
            bool ok = net_weather(&w);

            if (s_hooks && s_hooks->weather_done)
                s_hooks->weather_done(ok ? &w : NULL);
        }

        g_wx_busy = false;
    }
}

void net_weather_async(void)
{
    if (g_wx_busy)
    {
        Serial.println("[net] weather: busy, ignore");
        return;
    }

    g_wx_busy = true;

    if (g_wx_task) xTaskNotifyGive(g_wx_task);
}

/* ============================================================
 * 时间
 * ============================================================ */
bool net_time(struct tm *out)
{
    if (out == NULL) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    time_t now_t = time(NULL);
    struct tm tmv;

    localtime_r(&now_t, &tmv);

    if ((tmv.tm_year + 1900) < 2024) return false;   /* NTP 还没同步 */

    *out = tmv;
    return true;
}

/* ============================================================
 * 初始化
 * ============================================================ */
void net_init(const net_hooks_t *hooks)
{
    s_hooks = hooks;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    Serial.println("[net] boot: no auto connect, wait for user");

    xTaskCreate(ai_worker,      "aiworker", 16384, NULL, 1, &g_ai_task);
    xTaskCreate(weather_worker, "wxworker",  8192, NULL, 1, &g_wx_task);
}
