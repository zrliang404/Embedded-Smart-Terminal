#include "i2s.h"
#include "delay.h"

/* ============================================================
 * I2S2 音频输出驱动 —— 标准库(SPL)版本
 * 改写自 ALIENTEK 的 HAL 版本，寄存器行为完全一致
 *
 * 引脚：
 *   PB12 = I2S2_WS  (LRC，左右声道时钟)
 *   PB13 = I2S2_CK  (BCLK，位时钟)
 *   PC2  = I2S2_SD  (DACDAT，音频数据)
 *   PC3  = I2S2ext_SD (ADCDAT，本例不用)
 *   PC6  = I2S2_MCK (MCLK，主时钟 256×fs)
 * ============================================================ */

/* TX 回调函数指针，DMA 传输完成时由 I2S2_TXDMA_Init 里注册的回调调用 */
void (*i2s_tx_callback)(void);

/* ------------------------------------------------------------
 * I2S2 初始化
 * I2S_Standard     : I2S_STANDARD_PHILIPS / MSB / LSB / PCM_SHORT / PCM_LONG
 * I2S_Mode         : I2S_MODE_MASTER_TX / SLAVE_TX / MASTER_RX / SLAVE_RX
 * I2S_Clock_Polarity: I2S_CPOL_LOW / I2S_CPOL_HIGH
 * I2S_DataFormat   : I2S_DATAFORMAT_16B / 16B_EXTENDED / 24B / 32B
 * ------------------------------------------------------------ */
void I2S2_Init(u32 I2S_Standard, u32 I2S_Mode, u32 I2S_Clock_Polarity, u32 I2S_DataFormat)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    I2S_InitTypeDef  I2S_InitStructure;

    /* ---- 时钟使能 ---- */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);   /* I2S2 挂在 SPI2 上，APB1 */

    /* ---- PB12/13 -> AF5 (I2S2_WS / I2S2_CK) ---- */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_12 | GPIO_Pin_13;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_PinAFConfig(GPIOB, GPIO_PinSource12, GPIO_AF_SPI2);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource13, GPIO_AF_SPI2);

    /* ---- PC2/3/6 -> AF5 (I2S2_SD / I2S2ext_SD / I2S2_MCK) ---- */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    GPIO_PinAFConfig(GPIOC, GPIO_PinSource2, GPIO_AF_SPI2);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource3, GPIO_AF_SPI2);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource6, GPIO_AF_SPI2);

    /* ---- SPI2/I2S2 复位，清除旧状态 ---- */
    RCC_APB1PeriphResetCmd(RCC_APB1Periph_SPI2, ENABLE);
    RCC_APB1PeriphResetCmd(RCC_APB1Periph_SPI2, DISABLE);

    /* ---- I2S 参数配置 ----
     * 注意：SPL 的 I2S_Init() 内部会无条件执行 SPIx->I2SPR = 0x0002，
     *      所以采样率必须在 I2S2_Init() 之后调用 I2S2_SampleRate_Set() 设置。
     */
    I2S_InitStructure.I2S_Mode           = I2S_Mode;
    I2S_InitStructure.I2S_Standard       = I2S_Standard;
    I2S_InitStructure.I2S_DataFormat     = I2S_DataFormat;
    I2S_InitStructure.I2S_MCLKOutput     = I2S_MCLKOutput_Enable;   /* 输出 MCLK 给 WM8978 */
    I2S_InitStructure.I2S_AudioFreq      = I2S_AudioFreq_Default;
    I2S_InitStructure.I2S_CPOL           = I2S_Clock_Polarity;
    I2S_Init(SPI2, &I2S_InitStructure);

    /* ---- 使能 SPI2 TX DMA 请求 ---- */
    SPI2->CR2 |= 1 << 1;

    /* ---- 使能 I2S2 ---- */
    I2S_Cmd(SPI2, ENABLE);
}

