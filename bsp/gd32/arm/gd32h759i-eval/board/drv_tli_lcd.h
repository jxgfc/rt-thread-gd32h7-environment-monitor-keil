/* 480x272 RGB565 TLI 并行屏的 RT-Thread 图形设备接口。 */
#ifndef __DRV_TLI_LCD_H__
#define __DRV_TLI_LCD_H__

#include <rtthread.h>

#define GD32_TLI_LCD_WIDTH  480
#define GD32_TLI_LCD_HEIGHT 272

/* 仅注册 LCD 设备，不在注册阶段访问 TLI；LVGL 打开设备后才初始化硬件，
 * 这样启动日志和控制台可以先正常运行。 */
rt_err_t gd32_tli_lcd_register(void);

rt_err_t gd32_tli_lcd_blit(rt_uint16_t x, rt_uint16_t y,
                            rt_uint16_t width, rt_uint16_t height,
                            const rt_uint16_t *pixels, rt_uint16_t stride);
const rt_uint16_t *gd32_tli_lcd_framebuffer(void);

#endif
