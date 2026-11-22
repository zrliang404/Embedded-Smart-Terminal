#ifndef __FONT_BIN_H
#define __FONT_BIN_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 从 SD 卡加载 lv_font_conv 生成的 .bin 字库（按需读取，不整体加载）
 * path: 例如 "0:/FONT/Alimama_16.bin"
 * 成功返回 lv_font_t 指针，失败返回 0
 */
lv_font_t *font_bin_load(const char *path);

/* 优先从 W25Q16 读（快），没有则安装一次，再退回 SD */
lv_font_t *font_bin_load_any(const char *path);

/* 是否已经加载成功 */
uint8_t font_bin_ready(void);

/* 自检：解析几个字并打印结果（不涉及 LVGL 绘制） */
void font_bin_selftest(const lv_font_t *f);

/* 打印加载到的字体参数 */
void font_bin_dump(void);

/* 预热一段 UTF-8 文本的字形到缓存 */
void font_bin_preload_utf8(const char *s, uint32_t max_chars);

/* 异步预取：丢给字体任务（最低优先级），立即返回 */
void font_bin_preload_async(const char *s);

/* 字体预取任务入口 */
void font_bin_task(void *pv);

/* 打印并清零缓存命中率统计 */
void font_bin_stats(void);

#ifdef __cplusplus
}
#endif

#endif
