#include "sdio_sdcard.h"
#include "stdio.h"
#include "delay.h"
#include <stdio.h>
#include <string.h>

/* ============================================================
 * SD 卡驱动（标准外设库 SPL 版本）
 * 引脚：PC8~PC11 = D0~D3，PC12 = CK，PD2 = CMD
 * 时钟：识别阶段 331kHz，正常阶段 24MHz（SDIOCLK 48MHz / (2+0)）
 * ============================================================ */

SD_CardInfo SDCardInfo;

static uint32_t SD_RCA      = 0;    /* 卡的相对地址（CMD3 返回） */
static uint8_t  SD_CardType = 0;    /* 0 = SDSC，1 = SDHC/SDXC */
static uint8_t  SD_Is4Bit   = 0;    /* 是否已切换到 4 位总线 */

#define SD_CMD_TIMEOUT   0x0001FFFEu
/* 数据阶段超时：原来要等约 130ms，卡住时拖垮 UI；缩到约 30ms 足够 */
#define SD_DATA_TIMEOUT  0x0001FFFEu       /* 约 130ms：过短会造成假失败 */

/* ------------------------------------------------------------
 * 发送一条命令并等待响应
 * ------------------------------------------------------------ */
static uint8_t SD_SendCmd(uint8_t cmd, uint32_t arg, uint8_t resp, uint8_t idle)
{
    SDIO_CmdInitTypeDef c;
    volatile uint32_t dwt_start;

    if (idle)
    {
        /* 等待卡进入就绪状态，最多约 200ms */
        dwt_start = DWT->CYCCNT;

        while (1)
        {
            SDIO_CmdInitTypeDef q;

            SDIO_CmdStructInit(&q);
            q.SDIO_Argument = SD_RCA;
            q.SDIO_CmdIndex = 13;                 /* CMD13：查询卡状态 */
            q.SDIO_Response = SDIO_Response_Short;
            q.SDIO_CPSM     = SDIO_CPSM_Enable;

            SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL | SDIO_FLAG_CTIMEOUT |
                           SDIO_FLAG_CMDREND  | SDIO_FLAG_CMDSENT);
            SDIO_SendCommand(&q);

            {
                volatile uint32_t t0 = DWT->CYCCNT;
                uint8_t got = 0;

                while (1)
                {
                    if (SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) == SET) { SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT); return 0xFF; }
                    if (SDIO_GetFlagStatus(SDIO_FLAG_CMDREND)  == SET) { SDIO_ClearFlag(SDIO_FLAG_CMDREND);  got = 1; break; }
                    if ((DWT->CYCCNT - t0) > (168u * 20000u)) return 0xFF;   /* 单条命令 20ms 超时 */
                }

                if (!got) return 0xFF;
            }

            /* 响应 bit24 = READY_FOR_DATA */
            if (SDIO_GetResponse(SDIO_RESP1) & 0x01000000u) break;
            if ((DWT->CYCCNT - dwt_start) > (168u * 200000u)) return 0xFF;   /* 总共 200ms */
        }
    }

    SDIO_CmdStructInit(&c);
    c.SDIO_Argument = arg;
    c.SDIO_CmdIndex = cmd;
    c.SDIO_Wait     = SDIO_Wait_No;
    c.SDIO_CPSM     = SDIO_CPSM_Enable;

    if (resp == 0)      c.SDIO_Response = SDIO_Response_No;
    else if (resp == 1) c.SDIO_Response = SDIO_Response_Short;
    else                c.SDIO_Response = SDIO_Response_Long;

    SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL | SDIO_FLAG_CTIMEOUT |
                   SDIO_FLAG_CMDREND  | SDIO_FLAG_CMDSENT);

    SDIO_SendCommand(&c);

    dwt_start = DWT->CYCCNT;

    while (1)
    {
        if (SDIO_GetFlagStatus(SDIO_FLAG_CTIMEOUT) == SET) { SDIO_ClearFlag(SDIO_FLAG_CTIMEOUT); return 0xFF; }
        if (SDIO_GetFlagStatus(SDIO_FLAG_CCRCFAIL) == SET) { SDIO_ClearFlag(SDIO_FLAG_CCRCFAIL); return 0xFE; }

        if (resp == 0)
        {
            if (SDIO_GetFlagStatus(SDIO_FLAG_CMDSENT) == SET) { SDIO_ClearFlag(SDIO_FLAG_CMDSENT); return 0; }
        }
        else
        {
            if (SDIO_GetFlagStatus(SDIO_FLAG_CMDREND) == SET) { SDIO_ClearFlag(SDIO_FLAG_CMDREND); return 0; }
        }

        if ((DWT->CYCCNT - dwt_start) > (168u * 20000u)) return 0xFF;   /* 20ms 超时 */
    }
}

