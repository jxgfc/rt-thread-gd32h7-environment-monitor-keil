/*
 * 官方 29_TLI_Touch_Draw 电容屏的 GT911 轮询驱动。
 * 使用软件 I2C 是因为面板的 SCL/SDA 与 LCD/板载资源复用；本文件只
 * 向 LVGL 提供单指指针，不尝试把 GT911 的多点数据转换成多指事件。
 */
#include <rtdevice.h>
#include <board.h>
#include "drv_gt911.h"
#include "drv_tli_lcd.h"

#define GT911_SCL       GET_PIN(H, 7)
#define GT911_SDA       GET_PIN(F, 9)
#define GT911_RST       GET_PIN(F, 6)
#define GT911_INT       GET_PIN(H, 6)
#define GT911_ADDR_WR   0xBAU
#define GT911_ADDR_RD   0xBBU
#define GT911_STATUS    0x814EU
#define GT911_COORD     0x814FU
#define GT911_READY     0x80U
#define GT911_MAX_POINTS 5U
#define GT911_INIT_RETRY_MS 1000U
#define GT911_RUNTIME_RETRY_ERRORS 3U

/* Configuration from the GD32 29_TLI_Touch_Draw example.  The first 184
 * bytes are checksummed below; the last two bytes are placeholders for the
 * checksum and the configuration-valid flag. */
static const rt_uint8_t gt911_cfg_params[] = {
    0x5A,0xE0,0x01,0x10,0x01,0x01,0x8D,0x00,0x01,0x08,0x28,0x05,
    0x55,0x32,0x03,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x89,0x2A,0x0B,0x17,0x15,0x31,0x0D,0x00,0x00,
    0x02,0xB9,0x04,0x2C,0x00,0x00,0x00,0x00,0x00,0x03,0x64,0x32,
    0x00,0x00,0x00,0x0F,0x94,0x94,0xC5,0x02,0x07,0x00,0x00,0x04,
    0x8D,0x13,0x00,0x5C,0x1E,0x00,0x3C,0x30,0x00,0x29,0x4C,0x00,
    0x20,0x78,0x00,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x04,0x06,0x08,0x0A,0x0C,0x0E,0x10,
    0x12,0x14,0x16,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x24,0x22,
    0x21,0x20,0x1F,0x1E,0x1D,0x1C,0x18,0x16,0x12,0x10,0x0F,0x0A,
    0x08,0x06,0x04,0x02,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0x21,0x01
};

static rt_bool_t s_inited;
static rt_bool_t s_retry_waiting;
static rt_tick_t s_retry_tick;
static rt_uint8_t s_error_streak;
static rt_bool_t s_last_pressed;
static rt_uint16_t s_last_x;
static rt_uint16_t s_last_y;

/* 3 us 的半周期由板级 us 延时实现；若该函数未实现，触摸时序不可靠。 */
static void dly(void) { rt_hw_us_delay(3); }
static void scl(rt_int32_t v) { rt_pin_write(GT911_SCL, v ? PIN_HIGH : PIN_LOW); }
static void sda(rt_int32_t v) { rt_pin_write(GT911_SDA, v ? PIN_HIGH : PIN_LOW); }

static void i2c_start(void)
{
    sda(1); dly(); scl(1); dly(); sda(0); dly(); scl(0); dly();
}

static void i2c_stop(void)
{
    sda(0); dly(); scl(1); dly(); sda(1); dly();
}

static void i2c_recover(void)
{
    unsigned i;
    /* A CPU reset partway through a byte can leave the controller holding
     * SDA. Nine bounded clocks and a STOP release an unfinished transfer. */
    sda(1);
    for (i = 0; i < 9U; i++)
    {
        scl(0); dly(); scl(1); dly();
    }
    i2c_stop();
}

static void retry_later(void)
{
    s_inited = RT_FALSE;
    s_last_pressed = RT_FALSE;
    s_retry_waiting = RT_TRUE;
    s_retry_tick = rt_tick_get() + rt_tick_from_millisecond(GT911_INIT_RETRY_MS);
}

static void i2c_send(rt_uint8_t value)
{
    rt_uint8_t i;
    /* SDA 采用开漏输出：发送逻辑 1 时释放总线，由上拉电阻拉高。 */
    for (i = 0; i < 8; i++)
    {
        sda((value & 0x80U) != 0); dly(); scl(1); dly(); scl(0); dly();
        value <<= 1;
    }
    sda(1);
}

