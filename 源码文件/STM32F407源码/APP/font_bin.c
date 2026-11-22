#include "font_bin.h"
#include "ff.h"
#include "fs_lock.h"
#include "font_store.h"
#include "utf16.h"
#include "string.h"
#include "stdio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"



#define FB_MAX_CMAP    48
#define FB_CHUNK       512       /* 读卡块大小，与 SD 扇区一致 */
#define FB_ENTRY_MAX   512
#define FB_BMP_MAX     512

#define FB_PAGE_ENTRIES 64        /* 页索引粒度：每 64 个稀疏项一页 */
#define FB_MAX_PAGES    128       /* 页索引总容量 */

#define FB_DC_N        512        /* 描述缓存条数 */
#define FB_USE_BC      1         /* 1=启用点阵缓存  0=每次直接读（排错用） */
#define FB_BC_N        160        /* 点阵缓存条数（装得下整屏）*/
#define FB_DC_HASH     2048      /* 描述缓存哈希槽 */
#define FB_BC_HASH     512       /* 点阵缓存哈希槽 */
#define FB_BC_BYTES    144       /* 每条点阵缓存存条目字节数（实测最大 140）*/

typedef struct
{
    uint32_t data_off;
    uint32_t range_start;
    uint16_t range_len;
    uint16_t gid_start;
    uint16_t entries;
    uint8_t  fmt;
    uint8_t  pad;
} fb_cmap_t;

/* 描述缓存项 */
typedef struct
{
    uint32_t letter;
    uint32_t off;        /* 字形条目在文件中的绝对偏移 */
    uint16_t len;        /* 条目字节数 */
    uint16_t adv;        /* 已换算成像素 */
    int8_t   ox;
    int8_t   oy;
    uint8_t  w;
    uint8_t  h;
    uint8_t  valid;
} fb_dc_t;               /* 20 字节 */

/* 点阵缓存项（存原始条目字节，可反复重渲染） */
typedef struct
{
    uint32_t letter;
    uint16_t len;
    uint16_t w;              /* 渲染后的宽 */
    uint16_t h;              /* 渲染后的高 */
    uint8_t  valid;
    uint8_t  pad;
    uint8_t  data[FB_BC_BYTES];   /* 存「渲染后的位图」，不再存原始条目 */
} fb_bc_t;               /* 168 字节 */

typedef struct
{
    FIL      fp;
    uint8_t  opened;

    uint16_t line_height;
    uint16_t base_line;
    uint8_t  bpp;
    uint8_t  xy_bits;
    uint8_t  wh_bits;
    uint8_t  adv_bits;
    uint8_t  adv_fmt;
    uint8_t  loc_fmt;
    uint8_t  gid_fmt;
    uint8_t  subpx;
    uint8_t  bitpos;         /* 描述占的位数（30） */
    int8_t   underline_pos;

    uint32_t  cmap_base;
    uint32_t  n_cmap;
    fb_cmap_t cmap[FB_MAX_CMAP];

    uint32_t loca_base;
    uint32_t loca_count;
    uint32_t glyf_base;
    uint32_t glyf_size;      /* glyf 段总长（含 8 字节头） */

    uint32_t chunk_off;
    uint16_t chunk_len;
    uint8_t  chunk_valid;
    uint8_t  chunk[FB_CHUNK];

    /* 最近一次字形（LVGL 先问描述、再要点阵） */
    uint32_t last_letter;
    uint8_t  last_ok;
    lv_font_glyph_dsc_t last_dsc;

    uint8_t entry[FB_ENTRY_MAX];
    uint8_t bmp[FB_BMP_MAX];

    /* 缓存 */
    /* cmap 页索引（洞：稀疏表二分查找不必反复读卡） */
    uint8_t  cmap_pbase[FB_MAX_CMAP];
    uint8_t  cmap_pnum[FB_MAX_CMAP];
    uint8_t  page_cnt;

    lv_font_t font;
} fb_t;

static fb_t s_fb;

/* 字形缓存放 CCM：纯 CPU 访问，不涉及 DMA，
 * 这样能省下约 7.3KB 普通 SRAM */
