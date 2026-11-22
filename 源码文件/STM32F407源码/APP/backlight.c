#include "backlight.h"
#include "stm32f4xx.h"

#define BL_GPIO     GPIOD
#define BL_PIN      GPIO_Pin_12
#define BL_AF       GPIO_AF_TIM4
#define BL_TIM      TIM4
#define BL_ARR      1000u        /* 1 kHz */

void backlight_init(void)
{
    GPIO_InitTypeDef        g;
    TIM_TimeBaseInitTypeDef tb;
    TIM_OCInitTypeDef       oc;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);

    /* PD12 -> TIM4_CH1 (复用功能2) */
    GPIO_PinAFConfig(BL_GPIO, GPIO_PinSource12, BL_AF);

    g.GPIO_Pin   = BL_PIN;
    g.GPIO_Mode  = GPIO_Mode_AF;
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(BL_GPIO, &g);

    /* 84 MHz / 84 = 1 MHz，ARR 1000 -> 1 kHz */
    tb.TIM_Prescaler     = 84 - 1;
    tb.TIM_CounterMode   = TIM_CounterMode_Up;
    tb.TIM_Period        = BL_ARR - 1;
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    tb.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(BL_TIM, &tb);

    TIM_OCStructInit(&oc);
    oc.TIM_OCMode      = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_Pulse       = BL_ARR;
    oc.TIM_OCPolarity  = TIM_OCPolarity_High;
    TIM_OC1Init(BL_TIM, &oc);
    TIM_OC1PreloadConfig(BL_TIM, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(BL_TIM, ENABLE);

    TIM_Cmd(BL_TIM, ENABLE);

    backlight_set(100);
}

void backlight_set(uint8_t pct)
{
    uint32_t duty;

    if (pct > 100) pct = 100;

    /* 保留 3% 最低亮度，避免全黑看不见 */
    duty = 30u + (uint32_t)pct * (BL_ARR - 30u) / 100u;

    TIM4->CCR1 = duty;
}