/* ------------------------------------------------------------
 * 采样率分频表（HSE=8MHz, pllm=8 -> PLLI2S 输入 1MHz）
 * 格式：{ 采样率/10, PLLI2SN, PLLI2SR, I2SDIV, ODD }
 * 公式：Fs = I2SxCLK / [256 * (2*I2SDIV + ODD)]
 *       I2SxCLK = (HSE/pllm) * PLLI2SN / PLLI2SR
 * ------------------------------------------------------------ */
const u16 I2S_PSC_TBL[][5] =
{
    {800,  256, 5, 12, 1},   /* 8kHz     */
    {1102, 429, 4, 19, 0},   /* 11.025kHz*/
    {1600, 213, 2, 13, 0},   /* 16kHz    */
    {2205, 429, 4,  9, 1},   /* 22.05kHz */
    {3200, 213, 2,  6, 1},   /* 32kHz    */
    {4410, 271, 2,  6, 0},   /* 44.1kHz  */
    {4800, 258, 3,  3, 1},   /* 48kHz    */
    {8820, 316, 2,  3, 1},   /* 88.2kHz  */
    {9600, 344, 2,  3, 1},   /* 96kHz    */
    {17640,361, 2,  2, 0},   /* 176.4kHz */
};

#define I2S_PSC_TBL_SIZE   (sizeof(I2S_PSC_TBL) / sizeof(I2S_PSC_TBL[0]))

/* ------------------------------------------------------------
 * 设置 I2S2 采样率
 * 返回值：0 = 成功，1 = 不支持该采样率
 * ------------------------------------------------------------ */
u8 I2S2_SampleRate_Set(u32 samplerate)
{
    u8  i;
    u32 tempreg;
    u32 plli2sn, plli2sr;

    for (i = 0; i < I2S_PSC_TBL_SIZE; i++)
    {
        if ((samplerate / 10) == I2S_PSC_TBL[i][0]) break;
    }
    if (i >= I2S_PSC_TBL_SIZE) return 1;      /* 表里没有，无法设置 */

    plli2sn = I2S_PSC_TBL[i][1];
    plli2sr = I2S_PSC_TBL[i][2];

    /* ---- 先关 PLLI2S ---- */
    RCC->CR &= ~(1 << 26);

    /* ---- 配置 PLLI2SN / PLLI2SR ----
     *   RCC_PLLI2SCFGR : bit6        PLLI2SON
     *                    bit[14:6]   PLLI2SN (6~14)
     *                    bit[30:28]  PLLI2SR (28~30)
     */
    RCC->PLLI2SCFGR &= ~((3u << 28) | (0x1FFu << 6));
    RCC->PLLI2SCFGR |=  ((plli2sr & 0x7u) << 28);
    RCC->PLLI2SCFGR |=  ((plli2sn & 0x1FFu) << 6);

    /* ---- 开启 PLLI2S 并等待锁定（带超时，避免死等卡住系统）---- */
    RCC->CR |= (1 << 26);
    {
        volatile uint32_t t0 = DWT->CYCCNT;
        while ((RCC->CR & (1 << 27)) == 0)
        {
            /* 超时 100ms，168MHz * 100000 */
            if ((DWT->CYCCNT - t0) > (168u * 100000u))
            {
                return 3;               /* PLLI2S 未锁定 */
            }
        }
    }

    /* ---- 配置 I2SPR：I2SDIV / ODD / MCKOE ---- */
    tempreg  = I2S_PSC_TBL[i][3] << 0;        /* I2SDIV */
    tempreg |= I2S_PSC_TBL[i][4] << 8;        /* ODD    */
    tempreg |= 1 << 9;                        /* MCKOE：输出 MCLK */
    SPI2->I2SPR = tempreg;

    return 0;
}

/* ------------------------------------------------------------
 * I2S2 TX DMA 配置（双缓冲）
 * buf0 : 缓冲区 0
 * buf1 : 缓冲区 1
 * num  : 每次传输的数据个数（16 位为单位）
 * ------------------------------------------------------------ */
