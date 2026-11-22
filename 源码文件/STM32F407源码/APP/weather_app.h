#ifndef __WEATHER_APP_H
#define __WEATHER_APP_H

#include <stdint.h>

/* 天气页：进入自动刷新，点更新图标也刷新 */
void weather_app_init(void);

/* 处理一行串口数据，返回 1 表示已消耗 */
int  weather_app_handle_line(char *s);

#endif
