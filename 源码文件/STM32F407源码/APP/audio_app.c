#include "audio_app.h"
#include "ff.h"
#include "utf16.h"
#include "exfuns.h"
#include "i2s.h"
#include "wm8978.h"
#include "string.h"
#include "stdio.h"
#include "fs_lock.h"
#include "FreeRTOS.h"
#include "task.h"

#define AUDIO_BUF_SIZE   12288
#define AUDIO_MAX_SONGS  32
#define AUDIO_PATH_LEN   128

typedef struct
{
    uint32_t dataOfs;         /* data 段起始偏移 */
    uint32_t dataSize;        /* data 段字节数 */
    uint32_t sampleRate;      /* 采样率 */
    uint32_t byteRate;        /* WAV 头部 nAvgBytesPerSec */
    uint16_t blockAlign;      /* 关键：通道数 * (位深/8) */
    uint16_t bitsPerSample;   /* 位深 */
    uint16_t audioFormat;     /* 编码格式（1=PCM） */
    uint16_t numChannels;     /* 通道数 */
} WAV_INFO;

/* ---- 歌曲列表 ---- */
static TCHAR s_path[AUDIO_MAX_SONGS][AUDIO_PATH_LEN];   /* UTF-16 完整路径 */
static char s_name[AUDIO_MAX_SONGS][64];                /* UTF-8 显示名（去扩展名） */
static int  s_total = 0;                                /* 扫描到的歌曲数 */

/* ---- 播放状态 ---- */
static FIL      s_fp;                  /* 当前打开的文件 */
static uint8_t  s_file_open = 0;       /* 文件是否已打开 */
static WAV_INFO s_wav;                 /* 当前曲目 WAV 信息 */
static uint8_t  s_playing   = 0;       /* 1=正在播放 */
static volatile uint8_t s_eof = 0;     /* 1 = 本曲已自然播完（供 UI 取走后清零）*/
static uint8_t  s_want_play = 0;       /* 1=有播放请求待处理 */
static int      s_cur       = -1;      /* 当前曲目索引，-1=无 */

static uint32_t s_read      = 0;       /* 已从文件读到的字节（含预读） */
static uint32_t s_consumed  = 0;       /* 已被 DMA 消费的字节（=播放位置） */
static uint32_t s_pause_pos = 0;       /* 暂停时的文件位置 */
static int      s_seek_pct  = -1;      /* 待处理的 seek 百分比，-1=无 */
static int      s_vol_pct   = 70;      /* 音量百分比 */

/* ---- Seek 处理状态 ---- */
static volatile uint8_t  s_seek_req = 0;   /* 播放中 seek 请求标志 */
static volatile uint32_t s_seek_off = 0;   /* seek 目标偏移（已对齐） */

static volatile uint32_t s_dur_ms = 0;     /* 总时长 ms */
static volatile uint32_t s_pos_ms = 0;     /* 当前播放位置 ms */

static uint32_t s_tick_start  = 0;

__align(16) static uint8_t s_buf0[AUDIO_BUF_SIZE];   /* DMA 双缓冲 0 */
__align(16) static uint8_t s_buf1[AUDIO_BUF_SIZE];   /* DMA 双缓冲 1 */

/* ---------------- 基础工具函数 ---------------- */