static fb_dc_t s_dc[FB_DC_N] __attribute__((section(".ccmram"), zero_init));
static uint16_t s_dc_rr;
static fb_bc_t s_bc[FB_BC_N] __attribute__((section(".ccmram"), zero_init));
static uint8_t s_bc_rr;
static uint16_t s_dc_hash[FB_DC_HASH];   /* 0=空，否则 slot+1 */
static uint16_t s_bc_hash[FB_BC_HASH];

/* ---- 调试统计 ---- */
static uint32_t s_st_dsc_hit, s_st_dsc_miss;
static uint32_t s_st_bmp_hit, s_st_bmp_miss;
static uint32_t s_st_sd;
static uint32_t s_st_dcyc, s_st_dn;   /* dsc 累计周期/次数 */

static uint8_t s_use_store;
static SemaphoreHandle_t s_fb_mtx = 0;   /* 缓存互斥（LVGL 任务 + 字体任务） */
static uint32_t s_last_miss;      /* 上一个报过的缺字，避免刷屏 */   /* 1 = 从 W25Q16 读，0 = 从 SD 卡读 */

/* 页索引：每页记录该页第一个「相对码点」 */
static uint16_t s_page[FB_MAX_PAGES];

/* ---------------- 安全读取 ---------------- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t rdi16(const uint8_t *p)
{
    return (int16_t)rd16(p);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t fb_read(uint32_t off, void *dst, uint32_t len)
{
    UINT br;

    s_st_sd++;
    uint8_t ok;

    if (s_use_store)
    {
        font_store_read(off, dst, len);
        return 1;
    }

    fs_lock();
    ok = (f_lseek(&s_fb.fp, off) == FR_OK) &&
         (f_read(&s_fb.fp, dst, len, &br) == FR_OK) &&
         (br == len);
    fs_unlock();

    return ok ? 1 : 0;
}

/* 带块缓存的小读取 */
static uint8_t fb_get(uint32_t off, void *dst, uint32_t len)
{
    uint32_t rel;
    uint32_t base = off & ~(uint32_t)(FB_CHUNK - 1);

    /* 请求跨块：缓存装不下，直接读，避免越界读到垃圾 */
    if ((off - base) + len > FB_CHUNK)
    {
        return fb_read(off, dst, len);
    }

    if (!s_fb.chunk_valid || s_fb.chunk_off != base)
    {
        if (!fb_read(base, s_fb.chunk, FB_CHUNK))
        {
            return fb_read(off, dst, len);
        }
        s_fb.chunk_off   = base;
        s_fb.chunk_len   = FB_CHUNK;
        s_fb.chunk_valid = 1;
    }

    rel = off - s_fb.chunk_off;
    memcpy(dst, &s_fb.chunk[rel], len);
    return 1;
}

static uint16_t fb_u16(uint32_t off)
{
    uint16_t v = 0;
    fb_get(off, &v, 2);
    return v;
}

static uint32_t fb_u32(uint32_t off)
{
    uint32_t v = 0;
    fb_get(off, &v, 4);
    return v;
}

/* ---------------- 缓存 ---------------- */

static uint32_t fb_h(uint32_t letter, uint32_t mask)
{
    return (letter ^ (letter >> 9)) & mask;
}

/* 直接映射索引：冲突只会导致“未命中”，不会弄坏链表 */
static void fb_dc_touch(fb_dc_t *d)
{
    s_dc_hash[fb_h(d->letter, FB_DC_HASH - 1)] = (uint16_t)((d - s_dc) + 1);
}

static void fb_bc_touch(fb_bc_t *b)
{
    s_bc_hash[fb_h(b->letter, FB_BC_HASH - 1)] = (uint16_t)((b - s_bc) + 1);
}

static fb_dc_t *fb_dc_find(uint32_t letter)
{
    uint16_t s = s_dc_hash[fb_h(letter, FB_DC_HASH - 1)];
    uint32_t i;

    if (s)
    {
        fb_dc_t *d = &s_dc[s - 1];

        if (d->valid && d->letter == letter) return d;
    }

    /* 哈希冲突时回退线性扫描，免得「假未命中」导致反复读卡 */
    for (i = 0; i < FB_DC_N; i++)
    {
        if (s_dc[i].valid && s_dc[i].letter == letter)
        {
            s_dc_hash[fb_h(letter, FB_DC_HASH - 1)] = (uint16_t)(i + 1);
            return &s_dc[i];
        }
    }
    return 0;
}

