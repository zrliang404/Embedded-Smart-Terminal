#ifndef __WIFI_APP_H
#define __WIFI_APP_H

#include <stdint.h>

/* 设置页：WiFi 扫描 / 密码输入 / 连接 */
void wifi_app_init(void);

/* 处理一行串口数据，返回 1 表示已消耗 */
int  wifi_app_handle_line(char *s);

#endif