static rt_bool_t i2c_ack(void)
{
    rt_bool_t nack;
    sda(1); dly(); scl(1); dly(); nack = (rt_pin_read(GT911_SDA) != 0); scl(0); dly();
    /* 返回值为 NACK=true，调用处因此统一使用 if (i2c_ack()) 进入失败路径。 */
    return nack;
}

static rt_uint8_t i2c_recv(rt_bool_t last)
{
    rt_uint8_t i, value = 0;
    /* Like the vendor example, release the open-drain pin and sample its
     * input register.  Do not switch to PIN_MODE_INPUT: the shared GD32 pin
     * driver currently selects a reserved H7 pull configuration for it. */
    sda(1); dly();
    for (i = 0; i < 8; i++)
    {
        value <<= 1; scl(1); dly();
        if (rt_pin_read(GT911_SDA)) value |= 1U;
        scl(0); dly();
    }
    sda(last ? 1 : 0); dly(); scl(1); dly(); scl(0); dly(); sda(1);
    return value;
}

static rt_err_t reg_read(rt_uint16_t reg, rt_uint8_t *buf, rt_size_t len)
{
    rt_size_t i;
    /* GT911 寄存器地址为大端 16 位，读操作使用 repeated-start。 */
    if (buf == RT_NULL || len == 0) return -RT_EINVAL;
    i2c_start(); i2c_send(GT911_ADDR_WR);
    if (i2c_ack()) goto fail;
    i2c_send((rt_uint8_t)(reg >> 8)); if (i2c_ack()) goto fail;
    i2c_send((rt_uint8_t)reg);       if (i2c_ack()) goto fail;
    i2c_start(); i2c_send(GT911_ADDR_RD);
    if (i2c_ack()) goto fail;
    for (i = 0; i < len; i++) buf[i] = i2c_recv(i + 1 == len);
    i2c_stop();
    return RT_EOK;
fail:
    i2c_stop();
    return -RT_ERROR;
}

static rt_err_t reg_write(rt_uint16_t reg, const rt_uint8_t *buf, rt_size_t len)
{
    rt_size_t i;
    if (buf == RT_NULL || len == 0) return -RT_EINVAL;
    i2c_start(); i2c_send(GT911_ADDR_WR);
    if (i2c_ack()) goto fail;
    i2c_send((rt_uint8_t)(reg >> 8)); if (i2c_ack()) goto fail;
    i2c_send((rt_uint8_t)reg);       if (i2c_ack()) goto fail;
    for (i = 0; i < len; i++) { i2c_send(buf[i]); if (i2c_ack()) goto fail; }
    i2c_stop();
    return RT_EOK;
fail:
    i2c_stop();
    return -RT_ERROR;
}

rt_err_t gd32_gt911_init(void)
{
    rt_uint8_t id[4];
    rt_uint8_t cfg[sizeof(gt911_cfg_params)];
    rt_uint8_t checksum = 0;
    rt_size_t i;
    /* 初始化在 LVGL 首次轮询时惰性执行，避免启动阶段触摸故障阻塞 LCD。 */
    if (s_inited) return RT_EOK;
    /* A transient power-up/I2C failure must not permanently disable touch.
     * Retry at a bounded rate so a disconnected panel cannot monopolize the
     * LVGL thread with repeated reset/configuration cycles. */
    if (s_retry_waiting && (rt_int32_t)(rt_tick_get() - s_retry_tick) < 0)
        return -RT_EBUSY;
    s_last_pressed = RT_FALSE;
    rt_pin_mode(GT911_SCL, PIN_MODE_OUTPUT_OD);
    rt_pin_mode(GT911_SDA, PIN_MODE_OUTPUT_OD);
    rt_pin_write(GT911_SCL, PIN_HIGH);
    rt_pin_write(GT911_SDA, PIN_HIGH);
    i2c_recover();
    rt_pin_mode(GT911_RST, PIN_MODE_OUTPUT);
    /* INT 在复位期间拉低选择 0x5d 地址，释放后只作为输入使用。 */
    rt_pin_mode(GT911_INT, PIN_MODE_OUTPUT);
    rt_pin_write(GT911_RST, PIN_LOW);
    rt_pin_write(GT911_INT, PIN_LOW);
    rt_thread_mdelay(20);
    rt_pin_write(GT911_RST, PIN_HIGH);
    rt_thread_mdelay(8);
    rt_pin_mode(GT911_INT, PIN_MODE_INPUT_PULLUP);
    rt_thread_mdelay(100);
    if (reg_read(0x8140, id, sizeof(id)) != RT_EOK ||
        id[0] != '9' || id[1] != '1' || id[2] != '1')
    {
        retry_later();
        rt_kprintf("touch: GT911 unavailable at 0x5d; retry in 1 s (SCL=PH7 SDA=PF9 RST=PF6 INT=PH6)\n");
        return -RT_ERROR;
    }
    rt_memcpy(cfg, gt911_cfg_params, sizeof(cfg));
    for (i = 0; i < sizeof(cfg) - 2U; i++) checksum = (rt_uint8_t)(checksum + cfg[i]);
    cfg[sizeof(cfg) - 2U] = (rt_uint8_t)(~checksum + 1U);
    cfg[sizeof(cfg) - 1U] = 1U;
    if (reg_write(0x8047, cfg, sizeof(cfg)) != RT_EOK)
    {
        retry_later();
        rt_kprintf("touch: GT911 configuration write failed; retry in 1 s\n");
        return -RT_ERROR;
    }
    /* The official example also waits for the new configuration to apply. */
    rt_thread_mdelay(10);
    s_inited = RT_TRUE;
    s_retry_waiting = RT_FALSE;
    s_error_streak = 0;
    rt_kprintf("touch: GT911 ready, ID=%02x%02x%02x%02x, addr=0x5d\n",
               id[0], id[1], id[2], id[3]);
    return RT_EOK;
}