static fb_dc_t *fb_dc_alloc(void)
{
    fb_dc_t *d = &s_dc[s_dc_rr];
    s_dc_rr = (uint16_t)((s_dc_rr + 1) % FB_DC_N);
    return d;
}

static fb_bc_t *fb_bc_find(uint32_t letter)
{
    uint16_t s = s_bc_hash[fb_h(letter, FB_BC_HASH - 1)];
    uint32_t i;

    if (s)
    {
        fb_bc_t *b = &s_bc[s - 1];

        if (b->valid && b->letter == letter) return b;
    }

    for (i = 0; i < FB_BC_N; i++)
    {
        if (s_bc[i].valid && s_bc[i].letter == letter)
        {
            s_bc_hash[fb_h(letter, FB_BC_HASH - 1)] = (uint16_t)(i + 1);
            return &s_bc[i];
        }
    }
    return 0;
}

static fb_bc_t *fb_bc_alloc(void)
{
    fb_bc_t *b = &s_bc[s_bc_rr];
    s_bc_rr = (uint8_t)((s_bc_rr + 1) % FB_BC_N);
    return b;
}

/* ---------------- 查找 ---------------- */

static uint16_t fb_lookup(uint32_t letter)
{
    uint32_t i;

    for (i = 0; i < s_fb.n_cmap; i++)
    {
        fb_cmap_t *c = &s_fb.cmap[i];
        uint32_t rcp;

        if (letter < c->range_start) continue;

        rcp = letter - c->range_start;
        if (rcp >= c->range_len) continue;

        if (c->fmt == 2)
        {
            return (uint16_t)(c->gid_start + rcp);
        }
        else if (c->fmt == 0)
        {
            /* FORMAT0_FULL：偏移表固定是 uint8_t
             *（lv_font_loader.c: ids_size = sizeof(uint8_t) * data_entries_count）
             * 不能用 gid_fmt 判断，gid_fmt 对应的是 SPARSE_FULL 的 uint16 表 */
            uint8_t v = 0;
            fb_get(s_fb.cmap_base + c->data_off + rcp, &v, 1);
            return (uint16_t)(c->gid_start + v);
        }
        else if (c->fmt == 3 || c->fmt == 1)
        {
            uint32_t base = s_fb.cmap_base + c->data_off;
            uint32_t idx;
            uint8_t  pnum = s_fb.cmap_pnum[i];
            uint8_t  pbase = s_fb.cmap_pbase[i];

            if (pnum > 0)
            {
                /* 先用页索引定位到 64 项的小窗口，只读一次卡 */
                uint16_t win[FB_PAGE_ENTRIES];
                uint32_t plo = 0, phi = pnum, page, first, cnt, k;
                uint32_t need;

                while (plo + 1 < phi)
                {
                    uint32_t mid = (plo + phi) >> 1;
                    if (s_page[pbase + mid] <= rcp) plo = mid;
                    else                            phi = mid;
                }
                page  = plo;
                first = page * FB_PAGE_ENTRIES;
                cnt   = c->entries - first;
                if (cnt > FB_PAGE_ENTRIES) cnt = FB_PAGE_ENTRIES;
                need  = cnt * 2;

                if (!fb_get(base + first * 2, win, need)) return 0;

                for (k = 0; k < cnt; k++)
                {
                    if (win[k] == rcp)
                    {
                        idx = first + k;
                        if (c->fmt == 3) return (uint16_t)(c->gid_start + idx);
                        return (uint16_t)(c->gid_start + fb_u16(base + c->entries * 2 + idx * 2));
                    }
                    if (win[k] > rcp) break;
                }
            }
            else
            {
                /* 没建索引就退化为全表二分 */
                uint32_t lo = 0, hi = c->entries;
                while (lo < hi)
                {
                    uint32_t mid = (lo + hi) >> 1;
                    uint16_t key = fb_u16(base + mid * 2);
                    if (key == rcp)
                    {
                        if (c->fmt == 3) return (uint16_t)(c->gid_start + mid);
                        return (uint16_t)(c->gid_start + fb_u16(base + c->entries * 2 + mid * 2));
                    }
                    if (key < rcp) lo = mid + 1;
                    else           hi = mid;
                }
            }
        }
    }

    return 0;
}

