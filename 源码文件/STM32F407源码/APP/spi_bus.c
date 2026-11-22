#include "spi_bus.h"
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_spi_mutex = 0;

void spi_bus_init(void)
{
    if (s_spi_mutex == 0)
        s_spi_mutex = xSemaphoreCreateRecursiveMutex();
}

void spi_bus_lock(void)
{
    if (s_spi_mutex) xSemaphoreTakeRecursive(s_spi_mutex, portMAX_DELAY);
}

void spi_bus_unlock(void)
{
    if (s_spi_mutex) xSemaphoreGiveRecursive(s_spi_mutex);
}
