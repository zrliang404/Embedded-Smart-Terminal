#include "fs_lock.h"
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_fs_mutex = 0;

void fs_lock_init(void)
{
    if (s_fs_mutex == 0)
    {
        s_fs_mutex = xSemaphoreCreateRecursiveMutex();
    }
}

void fs_lock(void)
{
    if (s_fs_mutex)
    {
        xSemaphoreTakeRecursive(s_fs_mutex, portMAX_DELAY);
    }
}

void fs_unlock(void)
{
    if (s_fs_mutex)
    {
        xSemaphoreGiveRecursive(s_fs_mutex);
    }
}