/* 取字形条目在文件中的偏移与长度 */
static uint8_t fb_entry_pos(uint16_t gid, uint32_t *off, uint16_t *len)
{
    uint32_t a, b;

    if (gid >= s_fb.loca_count) return 0;

    /* loc_fmt = 0 -> uint16 偏移；1 -> uint32 偏移
     * （这个 .bin 是 16 位：loca 段 300 字节 = 12 + 144*2） */
    if (s_fb.loc_fmt == 0)
    {
        a = fb_u16(s_fb.loca_base + gid * 2u);
        b = (gid + 1u < s_fb.loca_count)
              ? fb_u16(s_fb.loca_base + (gid + 1u) * 2u)
              : s_fb.glyf_size;
    }
    else
    {
        a = fb_u32(s_fb.loca_base + gid * 4u);
        b = (gid + 1u < s_fb.loca_count)
              ? fb_u32(s_fb.loca_base + (gid + 1u) * 4u)
              : s_fb.glyf_size;
    }

    if (b <= a)
    {
        return 0;
    }
    if (b - a > 0xFFFF) return 0;

    *off = s_fb.glyf_base + a;
    *len = (uint16_t)(b - a);
    return 1;
}

/* ---------------- 位解码 ---------------- */

static uint32_t fb_bits(const uint8_t *buf, uint32_t bitpos, uint32_t n)
{
    uint32_t v = 0, k;

    for (k = 0; k < n; k++)
    {
        uint32_t p = bitpos + k;
        v = (v << 1) | ((buf[p >> 3] >> (7 - (p & 7))) & 1u);
    }
    return v;
}

static int32_t fb_bits_signed(const uint8_t *buf, uint32_t bitpos, uint32_t n)
{
    uint32_t v = fb_bits(buf, bitpos, n);
    if (n < 32 && (v & (1u << (n - 1)))) v |= ~0u << n;
    return (int32_t)v;
}

static uint32_t fb_parse_dsc(const uint8_t *e, fb_dc_t *d)
{
    uint32_t pos = 0;
    uint32_t raw_adv, adv;

    raw_adv = s_fb.adv_bits ? fb_bits(e, pos, s_fb.adv_bits) : 0;
    pos += s_fb.adv_bits;

    adv = s_fb.adv_fmt ? ((raw_adv + 8u) >> 4) : raw_adv;

    d->adv = (uint16_t)adv;
    d->ox   = (int8_t)fb_bits_signed(e, pos, s_fb.xy_bits); pos += s_fb.xy_bits;
    d->oy   = (int8_t)fb_bits_signed(e, pos, s_fb.xy_bits); pos += s_fb.xy_bits;
    d->w    = (uint8_t)fb_bits(e, pos, s_fb.wh_bits);       pos += s_fb.wh_bits;
    d->h    = (uint8_t)fb_bits(e, pos, s_fb.wh_bits);       pos += s_fb.wh_bits;

    return pos;
}

static const uint8_t *fb_render(const uint8_t *e, uint16_t w, uint16_t h)
{
    /* LVGL 的点阵是【连续位流】：每行占 w*bpp 个 bit，不按字节补齐
     *（见 lv_draw_sw_letter.c: width_bit = box_w * bpp; bit_ofs = row_start*width_bit）
     * 所以输出也必须连续按位写入，不能按行字节对齐。 */
    uint32_t total = (uint32_t)w * h * s_fb.bpp;
    uint32_t need  = (total + 7u) >> 3;
    uint32_t y, x, i;

    if (need == 0 || need > FB_BMP_MAX) return 0;

    memset(s_fb.bmp, 0, need);

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            uint32_t v  = fb_bits(e, s_fb.bitpos + ((uint32_t)y * w + x) * s_fb.bpp, s_fb.bpp);
            uint32_t ob = ((uint32_t)y * w + x) * s_fb.bpp;

            for (i = 0; i < s_fb.bpp; i++)
            {
                if ((v >> (s_fb.bpp - 1u - i)) & 1u)
                    s_fb.bmp[(ob + i) >> 3] |= (uint8_t)(0x80u >> ((ob + i) & 7u));
            }
        }
    }

    return s_fb.bmp;
}

