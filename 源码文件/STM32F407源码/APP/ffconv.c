#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"

/* FatFs 的 OEM 码页转换钩子。
 * _LFN_UNICODE = 1 时长文件名直接以 UTF-16 返回，不需要 GBK 码表，
 * 这里只处理 ASCII（纯 8.3 短文件名会用到）。 */
WCHAR ff_convert(WCHAR chr, UINT dir)
{
    (void)dir;

    if (chr < 0x80) return chr;

    return 0;
}

/* 大写转换：cc936.c 原本提供，去掉码表后自己实现（只处理 ASCII） */
WCHAR ff_wtoupper(WCHAR chr)
{
    if (chr >= 'a' && chr <= 'z')
    {
        return (WCHAR)(chr - 'a' + 'A');
    }
    return chr;
}

/* 内存分配钩子：_USE_LFN == 3 时 FatFs 用它申请长文件名缓冲。
 * 原本由 cc936.c 提供，这里改用 FreeRTOS 堆，并保证 4 字节对齐。 */
void *ff_memalloc(UINT msize)
{
    return pvPortMalloc((size_t)msize);
}

void ff_memfree(void *mblock)
{
    vPortFree(mblock);
}
