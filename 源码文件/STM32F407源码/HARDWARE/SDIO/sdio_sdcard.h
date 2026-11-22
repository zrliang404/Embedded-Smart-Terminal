#ifndef _SDIO_SDCARD_H
#define _SDIO_SDCARD_H
#include "sys.h"

/* ============================================================
 * SD 卡驱动（标准库 SPL 版本）
 * 引脚：PC8~PC11 = D0~D3, PC12 = CK, PD2 = CMD
 * ============================================================ */

#define SD_TIMEOUT          ((uint32_t)100000000)

#define SD_TRANSFER_OK      ((uint8_t)0x00)
#define SD_TRANSFER_BUSY    ((uint8_t)0x01)

/* SD 卡信息（替代 HAL 的 HAL_SD_CardInfoTypeDef） */
typedef struct
{
    uint32_t CardVersion;       /* 版本 1=1.x  2=2.0(SC/HC) */
    uint32_t Class;
    uint32_t RelCardAdd;        /* RCA，卡相对地址 */
    uint32_t BlockNbr;          /* 扇区(512B)总数 */
    uint32_t BlockSize;         /* 扇区大小，固定 512 */
    uint32_t LogBlockNbr;       /* 逻辑扇区总数 */
    uint32_t LogBlockSize;      /* 逻辑扇区大小 */
} SD_CardInfo;

extern SD_CardInfo SDCardInfo;

/* 错误码 */
#define SD_OK               0
#define SD_ERR_CMD0         1     /* CMD0 无响应 */
#define SD_ERR_CMD8         2
#define SD_ERR_ACMD41       3     /* 卡初始化超时 */
#define SD_ERR_CMD2         4
#define SD_ERR_CMD3         5
#define SD_ERR_CMD9         6
#define SD_ERR_CMD7         7
#define SD_ERR_ACMD6        8     /* 切换到 4 位总线失败 */
#define SD_ERR_RW           9     /* 读写错误 */

u8   SD_Init(void);
void SD_GetCardInfo(SD_CardInfo *cardinfo);
u8   SD_GetCardState(void);
u8   SD_ReadDisk(u8 *buf, u32 sector, u32 cnt);
u8   SD_WriteDisk(u8 *buf, u32 sector, u32 cnt);

#endif