/* ---------------- LVGL 回调 ---------------- */

/* 记录“字库里没有这个字”，避免每帧重复查表 + 读 SPI */
static void fb_dc_miss(uint32_t letter)
{
    fb_dc_t *m = fb_dc_alloc();

    m->letter = letter;
    m->off    = 0;
    m->len    = 0;
    m->adv    = 0;
    m->ox     = 0;
    m->oy     = 0;
    m->w      = 0;
    m->h      = 0;
    m->valid  = 1;

    fb_dc_touch(m);
}

static bool fb_get_dsc_i(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                          uint32_t letter, uint32_t letter_next)
{
    fb_dc_t *d;
    uint16_t gid, len;
    uint32_t off;

    (void)font;
    (void)letter_next;

    if (letter == '\t') letter = ' ';

    if (letter < 0x20) return false;      /* 换行/控制字符：字库里没有，静默跳过 */

    if (s_fb.last_ok && letter == s_fb.last_letter)
    {
        *dsc = s_fb.last_dsc;
        s_st_dsc_hit++;
        return true;
    }

    d = fb_dc_find(letter);
    if (d == 0)
    {
        s_st_dsc_miss++;
        gid = fb_lookup(letter);

        if (gid == 0)
        {
            if (letter != s_last_miss) { s_last_miss = letter;  }
            fb_dc_miss(letter);
            return false;
        }

        if (!fb_entry_pos(gid, &off, &len))
        {
            if (letter != s_last_miss) { s_last_miss = letter;  }
            fb_dc_miss(letter);
            return false;
        }

        if (len > FB_ENTRY_MAX)
        {
            if (letter != s_last_miss) { s_last_miss = letter;  }
            fb_dc_miss(letter);
            return false;
        }

        if (!fb_read(off, s_fb.entry, len))
        {
            if (letter != s_last_miss)
            {
                s_last_miss = letter;
            }
            return false;                     /* 读失败不缓存，下次重试 */
        }

        d = fb_dc_alloc();
        d->letter = letter;
        d->off    = off;
        d->len    = len;
        fb_parse_dsc(s_fb.entry, d);
        d->valid  = 1;

        fb_dc_touch(d);
    }

    s_st_dsc_hit++;

    if (d->len == 0) return false;      /* 负缓存：字库里没有这个字 */

    dsc->adv_w = d->adv;
    dsc->box_w = d->w;
    dsc->box_h = d->h;
    dsc->ofs_x = d->ox;
    dsc->ofs_y = d->oy;
    dsc->bpp   = s_fb.bpp;
    dsc->is_placeholder = 0;

    s_fb.last_letter = letter;
    s_fb.last_dsc    = *dsc;
    s_fb.last_ok     = 1;

    return true;
}

