#include "ui.h"
#include "ai_app.h"
#include "backlight.h"
#include "wifi_app.h"
#include "time_app.h"
#include "weather_app.h"
#include "ui_user.h"
#include "string.h"
#include "stdio.h"
#include "ff.h"
#include "exfuns.h"
#include "audio_app.h"
#include "spi.h"
#include "font_bin.h"
#include "monitorPage.h"
#include "FreeRTOS.h"
/* ============================================================
 * ui_user.c  —— 用户自定义界面逻辑（放 APP 目录，不会被 SquareLine 删）
 *
 * 当前实现：
 *   1. 上电扫描 SD 卡 MUSIC 目录，把 WAV 文件名填进滚轮
 *   2. 滚轮选中只记录，不播放（按播放键才出声）
 *   3. 上一首/下一首 同步移动滚轮
 *   4. 音量滑条 -> WM8978 音量
 * ============================================================ */

/* 1 = 用 SD 上的 .bin 字体；0 = 回到 C 数组字体（白屏排查用） */
#define UI_USE_BIN_FONT   1

#define MAX_SONGS       32          /* 最多收录曲目数 */
#define MAX_NAME_LEN    64          /* 单个文件名最大长度 */
#define OPT_BUF_SIZE    (MAX_SONGS * (MAX_NAME_LEN * 3 + 1) + 16)   /* UTF-8 最多 3 倍 */

static int32_t s_song_index = 0;
static int32_t s_song_count = 0;
static uint8_t s_is_playing  = 0;
static uint8_t s_play_mode   = 0;   /* 0 = 循环播放（默认），1 = 随机播放 */

static char s_opt_buf[OPT_BUF_SIZE] __attribute__((section(".ccmram"), zero_init));                /* 滚轮选项缓冲（UTF-8） */
static char s_utf8_name[MAX_NAME_LEN * 3] __attribute__((section(".ccmram"), zero_init));          /* 单曲名 UTF-8 转换缓冲 */

