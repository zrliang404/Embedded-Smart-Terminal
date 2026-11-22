/*-----------------------------------------------------------------------*/
/* FatFs 底层磁盘 I/O 适配层（STM32F407 + SDIO + W25Q16）                  */
/*                                                                       */
/* 说明：本文件把 FatFs 的 disk_read / disk_write 等接口接到实际硬件上：   */
/*       卷 0 = SD 卡（SDIO 4 位模式）                                     */
/*       卷 1 = W25Q16（SPI Flash，用于存放字库）                          */
/*-----------------------------------------------------------------------*/

#include "diskio.h"          /* FatFs 底层接口定义 */
#include "sdio_sdcard.h"
#include "w25qxx.h"
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

/* _USE_LFN = 3 时长文件名的缓冲区分配在栈上，不需要 malloc/free，
 * 因此这里没有实现 ff_memalloc / ff_memfree */

#define SD_CARD     0        /* 卷 0：SD 卡 */
#define EX_FLASH    1        /* 卷 1：外部 SPI Flash */

#define FLASH_SECTOR_SIZE   512

/* W25Q16 共 2 MB，这里保留原工程的 12 MB 逻辑容量写法
 * （超出实际容量的部分不会被访问） */
u16 FLASH_SECTOR_COUNT = 2048 * 12;

#define FLASH_BLOCK_SIZE    8    /* 每个块 = 8 个扇区 */

/*-----------------------------------------------------------------------*/
/* 获取磁盘状态：本工程固定返回 OK（真正的就绪判断在读写函数里做）        */
/*-----------------------------------------------------------------------*/
DSTATUS disk_status (
    BYTE pdrv                /* 物理驱动器号 */
)
{
    (void)pdrv;
    return RES_OK;
}

/*-----------------------------------------------------------------------*/
/* 初始化磁盘                                                            */
/*-----------------------------------------------------------------------*/
DSTATUS disk_initialize (
    BYTE pdrv                /* 物理驱动器号 */
)
{
    u8 res = 0;

    switch (pdrv)
    {
        case SD_CARD:        /* SD 卡 */
            res = SD_Init();
            break;

        case EX_FLASH:       /* 外部 SPI Flash */
            W25QXX_Init();
            break;

        default:
            res = 1;
    }

    if (res) return STA_NOINIT;
    else     return 0;       /* 初始化成功 */
}

