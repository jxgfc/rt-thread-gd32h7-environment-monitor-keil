/*
 * GD32H759I-EVAL TLI 并行 RGB565 屏驱动。
 *
 * 这是官方 29_TLI_Touch_Draw 使用的并行 RGB565 屏，不使用 SPI LCD。
 * TLI 外设持续扫描 AXI SRAM 中的完整帧缓冲，LVGL 只把脏区域复制到该
 * 缓冲区；因此帧缓冲地址、Cache 清理和 LCD 时序必须保持一致。
 */
#include <rtdevice.h>
#include <rthw.h>
#include <stdint.h>
#include <board.h>
#include "drv_tli_lcd.h"

#include "gd32h7xx_tli.h"

#define LCD_WIDTH  GD32_TLI_LCD_WIDTH
#define LCD_HEIGHT GD32_TLI_LCD_HEIGHT

#define HORIZONTAL_SYNCHRONOUS_PULSE  41U
#define HORIZONTAL_BACK_PORCH          2U
#define HORIZONTAL_FRONT_PORCH         2U
#define VERTICAL_SYNCHRONOUS_PULSE    10U
#define VERTICAL_BACK_PORCH            2U
#define VERTICAL_FRONT_PORCH           2U

#define LCD_BL_PIN GET_PIN(G, 13)

/*
 * 保持帧缓冲地址与官方例程一致：
 * 29_TLI_Touch_Draw project.  TLI is a bus master and the address must be in
 * AXI SRAM (DTCM at 0x20000000 is not visible to it).  This address is also
 * 32-byte aligned, which is required by the cache maintenance below.
 */
#define FRAME_BUFFER_START_ADDR ((rt_uint32_t)0x24005000U)
#define FRAME_BUFFER_BYTES      ((rt_size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(rt_uint16_t))
static rt_uint16_t *const s_framebuffer =
    (rt_uint16_t *)(rt_uintptr_t)FRAME_BUFFER_START_ADDR;
static struct rt_device s_lcd;
static struct rt_device_graphic_info s_info;
static rt_bool_t s_ready;

/* TLI 是总线主设备，CPU 写完缓存后必须 clean，否则屏幕可能仍显示旧数据。 */
static void cache_clean_region(const void *addr, rt_size_t len)
{
    rt_uintptr_t start;
    rt_uintptr_t end;
    if (addr == RT_NULL || len == 0) return;
    start = (rt_uintptr_t)addr & ~(rt_uintptr_t)31U;
    end = ((rt_uintptr_t)addr + len + 31U) & ~(rt_uintptr_t)31U;
    SCB_CleanDCache_by_Addr((int32_t *)start, (int32_t)(end - start));
}

