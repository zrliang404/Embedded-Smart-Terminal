#include "utf16.h"

int wstr_len(const uint16_t *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

void wstr_cpy(uint16_t *dst, const uint16_t *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void wstr_cat(uint16_t *dst, const uint16_t *src, int cap)
{
    int i = wstr_len(dst);
    int j = 0;
    if (cap <= 0) return;
    while (src[j] && i < cap - 1) { dst[i++] = src[j++]; }
    dst[i] = 0;
}

/* UTF-8 -> UTF-16（非法序列输出 '?'） */
int utf8_to_utf16(const char *src, uint16_t *dst, int dstcap)
{
    int w = 0;
    if (dstcap <= 0) return 0;

    while (*src)
    {
        unsigned char c = (unsigned char)*src;
        unsigned int cp;
        int extra;

        if (c < 0x80)                { cp = c;         extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F;  extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F;  extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07;  extra = 3; }
        else                         { cp = '?';       extra = 0; }

        src++;
        while (extra-- > 0 && *src)
        {
            cp = (cp << 6) | ((unsigned char)*src & 0x3F);
            src++;
        }

        if (cp > 0xFFFF)
        {
            cp -= 0x10000;
            if (w + 2 >= dstcap) break;
            dst[w++] = (uint16_t)(0xD800 + (cp >> 10));
            dst[w++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        }
        else
        {
            if (w + 1 >= dstcap) break;
            dst[w++] = (uint16_t)cp;
        }
    }

    dst[w] = 0;
    return w;
}

/* UTF-16 -> UTF-8 */
int utf16_to_utf8(const uint16_t *src, char *dst, int dstcap)
{
    int w = 0;

    if (dstcap <= 0) return 0;

    while (*src)
    {
        unsigned int cp = *src++;
        int n;

        if (cp >= 0xD800 && cp <= 0xDBFF && *src >= 0xDC00 && *src <= 0xDFFF)
        {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((*src++) - 0xDC00);
        }

        if (cp < 0x80)         n = 1;
        else if (cp < 0x800)   n = 2;
        else if (cp < 0x10000) n = 3;
        else                   n = 4;

        if (w + n >= dstcap) break;

        if (n == 1) dst[w++] = (char)cp;
        else if (n == 2)
        {
            dst[w++] = (char)(0xC0 | (cp >> 6));
            dst[w++] = (char)(0x80 | (cp & 0x3F));
        }
        else if (n == 3)
        {
            dst[w++] = (char)(0xE0 | (cp >> 12));
            dst[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[w++] = (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            dst[w++] = (char)(0xF0 | (cp >> 18));
            dst[w++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[w++] = (char)(0x80 | (cp & 0x3F));
        }
    }

    dst[w] = 0;
    return w;
}
