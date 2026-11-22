#ifndef __AUDIO_APP_H
#define __AUDIO_APP_H

#include "sys.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 初始化（在 main 里、调度器启动前调用） ---- */
void audio_app_init(void);

/* ---- 播放列表：扫描 SD 卡目录，返回曲目数 ---- */
int  audio_scan_dir(void);

/* 取第 idx 首的原始文件名（GBK，无扩展名，不可改） */
const char *audio_get_name_raw(int idx);

/* ---- 控制命令（LVGL 任务里调用，内部只置标志，不阻塞） ---- */
void audio_play_idx(int idx);        /* 播放指定曲目（已在播则忽略重复） */
void audio_pause(void);              /* 暂停 */
void audio_resume(void);             /* 继续 */
void audio_stop(void);               /* 停止 */
void audio_seek_pct(int pct);        /* 跳到百分比位置 */
void audio_set_volume(int pct);
int     audio_get_volume(void);      /* 返回当前音量 0~100 */

/* ---- 状态查询（LVGL 里轮询） ---- */
uint8_t  audio_is_playing(void);
uint32_t audio_pos_ms(void);
uint32_t audio_dur_ms(void);
int      audio_cur_index(void);

/* ---- 音频任务入口（rtos_app 里创建） ---- */
void audio_task_entry(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif

uint8_t  audio_take_eof(void);   /* 1 = 刚刚自然播完一首 */