/* 读取 128 位的长响应（CID / CSD） */
static void SD_GetLongResponse(uint32_t *buf)
{
    buf[0] = SDIO_GetResponse(SDIO_RESP1);
    buf[1] = SDIO_GetResponse(SDIO_RESP2);
    buf[2] = SDIO_GetResponse(SDIO_RESP3);
    buf[3] = SDIO_GetResponse(SDIO_RESP4);
}

/* ------------------------------------------------------------
 * 查询卡状态：TRAN(4) 表示可以收发数据
 * ------------------------------------------------------------ */
uint8_t SD_GetCardState(void)
{
    if (SD_SendCmd(13, SD_RCA, 1, 0) != 0) return SD_TRANSFER_BUSY;

    /* R1 的 bit[12:9] = CURRENT_STATE */
    return (((SDIO_GetResponse(SDIO_RESP1) >> 9) & 0x0F) == 4) ? SD_TRANSFER_OK : SD_TRANSFER_BUSY;
}

/* ------------------------------------------------------------
 * 由 CSD 解析容量（扇区数）
 * ------------------------------------------------------------ */
static void SD_ParseCSD(uint32_t *csd)
{
    uint32_t c_size, c_size_mult, read_bl_len, block_count;

    if (SD_CardType)
    {
        c_size      = (csd[1] >> 8) & 0x3FFFFF;
        block_count = (c_size + 1u) * 1024u;
    }
    else
    {
        read_bl_len = (csd[1] >> 24) & 0x000F;
        c_size      = ((csd[1] & 0x3FFu) << 2) | ((csd[2] >> 30) & 0x3u);
        c_size_mult = (csd[2] >> 15) & 0x0007;

        block_count = (c_size + 1u) * (1u << (c_size_mult + 2u)) * (1u << read_bl_len) / 512u;
    }

    SDCardInfo.BlockNbr     = block_count;
    SDCardInfo.LogBlockNbr  = block_count;
    SDCardInfo.BlockSize    = 512;
    SDCardInfo.LogBlockSize = 512;
    SDCardInfo.RelCardAdd   = SD_RCA >> 16;
    SDCardInfo.CardVersion  = SD_CardType ? 2 : 1;
}

/* ------------------------------------------------------------
 * 数据传输：SDIO + DMA2（固定使用 DMA，不再保留轮询方式）
 * 关键配置：FIFO 模式 + 4 字突发 + 外设流控（必须）
 * ------------------------------------------------------------ */

#define SD_DMA_RX_STREAM   DMA2_Stream3
#define SD_DMA_TX_STREAM   DMA2_Stream6
#define SD_DMA_CHANNEL     DMA_Channel_4

#define SD_RX_TC   DMA_FLAG_TCIF3
#define SD_RX_TE   DMA_FLAG_TEIF3
#define SD_RX_DME  DMA_FLAG_DMEIF3
#define SD_RX_FE   DMA_FLAG_FEIF3

#define SD_TX_TC   DMA_FLAG_TCIF6
#define SD_TX_TE   DMA_FLAG_TEIF6
#define SD_TX_DME  DMA_FLAG_DMEIF6
#define SD_TX_FE   DMA_FLAG_FEIF6

/* 关闭 DMA 流（带超时） */
static void sd_dma_stop(DMA_Stream_TypeDef *s)
{
    uint32_t tick = 0x0000FFFFu;

    DMA_Cmd(s, DISABLE);

    while ((s->CR & DMA_SxCR_EN) != 0u)
    {
        if (--tick == 0u) { s->CR &= ~DMA_SxCR_EN; break; }
    }

    DMA_ClearFlag(s, DMA_FLAG_TCIF3 | DMA_FLAG_TEIF3 | DMA_FLAG_DMEIF3 | DMA_FLAG_FEIF3 |
                     DMA_FLAG_TCIF6 | DMA_FLAG_TEIF6 | DMA_FLAG_DMEIF6 | DMA_FLAG_FEIF6);
}

/* ------------------------------------------------------------------
 * 中断当前数据阶段并复位 SDIO 数据通路
 *
 * 关键：只关 DMA 还不够，SDIO 的 DPSM（数据状态机）仍处于使能状态，
 *       下一次 CMD18 会因为状态机残留而永远等不到 DATAEND，
 *       表现就是连续读盘失败，直到 SD_Init 才恢复。
 * ------------------------------------------------------------------ */