static const uint8_t *fb_get_bmp_i(const lv_font_t *font, uint32_t letter)
{
    fb_bc_t *b;
    fb_dc_t *d;

    (void)font;

    if (letter == '\t') letter = ' ';

    if (!s_fb.last_ok || letter != s_fb.last_letter)
    {
        lv_font_glyph_dsc_t tmp;
        if (!fb_get_dsc_i(font, &tmp, letter, 0)) return 0;
    }

    if (s_fb.last_dsc.box_w == 0 || s_fb.last_dsc.box_h == 0) return 0;

#if FB_USE_BC
    b = fb_bc_find(letter);
    if (b == 0)
    {
        const uint8_t *pb;
        uint32_t need;

        s_st_bmp_miss++;

        d = fb_dc_find(letter);
        if (d == 0) return 0;
        if (d->len > FB_ENTRY_MAX) return 0;
        if (!fb_read(d->off, s_fb.entry, d->len)) return 0;

        pb = fb_render(s_fb.entry, d->w, d->h);
        if (pb == 0) return 0;

        need = ((uint32_t)d->w * d->h * s_fb.bpp + 7u) >> 3;
        if (need > FB_BC_BYTES) return pb;          /* 太大不缓存 */

        b = fb_bc_alloc();
        b->letter = letter;
        b->len    = d->len;
        b->w      = d->w;
        b->h      = d->h;
        memcpy(b->data, pb, need);
        b->valid  = 1;
        fb_bc_touch(b);

        return b->data;
    }

    d = fb_dc_find(letter);
    if (d == 0) return 0;

    s_st_bmp_hit++;

    return b->data;                                 /* 直接返回缓存好的位图 */
#else
    /* 缓存关闭：每次都从 W25Q 读原始条目再渲染 */
    {
        fb_dc_t *d2 = fb_dc_find(letter);

        if (d2 == 0) return 0;
        if (d2->len > FB_ENTRY_MAX) return 0;
        if (!fb_read(d2->off, s_fb.entry, d2->len)) return 0;

        return fb_render(s_fb.entry, d2->w, d2->h);
    }
#endif
}

/* ---------------- 加载 ---------------- */

/* ---------------- 加载 ---------------- */

/* 解析头部并组装 lv_font_t（数据源由 s_use_store 决定） */
static bool fb_get_dsc_cb(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                          uint32_t letter, uint32_t letter_next)
{
    uint32_t c0 = DWT->CYCCNT;
    bool r = fb_get_dsc_i(font, dsc, letter, letter_next);

    s_st_dcyc += DWT->CYCCNT - c0;
    s_st_dn++;

    return r;
}

static const uint8_t *fb_get_bmp_cb(const lv_font_t *font, uint32_t letter)
{
    return fb_get_bmp_i(font, letter);
}

