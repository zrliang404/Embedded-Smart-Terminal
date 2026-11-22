#ifndef __LINK_H
#define __LINK_H

#include "net.h"

/* ============================================================
 * 通信层：与 F407 的软件串口协议
 * 只负责“收命令行 / 解析 / 组装应答”，网络动作交给 net 层
 * ============================================================ */

/* 初始化串口（不启动网络） */
void link_init(void);

/* 主循环调用：收 F407 与 USB 调试口的命令 */
void link_poll(void);

/* 供 main 注册给网络层的回调表 */
const net_hooks_t *link_hooks(void);

#endif
