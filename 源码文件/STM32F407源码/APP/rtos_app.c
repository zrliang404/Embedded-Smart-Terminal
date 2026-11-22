#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include "ui.h"
#include "delay.h"
#include "stdio.h"

#include "font_bin.h"
#include "soft_uart.h"
#include "ai_app.h"
#include "uart_task.h"


/* 用户自定义界面逻辑初始化（定义在 UI\ui_user.c） */
void ui_user_init(void);
void audio_task_entry(void *pvParameters);

#define LVGL_TASK_STACK   2048       /* 8KB，LVGL 渲染 + ui_init 调用链较深，3KB 太紧 */

static TaskHandle_t xLvglTaskHandle    = NULL;

/* ---------------- LVGL 任务：唯一允许碰 LVGL 的任务 ---------------- */

void vLvglTask(void *pvParameters)
{
    (void)pvParameters;

    printf("[12] lvgl task started\r\n");

    ui_init();
    printf("[13] ui_init done\r\n");                       /* 建屏幕（此时 LVGL 已 lv_init 过） */
    ui_user_init();
    printf("[14] ui_user_init done\r\n");

    for (;;)
    {
        ai_app_poll();               /* AI 页：串口协议处理 */

        lv_timer_handler();          /* LVGL 主循环：处理事件、刷新、动画 */


        delay_ms(10);                /* 让出 CPU，给空闲任务 */
    }
}

void RTOS_AppStart(void)
{
    uart_task_start();      /* ESP 交互任务（优先级 1） */

    xTaskCreate(vLvglTask,    "LVGL", LVGL_TASK_STACK, NULL, 2, &xLvglTaskHandle);
    xTaskCreate(audio_task_entry, "AUDIO", 3072,        NULL, 4, NULL);   /* 音频优先级最高 */
    xTaskCreate(font_bin_task,    "FONT",  512,         NULL, 1, NULL);   /* 字体预取：最低优先级 */

    configASSERT(xLvglTaskHandle    != NULL);
}

/* ---------------- 内核钩子（保持不动） ---------------- */
volatile char    *g_pcOverflowTask = NULL;
volatile uint32_t g_ulOverflowCnt  = 0;

/* FreeRTOS 断言：打印出错的文件和行号后再停机，便于定位 */
void vAssertCalled(const char *file, int line)
{
    taskDISABLE_INTERRUPTS();

    printf("\r\n!!! ASSERT %s:%d !!!\r\n", file, line);

    for (;;) { }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    g_pcOverflowTask = pcTaskName;
    g_ulOverflowCnt++;
    printf("[overflow] task=%s cnt=%u\r\n", pcTaskName, (unsigned)g_ulOverflowCnt);
    for (;;) { }
}

void vApplicationMallocFailedHook(void)
{
    for (;;) { }
}
