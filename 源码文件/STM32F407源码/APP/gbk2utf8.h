#ifndef __GBK2UTF8_H
#define __GBK2UTF8_H

#ifdef __cplusplus
extern "C" {
#endif

/* GBK(GB2312 部分) 双字节 -> UTF-8
 * src: 以 0 结尾的 GBK 字符串
 * dst: 输出缓冲（UTF-8，以 0 结尾）
 * dstsz: dst 容量（字节）
 * 返回: 写入 dst 的字节数（不含结尾 0）
 */
int gbk_to_utf8(const char *src, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif
