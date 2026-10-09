#ifndef GD32_BOARD_DRV_RTC_H
#define GD32_BOARD_DRV_RTC_H

#include <rtthread.h>

/* 仅在线程上下文调用：默认日期或 RTC 失败时返回 false，标准 date 设置成功
 * 后返回 true。标志保存在备份域；掉电期间 IRC32K 是否持续运行不作保证。 */
rt_bool_t gd32_rtc_time_is_set(void);

#endif
