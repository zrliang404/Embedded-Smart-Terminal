#include "FreeRTOS.h"
#include "task.h"
#include "monitorPage.h"
#include "ui.h"
#include "stm32f4xx.h"
#include <stdio.h>


uint32_t RunTimeStat_Get(void)
{
    return DWT->CYCCNT;
}


extern uint32_t Image$$RW_IRAM1$$ZI$$Limit;
extern uint32_t Image$$RW_CCM$$ZI$$Limit;
extern uint32_t Image$$ER_IROM1$$Limit;

#define MON_SRAM_BASE     0x20000000u
#define MON_SRAM_TOTAL    (128u * 1024u)
#define MON_CCM_BASE      0x10000000u
#define MON_CCM_TOTAL     (64u * 1024u)
#define MON_FLASH_BASE    0x08000000u
#define MON_FLASH_TOTAL   (512u * 1024u)


uint8_t monitor_cpu_pct(void)
{
    static uint32_t last_idle = 0, last_cyc = 0;

    uint32_t idle_now = (uint32_t)ulTaskGetIdleRunTimeCounter();
    uint32_t cyc_now  = DWT->CYCCNT;
    uint32_t d_idle   = idle_now - last_idle;
    uint32_t d_cyc    = cyc_now  - last_cyc;
    uint32_t idle_pct;

    last_idle = idle_now;
    last_cyc  = cyc_now;

    if (d_cyc == 0u) return 0u;

    idle_pct = (uint32_t)(((uint64_t)d_idle * 100u) / d_cyc);
    if (idle_pct > 100u) idle_pct = 100u;

    return (uint8_t)(100u - idle_pct);
}

/* ---------------- RAM \u5360\u7528\u7387 ----------------
 * = \u9759\u6001\u6bb5\uff08RW+ZI\uff09 + FreeRTOS \u5806\u5df2\u7528\uff0c\u5206\u6bcd SRAM(128K) + CCM(64K) */
uint8_t monitor_ram_pct(void)
{
    uint32_t heap_total = (uint32_t)configTOTAL_HEAP_SIZE;
    uint32_t heap_used  = heap_total - (uint32_t)xPortGetFreeHeapSize();
    uint32_t sram_stat  = (uint32_t)&Image$$RW_IRAM1$$ZI$$Limit - MON_SRAM_BASE;
    uint32_t ccm_stat   = (uint32_t)&Image$$RW_CCM$$ZI$$Limit  - MON_CCM_BASE;
    uint32_t sram_used, used;

    if (sram_stat < heap_total) return 0u;      /* \u9759\u6001\u6bb5\u91cc\u5df2\u5305\u542b\u6574\u4e2a\u5806\u6570\u7ec4 */

    sram_used = sram_stat - heap_total + heap_used;
    used      = sram_used + ccm_stat;

    return (uint8_t)((used * 100u) / (MON_SRAM_TOTAL + MON_CCM_TOTAL));
}

/* ---------------- \u5185\u90e8 FLASH \u5360\u7528\u7387 ---------------- */
uint8_t monitor_flash_pct(void)
{
    uint32_t used = (uint32_t)&Image$$ER_IROM1$$Limit - MON_FLASH_BASE;

    if (used > MON_FLASH_TOTAL) used = MON_FLASH_TOTAL;

    return (uint8_t)((used * 100u) / MON_FLASH_TOTAL);
}

/* ---------------- \u82af\u7247\u6e29\u5ea6 ----------------
 * F407 \u5185\u90e8\u6e29\u5ea6\u4f20\u611f\u5668 = ADC1_IN16
 * T = (Vsense - 0.76V) / 2.5mV + 25 */
static void mon_temp_init(void)
{
    ADC_CommonInitTypeDef cc;
    ADC_InitTypeDef       ai;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    ADC_CommonStructInit(&cc);
    cc.ADC_Mode             = ADC_Mode_Independent;
    cc.ADC_Prescaler        = ADC_Prescaler_Div4;
    cc.ADC_DMAAccessMode    = ADC_DMAAccessMode_Disabled;
    cc.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles;
    ADC_CommonInit(&cc);

    ADC_StructInit(&ai);
    ai.ADC_Resolution           = ADC_Resolution_12b;
    ai.ADC_ScanConvMode         = DISABLE;
    ai.ADC_ContinuousConvMode   = DISABLE;
    ai.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None;
    ai.ADC_DataAlign            = ADC_DataAlign_Right;
    ai.ADC_NbrOfConversion      = 1;
    ADC_Init(ADC1, &ai);

    ADC_TempSensorVrefintCmd(ENABLE);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_16, 1, ADC_SampleTime_480Cycles);
    ADC_Cmd(ADC1, ENABLE);
}

int16_t monitor_temp_c(void)
{
    uint16_t raw;
    int32_t  mv, t10;

    ADC_SoftwareStartConv(ADC1);
    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET) { }

    raw = ADC_GetConversionValue(ADC1);

    mv  = ((int32_t)raw * 3300) / 4095;     /* \u6beb\u4f0f */
    t10 = (mv - 760) * 4 + 250;             /* (mv-760)/2.5 + 25\uff0c\u5355\u4f4d 0.1\u2103 */

    return (int16_t)(t10 / 10);
}

/* ---------------- \u754c\u9762\u5237\u65b0 ---------------- */
static void monitor_timer_cb(lv_timer_t *tmr)
{
    uint8_t cpu   = monitor_cpu_pct();
    uint8_t ram   = monitor_ram_pct();
    uint8_t flash = monitor_flash_pct();
    int16_t t     = monitor_temp_c();

    (void)tmr;

    if (t < 0)   t = 0;
    if (t > 100) t = 100;

    lv_arc_set_value(ui_cpuUsageRate, cpu);
    lv_label_set_text_fmt(ui_cpuUsageRateLabel, "%u%%", (unsigned)cpu);

    lv_arc_set_value(ui_ramUsage, ram);
    lv_label_set_text_fmt(ui_ramUsageLabel, "%u%%", (unsigned)ram);

    lv_arc_set_value(ui_flashUsage, flash);
    lv_label_set_text_fmt(ui_flashUsageLabel, "%u%%", (unsigned)flash);

    lv_arc_set_value(ui_cpuTemperature, t);
    lv_label_set_text_fmt(ui_Label5, "%d C", (int)t);
}

void monitor_init(void)
{
    static uint8_t inited = 0;

    if (inited) return;
    inited = 1;

    mon_temp_init();
    monitor_timer_cb(NULL);                     /* \u5148\u5237\u4e00\u6b21 */
    lv_timer_create(monitor_timer_cb, 500, NULL);
}

/* configUSE_IDLE_HOOK = 1，FreeRTOS 要求提供这个钩子（空实现即可） */
void vApplicationIdleHook(void)
{
}