static lv_font_t *font_bin_parse(void)
{
    uint8_t  buf[64];

    if (s_fb_mtx == 0) s_fb_mtx = xSemaphoreCreateMutex();
    uint32_t head_size, cmap_size, loca_size, glyf_size, i;
    uint32_t off;

    if (!fb_read(0, buf, 48))
    {
        return 0;
    }
    if (memcmp(&buf[4], "head", 4) != 0)
    {
        return 0;
    }

    head_size = rd32(&buf[0]);

    s_fb.line_height = (uint16_t)((int32_t)rd16(&buf[0x10]) - (int32_t)rdi16(&buf[0x12]));
    s_fb.base_line   = (uint16_t)(-(int32_t)rdi16(&buf[0x12]));
    s_fb.loc_fmt     = buf[0x22];
    s_fb.gid_fmt     = buf[0x23];
    s_fb.adv_fmt     = buf[0x24];
    s_fb.bpp         = buf[0x25];
    s_fb.xy_bits     = buf[0x26];
    s_fb.wh_bits     = buf[0x27];
    s_fb.adv_bits    = buf[0x28];
    s_fb.subpx       = buf[0x2A];
    s_fb.underline_pos = (int8_t)rdi16(&buf[0x2C]);

    s_fb.bitpos = (uint8_t)(s_fb.adv_bits + 2 * s_fb.xy_bits + 2 * s_fb.wh_bits);

    if (s_fb.bpp != 1 && s_fb.bpp != 2 && s_fb.bpp != 4 && s_fb.bpp != 8) return 0;

    s_fb.cmap_base = head_size;
    if (!fb_read(s_fb.cmap_base, buf, 12)) return 0;
    if (memcmp(&buf[4], "cmap", 4) != 0) return 0;

    cmap_size   = rd32(&buf[0]);
    s_fb.n_cmap = rd32(&buf[8]);
    if (s_fb.n_cmap > FB_MAX_CMAP) return 0;

    for (i = 0; i < s_fb.n_cmap; i++)
    {
        uint8_t d[16];
        if (!fb_read(s_fb.cmap_base + 12 + i * 16, d, 16)) return 0;

        s_fb.cmap[i].data_off    = rd32(&d[0]);
        s_fb.cmap[i].range_start = rd32(&d[4]);
        s_fb.cmap[i].range_len   = rd16(&d[8]);
        s_fb.cmap[i].gid_start   = rd16(&d[10]);
        s_fb.cmap[i].entries     = rd16(&d[12]);
        s_fb.cmap[i].fmt         = d[14];
    }

    /* 建 cmap 页索引：每个稀疏子表每 64 项取一个「相对码点」 */
    s_fb.page_cnt = 0;
    for (i = 0; i < s_fb.n_cmap; i++)
    {
        fb_cmap_t *c = &s_fb.cmap[i];
        uint32_t pages;
        uint32_t p;

        s_fb.cmap_pbase[i] = s_fb.page_cnt;
        s_fb.cmap_pnum[i]  = 0;

        if (c->fmt != 1 && c->fmt != 3) continue;

        pages = (c->entries + FB_PAGE_ENTRIES - 1) / FB_PAGE_ENTRIES;
        if (s_fb.page_cnt + pages > FB_MAX_PAGES) continue;   /* 放不下就不建 */

        for (p = 0; p < pages; p++)
        {
            s_page[s_fb.page_cnt++] = fb_u16(s_fb.cmap_base + c->data_off + p * FB_PAGE_ENTRIES * 2);
        }
        s_fb.cmap_pnum[i] = (uint8_t)pages;
    }
    off = s_fb.cmap_base + cmap_size;
    if (!fb_read(off, buf, 12)) return 0;
    if (memcmp(&buf[4], "loca", 4) != 0) return 0;

    loca_size       = rd32(&buf[0]);
    s_fb.loca_count = rd32(&buf[8]);
    s_fb.loca_base  = off + 12;

    off = s_fb.cmap_base + cmap_size + loca_size;
    if (!fb_read(off, buf, 8)) return 0;
    if (memcmp(&buf[4], "glyf", 4) != 0) return 0;

    glyf_size      = rd32(&buf[0]);
    s_fb.glyf_base = off;
    s_fb.glyf_size = glyf_size;

    s_fb.font.get_glyph_dsc       = fb_get_dsc_cb;
    s_fb.font.get_glyph_bitmap    = fb_get_bmp_cb;
    s_fb.font.line_height         = (lv_coord_t)s_fb.line_height;
    s_fb.font.base_line           = (lv_coord_t)s_fb.base_line;
    s_fb.font.subpx               = s_fb.subpx;
    s_fb.font.underline_position  = s_fb.underline_pos;
    s_fb.font.underline_thickness = 0;
    s_fb.font.dsc                 = &s_fb;
    s_fb.font.fallback            = 0;

    return &s_fb.font;
}

/* 直接从 SD 卡文件加载（兜底路径） */
lv_font_t *font_bin_load(const char *path)
{
    memset(&s_fb, 0, sizeof(s_fb));
    s_use_store = 0;

    WCHAR wpath[64];
    utf8_to_utf16(path, wpath, 64);
    fs_lock();
    if (f_open(&s_fb.fp, wpath, FA_READ | FA_OPEN_EXISTING) != FR_OK)
    {
        fs_unlock();
        printf("[fb] open FAILED\r\n");
        return 0;
    }
    fs_unlock();
    s_fb.opened = 1;

    return font_bin_parse();
}

/* 优先用 W25Q16（快），没有就安装一次，再不行退回 SD */
lv_font_t *font_bin_load_any(const char *path)
{
    uint32_t sz = 0;

    if (FONT_STORE_FORCE || !font_store_valid(&sz))
    {
        if (!font_store_install(path, &sz))
        {
            return font_bin_load(path);
        }
    }

    memset(&s_fb, 0, sizeof(s_fb));
    memset(s_dc, 0, sizeof(s_dc));
    memset(s_bc, 0, sizeof(s_bc));
    s_dc_rr = 0;
    s_bc_rr = 0;
    s_use_store = 1;
    s_fb.opened = 1;

    return font_bin_parse();
}

