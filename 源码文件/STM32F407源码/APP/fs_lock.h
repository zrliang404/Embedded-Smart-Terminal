#ifndef __FS_LOCK_H
#define __FS_LOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/* FatFs 不是线程安全的（_FS_REENTRANT = 0）。
 * 音频任务和界面任务都会访问 SD 卡，所以用一把互斥锁串行化。 */

void fs_lock_init(void);      /* 在调度器启动前调用一次 */
void fs_lock(void);           /* 获取锁 */
void fs_unlock(void);         /* 释放锁 */

#ifdef __cplusplus
}
#endif

#endif