/* 把扫描结果写进滚轮（用 '\n' 分隔） */
static void fill_roller(int32_t n)
{
    int i;
    int pos = 0;

    s_opt_buf[0] = 0;

    if (n <= 0)
    {
        lv_roller_set_options(ui_Music_Roller1, "No Music", LV_ROLLER_MODE_NORMAL);
        s_song_count = 0;
        s_song_index = 0;
        return;
    }

    for (i = 0; i < n; i++)
    {
        int len;

        /* GBK -> UTF-8（LVGL 只认 UTF-8，否则中文乱码） */
        strncpy(s_utf8_name, audio_get_name_raw(i), sizeof(s_utf8_name) - 1);
        s_utf8_name[sizeof(s_utf8_name) - 1] = 0;
        len = (int)strlen(s_utf8_name);

        if (pos + len + 2 >= OPT_BUF_SIZE) break;

        memcpy(&s_opt_buf[pos], s_utf8_name, len);
        pos += len;
        s_opt_buf[pos++] = (i == n - 1) ? 0 : '\n';
    }
    s_opt_buf[pos] = 0;

    lv_roller_set_options(ui_Music_Roller1, s_opt_buf, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(ui_Music_Roller1, 0, LV_ANIM_OFF);

    /* 关键：把每个选项标签宽度固定 + 裁剪，
     * 否则滚动时每帧都会重新测量文字（每帧上千次 dsc） */
    {
        uint32_t i, cnt = lv_obj_get_child_cnt(ui_Music_Roller1);
        lv_coord_t w = lv_obj_get_content_width(ui_Music_Roller1);

        for (i = 0; i < cnt; i++)
        {
            lv_obj_t *c = lv_obj_get_child(ui_Music_Roller1, i);

            if (lv_obj_check_type(c, &lv_label_class))
            {
                lv_label_set_long_mode(c, LV_LABEL_LONG_CLIP);
                lv_obj_set_width(c, w);
                lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
            }
        }
    }

    s_song_count = (int32_t)lv_roller_get_option_cnt(ui_Music_Roller1);
    s_song_index = 0;

}

/* ---------------- 正在播放：显示歌名 ---------------- */

static char s_name_utf8[MAX_NAME_LEN * 3];      /* 歌名 UTF-8 缓冲 */
static char s_prefix_utf8[24];                  /* "正在播放:" 的 UTF-8 版本 */
static uint8_t s_prefix_ready = 0;

/* 把 GBK 源码里的前缀转成 UTF-8（只做一次） */
static void ensure_prefix(void)
{
    if (!s_prefix_ready)
    {
        static const char pf[] = "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x92\xAD\xE6\x94\xBE\x3A";
        strncpy(s_prefix_utf8, pf, sizeof(s_prefix_utf8) - 1);
        s_prefix_utf8[sizeof(s_prefix_utf8) - 1] = 0;
        s_prefix_ready = 1;
    }
}

/* 在 ui_Label1 上显示指定曲目的名称 */
static void show_song_name(int32_t idx)
{
    if (idx < 0) return;

    ensure_prefix();
    strncpy(s_name_utf8, audio_get_name_raw((int)idx), sizeof(s_name_utf8) - 1);
    s_name_utf8[sizeof(s_name_utf8) - 1] = 0;

    /* 滚动模式每帧重测文本，很费 CPU；先用裁剪 */
    lv_label_set_long_mode(ui_Label1, LV_LABEL_LONG_SCROLL_CIRCULAR);   /* 正在播放：滚动 */
    lv_label_set_text_fmt(ui_Label1, "%s%s", s_prefix_utf8, s_name_utf8);

    /* 预热这首歌的字形（交给字体任务） */
    font_bin_preload_async(s_name_utf8);
}

/* ---------------- 事件回调 ---------------- */

static void on_roller_changed(lv_event_t *e)
{
    (void)e;
    s_song_index = (int32_t)lv_roller_get_selected(ui_Music_Roller1);
}

static void on_play_changed(lv_event_t *e)
{
    s_is_playing = lv_obj_has_state(ui_Play_and_Stop, LV_STATE_CHECKED) ? 1 : 0;

    if (s_is_playing)
    {
        audio_play_idx((int)s_song_index);      /* 开始/继续播放当前曲目 */
        show_song_name(s_song_index);           /* 顶部显示这首歌的名字 */
    }
    else
    {
        audio_pause();
    }

    (void)e;
}

static void on_progress_changed(lv_event_t *e)
{
    static uint32_t s_last_seek_ms = 0;

    lv_event_code_t code = lv_event_get_code(e);
    int32_t v = (int32_t)lv_slider_get_value(ui_Music_progressBar);

    if (code == LV_EVENT_RELEASED)
    {
        uint32_t now = lv_tick_get();

        /* 防抖：150ms 内重复松手只响应一次，避免连续 seek 拖垮音频任务 */
        if (now - s_last_seek_ms < 150u) return;

        s_last_seek_ms = now;
        audio_seek_pct((int)v);
    }
}

static void on_volume_changed(lv_event_t *e)
{
    int32_t v = (int32_t)lv_slider_get_value(ui_volume);
    audio_set_volume((int)v);
    (void)e;
}



static void on_prev_clicked(lv_event_t *e)
{
    int32_t idx = s_song_index - 1;

    (void)e;
    if (s_song_count <= 0) return;

    if (idx < 0) idx = s_song_count - 1;
    s_song_index = idx;

    lv_roller_set_selected(ui_Music_Roller1, (uint16_t)idx, LV_ANIM_ON);

    lv_slider_set_value(ui_Music_progressBar, 0, LV_ANIM_OFF);   /* 新歌进度从 0 开始 */

    if (audio_is_playing())
    {
        audio_play_idx((int)idx);
        show_song_name(idx);
    }
}

static void on_next_clicked(lv_event_t *e)
{
    int32_t idx = s_song_index + 1;

    (void)e;
    if (s_song_count <= 0) return;

    if (idx >= s_song_count) idx = 0;
    s_song_index = idx;

    lv_roller_set_selected(ui_Music_Roller1, (uint16_t)idx, LV_ANIM_ON);

    lv_slider_set_value(ui_Music_progressBar, 0, LV_ANIM_OFF);   /* 新歌进度从 0 开始 */

    if (audio_is_playing())
    {
        audio_play_idx((int)idx);
        show_song_name(idx);
    }
}

/* ---------------- 播放模式 ---------------- */

static void on_playmode_changed(lv_event_t *e)
{
    (void)e;

    /* 开关未选中 = 循环，选中 = 随机 */
    s_play_mode = lv_obj_has_state(ui_PlayMode, LV_STATE_CHECKED) ? 1 : 0;
}

/* 指定索引开始播放（同步滚轮/进度条/标题） */
static void play_song_at(int32_t idx, lv_anim_enable_t anim)
{
    if (idx < 0 || idx >= s_song_count) return;

    s_song_index = idx;

    lv_roller_set_selected(ui_Music_Roller1, (uint16_t)idx, anim);
    lv_slider_set_value(ui_Music_progressBar, 0, LV_ANIM_OFF);

    audio_play_idx((int)idx);
    show_song_name(idx);
}

/* 一首播完后按模式选下一首 */
static void play_next_by_mode(void)
{
    int32_t idx;

    if (s_song_count <= 0) return;

    if (s_play_mode == 0)                    /* 循环：顺序下一首，尾部回到第一首 */
    {
        idx = (s_song_index + 1) % s_song_count;
    }
    else                                     /* 随机：避开当前这首 */
    {
        if (s_song_count == 1)
        {
            idx = 0;
        }
        else
        {
            do
            {
                idx = (int32_t)lv_rand(0, (uint32_t)(s_song_count - 1));
            } while (idx == s_song_index);
        }
    }

    play_song_at(idx, LV_ANIM_OFF);
}

/* ---------------- 界面刷新定时器（进度/时间） ---------------- */

static void on_ui_timer(lv_timer_t *tmr)
{
    u32 pos, dur;
    int pct;

    (void)tmr;

    pos = audio_pos_ms();
    dur = audio_dur_ms();

    /* 每首歌时长不同：一律按当前曲目时长映射到 0~100 */
    if (dur > 0)
    {
        if (pos > dur) pos = dur;
        pct = (int)((pos * 100u) / dur);
        if (pct > 100) pct = 100;
    }
    else pct = 0;

    /* 用户正在拖动进度条时不要去改它 */
    if (!lv_obj_has_state(ui_Music_progressBar, LV_STATE_PRESSED))
        lv_slider_set_value(ui_Music_progressBar, pct, LV_ANIM_OFF);

    /* 时间显示：mm:ss / mm:ss */
    lv_label_set_text_fmt(ui_Progress_Time, "%02u:%02u / %02u:%02u",
                          (unsigned)(pos / 60000u), (unsigned)((pos / 1000u) % 60u),
                          (unsigned)(dur / 60000u), (unsigned)((dur / 1000u) % 60u));

    /* 一首自然播完：按当前模式自动切到下一首 */
    if (s_is_playing && audio_take_eof())
    {
        play_next_by_mode();
    }

         /* 调试：字形缓存命中率 */
}

/* ---------------- 初始化 ---------------- */

/* 把对象树里所有「用 ui_font_Alimama16 的对象」换成 f
 * （SquareLine 生成的 ui_Music.c 不动，运行时覆盖） */
/* SquareLine 新导出用 Montserrat（无汉字），统一换成 .bin 字体 */
lv_font_t *g_bin_font = 0;   /* 全局：SD 字库字体，供其他模块使用 */

static void apply_bin_font_tree(lv_obj_t *obj, const lv_font_t *f)
{
    uint32_t i, n;

    if (!obj) return;

    /* 无条件换成 SD 字库字体（字库里包含 ASCII） */
    lv_obj_set_style_text_font(obj, f, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(obj, f, LV_PART_SELECTED | LV_STATE_DEFAULT);

    n = lv_obj_get_child_cnt(obj);
    for (i = 0; i < n; i++)
        apply_bin_font_tree(lv_obj_get_child(obj, i), f);
}


/* 亮度滑块 */
static void on_brightness(lv_event_t *e)
{
    lv_obj_t *s = (e != NULL) ? lv_event_get_target(e) : ui_Slider1;
    int v = (int)lv_slider_get_value(s);

    if (v < 0)   v = 0;
    if (v > 100) v = 100;

    backlight_set((uint8_t)v);
    lv_label_set_text_fmt(ui_Label12, "%d%%", v);
}

void ui_user_init(void)
{
    int32_t n;

    /* 1. 去掉误设的"永久按下"状态 */
    lv_obj_clear_state(ui_Music_Roller1, LV_STATE_PRESSED);

    /* 1.1 给滚轮设置中文字体（SquareLine 里只给标签设了，滚轮漏了）
     *     不设的话滚轮用默认 Montserrat，汉字显示为空白/方块 */
    {
        static const lv_font_t *s_bin_font  = NULL;
        static uint8_t          s_bin_tried = 0;
        const lv_font_t        *use;

        if (!s_bin_tried)
        {
            s_bin_tried = 1;
#if UI_USE_BIN_FONT
            /* 只从 SD 读 .bin，不写 W25Q16 */
            s_bin_font = font_bin_load_any("0:/FONT/Alimama_16.bin");   /* 优先 W25Q16，没有就自动安装 */

            /* 合法性检查：字体参数异常就弃用，避免白屏 */
            if (s_bin_font &&
                (s_bin_font->line_height < 8 || s_bin_font->line_height > 64))
            {
                
                s_bin_font = NULL;
            }

#else
#endif
        }

        use = s_bin_font ? s_bin_font : &lv_font_montserrat_14;

        /* 滚轮（含选中行） */
        lv_obj_set_style_text_font(ui_Music_Roller1, use,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ui_Music_Roller1, use,
                                   LV_PART_SELECTED | LV_STATE_DEFAULT);

        /* 整个音乐界面：凡是用 ui_font_Alimama16 的对象全部换成 .bin 字体
         * 包括 ui_Label1（正在播放）、ui_volumeLabel、ui_Progress_Time */
        if (s_bin_font)
        {
            g_bin_font = (lv_font_t *)s_bin_font;

            apply_bin_font_tree(ui_Music, s_bin_font);

            /* AI 页也要用字库（输入框/输出框显示汉字）*/
            apply_bin_font_tree(ui_AIPage, s_bin_font);
            apply_bin_font_tree(ui_Weather_Page, s_bin_font);   /* 天气页用字库 */
        }
    }
    //键盘
    /* 新版 AI 页已去掉 lv_keyboard，键盘由 ai_app 自建 */

    /* 1.9 字库：已在上面 1.1 处从 SD 加载（失败回退 C 数组字体） */

    /* 2. 扫描 SD 卡并填充滚轮（SD/FatFs 已在 main 里初始化好） */
    n = audio_scan_dir();
    fill_roller(n);
    /* 交给字体任务预取（最低优先级，不阻塞 UI） */
    font_bin_preload_async(s_opt_buf);

    /* 2.5 两个滑条都固定 0~100（进度=百分比，音量=百分比） */
    lv_slider_set_range(ui_Music_progressBar, 0, 100);
    lv_slider_set_range(ui_volume, 0, 100);
    lv_slider_set_value(ui_Music_progressBar, 0, LV_ANIM_OFF);

    /* 让滑条显示音频模块的真实音量（上电默认 70），
     * 否则 SquareLine 里设的 0 会和实际音量对不上 */
    lv_slider_set_value(ui_volume, (int32_t)audio_get_volume(), LV_ANIM_OFF);

    /* 3. 注册事件 */
    lv_obj_add_event_cb(ui_Music_Roller1,     on_roller_changed,   LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ui_Play_and_Stop,     on_play_changed,     LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ui_Music_progressBar, on_progress_changed, LV_EVENT_RELEASED,      NULL);
    lv_obj_add_event_cb(ui_volume,            on_volume_changed,   LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ui_prevButton,        on_prev_clicked,     LV_EVENT_CLICKED,       NULL);
    lv_obj_add_event_cb(ui_nextButton,        on_next_clicked,     LV_EVENT_CLICKED,       NULL);
    lv_obj_add_event_cb(ui_PlayMode,          on_playmode_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* 默认循环播放：开关保持未选中 */
    lv_obj_clear_state(ui_PlayMode, LV_STATE_CHECKED);

    lv_timer_create(on_ui_timer, 500, NULL);

    monitor_init();

    /* 亮度滑块初始化 */
    lv_obj_add_event_cb(ui_Slider1, on_brightness, LV_EVENT_VALUE_CHANGED, NULL);
    on_brightness(NULL);

    /* AI 助手页：键盘 + 输入框 + 输出框 */
    ai_app_init(NULL, ui_sendInput, ui_aiOutputBox);

    /* 设置页：WiFi 扫描 / 密码 / 连接 */
    wifi_app_init();

    /* 主界面：联网后同步时间 */
    time_app_init();

    /* 天气页：进入自动刷新 */
    weather_app_init();   /* AI 页：键盘/输入/输出 */

}
