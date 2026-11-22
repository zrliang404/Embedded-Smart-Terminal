/* ============================================================
 * F407 AI 助手（ESP32-C3）
 *
 * 模块划分：
 *   config.h  —— 引脚、波特率、密钥等配置
 *   link.*    —— 与 F407 的串口协议（收命令 / 发应答）
 *   net.*     —— 网络层（WiFi 扫描/连接、NTP 时间、云端 AI）
 *   ime.*     —— 拼音输入法
 *
 * 本文件只负责把各模块接起来。
 * ============================================================ */

#include <Arduino.h>

#include "config.h"
#include "link.h"
#include "net.h"

void setup(void)
{
    Serial.begin(115200);
    delay(800);

    Serial.println();
    Serial.println("==== F407 AI Assistant (IME + Cloud AI) ====");

    link_init();                  /* 串口就绪，上报 BOOT / WST 0 */
    net_init(link_hooks());       /* 注册网络事件回调，创建 AI 任务 */

    Serial.printf("[sys] heap = %u\n", (unsigned)ESP.getFreeHeap());
}

void loop(void)
{
    net_poll();      /* 连接超时/成功等异步事件 */
    link_poll();     /* F407 与 USB 调试口的命令行 */

    delay(5);
}
