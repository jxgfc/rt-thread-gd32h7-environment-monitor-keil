/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GD32_HMI_PAGES_H
#define GD32_HMI_PAGES_H
#include <lvgl.h>

/* 页面对象由 LVGL 线程创建；update 也必须在同一线程调用。 */
void gd32_hmi_pages_init(lv_obj_t *screen, lv_obj_t *home);
void gd32_hmi_pages_update(void);
#endif
