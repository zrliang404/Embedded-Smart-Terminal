#include "lvgl.h"
#include "lcd.h"          /* 提供 ILI9341_OpenWindow / FSMC_Addr_ILI9341_DATA / CMD_SetPixel */

/* ============================================================
 * lv_port_disp.c  —— LVGL 显示对接（ILI9341 + FSMC 16位并口）
 * ============================================================ */

/* --- 屏幕方向参数：必须和 main.c 里 ILI9341_GramScan(1) 的横屏一致 --- */
#define DISP_HOR_RES    320     /* 横屏宽 */
#define DISP_VER_RES    240     /* 横屏高 */
#define DISP_BUF_LINES  24      /* 一次刷 24 行，320*24*2 = 15,360 字节 */

/* lcd.c 里这两个是全局函数（非 static），这里声明一下就能用 */
extern void ILI9341_Write_Cmd(uint16_t usCmd);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t         buf_1[DISP_HOR_RES * DISP_BUF_LINES] __attribute__((section(".ccmram"), zero_init));

/* ------------------------------------------------------------
 * flush 回调：LVGL 把画好的一块矩形交给我们，我们推给 ILI9341
 * ------------------------------------------------------------ */
static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);
    uint32_t cnt = (uint32_t)w * h;

    /* FSMC 数据口地址（0x60020000，RS=1）。直接解引用就是往 LCD 写像素 */
    volatile uint16_t *pData = (volatile uint16_t *)FSMC_Addr_ILI9341_DATA;
    const uint16_t    *pSrc  = (const uint16_t *)color_p;

    /* ① 设置显示窗口（内部发 0x2A / 0x2B） */
    ILI9341_OpenWindow((uint16_t)area->x1, (uint16_t)area->y1, w, h);

    /* ② 发 RAMWR(0x2C) 命令，之后连续写的就是像素数据 */
    ILI9341_Write_Cmd(CMD_SetPixel);

        /* ---- 快速搬运 ----
     * FSMC 是 16 位总线，但对它做 32 位写会被自动拆成两个 16 位周期，
     * 所以一次 32 位写 = 搬 2 个像素，指令数减半。
     * -O0 下这个优化效果明显（每次 flush 少一半循环开销）。 */
    {
        volatile uint32_t *pD32 = (volatile uint32_t *)pData;
        const uint16_t    *pS16 = (const uint16_t *)pSrc;
        uint32_t n  = cnt >> 1;          /* 像素对数 */
        uint32_t r  = cnt & 1u;          /* 奇数尾巴 */

        while (n--) {
            uint32_t lo = *pS16++;
            uint32_t hi = *pS16++;
            *pD32++ = lo | (hi << 16);
        }
        if (r) {
            *(volatile uint16_t *)pD32 = *pS16;
        }
    }

    /* ④ 必须告诉 LVGL“这块刷完了”，否则它不会再给下一块（只刷第一块就卡住） */
    lv_disp_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    /* 第 4 个参数是【像素个数】，不是字节数 */
    lv_disp_draw_buf_init(&draw_buf, buf_1, NULL, DISP_HOR_RES * DISP_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = DISP_HOR_RES;
    disp_drv.ver_res  = DISP_VER_RES;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
}