static void tli_gpio_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);
    rcu_periph_clock_enable(RCU_GPIOE);
    rcu_periph_clock_enable(RCU_GPIOF);
    rcu_periph_clock_enable(RCU_GPIOG);
    rcu_periph_clock_enable(RCU_GPIOH);

    /* 同步信号：HSYNC=PE15、VSYNC=PA7、PCLK=PG7。 */
    gpio_af_set(GPIOE, GPIO_AF_10, GPIO_PIN_15);
    gpio_af_set(GPIOA, GPIO_AF_14, GPIO_PIN_7);
    gpio_af_set(GPIOG, GPIO_AF_14, GPIO_PIN_7);
    gpio_mode_set(GPIOE, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_15);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_7);
    gpio_mode_set(GPIOG, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_7);
    gpio_output_options_set(GPIOE, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_15);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_7);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_7);

    /* RGB 数据线必须与 29_TLI_Touch_Draw 完全一致，不能按 SPI LCD 接法改动。 */
    gpio_af_set(GPIOG, GPIO_AF_14, GPIO_PIN_6);
    gpio_af_set(GPIOH, GPIO_AF_14, GPIO_PIN_12 | GPIO_PIN_11 |
                GPIO_PIN_9 | GPIO_PIN_8 | GPIO_PIN_3 | GPIO_PIN_2);
    gpio_af_set(GPIOA, GPIO_AF_14, GPIO_PIN_5);
    gpio_af_set(GPIOD, GPIO_AF_14, GPIO_PIN_3);
    gpio_af_set(GPIOC, GPIO_AF_14, GPIO_PIN_7 | GPIO_PIN_1 | GPIO_PIN_11);
    gpio_af_set(GPIOH, GPIO_AF_14, GPIO_PIN_15 | GPIO_PIN_14 | GPIO_PIN_13);
    gpio_af_set(GPIOB, GPIO_AF_14, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_9 | GPIO_PIN_8);
    gpio_af_set(GPIOB, GPIO_AF_3, GPIO_PIN_5);
    gpio_af_set(GPIOG, GPIO_AF_14, GPIO_PIN_11 | GPIO_PIN_10 | GPIO_PIN_12 | GPIO_PIN_14);
    gpio_af_set(GPIOF, GPIO_AF_14, GPIO_PIN_10);

    gpio_mode_set(GPIOG, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_6 | GPIO_PIN_11 |
                  GPIO_PIN_10 | GPIO_PIN_12 | GPIO_PIN_14);
    gpio_mode_set(GPIOH, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_12 | GPIO_PIN_11 |
                  GPIO_PIN_9 | GPIO_PIN_8 | GPIO_PIN_3 | GPIO_PIN_2 |
                  GPIO_PIN_15 | GPIO_PIN_14 | GPIO_PIN_13);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_5);
    gpio_mode_set(GPIOD, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_3);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_7 | GPIO_PIN_1 | GPIO_PIN_11);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_0 | GPIO_PIN_1 |
                  GPIO_PIN_9 | GPIO_PIN_8 | GPIO_PIN_5);
    gpio_mode_set(GPIOF, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_10);

    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_6 | GPIO_PIN_11 | GPIO_PIN_10 | GPIO_PIN_12 | GPIO_PIN_14);
    gpio_output_options_set(GPIOH, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_12 | GPIO_PIN_11 | GPIO_PIN_9 | GPIO_PIN_8 |
                            GPIO_PIN_3 | GPIO_PIN_2 | GPIO_PIN_15 | GPIO_PIN_14 | GPIO_PIN_13);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_5);
    gpio_output_options_set(GPIOD, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_3);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_7 | GPIO_PIN_1 | GPIO_PIN_11);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_9 | GPIO_PIN_8 | GPIO_PIN_5);
    gpio_output_options_set(GPIOF, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_10);

    /* LCD PWM BackLight (PG13), same polarity and electrical mode as the
     * vendor example.  Keep it enabled while TLI is initialized so a valid
     * test pattern is visible immediately after the controller starts. */
    gpio_mode_set(GPIOG, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, GPIO_PIN_13);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_13);
    gpio_bit_set(GPIOG, GPIO_PIN_13);
}