/* 从文件中读 4 字节并按小端拼成 uint32_t；读取失败返回 0 */
static uint32_t rd_u32(FIL *fp)
{
    uint8_t b[4]; UINT br;
    if (f_read(fp, b, 4, &br) != FR_OK || br != 4) return 0;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* 从文件中读 2 字节并按小端拼成 uint16_t；读取失败返回 0 */
static uint16_t rd_u16(FIL *fp)
{
    uint8_t b[2]; UINT br;
    if (f_read(fp, b, 2, &br) != FR_OK || br != 2) return 0;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

/* 计算每秒字节数：优先用头部 byteRate，缺失时按 采样率*通道*位深 推算 */
static uint32_t wav_byte_rate(void)
{
    if (s_wav.byteRate > 0) return s_wav.byteRate;
    return s_wav.sampleRate * s_wav.numChannels * (s_wav.bitsPerSample / 8u);
}

/* 解析 WAV 文件头：
 * 校验 RIFF/WAVE，遍历 chunk 找到 "fmt " 与 "data"，
 * 填 WAV_INFO（含 blockAlign 兜底推算）。
 * 返回 0=成功，1=打开失败，2=非RIFF，3=非WAVE，4=无data */
static uint8_t wav_parse(const TCHAR *path, WAV_INFO *w)
{
    FIL fp; UINT br; uint8_t id[4]; uint32_t sz, pos; uint8_t i;

    memset(w, 0, sizeof(WAV_INFO));

    if (f_open(&fp, path, FA_READ | FA_OPEN_EXISTING) != 0) return 1;

    f_read(&fp, id, 4, &br);
    if (!(id[0]=='R'&&id[1]=='I'&&id[2]=='F'&&id[3]=='F')) { f_close(&fp); return 2; }
    rd_u32(&fp);                                    /* RIFF size */
    f_read(&fp, id, 4, &br);
    if (!(id[0]=='W'&&id[1]=='A'&&id[2]=='V'&&id[3]=='E')) { f_close(&fp); return 3; }

    pos = 12;
    for (i = 0; i < 16; i++)
    {
        f_lseek(&fp, pos);
        f_read(&fp, id, 4, &br);
        if (br != 4) break;
        sz = rd_u32(&fp);

        if (id[0]=='f'&&id[1]=='m'&&id[2]=='t'&&id[3]==' ')
        {
            w->audioFormat   = rd_u16(&fp);
            w->numChannels   = rd_u16(&fp);
            w->sampleRate    = rd_u32(&fp);
            w->byteRate      = rd_u32(&fp);
            w->blockAlign    = rd_u16(&fp);          /* 记录 blockAlign */
            w->bitsPerSample = rd_u16(&fp);
        }
        else if (id[0]=='d'&&id[1]=='a'&&id[2]=='t'&&id[3]=='a')
        {
            w->dataOfs  = pos + 8;
            w->dataSize = sz;
            break;
        }
        pos += 8 + sz;
        if (sz & 1) pos++;                           /* chunk 按偶数字节对齐 */
    }
    f_close(&fp);

    /* 校验并补齐 blockAlign */
    if (w->blockAlign == 0 && w->numChannels && w->bitsPerSample)
    {
        w->blockAlign = w->numChannels * (w->bitsPerSample / 8u);
    }

    return (w->dataSize > 0) ? 0 : 4;
}

/* 把 s_vol_pct(0~100) 换算成 WM8978 的 0~63 音量并写入耳机/喇叭 */
static void apply_volume(void)
{
    uint8_t v = (uint8_t)((s_vol_pct * 63 + 50) / 100);   /* 四舍五入 */
    if (v > 63) v = 63;
    WM8978_HPvol_Set(v, v);
    WM8978_SPKvol_Set(v);
}

/* 释放播放资源：停止 I2S、关闭文件（加锁）、清播放标志和位置 */
static void audio_release(void)
{
    I2S_Play_Stop();
    if (s_file_open)
    {
        fs_lock();
        f_close(&s_fp);
        fs_unlock();
        s_file_open = 0;
    }
    s_playing = 0;
    s_pos_ms  = 0;
}

/* 对 16-bit PCM 应用线性渐变增益 g0→g1（Q12 定点，0~4096=0~100%）。
 * 用于 seek 后淡入，抑制直流突变造成的爆音 */
static void audio_apply_gain(uint8_t *buf, uint32_t bytes, int32_t g0, int32_t g1)
{
    int16_t *p = (int16_t *)buf;
    uint32_t n = bytes >> 1;
    uint32_t i;
    int32_t  step;

    if (n == 0) return;
    step = ((g1 - g0) << 12) / (int32_t)n;

    for (i = 0; i < n; i++)
    {
        int32_t g = g0 + (int32_t)(((int64_t)step * (int32_t)i) >> 12);
        if (g < 0)    g = 0;
        if (g > 4096) g = 4096;
        p[i] = (int16_t)(((int32_t)p[i] * g) >> 12);
    }
}

/* 从文件读取至多 want 字节到 dst，按 blockAlign 对齐；
 * 到达 data 尾部时不足部分补 0。返回实际有效数据字节数。
 * 倍速由 I2S 时钟决定，这里不做抽取 */
static uint32_t audio_fill(uint8_t *dst, uint32_t want)
{
    uint32_t align  = s_wav.blockAlign ? s_wav.blockAlign : 4u;
    uint32_t filled = 0;

    while (filled < want)
    {
        uint32_t remain = (s_read < s_wav.dataSize) ? (s_wav.dataSize - s_read) : 0u;
        uint32_t room   = want - filled;
        UINT     br     = 0;

        if (remain == 0) break;
        if (room > remain) room = remain;
        room = (room / align) * align;                /* 按音频帧对齐 */
        if (room == 0) break;

        fs_lock();
        if (f_read(&s_fp, dst + filled, room, &br) != FR_OK) br = 0;
        fs_unlock();

        if (br == 0) break;
        s_read += br;
        filled += br;
    }

    if (filled < want) memset(dst + filled, 0, want - filled);   /* 尾部补静音 */
    return filled;
}

/* 启动一次流式播放：
 * 清空双缓冲 → 预填两块 → 校正 consumed 起点 → 初始化 I2S/DMA → 启动 */
static uint8_t audio_start_stream(void)
{
    uint8_t r;

    memset(s_buf0, 0, AUDIO_BUF_SIZE);
    memset(s_buf1, 0, AUDIO_BUF_SIZE);

    audio_fill(s_buf0, AUDIO_BUF_SIZE);
    audio_fill(s_buf1, AUDIO_BUF_SIZE);

    {
        uint32_t buffered = (uint32_t)AUDIO_BUF_SIZE * 2u;

        /* 已预读两整块，播放位置应从 起点 回退两块 */
        s_consumed = (s_read > buffered) ? (s_read - buffered) : 0u;
        if (s_consumed > s_wav.dataSize) s_consumed = s_wav.dataSize;
    }

    I2S2_Init(I2S_STANDARD_PHILIPS, I2S_MODE_MASTER_TX,
              I2S_CPOL_LOW, I2S_DATAFORMAT_16B_EXTENDED);
    r = I2S2_SampleRate_Set(s_wav.sampleRate);
    if (r != 0) return 2;                              /* 采样率不支持 */

    I2S2_TX_DMA_Init(s_buf0, s_buf1, AUDIO_BUF_SIZE / 2);
    I2S_Play_Start();

    s_eof         = 0;
    s_playing     = 1;
    s_tick_start  = xTaskGetTickCount();

    return 0;
}

/* ---------------- 目录扫描 ---------------- */

/* 扫描 "0:/MUSIC" 目录下的 .wav 文件（扩展名不区分大小写），
 * 把 UTF-16 全路径存入 s_path，UTF-8 去扩展名存入 s_name。
 * 返回歌曲数量 */
int audio_scan_dir(void)
{
    DIR dir; FILINFO fno; FRESULT fr; int n = 0;
    WCHAR dir_w[24];
    WCHAR pre_w[24];

    utf8_to_utf16("0:/MUSIC",  dir_w, 24);
    utf8_to_utf16("0:/MUSIC/", pre_w, 24);

    s_total = 0;
    fs_lock();
    fr = f_opendir(&dir, (const TCHAR *)dir_w);
    if (fr != FR_OK) { fs_unlock(); return 0; }

    while (n < AUDIO_MAX_SONGS)
    {
        int len;

        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        if (fno.fattrib & AM_DIR) continue;           /* 跳过目录 */

        len = wstr_len((const uint16_t *)fno.fname);
        if (len < 5) continue;                        /* 至少 a.wav */
        if (fno.fname[len - 4] != '.') continue;
        {
            WCHAR c1 = fno.fname[len - 3];
            WCHAR c2 = fno.fname[len - 2];
            WCHAR c3 = fno.fname[len - 1];

            /* 扩展名转小写后比较 */
            if (c1 >= 'A' && c1 <= 'Z') c1 = (WCHAR)(c1 - 'A' + 'a');
            if (c2 >= 'A' && c2 <= 'Z') c2 = (WCHAR)(c2 - 'A' + 'a');
            if (c3 >= 'A' && c3 <= 'Z') c3 = (WCHAR)(c3 - 'A' + 'a');
            if (!(c1 == 'w' && c2 == 'a' && c3 == 'v')) continue;
        }

        wstr_cpy(s_path[n], (const uint16_t *)pre_w, AUDIO_PATH_LEN);
        wstr_cat(s_path[n], (const uint16_t *)fno.fname, AUDIO_PATH_LEN);

        utf16_to_utf8((const uint16_t *)fno.fname, s_name[n], sizeof(s_name[n]));
        {
            int l8 = (int)strlen(s_name[n]);
            if (l8 > 4) s_name[n][l8 - 4] = 0;        /* 去掉 ".wav" */
        }
        n++;
    }
    f_closedir(&dir);
    fs_unlock();

    s_total = n;
    return n;
}

/* 返回第 idx 首歌曲的 UTF-8 显示名；越界返回空串 */
const char *audio_get_name_raw(int idx)
{
    if (idx < 0 || idx >= s_total) return "";
    return s_name[idx];
}

/* 返回是否正在播放 */
uint8_t  audio_is_playing(void) { return s_playing; }
/* 返回当前播放位置（ms） */
uint32_t audio_pos_ms(void)     { return s_pos_ms; }
/* 返回当前曲目总时长（ms） */
uint32_t audio_dur_ms(void)     { return s_dur_ms; }
/* 返回当前曲目索引，-1=无 */
int      audio_cur_index(void)  { return s_cur; }

/* ---------------- 控制接口 ---------------- */

/* 请求播放第 idx 首：
 * 同一首在播→忽略；同首暂停→请求续播；不同首→释放旧曲并记录请求，
 * 真正打开/播放交给 audio_task_entry */
void audio_play_idx(int idx)
{
    if (idx < 0 || idx >= s_total) return;

    if (s_cur == idx)
    {
        if (s_playing) return;
        s_want_play = 1;
        return;
    }

    audio_release();
    s_cur       = idx;
    s_want_play = 1;
    s_pause_pos = 0;
    s_seek_pct  = -1;
}

/* 暂停：记录当前消费位置，停止 I2S，清播放标志 */
void audio_pause(void)
{
    if (!s_playing) return;

    s_pause_pos = s_consumed;
    if (s_pause_pos > s_wav.dataSize) s_pause_pos = s_wav.dataSize;

    I2S_Play_Stop();
    s_playing   = 0;
    s_want_play = 0;
}

/* 续播：只置播放请求标志，由任务负责重新打开并启动 */
void audio_resume(void)
{
    if (s_cur < 0) return;
    s_want_play = 1;
}

/* 停止：释放资源、清请求、清当前曲目 */
void audio_stop(void)
{
    audio_release();
    s_want_play = 0;
    s_cur = -1;
}

/* 请求按百分比 seek（0~100），实际处理在任务中 */
void audio_seek_pct(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_seek_pct = pct;
}

/* 设置音量（0~100）并立即生效 */
void audio_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_vol_pct = pct;
    apply_volume();
}

/* 返回当前音量百分比 */
int audio_get_volume(void)
{
    return s_vol_pct;
}

/* 初始化音频硬件：WM8978 编解码器配置 + 应用音量 */
void audio_app_init(void)
{
    WM8978_Init();
    WM8978_ADDA_Cfg(1, 0);          /* 开 DAC，关 ADC */
    WM8978_Input_Cfg(0, 0, 0);      /* 不启用输入 */
    WM8978_Output_Cfg(1, 1);        /* 开耳机 + 喇叭 */
    WM8978_I2S_Cfg(2, 0);           /* I2S 从模式（由 STM32 主控发时钟） */
    apply_volume();
}

/* ---------------- 音频任务主逻辑 ---------------- */

/* 音频播放主任务（常驻循环）：
 *  1) 处理 seek 请求（暂停态直接定位，播放态置标志）
 *  2) 处理播放请求（打开文件、解析 WAV、启动流）
 *  3) 播放中：DMA 半/全满中断到来时填充下一块，处理 seek 淡入，
 *     更新播放进度，判断结束
 *  无事件时按状态延时 2/20ms 让出 CPU */
void audio_task_entry(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        /* 1) 处理 Seek 请求 */
        if (s_seek_pct >= 0 && s_file_open)
        {
            int      pct   = s_seek_pct;
            uint32_t align = s_wav.blockAlign ? s_wav.blockAlign : 4u;
            uint32_t off;

            s_seek_pct = -1;

            off = (uint32_t)(((uint64_t)s_wav.dataSize * (uint32_t)pct) / 100u);

            /* 强制按音频帧对齐，杜绝左右声道/位深倒置引发的炸音 */
            off = (off / align) * align;
            if (off >= s_wav.dataSize)
            {
                off = (s_wav.dataSize >= align) ? (s_wav.dataSize - align) : 0u;
            }

            s_seek_off = off;

            if (!s_playing)
            {
                uint32_t brate;
                FRESULT  fr;

                fs_lock();
                fr = f_lseek(&s_fp, s_wav.dataOfs + off);
                fs_unlock();

                if (fr != FR_OK)
                {
                    printf("[aud] seek lseek FAILED (paused)\r\n");
                    continue;               /* 定位失败：保持原位置 */
                }

                s_read      = off;
                s_consumed  = off;
                s_pause_pos = off;

                brate = wav_byte_rate();
                s_pos_ms = brate ? (uint32_t)(((uint64_t)off * 1000u) / brate) : 0u;
                if (s_pos_ms > s_dur_ms) s_pos_ms = s_dur_ms;
            }
            else
            {
                /* 正在播放时触发 Seek 请求标志 */
                s_seek_req = 1;
            }
            continue;
        }

        /* 2) 开始播放歌曲 */
        if (s_want_play && !s_playing && s_cur >= 0)
        {
            uint8_t  r;
            uint32_t align;

            s_want_play = 0;

            if (s_file_open) { f_close(&s_fp); s_file_open = 0; }

            fs_lock();
            if (f_open(&s_fp, s_path[s_cur], FA_READ | FA_OPEN_EXISTING) != 0)
            {
                s_cur = -1;
                fs_unlock();
                continue;
            }
            s_file_open = 1;

            if (wav_parse(s_path[s_cur], &s_wav) != 0 || s_wav.bitsPerSample != 16)
            {
                f_close(&s_fp); s_file_open = 0; s_cur = -1;
                fs_unlock();
                continue;
            }

            {
                uint32_t fsz  = (uint32_t)f_size(&s_fp);
                uint32_t real = (fsz > s_wav.dataOfs) ? (fsz - s_wav.dataOfs) : 0u;
                uint32_t brate = wav_byte_rate();

                /* 以实际文件长度修正 dataSize，避免头部长度不准 */
                if (real > 0) s_wav.dataSize = real;
                s_dur_ms = (brate > 0) ? (uint32_t)(((uint64_t)s_wav.dataSize * 1000u) / brate) : 0u;
                printf("WAV[%d] %s: ch=%d rate=%u bits=%d blockAlign=%d byteRate=%u dataSize=%u fileSize=%u\n",
                       s_cur, s_name[s_cur],
                       s_wav.numChannels, s_wav.sampleRate, s_wav.bitsPerSample,
                       s_wav.blockAlign, s_wav.byteRate, s_wav.dataSize, fsz);
            }

            align = s_wav.blockAlign ? s_wav.blockAlign : 4u;
            s_read = (s_pause_pos / align) * align;
            if (s_read > s_wav.dataSize) s_read = 0;

            f_lseek(&s_fp, s_wav.dataOfs + s_read);
            fs_unlock();

            r = audio_start_stream();
            if (r != 0) { f_close(&s_fp); s_file_open = 0; s_cur = -1; }
            continue;
        }

        /* 3) 播放中流维护与缓冲填充 */
        if (s_playing)
        {
            /* DMA 半传输/全传输标志：说明有一块缓冲空出来可以填了 */
            if (DMA_GetFlagStatus(DMA1_Stream4, DMA_FLAG_HTIF4) ||
                DMA_GetFlagStatus(DMA1_Stream4, DMA_FLAG_TCIF4))
            {
                uint8_t *target;
                uint32_t remain, brate, pms;

                /* CT=1 正在传输 M1，CPU 填充 M0；CT=0 传输 M0，填充 M1 */
                target = (DMA1_Stream4->CR & DMA_SxCR_CT) ? s_buf0 : s_buf1;

                /* 【核心修复点 2】：响应 Seek 时立即装载新扇区，淡入新数据并清空残余缓存 */
                if (s_seek_req)
                {
                    FRESULT fr;

                    fs_lock();
                    fr = f_lseek(&s_fp, s_wav.dataOfs + s_seek_off);
                    fs_unlock();

                    s_seek_req = 0;             /* 无论成功失败都消费掉请求 */

                    if (fr != FR_OK)
                    {
                        printf("[aud] seek lseek FAILED (playing)\r\n");
                    }
                    else
                    {
                        s_read = s_seek_off;
                        audio_fill(target, AUDIO_BUF_SIZE);

                        /* 对切入的新缓冲施加 0 -> 100% 快速淡入，平滑直流阶跃消除“咔哒”爆音 */
                        audio_apply_gain(target, AUDIO_BUF_SIZE, 0, 4096);

                        s_consumed = s_seek_off;
                    }
                }
                else
                {
                    remain = (s_read < s_wav.dataSize) ? (s_wav.dataSize - s_read) : 0u;

                    if (remain > 0)
                    {
                        audio_fill(target, AUDIO_BUF_SIZE);
                    }
                    else
                    {
                        memset(target, 0, AUDIO_BUF_SIZE);   /* 播完补静音 */
                    }
                }

                DMA_ClearFlag(DMA1_Stream4, DMA_FLAG_HTIF4 | DMA_FLAG_TCIF4);

                /* 更新播放进度 */
                s_consumed += AUDIO_BUF_SIZE;
                if (s_consumed > s_wav.dataSize) s_consumed = s_wav.dataSize;

                brate = wav_byte_rate();
                pms = brate ? (uint32_t)(((uint64_t)s_consumed * 1000u) / brate) : 0u;
                if (pms > s_dur_ms) pms = s_dur_ms;
                s_pos_ms = pms;

                /* 判断自然结束 */
                if (s_consumed >= s_wav.dataSize)
                {
                    s_eof       = 1;          /* 通知 UI */
                    s_playing   = 0;
                    s_want_play = 0;
                    s_pos_ms    = s_dur_ms;
                    s_pause_pos = 0;
                }

                continue;
            }
            vTaskDelay(2);
            continue;
        }

        vTaskDelay(20);
    }
}

/* UI 取走"本曲播完"标志（读一次即清零，用于自动切下一首） */
uint8_t audio_take_eof(void)
{
    uint8_t e = s_eof;

    s_eof = 0;
    return e;
}
