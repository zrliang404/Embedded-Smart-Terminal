#include "lcd.h"
#include "delay.h"
#include "sys.h"
#include "usart.h"
#include "touch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "RTOS_APP.h"
#include "lvgl.h"
#include "ui.h"
#include "spi.h"
#include "w25qxx.h"
#include "sdio_sdcard.h"
#include "ff.h"
#include "exfuns.h"
#include "audio_app.h"
#include "soft_uart.h"
#include "backlight.h"
#include "fs_lock.h"
#include "spi_bus.h"
#include "stdio.h"
#include "string.h"

void lv_port_disp_init(void);
void lv_port_indev_init(void);

extern u8 TP_Init(void);

/* SD 卡初始化：放在最前面 + 重试，和之前测试通过的顺序一致 */
static u8 sd_init_retry(void)
{
    u8 i, r;

    for (i = 0; i < 3; i++)
    {
        r = SD_Init();
        if (r == 0) return 0;
        delay_us(200000);          /* 200ms 再试 */
    }
    return r;
}

int main(void)
{
    u8 r;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    Delay_Init(168);
    uart_init(115200);

    printf("\r\n=== BOOT ===\r\n");
    printf("[1] uart ok\r\n");

    /* 1. SD 卡最先初始化（和之前验证通过的顺序一致） */
    r = sd_init_retry();
    printf("[2] sd = %d\r\n", (int)r);

    if (r == 0)
    {
        r = exfuns_init();
        if (r == 0)
        {
            static const WCHAR s_drv0[] = { '0', ':', 0 };
            r = f_mount(fs[0], (const TCHAR *)s_drv0, 1);
        }
    }

    /* 2. W25Q16：SPI1 必须先就绪，触摸读校准参数要用 */
    printf("[3] sd mount done\r\n");

    /* 触摸与 W25Q 共用 PA5/PA6/PA7：先把触摸片选 PB14 拉高，
     * 否则 XPT2046 会占住 MISO，读 W25Q 全是 0 */
    {
        GPIO_InitTypeDef g;

        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
        g.GPIO_Pin   = GPIO_Pin_14;
        g.GPIO_Mode  = GPIO_Mode_OUT;
        g.GPIO_OType = GPIO_OType_PP;
        g.GPIO_Speed = GPIO_Speed_100MHz;
        g.GPIO_PuPd  = GPIO_PuPd_UP;
        GPIO_Init(GPIOB, &g);
        GPIO_SetBits(GPIOB, GPIO_Pin_14);
    }

    W25QXX_Init();
    {
        u16 id = W25QXX_ReadID();
        u8  sr = W25QXX_ReadSR();

        u8 jed[3];

        W25QXX_ReadJEDEC(jed);
        printf("[4] w25qxx ok  ID=0x%04X  SR=0x%02X  JEDEC=%02X %02X %02X\r\n",
               id, sr, jed[0], jed[1], jed[2]);

        if (jed[0] == 0xEF)
        {
            u32 bytes = (jed[2] >= 0x10) ? (1u << (jed[2] - 0x10)) : 0;
            printf("[4] Winbond, capacity code 0x%02X = %u KB\r\n", jed[2], (unsigned)(bytes / 1024));
        }

        if (id == 0x0000 || id == 0xFFFF)
        {
            printf("[4] !! W25Q 无响应（检查焊接/CS/HOLD/WP）\r\n");
        }
        else if ((id >> 8) == 0xEF)
        {
            printf("[4] Winbond 芯片, 容量代码 0x%02X\r\n", id & 0xFF);
        }
    }

    /* 3. LCD */
    printf("[4.1] lcd init start\r\n");


    ILI9341_Init();
    printf("[4.2] lcd init done\r\n");

    ILI9341_GramScan(3);
    printf("[5] lcd ok\r\n");

    /* 背光 PWM：PD12 = TIM4_CH1 */
    backlight_init();


    /* 4. 触摸 */
    TP_Init();

    /* TP_Init 会把 PA5/PA6/PA7 切成 GPIO 做位翻转，
     * 这里必须切回 SPI1，否则后面读 W25Q（字库）全是 0 */
    SPI1_PinsToSPI();

    printf("[6] tp ok\r\n");

    /* 4.0 FatFs 互斥锁（音频任务和界面任务都访问 SD） */
    fs_lock_init();
    spi_bus_init();
    printf("[7] fs lock ok\r\n");

    /* 4.1 音频硬件初始化（WM8978 + I2S 时钟在首次播放时配置） */
    audio_app_init();
    printf("[8] audio hw ok\r\n");


    /* 5. LVGL */
    lv_init();
    printf("[9] lv_init ok\r\n");
    lv_port_disp_init();
    lv_port_indev_init();
    printf("[10] lv ports ok\r\n");

    /* 软件串口：PA11=TX, PA12=RX, 9600 8N1 */
    suart_init(38400);
    suart_send_str("F407 READY\r\n");

    /* 顺带让 ESP32 也重启一次，保证两边状态一致 */
    suart_send_str("RST\n");
    printf("[10.5] soft uart ok (PA11=TX PA12=RX 9600)\r\n");

    /* 6. RTOS */
    printf("[11] starting scheduler\r\n");
    RTOS_AppStart();
    vTaskStartScheduler();

    while (1);
}