static void sd_data_abort(void)
{
    SDIO_DMACmd(DISABLE);

    sd_dma_stop(SD_DMA_RX_STREAM);
    sd_dma_stop(SD_DMA_TX_STREAM);

    SDIO->DCTRL &= ~(1u << 0);          /* DTEN = 0：关闭数据通路 */

    SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL | SDIO_FLAG_DTIMEOUT |
                   SDIO_FLAG_RXOVERR  | SDIO_FLAG_TXUNDERR |
                   SDIO_FLAG_STBITERR | SDIO_FLAG_DATAEND);
}

/* 配置 DMA 流（只配置，不使能） */
static void sd_dma_config(DMA_Stream_TypeDef *s, uint8_t *buf, uint32_t bytes, uint32_t dir, uint32_t prio)
{
    DMA_InitTypeDef d;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, ENABLE);

    sd_dma_stop(s);
    DMA_DeInit(s);

    d.DMA_Channel            = SD_DMA_CHANNEL;
    d.DMA_PeripheralBaseAddr = (uint32_t)&SDIO->FIFO;
    d.DMA_Memory0BaseAddr    = (uint32_t)buf;
    d.DMA_DIR                = dir;
    d.DMA_BufferSize         = bytes / 4u;
    d.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    d.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    d.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Word;
    d.DMA_MemoryDataSize     = DMA_MemoryDataSize_Word;
    d.DMA_Mode               = DMA_Mode_Normal;
    d.DMA_Priority           = prio;

    /* STM32F4 SDIO DMA 必须开启 FIFO + 4字突发 + 外设流控 */
    d.DMA_FIFOMode           = DMA_FIFOMode_Enable;
    d.DMA_FIFOThreshold      = DMA_FIFOThreshold_Full;
    d.DMA_MemoryBurst        = DMA_MemoryBurst_INC4;
    d.DMA_PeripheralBurst    = DMA_PeripheralBurst_INC4;

    DMA_Init(s, &d);
    DMA_FlowControllerConfig(s, DMA_FlowCtrl_Peripheral);
}

/* ------------------------------------------------------------
 * 读取若干扇区
 * ------------------------------------------------------------ */
static uint8_t SD_ReadBlocks(uint8_t *buf, uint32_t sector, uint32_t cnt)
{
    SDIO_DataInitTypeDef data;
    uint32_t tick, bytes;
    uint8_t  cmd, err = 0;

    {
        uint32_t guard = 20;      /* 最多等约 400ms，避免卡死时长时间占 CPU */
        while (SD_GetCardState() != SD_TRANSFER_OK)
        {
            if (--guard == 0) return SD_ERR_RW;
            delay_us(100);
        }
    }

    if (SD_CardType == 0) sector *= 512;
    bytes = cnt * 512;

    SDIO_DataStructInit(&data);
    data.SDIO_DataTimeOut   = SD_DATA_TIMEOUT;
    data.SDIO_DataLength    = bytes;
    data.SDIO_DataBlockSize = SDIO_DataBlockSize_512b;
    data.SDIO_TransferDir   = SDIO_TransferDir_ToSDIO;
    data.SDIO_TransferMode  = SDIO_TransferMode_Block;
    data.SDIO_DPSM          = SDIO_DPSM_Enable;

    SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL | SDIO_FLAG_DTIMEOUT | SDIO_FLAG_RXOVERR |
                   SDIO_FLAG_STBITERR | SDIO_FLAG_DATAEND);

    sd_data_abort();        /* 先复位数据通路，清掉上一次可能的残留 */

    /* 1. 先发送读命令：多块用 CMD18，单块用 CMD17 */
    cmd = (cnt > 1) ? 18 : 17;
    if (SD_SendCmd(cmd, sector, 1, 0) != 0)
    {
        return SD_ERR_RW;
    }

    /* 2. 配置数据通路与 RX DMA */
    SDIO_DataConfig(&data);
    sd_dma_config(SD_DMA_RX_STREAM, buf, bytes, DMA_DIR_PeripheralToMemory, DMA_Priority_High);
    DMA_Cmd(SD_DMA_RX_STREAM, ENABLE);
    SDIO_DMACmd(ENABLE);

    /* 3. 等待 SDIO DATAEND */
    tick = SD_DATA_TIMEOUT;
    while (SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) == RESET)
    {
        if (SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) == SET) { SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); err = 1; break; }
        if (SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) == SET) { SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL); err = 1; break; }
        if (SDIO_GetFlagStatus(SDIO_FLAG_RXOVERR)  == SET) { SDIO_ClearFlag(SDIO_FLAG_RXOVERR);  err = 1; break; }
        if (DMA_GetFlagStatus(SD_DMA_RX_STREAM, SD_RX_TE)  == SET) { DMA_ClearFlag(SD_DMA_RX_STREAM, SD_RX_TE);  err = 1; break; }
        if (DMA_GetFlagStatus(SD_DMA_RX_STREAM, SD_RX_DME) == SET) { DMA_ClearFlag(SD_DMA_RX_STREAM, SD_RX_DME); err = 1; break; }
        if (--tick == 0) { err = 1; break; }
    }

    /* 4. 等待 DMA 传输完成（TCIF3） */
    if (!err)
    {
        tick = SD_DATA_TIMEOUT;
        while (DMA_GetFlagStatus(SD_DMA_RX_STREAM, SD_RX_TC) == RESET)
        {
            if (DMA_GetFlagStatus(SD_DMA_RX_STREAM, SD_RX_TE)  == SET) { DMA_ClearFlag(SD_DMA_RX_STREAM, SD_RX_TE);  err = 1; break; }
            if (DMA_GetFlagStatus(SD_DMA_RX_STREAM, SD_RX_FE)  == SET) { DMA_ClearFlag(SD_DMA_RX_STREAM, SD_RX_FE);  err = 1; break; }
            if (--tick == 0) { err = 1; break; }
        }
    }

    sd_data_abort();        /* 关 DMA + 复位 SDIO 数据通路（关键） */

    /* 多块传输无论成败都必须发 CMD12 释放卡；
     * 否则卡停在"数据传输"状态，后续所有命令都会超时 */
    if (cnt > 1) SD_SendCmd(12, 0, 1, 0);

    SDIO_ClearFlag(SDIO_FLAG_DATAEND);

    if (err) return SD_ERR_RW;

    return SD_OK;
}