void I2S2_TX_DMA_Init(u8 *buf0, u8 *buf1, u16 num)
{
    DMA_InitTypeDef DMA_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);

    /* DMA1_Stream4 复位 */
    DMA_DeInit(DMA1_Stream4);

    /* 等待 DMA1_Stream4 空闲 */
    while (DMA_GetCmdStatus(DMA1_Stream4) != DISABLE);

    DMA_InitStructure.DMA_Channel            = DMA_Channel_0;          /* I2S2_TX 用通道 0 */
    DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)&SPI2->DR;         /* 外设地址：SPI2 数据寄存器 */
    DMA_InitStructure.DMA_Memory0BaseAddr    = (u32)buf0;              /* 双缓冲模式用 M0/M1 */
    DMA_InitStructure.DMA_DIR                = DMA_DIR_MemoryToPeripheral;
    DMA_InitStructure.DMA_BufferSize         = num;
    DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;  /* 16 位 */
    DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_HalfWord;      /* 16 位 */
    DMA_InitStructure.DMA_Mode               = DMA_Mode_Circular;      /* 循环 + 双缓冲 */
    DMA_InitStructure.DMA_Priority           = DMA_Priority_VeryHigh;
    DMA_InitStructure.DMA_FIFOMode           = DMA_FIFOMode_Disable;
    DMA_InitStructure.DMA_FIFOThreshold      = DMA_FIFOThreshold_1QuarterFull;
    DMA_InitStructure.DMA_MemoryBurst        = DMA_MemoryBurst_Single;
    DMA_InitStructure.DMA_PeripheralBurst    = DMA_PeripheralBurst_Single;
    DMA_Init(DMA1_Stream4, &DMA_InitStructure);

    /* ---- 配置双缓冲 ----
     * DMA_DoubleBufferModeConfig(): 设置 M1AR (= buf1)，并指定以 Memory0 为起始
     * DMA_DoubleBufferModeCmd()   : 打开双缓冲模式 (DMA_SxCR_DBM)
     */
    DMA_DoubleBufferModeConfig(DMA1_Stream4, (u32)buf1, DMA_Memory_0);
    DMA_DoubleBufferModeCmd(DMA1_Stream4, ENABLE);

    /* ---- 清除全部中断标志 ---- */
    DMA_ClearFlag(DMA1_Stream4,
                  DMA_FLAG_TCIF4 | DMA_FLAG_HTIF4 | DMA_FLAG_TEIF4 |
                  DMA_FLAG_DMEIF4 | DMA_FLAG_FEIF4);

    /* ---- 使能传输完成中断 ---- */
    DMA_ITConfig(DMA1_Stream4, DMA_IT_TC, ENABLE);

    /* ---- NVIC 配置 ---- */
    NVIC_InitStructure.NVIC_IRQChannel                   = DMA1_Stream4_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;    /* 比 configMAX_SYSCALL 高但低于触摸 */
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    /* ---- 先不开启 DMA，等 I2S_Play_Start() ---- */
    DMA_Cmd(DMA1_Stream4, DISABLE);
}

/* ------------------------------------------------------------
 * DMA1_Stream4 中断服务函数（I2S2 TX）
 * ------------------------------------------------------------ */
void DMA1_Stream4_IRQHandler(void)
{
    if (DMA_GetFlagStatus(DMA1_Stream4, DMA_FLAG_TCIF4) != RESET)
    {
        DMA_ClearFlag(DMA1_Stream4, DMA_FLAG_TCIF4);
        if (i2s_tx_callback != 0) i2s_tx_callback();
    }
}

/* ------------------------------------------------------------
 * 开始播放
 * ------------------------------------------------------------ */
void I2S_Play_Start(void)
{
    DMA_Cmd(DMA1_Stream4, ENABLE);
}

/* ------------------------------------------------------------
 * 停止播放
 * ------------------------------------------------------------ */
void I2S_Play_Stop(void)
{
    DMA_Cmd(DMA1_Stream4, DISABLE);
}