uint8_t font_bin_ready(void)
{
    return s_fb.opened;
}

void font_bin_dump(void)
{
}

void font_bin_selftest(const lv_font_t *f)
{
    static const uint32_t test[] = { 'A', 0x5C0F, 0x7535, 0x53F0, 0x5FC3, '-' };
    uint32_t i;

    if (!f) {  return; }

    for (i = 0; i < sizeof(test) / sizeof(test[0]); i++)
    {
        lv_font_glyph_dsc_t d;
        const uint8_t *bmp;

        memset(&d, 0, sizeof(d));

        if (!f->get_glyph_dsc(f, &d, test[i], 0))
        {
            continue;
        }

        bmp = f->get_glyph_bitmap(f, test[i]);

        if (bmp)
        {
        }
        else
        {
        }
    }
}

/* 预热：把一段 UTF-8 文本里出现的字形全部读进缓存（最多 max_chars 个）
 * 填列表 / 换歌后调一次，滚动时就不用再去 SD 读字形 */
void font_bin_preload_utf8(const char *s, uint32_t max_chars)
{
    uint32_t n = 0;

    if (s == 0 || !s_fb.opened) return;

    while (*s && n < max_chars)
    {
        uint32_t cp;
        uint8_t  c = (uint8_t)*s++;

        if (c < 0x80u) cp = c;
        else if ((c & 0xE0u) == 0xC0u)
        {
            cp = ((uint32_t)(c & 0x1Fu) << 6) | ((uint8_t)*s++ & 0x3Fu);
        }
        else if ((c & 0xF0u) == 0xE0u)
        {
            cp = ((uint32_t)(c & 0x0Fu) << 12) |
                 (((uint32_t)(uint8_t)s[0] & 0x3Fu) << 6) |
                 ((uint32_t)(uint8_t)s[1] & 0x3Fu);
            s += 2;
        }
        else if ((c & 0xF8u) == 0xF0u)
        {
            cp = ((uint32_t)(c & 0x07u) << 18) |
                 (((uint32_t)(uint8_t)s[0] & 0x3Fu) << 12) |
                 (((uint32_t)(uint8_t)s[1] & 0x3Fu) << 6) |
                 ((uint32_t)(uint8_t)s[2] & 0x3Fu);
            s += 3;
        }
        else continue;

        if (cp == '\n' || cp == '\r' || cp == 0) continue;

        {
            lv_font_glyph_dsc_t d;

            memset(&d, 0, sizeof(d));

            if (fb_get_dsc_i(&s_fb.font, &d, cp, 0)) (void)fb_get_bmp_i(&s_fb.font, cp);
        }

        n++;
    }
}

/* 打印并清零缓存统计 */
void font_bin_stats(void)
{
    s_st_dcyc = s_st_dn = 0;

    s_st_dsc_hit = s_st_dsc_miss = 0;
    s_st_bmp_hit = s_st_bmp_miss = 0;
    s_st_sd = 0;
}

/* ---------------- 字体预取任务（最低优先级） ---------------- */

#define FB_PRELOAD_LEN   256

static QueueHandle_t s_fb_q   = 0;
static char          s_fb_txt[FB_PRELOAD_LEN];

void font_bin_task(void *pv)
{
    (void)pv;

    s_fb_q = xQueueCreate(1, FB_PRELOAD_LEN);

    for (;;)
    {
        if (s_fb_q == 0)
        {
            vTaskDelay(100);
            continue;
        }

        if (xQueueReceive(s_fb_q, s_fb_txt, portMAX_DELAY) == pdTRUE)
            font_bin_preload_utf8(s_fb_txt, 160);
    }
}

/* UI 调用：只把要预读的文本丢给任务，立即返回 */
void font_bin_preload_async(const char *s)
{
    char buf[FB_PRELOAD_LEN];

    if (s == 0) return;

    if (s_fb_q == 0)                 /* 任务还没起来：同步做 */
    {
        font_bin_preload_utf8(s, 160);
        return;
    }

    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    (void)xQueueOverwrite(s_fb_q, buf);   /* 队列长度 1，总是保留最新 */
}