/* ------------------------------------------------------------
 * 写入若干扇区
 * ------------------------------------------------------------ */
static uint8_t SD_WriteBlocks(uint8_t *buf, uint32_t sector, uint32_t cnt)
{
    SDIO_DataInitTypeDef data;
    uint32_t tick, bytes;
    uint8_t  cmd, err = 0;

    {
        uint32_t guard = 20;      /* 最多等约 400ms，避免卡死时长时间占 CPU */
        while (SD_GetCardState() != SD_TRANSFER_OK)
        {
            if (--guard == 0) return SD_ERR_RW;
            delay_us(100);
        }
    }

    if (SD_CardType == 0) sector *= 512;
    bytes = cnt * 512;

    SDIO_DataStructInit(&data);
    data.SDIO_DataTimeOut   = SD_DATA_TIMEOUT;
    data.SDIO_DataLength    = bytes;
    data.SDIO_DataBlockSize = SDIO_DataBlockSize_512b;
    data.SDIO_TransferDir   = SDIO_TransferDir_ToCard;
    data.SDIO_TransferMode  = SDIO_TransferMode_Block;
    data.SDIO_DPSM          = SDIO_DPSM_Enable;

    SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL | SDIO_FLAG_DTIMEOUT | SDIO_FLAG_TXUNDERR |
                   SDIO_FLAG_STBITERR | SDIO_FLAG_DATAEND);

    sd_data_abort();        /* 先复位数据通路 */

    /* 1. 先发送写命令：多块用 CMD25，单块用 CMD24 */
    cmd = (cnt > 1) ? 25 : 24;
    if (SD_SendCmd(cmd, sector, 1, 0) != 0)
    {
        return SD_ERR_RW;
    }

    /* 2. 配置数据通路与 TX DMA */
    SDIO_DataConfig(&data);
    sd_dma_config(SD_DMA_TX_STREAM, buf, bytes, DMA_DIR_MemoryToPeripheral, DMA_Priority_VeryHigh);
    DMA_Cmd(SD_DMA_TX_STREAM, ENABLE);
    SDIO_DMACmd(ENABLE);

    /* 3. 等待 TX DMA 传输完成（TCIF6） */
    tick = SD_DATA_TIMEOUT;
    while (DMA_GetFlagStatus(SD_DMA_TX_STREAM, SD_TX_TC) == RESET)
    {
        if (DMA_GetFlagStatus(SD_DMA_TX_STREAM, SD_TX_TE)  == SET) { DMA_ClearFlag(SD_DMA_TX_STREAM, SD_TX_TE);  err = 1; break; }
        if (DMA_GetFlagStatus(SD_DMA_TX_STREAM, SD_TX_DME) == SET) { DMA_ClearFlag(SD_DMA_TX_STREAM, SD_TX_DME); err = 1; break; }
        if (DMA_GetFlagStatus(SD_DMA_TX_STREAM, SD_TX_FE)  == SET) { DMA_ClearFlag(SD_DMA_TX_STREAM, SD_TX_FE);  err = 1; break; }
        if (SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) == SET) { SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); err = 1; break; }
        if (SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) == SET) { SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL); err = 1; break; }
        if (SDIO_GetFlagStatus(SDIO_FLAG_TXUNDERR) == SET) { SDIO_ClearFlag(SDIO_FLAG_TXUNDERR); err = 1; break; }
        if (--tick == 0) { err = 1; break; }
    }

    /* 4. 等待 SDIO DATAEND */
    if (!err)
    {
        tick = SD_DATA_TIMEOUT;
        while (SDIO_GetFlagStatus(SDIO_FLAG_DATAEND) == RESET)
        {
            if (SDIO_GetFlagStatus(SDIO_FLAG_DTIMEOUT) == SET) { SDIO_ClearFlag(SDIO_FLAG_DTIMEOUT); err = 1; break; }
            if (SDIO_GetFlagStatus(SDIO_FLAG_DCRCFAIL) == SET) { SDIO_ClearFlag(SDIO_FLAG_DCRCFAIL); err = 1; break; }
            if (--tick == 0) { err = 1; break; }
        }
    }

    sd_data_abort();        /* 关 DMA + 复位 SDIO 数据通路 */

    /* 失败也要发 CMD12 释放卡，避免卡停在数据传输状态 */
    if (cnt > 1) SD_SendCmd(12, 0, 1, 0);

    SDIO_ClearFlag(SDIO_FLAG_DATAEND);

    if (err) return SD_ERR_RW;

    tick = SD_DATA_TIMEOUT;
    while (SD_GetCardState() != SD_TRANSFER_OK)
    {
        if (--tick == 0) return SD_ERR_RW;
    }

    return SD_OK;
}

