#include "soft_uart.h"
#include "stm32f4xx.h"
#include <stdio.h>
#include <string.h>

/* ============================================================
 * 软件串口：PA11 = TX，PA12 = RX，8N1，TIM7 位定时，半双工
 * ============================================================ */

#define SU_TX_PORT   GPIOA
#define SU_TX_PIN    GPIO_Pin_11
#define SU_RX_PORT   GPIOA
#define SU_RX_PIN    GPIO_Pin_12

#define SU_TXBUF     256
#define SU_RXBUF     512

static volatile uint8_t  s_tx_buf[SU_TXBUF];
static volatile uint16_t s_tx_len, s_tx_pos;
static volatile uint8_t  s_tx_shift, s_tx_bits, s_tx_active;

static volatile uint8_t  s_rx_buf[SU_RXBUF];
static volatile uint16_t s_rx_head, s_rx_tail;
static volatile uint8_t  s_rx_shift, s_rx_bits, s_rx_active;
static volatile uint8_t  s_rx_stop_pending;
volatile uint32_t s_rx_err = 0;   /* 帧错计数（调试用） */

static uint16_t s_arr_bit;
static uint16_t s_arr_1p5;

/* 当前 TIM7 在干什么 */
#define MODE_IDLE  0
#define MODE_TX    1
#define MODE_RX    2
static volatile uint8_t s_mode;

static inline uint8_t rx_pin(void)
{
    return (GPIO_ReadInputDataBit(SU_RX_PORT, SU_RX_PIN) != RESET) ? 1u : 0u;
}

static inline void tx_pin(uint8_t v)
{
    if (v) GPIO_SetBits(SU_TX_PORT, SU_TX_PIN);
    else   GPIO_ResetBits(SU_TX_PORT, SU_TX_PIN);
}

/* ---------------- 初始化 ---------------- */
void suart_init(uint32_t baud)
{
    GPIO_InitTypeDef   g;
    TIM_TimeBaseInitTypeDef tb;
    NVIC_InitTypeDef   n;
    EXTI_InitTypeDef   ex;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM7, ENABLE);

    /* PA11 = 输出（空闲高） */
    g.GPIO_Pin   = SU_TX_PIN;
    g.GPIO_Mode  = GPIO_Mode_OUT;
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(SU_TX_PORT, &g);
    GPIO_SetBits(SU_TX_PORT, SU_TX_PIN);

    /* PA12 = 输入上拉 */
    g.GPIO_Pin  = SU_RX_PIN;
    g.GPIO_Mode = GPIO_Mode_IN;
    g.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(SU_RX_PORT, &g);

    /* PA12 下降沿中断（起始位） */
    SYSCFG_EXTILineConfig(EXTI_PortSourceGPIOA, EXTI_PinSource12);
    ex.EXTI_Line    = EXTI_Line12;
    ex.EXTI_Mode    = EXTI_Mode_Interrupt;
    ex.EXTI_Trigger = EXTI_Trigger_Falling;
    ex.EXTI_LineCmd = ENABLE;
    EXTI_Init(&ex);

    /* TIM7：APB1 定时器时钟 84MHz -> 1MHz 计数 */
    tb.TIM_Prescaler     = 0;              /* 84 MHz 直接计数，提高波特率精度 */
    tb.TIM_CounterMode   = TIM_CounterMode_Up;
    tb.TIM_Period        = 0xFFFF;
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    tb.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM7, &tb);

    s_arr_bit  = (uint16_t)((84000000u / baud) - 1u);                 /* 1 位 */
    s_arr_1p5  = (uint16_t)((84000000u * 3u / (2u * baud)) - 1u);     /* 1.5 位 */

    TIM_ClearITPendingBit(TIM7, TIM_IT_Update);
    TIM_ITConfig(TIM7, TIM_IT_Update, ENABLE);

    /* 中断优先级：低于 FreeRTOS 管理阈值，不调用任何 RTOS API */
    n.NVIC_IRQChannel                   = TIM7_IRQn;
    n.NVIC_IRQChannelPreemptionPriority = 1;
    n.NVIC_IRQChannelSubPriority        = 0;
    n.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&n);

    n.NVIC_IRQChannel                   = EXTI15_10_IRQn;
    n.NVIC_IRQChannelPreemptionPriority = 1;
    n.NVIC_IRQChannelSubPriority        = 1;
    NVIC_Init(&n);

    s_mode = MODE_IDLE;
}

