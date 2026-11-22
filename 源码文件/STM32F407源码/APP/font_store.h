#ifndef __FONT_STORE_H
#define __FONT_STORE_H

#include "stm32f4xx.h"

/* W25Q16 (2MB SPI Flash) �ϵ��ֿ�洢
 * ����:  0x00100000  ͷ�� 16 �ֽ�
 *        0x00100010  ԭʼ .bin ���ݣ�ƫ�����ļ���ȫһ�£�
 * �ܿ�����У׼ռ�õ���� 64KB (0x1F0000) */

/* �ĳ� 1 ����ǿ�����´� SD ����װ�ֿ⣨������ʱ�ã� */
#define FONT_STORE_FORCE      0

#define FONT_STORE_HDR_ADDR   0x00100000u
#define FONT_STORE_DATA_ADDR  0x00100010u

uint8_t font_store_valid(uint32_t *size);
uint8_t font_store_install(const char *sd_path, uint32_t *size);
void    font_store_read(uint32_t off, void *dst, uint32_t len);

#endif