rt_err_t gd32_gt911_read(rt_bool_t *pressed, rt_uint16_t *x, rt_uint16_t *y)
{
    rt_uint8_t status, count, coord[8], clear = 0;
    rt_uint16_t next_x = s_last_x, next_y = s_last_y;
    /* 输出参数必须同时有效；错误路径返回最后坐标但一定上报释放。 */
    if (pressed == RT_NULL || x == RT_NULL || y == RT_NULL) return -RT_EINVAL;
    if (!s_inited && gd32_gt911_init() != RT_EOK)
    {
        *pressed = RT_FALSE; *x = s_last_x; *y = s_last_y;
        return -RT_ERROR;
    }
    if (reg_read(GT911_STATUS, &status, 1) != RT_EOK) goto error;
    /* No fresh frame is not a release.  Keep the last state until the
     * controller reports a ready frame with zero contacts. */
    if (!(status & GT911_READY)) goto report;
    count = status & 0x0FU;
    if (count > GT911_MAX_POINTS)
    {
        (void)reg_write(GT911_STATUS, &clear, 1);
        goto error;
    }
    if (count != 0U)
    {
        if (reg_read(GT911_COORD, coord, sizeof(coord)) != RT_EOK) goto error;
        /* 0x814f: track ID, X low, X high, Y low, Y high, size low/high,
         * reserved.  LVGL uses the first contact, as a single pointer. */
        next_x = (rt_uint16_t)coord[1] | ((rt_uint16_t)coord[2] << 8);
        next_y = (rt_uint16_t)coord[3] | ((rt_uint16_t)coord[4] << 8);
        if (next_x >= GD32_TLI_LCD_WIDTH) next_x = GD32_TLI_LCD_WIDTH - 1;
        if (next_y >= GD32_TLI_LCD_HEIGHT) next_y = GD32_TLI_LCD_HEIGHT - 1;
    }
    if (reg_write(GT911_STATUS, &clear, 1) != RT_EOK) goto error;
    s_last_x = next_x;
    s_last_y = next_y;
    s_last_pressed = count != 0U;
report:
    s_error_streak = 0;
    *pressed = s_last_pressed; *x = s_last_x; *y = s_last_y;
    return RT_EOK;
error:
    s_last_pressed = RT_FALSE;
    if (++s_error_streak >= GT911_RUNTIME_RETRY_ERRORS)
    {
        /* Re-run the controller reset/configuration sequence after several
         * failed transactions.  This recovers from a brown-out or a bus that
         * was disturbed by an externally connected module. */
        retry_later();
        s_error_streak = 0;
        rt_kprintf("touch: repeated I2C errors; retry in 1 s\n");
    }
    *pressed = RT_FALSE; *x = s_last_x; *y = s_last_y;
    return -RT_ERROR;
}
