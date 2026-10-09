/* LVGL 显示端口：小块绘图缓冲 + TLI 全屏静态帧缓冲。 */
#include <lvgl.h>
#include <rtdevice.h>
#include <rtdbg.h>
#include "drv_tli_lcd.h"

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static struct rt_device_graphic_info s_info;
static lv_color_t *s_buf1;
static lv_color_t *s_buf2;

/* LVGL 的 area 是闭区间坐标；裁剪后再交给 TLI，最后必须无条件通知
 * lv_disp_flush_ready，否则 LVGL 会停在 flushing 状态。 */
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                     lv_color_t *color_p)
{
    rt_int32_t x1 = area->x1;
    rt_int32_t y1 = area->y1;
    rt_int32_t x2 = area->x2;
    rt_int32_t y2 = area->y2;
    rt_int32_t src_stride = area->x2 - area->x1 + 1;
    const rt_uint16_t *src = (const rt_uint16_t *)color_p;

    /* LVGL 通常已经裁剪区域，但这里仍防御性处理越界坐标。 */
    if (x1 < 0) { src += -x1; x1 = 0; }
    if (y1 < 0) { src += (-y1) * src_stride; y1 = 0; }
    if (x2 >= (rt_int32_t)s_info.width) x2 = s_info.width - 1;
    if (y2 >= (rt_int32_t)s_info.height) y2 = s_info.height - 1;
    if (x1 <= x2 && y1 <= y2)
    {
        (void)gd32_tli_lcd_blit((rt_uint16_t)x1, (rt_uint16_t)y1,
                                (rt_uint16_t)(x2 - x1 + 1),
                                (rt_uint16_t)(y2 - y1 + 1),
                                src, (rt_uint16_t)src_stride);
    }
    lv_disp_flush_ready(drv);
}

rt_err_t lv_port_disp_init(void)
{
    rt_device_t lcd = rt_device_find("lcd");
    rt_size_t pixels;
    rt_kprintf("UI: LCD open begin\n");
    if (lcd == RT_NULL || rt_device_open(lcd, RT_DEVICE_FLAG_RDWR) != RT_EOK)
    {
        LOG_E("TLI LCD open failed");
        return -RT_EIO;
    }
    rt_kprintf("UI: LCD open complete\n");
    if (rt_device_control(lcd, RTGRAPHIC_CTRL_GET_INFO, &s_info) != RT_EOK)
    {
        LOG_E("TLI LCD info unavailable");
        return -RT_EIO;
    }
    /* 两个 40 行缓冲控制 LVGL 绘图堆占用；TLI 仍扫描独立的完整帧缓冲。 */
    pixels = (rt_size_t)s_info.width * 40U;
    s_buf1 = rt_malloc(pixels * sizeof(lv_color_t));
    s_buf2 = rt_malloc(pixels * sizeof(lv_color_t));
    if (s_buf1 == RT_NULL || s_buf2 == RT_NULL)
    {
        LOG_E("LVGL draw buffers allocation failed");
        if (s_buf1 != RT_NULL)
        {
            rt_free(s_buf1);
            s_buf1 = RT_NULL;
        }
        if (s_buf2 != RT_NULL)
        {
            rt_free(s_buf2);
            s_buf2 = RT_NULL;
        }
        return -RT_ENOMEM;
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, pixels);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = s_info.width;
    s_disp_drv.ver_res = s_info.height;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = flush_cb;
    if (lv_disp_drv_register(&s_disp_drv) == RT_NULL)
    {
        LOG_E("LVGL display registration failed");
        rt_free(s_buf1);
        rt_free(s_buf2);
        s_buf1 = RT_NULL;
        s_buf2 = RT_NULL;
        return -RT_ENOMEM;
    }
    rt_kprintf("UI: display registered\n");
    return RT_EOK;
}