/*-----------------------------------------------------------------------*/
/* 读扇区                                                                */
/*                                                                       */
/* pdrv   : 驱动器号 0~9                                                  */
/* buff   : 数据接收缓冲区                                                */
/* sector : 起始扇区（LBA）                                               */
/* count  : 要读取的扇区数                                                */
/*-----------------------------------------------------------------------*/
DRESULT disk_read (
    BYTE pdrv,               /* 驱动器号 */
    BYTE *buff,              /* 数据缓冲区 */
    DWORD sector,            /* 起始扇区 */
    UINT count               /* 扇区数 */
)
{
    u8 res = 0;

    if (!count) return RES_PARERR;      /* count 不能为 0 */

    switch (pdrv)
    {
        case SD_CARD:
            {
                /* 记录本次读盘耗时：超过 100ms 说明卡响应异常，打印出来便于排查 */
                TickType_t t0  = xTaskGetTickCount();
                uint32_t   dt;

                res = SD_ReadDisk(buff, sector, count);

                dt = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
                if (dt > 100)
                {
                    printf("[sd] SLOW read sector=%u cnt=%u %u ms\r\n",
                           (unsigned)sector, (unsigned)count, (unsigned)dt);
                }
            }

            if (res)        /* 读失败：最多重试 3 次 */
            {
                u8 retry;

                printf("[sd] READ fail sector=%u cnt=%u, retry...\r\n",
                       (unsigned)sector, (unsigned)count);

                for (retry = 0; retry < 3; retry++)
                {
                    vTaskDelay(1);      /* 让出 CPU，避免界面卡死 */

                    res = SD_ReadDisk(buff, sector, count);
                    if (res == 0)
                    {
                        printf("[sd]   retry %u OK\r\n", (unsigned)(retry + 1));
                        break;
                    }
                }

                if (res)
                {
                    static uint32_t s_last_reinit = 0;
                    uint32_t nowt = (uint32_t)xTaskGetTickCount();

                    /* 限制重初始化频率：500ms 内只做一次，避免反复 init 占 CPU */
                    if ((nowt - s_last_reinit) > 500u)
                    {
                        s_last_reinit = nowt;

                        printf("[sd]   ALL FAILED sector=%u, re-init card\r\n", (unsigned)sector);

                        SD_Init();          /* 重初始化，恢复 24MHz / 4 位 */

                        vTaskDelay(2);      /* 让界面先跑一下 */

                        res = SD_ReadDisk(buff, sector, count);
                        if (res == 0) printf("[sd]   re-init OK\r\n");
                    }
                }
            }
            break;

        case EX_FLASH:      /* 外部 Flash：按 512 字节逐扇区读 */
            for (; count > 0; count--)
            {
                W25QXX_Read(buff, sector * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
                sector++;
                buff += FLASH_SECTOR_SIZE;
            }
            res = 0;
            break;

        default:
            res = 1;
    }

    return (res == 0x00) ? RES_OK : RES_ERROR;
}

/*-----------------------------------------------------------------------*/
/* 写扇区                                                                */
/*                                                                       */
/* pdrv   : 驱动器号 0~9                                                  */
/* buff   : 数据缓冲区                                                    */
/* sector : 起始扇区（LBA）                                               */
/* count  : 要写入的扇区数                                                */
/*-----------------------------------------------------------------------*/
DRESULT disk_write (
    BYTE pdrv,               /* 驱动器号 */
    const BYTE *buff,        /* 待写入数据 */
    DWORD sector,            /* 起始扇区 */
    UINT count               /* 扇区数 */
)
{
    u8 res = 0;

    if (!count) return RES_PARERR;      /* count 不能为 0 */

    switch (pdrv)
    {
        case SD_CARD:
            res = SD_WriteDisk((u8 *)buff, sector, count);

            if (res)        /* 写失败：最多重试 3 次 */
            {
                u8 retry;

                printf("[sd] WRITE fail sector=%u cnt=%u, retry...\r\n",
                       (unsigned)sector, (unsigned)count);

                for (retry = 0; retry < 3; retry++)
                {
                    vTaskDelay(1);      /* 让出 CPU */

                    res = SD_WriteDisk((u8 *)buff, sector, count);
                    if (res == 0)
                    {
                        printf("[sd]   retry %u OK\r\n", (unsigned)(retry + 1));
                        break;
                    }
                }

                if (res) printf("[sd]   ALL FAILED sector=%u\r\n", (unsigned)sector);
            }
            break;

        case EX_FLASH:      /* 外部 Flash：按 512 字节逐扇区写 */
            for (; count > 0; count--)
            {
                W25QXX_Write((u8 *)buff, sector * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
                sector++;
                buff += FLASH_SECTOR_SIZE;
            }
            res = 0;
            break;

        default:
            res = 1;
    }

    return (res == 0x00) ? RES_OK : RES_ERROR;
}

/*-----------------------------------------------------------------------*/
/* 其它控制接口（获取扇区大小、块大小、扇区总数等）                       */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl (
    BYTE pdrv,               /* 驱动器号 */
    BYTE cmd,                /* 控制码 */
    void *buff               /* 收发控制数据的缓冲区 */
)
{
    DRESULT res;

    if (pdrv == SD_CARD)     /* SD 卡 */
    {
        switch (cmd)
        {
            case CTRL_SYNC:
                res = RES_OK;
                break;

            case GET_SECTOR_SIZE:
                *(DWORD *)buff = 512;
                res = RES_OK;
                break;

            case GET_BLOCK_SIZE:
                *(WORD *)buff = 1;      /* SD 卡的块对齐单位 = 1 个扇区 */
                res = RES_OK;
                break;

            case GET_SECTOR_COUNT:
                *(DWORD *)buff = SDCardInfo.LogBlockNbr;
                res = RES_OK;
                break;

            default:
                res = RES_PARERR;
                break;
        }
    }
    else if (pdrv == EX_FLASH)   /* 外部 Flash */
    {
        switch (cmd)
        {
            case CTRL_SYNC:
                res = RES_OK;
                break;

            case GET_SECTOR_SIZE:
                *(WORD *)buff = FLASH_SECTOR_SIZE;
                res = RES_OK;
                break;

            case GET_BLOCK_SIZE:
                *(WORD *)buff = FLASH_BLOCK_SIZE;
                res = RES_OK;
                break;

            case GET_SECTOR_COUNT:
                *(DWORD *)buff = FLASH_SECTOR_COUNT;
                res = RES_OK;
                break;

            default:
                res = RES_PARERR;
                break;
        }
    }
    else
    {
        res = RES_ERROR;        /* 其他驱动器不支持 */
    }

    return res;
}

/*-----------------------------------------------------------------------*/
/* 给 FatFs 提供当前时间（用于文件时间戳）                                 */
/*                                                                      */
/* 位域：31-25 年(自 1980)，24-21 月(1-12)，20-16 日(1-31)                */
/*       15-11 时，10-5 分，4-0 秒(0-29，单位 2 秒)                       */
/* 本工程不使用文件时间戳，直接返回 0                                     */
/*-----------------------------------------------------------------------*/
DWORD get_fattime (void)
{
    return 0;
}

/* ff_memalloc / ff_memfree 未实现：_USE_LFN = 3 时长文件名缓冲区在栈上，
 * 不需要动态内存分配 */