static rt_bool_t tli_controller_init(void)
{
    tli_parameter_struct tli;
    tli_layer_parameter_struct layer;

    rcu_periph_clock_enable(RCU_TLI);
    tli_gpio_init();

    /* TLI 像素时钟沿用官方例程配置；改 PLL 或分频会导致白屏/花屏。 */
    rcu_pll_input_output_clock_range_config(IDX_PLL2, RCU_PLL2RNG_1M_2M,
                                            RCU_PLL2VCO_192M_836M);
    if (ERROR == rcu_pll2_config(25, 192, 3, 3, 3))
        return RT_FALSE;
    rcu_pll_clock_output_enable(RCU_PLL2R);
    rcu_tli_clock_div_config(RCU_PLL2R_DIV8);
    rcu_osci_on(RCU_PLL2_CK);
    if (ERROR == rcu_osci_stab_wait(RCU_PLL2_CK))
        return RT_FALSE;

    tli_struct_para_init(&tli);
    tli.signalpolarity_hs = TLI_HSYN_ACTLIVE_LOW;
    tli.signalpolarity_vs = TLI_VSYN_ACTLIVE_LOW;
    tli.signalpolarity_de = TLI_DE_ACTLIVE_LOW;
    tli.signalpolarity_pixelck = TLI_PIXEL_CLOCK_TLI;
    tli.synpsz_hpsz = HORIZONTAL_SYNCHRONOUS_PULSE - 1U;
    tli.synpsz_vpsz = VERTICAL_SYNCHRONOUS_PULSE - 1U;
    tli.backpsz_hbpsz = HORIZONTAL_SYNCHRONOUS_PULSE + HORIZONTAL_BACK_PORCH - 1U;
    tli.backpsz_vbpsz = VERTICAL_SYNCHRONOUS_PULSE + VERTICAL_BACK_PORCH - 1U;
    tli.activesz_hasz = HORIZONTAL_SYNCHRONOUS_PULSE + HORIZONTAL_BACK_PORCH + LCD_WIDTH - 1U;
    tli.activesz_vasz = VERTICAL_SYNCHRONOUS_PULSE + VERTICAL_BACK_PORCH + LCD_HEIGHT - 1U;
    tli.totalsz_htsz = HORIZONTAL_SYNCHRONOUS_PULSE + HORIZONTAL_BACK_PORCH + LCD_WIDTH +
                       HORIZONTAL_FRONT_PORCH - 1U;
    tli.totalsz_vtsz = VERTICAL_SYNCHRONOUS_PULSE + VERTICAL_BACK_PORCH + LCD_HEIGHT +
                       VERTICAL_FRONT_PORCH - 1U;
    tli.backcolor_blue = 0;
    tli.backcolor_green = 0;
    tli.backcolor_red = 0;
    tli_init(&tli);

    tli_layer_struct_para_init(&layer);
    layer.layer_window_leftpos = HORIZONTAL_SYNCHRONOUS_PULSE + HORIZONTAL_BACK_PORCH;
    layer.layer_window_rightpos = layer.layer_window_leftpos + LCD_WIDTH - 1U;
    layer.layer_window_toppos = VERTICAL_SYNCHRONOUS_PULSE + VERTICAL_BACK_PORCH;
    layer.layer_window_bottompos = layer.layer_window_toppos + LCD_HEIGHT - 1U;
    layer.layer_ppf = LAYER_PPF_RGB565;
    layer.layer_sa = 255;
    layer.layer_acf1 = LAYER_ACF1_SA;
    layer.layer_acf2 = LAYER_ACF2_SA;
    layer.layer_frame_bufaddr = (rt_uint32_t)(uintptr_t)s_framebuffer;
    layer.layer_frame_line_length = (LCD_WIDTH * 2U) + 7U;
    layer.layer_frame_buf_stride_offset = LCD_WIDTH * 2U;
    layer.layer_frame_total_line_number = LCD_HEIGHT;
    tli_layer_init(LAYER0, &layer);
    tli_dither_config(TLI_DITHER_ENABLE);
    tli_layer_enable(LAYER0);
    tli_reload_config(TLI_REQUEST_RELOAD_EN);
    tli_enable();
    rt_pin_write(LCD_BL_PIN, PIN_HIGH);
    return RT_TRUE;
}

static rt_err_t lcd_init(rt_device_t device)
{
    RT_UNUSED(device);
    if (!s_ready)
    {
        /* 先刷白色测试背景；LVGL 初始化后会覆盖发生变化的区域。 */
        rt_memset(s_framebuffer, 0xff, FRAME_BUFFER_BYTES);
        if (!tli_controller_init())
            return -RT_ERROR;
        cache_clean_region(s_framebuffer, FRAME_BUFFER_BYTES);
        s_ready = RT_TRUE;
    }
    return RT_EOK;
}

