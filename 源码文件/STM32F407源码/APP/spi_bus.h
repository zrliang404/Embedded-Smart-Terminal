#ifndef __SPI_BUS_H
#define __SPI_BUS_H

/* SPI1 总线互斥锁：字库（W25Q16）与触摸屏（XPT2046）共用 SPI1，
 * 而且触摸驱动会把 PA5/6/7 在 SPI/GPIO 之间切换，必须互斥访问 */
void spi_bus_init(void);
void spi_bus_lock(void);
void spi_bus_unlock(void);

#endif
