/* ============================================================
 * lv_port_indev.c  —— LVGL 触摸输入对接（XPT2046）
 * ============================================================ */
#include "lvgl.h"
#include "touch.h"        /* 提供 tp_dev / TP_PRES_DOWN */

/* ------------------------------------------------------------
 * 读触摸：LVGL 每 30ms 调一次
 * ------------------------------------------------------------ */
static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    /* 松手时 LVGL 仍需要坐标，必须保存上一次的值，否则会跳到 (0,0) */
    static lv_coord_t last_x = 0;
    static lv_coord_t last_y = 0;

    tp_dev.scan(0);                          /* 约 0.6ms，位翻转 SPI */

    if (tp_dev.sta & TP_PRES_DOWN) {
        last_x = (lv_coord_t)tp_dev.x;       /* TP_Scan 内部已换算成屏幕坐标 */
        last_y = (lv_coord_t)tp_dev.y;
        data->state = LV_INDEV_STATE_PR;     /* 按下 */
    } else {
        data->state = LV_INDEV_STATE_REL;    /* 松开 */
    }

    data->point.x = last_x;
    data->point.y = last_y;
}

void lv_port_indev_init(void)
{
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    lv_indev_drv_register(&indev_drv);
}
