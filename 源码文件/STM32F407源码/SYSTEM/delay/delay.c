#include "delay.h"
#include "sys.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"                  // Device header


volatile static uint8_t  fac_us = 0;

void Delay_Init(uint8_t SYSCLK)
{
    fac_us = SYSCLK;                       /* 1us = SYSCLK 个 DWT 周期 */

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* 使能 DWT */
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;             /* 开始计数 */
}

void delay_us(uint32_t nus)
{
    volatile uint32_t start = DWT->CYCCNT;
    volatile uint32_t ticks = nus * fac_us;              /* 溢出安全：见下方说明 */
    while ((DWT->CYCCNT - start) < ticks) { }
}

void delay_ms(uint32_t nms)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        if (nms) vTaskDelay(pdMS_TO_TICKS(nms));
    }
    else
    {
        while (nms--)delay_us(1000);
    }
}


