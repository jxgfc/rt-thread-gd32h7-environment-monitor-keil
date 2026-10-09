/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DRV_SDCARD_H
#define DRV_SDCARD_H

#include <rtthread.h>

/* 必须在调度器启动后由线程调用；这里只注册裸 sd0 块设备，文件系统挂载
 * 由 env_log 存储服务负责，避免多个线程同时操作 FatFs。 */
int gd32_sdcard_init(void);
rt_bool_t gd32_sdcard_is_ready(void);
int gd32_sdcard_last_error(void);

#endif