static rt_err_t lcd_control(rt_device_t device, int cmd, void *args)
{
    RT_UNUSED(device);
    if (cmd == RTGRAPHIC_CTRL_GET_INFO)
    {
        if (args == RT_NULL) return -RT_EINVAL;
        rt_memcpy(args, &s_info, sizeof(s_info));
        return RT_EOK;
    }
    if (cmd == RTGRAPHIC_CTRL_POWERON) { rt_pin_write(LCD_BL_PIN, PIN_HIGH); return RT_EOK; }
    if (cmd == RTGRAPHIC_CTRL_POWEROFF) { rt_pin_write(LCD_BL_PIN, PIN_LOW); return RT_EOK; }
    /* RECT_UPDATE 的区域坐标来自 LVGL，先做完整边界检查再触碰帧缓冲。 */
    if (cmd == RTGRAPHIC_CTRL_RECT_UPDATE)
    {
        const struct rt_device_rect_info *r = args;
        if (r == RT_NULL) return -RT_EINVAL;
        if (r->width == 0 || r->height == 0 || r->x >= LCD_WIDTH || r->y >= LCD_HEIGHT)
            return -RT_EINVAL;
        if (r->width > LCD_WIDTH - r->x || r->height > LCD_HEIGHT - r->y)
            return -RT_EINVAL;
        return gd32_tli_lcd_blit(r->x, r->y, r->width, r->height,
                                  &s_framebuffer[r->y * LCD_WIDTH + r->x], LCD_WIDTH);
    }
    return -RT_ENOSYS;
}

rt_err_t gd32_tli_lcd_blit(rt_uint16_t x, rt_uint16_t y,
                           rt_uint16_t width, rt_uint16_t height,
                           const rt_uint16_t *pixels, rt_uint16_t stride)
{
    rt_uint16_t row;
    /* stride 是源图像每行的像素数，不能假设等于 width（LVGL 脏区通常有
     * 自己的行跨度）；目标帧缓冲始终使用 LCD_WIDTH。 */
    if (!s_ready || pixels == RT_NULL || stride == 0 || width == 0 || height == 0 ||
        x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return -RT_EINVAL;
    if (width > LCD_WIDTH - x) width = LCD_WIDTH - x;
    if (height > LCD_HEIGHT - y) height = LCD_HEIGHT - y;
    for (row = 0; row < height; row++)
    {
        rt_memcpy(&s_framebuffer[(y + row) * LCD_WIDTH + x],
                  &pixels[row * stride], width * sizeof(rt_uint16_t));
    }
    cache_clean_region(&s_framebuffer[y * LCD_WIDTH + x],
                       (height - 1U) * LCD_WIDTH * sizeof(rt_uint16_t) +
                       width * sizeof(rt_uint16_t));
    return RT_EOK;
}

const rt_uint16_t *gd32_tli_lcd_framebuffer(void) { return s_framebuffer; }

static void tli_test_cmd(int argc, char **argv)
{
    static const rt_uint16_t colors[] = {
        0xf800U, 0xffe0U, 0x07e0U, 0x07ffU,
        0x001fU, 0xf81fU, 0xffffU, 0x0000U
    };
    rt_uint16_t x;
    rt_uint16_t y;
    rt_uint16_t bar;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    if (!s_ready)
    {
        rt_kprintf("lcd is not initialized\n");
        return;
    }

    for (y = 0; y < LCD_HEIGHT; y++)
    {
        for (x = 0; x < LCD_WIDTH; x++)
        {
            bar = (rt_uint16_t)((x * (sizeof(colors) / sizeof(colors[0]))) / LCD_WIDTH);
            s_framebuffer[y * LCD_WIDTH + x] = colors[bar];
        }
    }
    cache_clean_region(s_framebuffer, FRAME_BUFFER_BYTES);
    rt_kprintf("TLI RGB565 color bars written to 0x%08x\n",
               (unsigned int)FRAME_BUFFER_START_ADDR);
}
MSH_CMD_EXPORT_ALIAS(tli_test_cmd, tli_test,
                     display TLI RGB565 color bars);

rt_err_t gd32_tli_lcd_register(void)
{
    if (rt_device_find("lcd") != RT_NULL)
        return RT_EOK;

    s_info.pixel_format = RTGRAPHIC_PIXEL_FORMAT_RGB565;
    s_info.bits_per_pixel = 16;
    s_info.pitch = LCD_WIDTH * 2;
    s_info.width = LCD_WIDTH;
    s_info.height = LCD_HEIGHT;
    s_info.framebuffer = (rt_uint8_t *)s_framebuffer;
    s_info.smem_len = FRAME_BUFFER_BYTES;
    s_lcd.type = RT_Device_Class_Graphic;
    s_lcd.init = lcd_init;
    s_lcd.control = lcd_control;
    return rt_device_register(&s_lcd, "lcd", RT_DEVICE_FLAG_RDWR);
}
