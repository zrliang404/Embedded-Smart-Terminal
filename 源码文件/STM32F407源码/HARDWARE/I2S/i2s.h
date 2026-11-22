#ifndef __I2S_H
#define __I2S_H
#include "sys.h"

/* ============================================================
 * I2S2 音频输出驱动（标准库 SPL 版本）
 * ============================================================ */

extern void (*i2s_tx_callback)(void);   /* TX 传输完成回调 */

/* I2S 标准 */
#define I2S_STANDARD_PHILIPS      I2S_Standard_Phillips
#define I2S_STANDARD_MSB          I2S_Standard_MSB
#define I2S_STANDARD_LSB          I2S_Standard_LSB
#define I2S_STANDARD_PCM_SHORT    I2S_Standard_PCMShort
#define I2S_STANDARD_PCM_LONG     I2S_Standard_PCMLong

/* I2S 模式 */
#define I2S_MODE_SLAVE_TX         I2S_Mode_SlaveTx
#define I2S_MODE_SLAVE_RX         I2S_Mode_SlaveRx
#define I2S_MODE_MASTER_TX        I2S_Mode_MasterTx
#define I2S_MODE_MASTER_RX        I2S_Mode_MasterRx

/* 时钟极性 */
#define I2S_CPOL_LOW              I2S_CPOL_Low
#define I2S_CPOL_HIGH             I2S_CPOL_High

/* 数据格式 */
#define I2S_DATAFORMAT_16B            I2S_DataFormat_16b
#define I2S_DATAFORMAT_16B_EXTENDED   I2S_DataFormat_16bextended
#define I2S_DATAFORMAT_24B            I2S_DataFormat_24b
#define I2S_DATAFORMAT_32B            I2S_DataFormat_32b

void I2S2_Init(u32 I2S_Standard, u32 I2S_Mode, u32 I2S_Clock_Polarity, u32 I2S_DataFormat);
u8   I2S2_SampleRate_Set(u32 samplerate);
void I2S2_TX_DMA_Init(u8 *buf0, u8 *buf1, u16 num);
void I2S_Play_Start(void);
void I2S_Play_Stop(void);

#endif
