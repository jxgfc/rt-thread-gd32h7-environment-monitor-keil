#ifndef __DRV_GT911_H__
#define __DRV_GT911_H__

#include <rtthread.h>

/* 29_TLI_Touch_Draw 面板上的 GT911（7 位 I2C 地址 0x5d），返回单指坐标。 */
rt_err_t gd32_gt911_init(void);
rt_err_t gd32_gt911_read(rt_bool_t *pressed, rt_uint16_t *x, rt_uint16_t *y);

#endif
