#include "font_store.h"
#include "w25qxx.h"
#include "spi.h"
#include "ff.h"
#include "fs_lock.h"
#include "spi_bus.h"
#include "utf16.h"
#include "string.h"
#include "stdio.h"

#define FONT_MAGIC   0x31425446u   /* "FTB1" */

typedef struct
{
    uint32_t magic;
    uint32_t size;
    uint32_t sum;
    uint32_t rsv;
} font_hdr_t;

uint8_t font_store_valid(uint32_t *size)
{
    font_hdr_t h;

    W25QXX_Read((u8 *)&h, FONT_STORE_HDR_ADDR, sizeof(h));

    if (h.magic != FONT_MAGIC) return 0;
    if (h.size < 64 || h.size > 0x000E0000u) return 0;   /* 最多 896KB */

    if (size) *size = h.size;
    return 1;
}

uint8_t font_store_install(const char *sd_path, uint32_t *out_size)
{
    FIL      fp;
    UINT     br;
    font_hdr_t h;
    uint32_t size, off, addr;
    static uint8_t buf[512];

    printf("[fs] install %s -> W25Q16\r\n", sd_path);

    WCHAR wpath[64];
    utf8_to_utf16(sd_path, wpath, 64);
    fs_lock();
    if (f_open(&fp, wpath, FA_READ) != FR_OK)
    {
        fs_unlock();
        printf("[fs] open failed\r\n");
        return 0;
    }
    size = f_size(&fp);
    fs_unlock();

    if (size < 64 || size > 0x000E0000u)
    {
        printf("[fs] bad size %u\r\n", (unsigned)size);
        f_close(&fp);
        return 0;
    }

    /* 按 64KB 块擦除（含头部区） */
    for (addr = FONT_STORE_HDR_ADDR; addr < FONT_STORE_DATA_ADDR + size; addr += 0x10000u)
    {
        spi_bus_lock();
        W25QXX_Erase_Block(addr);
        spi_bus_unlock();
        printf("[fs] erase 0x%06X\r\n", (unsigned)addr);
    }
    printf("[fs] erased, writing %u bytes...\r\n", (unsigned)size);

    off = 0;
    while (off < size)
    {
        UINT want = (UINT)((size - off > sizeof(buf)) ? sizeof(buf) : (size - off));

        fs_lock();
        if (f_read(&fp, buf, want, &br) != FR_OK || br != want)
        {
            fs_unlock();
            f_close(&fp);
            printf("[fs] read failed at %u\r\n", (unsigned)off);
            return 0;
        }
        fs_unlock();

        spi_bus_lock();
        W25QXX_Write_NoCheck(buf, FONT_STORE_DATA_ADDR + off, (u16)br);
        spi_bus_unlock();

        off += br;

        if ((off % 0x10000u) < sizeof(buf))
            printf("[fs] %u / %u\r\n", (unsigned)off, (unsigned)size);
    }

    f_close(&fp);

    h.magic = FONT_MAGIC;
    h.size  = size;
    h.sum   = 0;
    h.rsv   = 0;
    spi_bus_lock();
    W25QXX_Write_NoCheck((u8 *)&h, FONT_STORE_HDR_ADDR, sizeof(h));
    spi_bus_unlock();

    printf("[fs] install done: %u bytes\r\n", (unsigned)size);

    /* ---- 立刻回读校验 ---- */
    {
        uint8_t chk[16];
        font_hdr_t h2;

        W25QXX_Read(chk, FONT_STORE_DATA_ADDR, 16);
        printf("[fs] verify data: %02X %02X %02X %02X | %02X %02X %02X %02X | \
%02X %02X %02X %02X | %02X %02X %02X %02X\r\n",
               chk[0],chk[1],chk[2],chk[3],chk[4],chk[5],chk[6],chk[7],
               chk[8],chk[9],chk[10],chk[11],chk[12],chk[13],chk[14],chk[15]);
        printf("[fs] verify head tag: %c%c%c%c %s\r\n",
               chk[4],chk[5],chk[6],chk[7],
               (chk[4]=='h'&&chk[5]=='e'&&chk[6]=='a'&&chk[7]=='d') ? "OK" : "BAD");

        W25QXX_Read((u8 *)&h2, FONT_STORE_HDR_ADDR, sizeof(h2));
        printf("[fs] verify hdr: magic=0x%08X size=%u\r\n",
               (unsigned)h2.magic, (unsigned)h2.size);
    }

    if (out_size) *out_size = size;
    return 1;
}

void font_store_read(uint32_t off, void *dst, uint32_t len)
{
    spi_bus_lock();                              /* 与触摸屏共用 SPI1，互斥 */
    SPI1_SetSpeed(SPI_BaudRatePrescaler_4);      /* 10.5MHz */
    W25QXX_Read((u8 *)dst, FONT_STORE_DATA_ADDR + off, (u16)len);
    spi_bus_unlock();
}