/* ------------------------------------------------------------
 * SD 卡初始化
 * ------------------------------------------------------------ */
uint8_t SD_Init(void)
{
    GPIO_InitTypeDef   GPIO_InitStructure;
    SDIO_InitTypeDef   SDIO_InitStructure;
    uint32_t csd[4];
    uint8_t  r;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOD, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SDIO, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    GPIO_PinAFConfig(GPIOC, GPIO_PinSource8,  GPIO_AF_SDIO);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource9,  GPIO_AF_SDIO);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource10, GPIO_AF_SDIO);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource11, GPIO_AF_SDIO);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource12, GPIO_AF_SDIO);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_Init(GPIOD, &GPIO_InitStructure);
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource2, GPIO_AF_SDIO);

    SDIO_DMACmd(DISABLE);
    sd_dma_stop(SD_DMA_RX_STREAM);
    sd_dma_stop(SD_DMA_TX_STREAM);

    SDIO_DeInit();

    SDIO_InitStructure.SDIO_ClockEdge           = SDIO_ClockEdge_Rising;
    SDIO_InitStructure.SDIO_ClockBypass         = SDIO_ClockBypass_Disable;
    SDIO_InitStructure.SDIO_ClockPowerSave      = SDIO_ClockPowerSave_Disable;
    SDIO_InitStructure.SDIO_BusWide             = SDIO_BusWide_1b;
    SDIO_InitStructure.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Disable;
    SDIO_InitStructure.SDIO_ClockDiv            = 254;
    SDIO_Init(&SDIO_InitStructure);

    SDIO_ClockCmd(ENABLE);

    SDIO_SetPowerState(SDIO_PowerState_ON);
    {
        volatile uint32_t t0 = DWT->CYCCNT;
        while (SDIO_GetPowerState() != SDIO_PowerState_ON)
        {
            if ((DWT->CYCCNT - t0) > (168u * 2000u)) return SD_ERR_CMD0;
        }
    }

    delay_us(20000);

    SD_CardType = 0;
    SD_Is4Bit   = 0;
    SD_RCA      = 0;

    if (SD_SendCmd(0, 0, 0, 0) != 0) return SD_ERR_CMD0;

    r = SD_SendCmd(8, 0x1AA, 1, 0);
    (void)r;

    {
        volatile uint32_t acmd_start = DWT->CYCCNT;
        volatile uint32_t acmd_cnt   = 0;
        uint8_t  r55, r41;
        uint32_t ocr;

        do
        {
            r55 = SD_SendCmd(55, 0, 1, 0);
            if (r55 == 0xFF) goto acmd41_next;

            r41 = SD_SendCmd(41, 0x40FF8000, 1, 0);
            if (r41 == 0xFF) goto acmd41_next;

            ocr = SDIO_GetResponse(SDIO_RESP1);
            if (ocr & 0x80000000u)
            {
                break;
            }

acmd41_next:
            acmd_cnt++;
            if ((DWT->CYCCNT - acmd_start) > (168u * 1000000u)) return SD_ERR_ACMD41;
            if (acmd_cnt > 20000u) return SD_ERR_ACMD41;

        } while (1);
    }

    SD_CardType = (SDIO_GetResponse(SDIO_RESP1) & 0x40000000u) ? 1 : 0;

    if (SD_SendCmd(2, 0, 2, 0) != 0) return SD_ERR_CMD2;

    if (SD_SendCmd(3, 0, 1, 0) != 0) return SD_ERR_CMD3;
    SD_RCA = SDIO_GetResponse(SDIO_RESP1) & 0xFFFF0000u;

    if (SD_SendCmd(9, SD_RCA, 2, 0) != 0) return SD_ERR_CMD9;
    SD_GetLongResponse(csd);
    SD_ParseCSD(csd);

    if (SD_SendCmd(7, SD_RCA, 1, 0) != 0) return SD_ERR_CMD7;

    SD_SendCmd(55, SD_RCA, 1, 0);
    if (SD_SendCmd(6, 2, 1, 0) != 0) return SD_ERR_ACMD6;

    /* 提速到 24MHz 并开启硬件流控 */
    SDIO_InitStructure.SDIO_BusWide             = SDIO_BusWide_4b;
    SDIO_InitStructure.SDIO_ClockDiv            = 0;
    SDIO_InitStructure.SDIO_HardwareFlowControl = SDIO_HardwareFlowControl_Enable;

    SDIO_Init(&SDIO_InitStructure);
    SD_Is4Bit = 1;

    return SD_OK;
}

