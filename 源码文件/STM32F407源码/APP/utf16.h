#ifndef __UTF16_H
#define __UTF16_H

#include "stdint.h"

/* UTF-8 <-> UTF-16 转换（纯算法，无查表） */

int  utf8_to_utf16(const char *src, uint16_t *dst, int dstcap);
int  utf16_to_utf8(const uint16_t *src, char *dst, int dstcap);

int  wstr_len(const uint16_t *s);
void wstr_cpy(uint16_t *dst, const uint16_t *src, int cap);
void wstr_cat(uint16_t *dst, const uint16_t *src, int cap);

#endif