/* ---------------- 发送 ---------------- */
static void tx_start_byte(void)
{
    if (s_tx_pos >= s_tx_len)
    {
        TIM_Cmd(TIM7, DISABLE);
        s_mode = MODE_IDLE;

        s_tx_len = 0;          /* 归零，否则下次发送会接着往后写 */
        s_tx_pos = 0;
        return;
    }

    s_tx_shift = s_tx_buf[s_tx_pos++];
    s_tx_bits  = 0;
    s_tx_active = 1;

    tx_pin(0);                       /* 起始位 */

    TIM7->ARR = s_arr_bit;
    TIM7->CNT = 0;
    s_mode = MODE_TX;
    TIM_Cmd(TIM7, ENABLE);
}

void suart_send(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    /* 空闲且上一批已发完：重头开始写 */
    if (s_mode == MODE_IDLE && s_tx_pos >= s_tx_len)
    {
        s_tx_len = 0;
        s_tx_pos = 0;
    }

    for (i = 0; i < len; i++)
    {
        if (s_tx_len >= SU_TXBUF) break;
        s_tx_buf[s_tx_len++] = buf[i];
    }

    if (s_mode == MODE_IDLE) tx_start_byte();
}

void suart_send_str(const char *s)
{
    suart_send((const uint8_t *)s, (uint16_t)strlen(s));
}

/* ---------------- 接收 ---------------- */
static void rx_start(void)
{
    if (s_mode != MODE_IDLE) return;

    s_rx_shift  = 0;
    s_rx_bits   = 0;
    s_rx_active = 1;

    TIM7->ARR = s_arr_1p5;           /* 第 1 次采样落在 bit0 中间 */
    TIM7->CNT = 0;
    s_mode = MODE_RX;
    TIM_Cmd(TIM7, ENABLE);
}

int suart_available(void)
{
    return (int)((s_rx_head + SU_RXBUF - s_rx_tail) % SU_RXBUF);
}

int suart_read_byte(uint8_t *b)
{
    if (s_rx_head == s_rx_tail) return 0;

    *b = s_rx_buf[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1u) % SU_RXBUF);
    return 1;
}

/* ---------------- 中断 ---------------- */
void TIM7_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM7, TIM_IT_Update) == RESET) return;
    TIM_ClearITPendingBit(TIM7, TIM_IT_Update);

    if (s_mode == MODE_TX)
    {
        s_tx_bits++;

        if (s_tx_bits <= 8u)
        {
            tx_pin(s_tx_shift & 1u);
            s_tx_shift >>= 1;
        }
        else if (s_tx_bits == 9u)
        {
            tx_pin(1);               /* 停止位 */
        }
        else
        {
            tx_start_byte();         /* 下一字节或结束 */
        }
    }
    else if (s_mode == MODE_RX)
    {
        if (s_rx_stop_pending)
        {
            /* 现在是停止位中间 */
            uint16_t nx = (uint16_t)((s_rx_head + 1u) % SU_RXBUF);

            s_rx_stop_pending = 0;

            if (rx_pin() == 1u)
            {
                if (nx != s_rx_tail)
                {
                    s_rx_buf[s_rx_head] = s_rx_shift;
                    s_rx_head = nx;
                }
            }
            else
            {
                s_rx_err++;          /* 帧错：丢弃 */
            }

            TIM_Cmd(TIM7, DISABLE);
            s_mode = MODE_IDLE;

            if (s_tx_pos < s_tx_len) tx_start_byte();
            return;
        }

        s_rx_shift |= (uint8_t)(rx_pin() << s_rx_bits);
        s_rx_bits++;

        if (s_rx_bits >= 8u)
        {
            /* 八位数据已收完，下一次中断采样停止位 */
            s_rx_stop_pending = 1;
            TIM7->ARR = s_arr_bit;
        }
        else
        {
            TIM7->ARR = s_arr_bit;   /* 之后每 1 位采一次 */
        }
    }
}

void EXTI15_10_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line12) != RESET)
    {
        EXTI_ClearITPendingBit(EXTI_Line12);
        rx_start();
    }
}

/* ---------------- 调试：把收到的数据打到调试串口 ---------------- */
void suart_poll(void)
{
    uint8_t b;

    while (suart_read_byte(&b))
    {
        if (b >= 0x20 && b < 0x7F) printf("[esp] '%c'\r\n", (char)b);
        else                       printf("[esp] 0x%02X\r\n", (unsigned)b);

        suart_send(&b, 1);           /* 回显 */
    }
}