void SD_GetCardInfo(SD_CardInfo *cardinfo)
{
    uint32_t csd[4];

    if (SD_SendCmd(9, SD_RCA, 2, 0) == 0)
    {
        SD_GetLongResponse(csd);
        SD_ParseCSD(csd);
    }

    if (cardinfo) *cardinfo = SDCardInfo;
}

/* ------------------------------------------------------------
 * diskio 接口：读写入口
 * ------------------------------------------------------------ */
uint8_t SD_ReadDisk(uint8_t *buf, uint32_t sector, uint32_t cnt)
{
    /* INC4 突发传输要求内存首地址 16 字节对齐，且不能位于 CCMRAM (0x1000xxxx) */
    static __align(16) uint8_t tmp[512];
    uint8_t  ret;
    uint32_t i;
    uint8_t  is_ccm = (((uint32_t)buf & 0xFFFF0000u) == 0x10000000u) ? 1u : 0u;

    if ((((uint32_t)buf & 0x0Fu) == 0u) && !is_ccm)
    {
        return SD_ReadBlocks(buf, sector, cnt);
    }

    for (i = 0; i < cnt; i++)
    {
        ret = SD_ReadBlocks(tmp, sector + i, 1);
        if (ret != 0) return ret;
        memcpy(buf + i * 512u, tmp, 512);
    }

    return 0;
}

uint8_t SD_WriteDisk(uint8_t *buf, uint32_t sector, uint32_t cnt)
{
    static __align(16) uint8_t tmp[512];
    uint8_t  ret;
    uint32_t i;
    uint8_t  is_ccm = (((uint32_t)buf & 0xFFFF0000u) == 0x10000000u) ? 1u : 0u;

    if ((((uint32_t)buf & 0x0Fu) == 0u) && !is_ccm)
    {
        return SD_WriteBlocks(buf, sector, cnt);
    }

    for (i = 0; i < cnt; i++)
    {
        memcpy(tmp, buf + i * 512u, 512);
        ret = SD_WriteBlocks(tmp, sector + i, 1);
        if (ret != 0) return ret;
    }

    return 0;
}
