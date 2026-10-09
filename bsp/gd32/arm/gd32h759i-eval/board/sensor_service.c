/*
 * 环境传感器服务：通过 RT-Thread I2C 轮询 SHT40、BH1750 和 SGP30。
 *
 * 对外只发布一份带有效标志的快照，避免 UI、报警和日志线程直接访问
 * I2C。温度/湿度分别以 0.01 ℃/%RH 保存，光照以 0.1 lux 保存，气体
 * 指标使用 ppm/ppb；调用者必须先检查对应 valid/warming 标志。
 */
#include <rtdevice.h>
#include <rtdbg.h>
#include "sensor_service.h"

#define LOG_TAG "gd32.sensor"
#define SENSOR_I2C_BUS GD32_SENSOR_I2C_BUS
#define SHT40_ADDR 0x44
#define BH1750_ADDR 0x23
#define BH1750_ALT_ADDR 0x5c
#define SGP30_ADDR 0x58

static struct rt_i2c_bus_device *s_bus;
static struct gd32_sensor_snapshot s_snapshot;
static struct rt_mutex s_lock;
static struct rt_thread s_thread;
static rt_uint8_t s_stack[2048];
static rt_bool_t s_sgp30_ready;
static rt_tick_t s_sgp30_start;
static rt_bool_t s_bh1750_present;
static rt_uint8_t s_bh1750_address = BH1750_ADDR;

/* SHT40 与 SGP30 使用相同的 CRC-8 多项式 0x31，初值为 0xff。 */
static rt_uint8_t crc8(const rt_uint8_t *data, rt_size_t len)
{
    rt_uint8_t crc = 0xff;
    rt_size_t i;
    rt_uint8_t bit;
    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (rt_uint8_t)((crc << 1) ^ 0x31) : (rt_uint8_t)(crc << 1);
    }
    return crc;
}

static rt_err_t i2c_write(rt_uint8_t addr, const rt_uint8_t *data, rt_uint16_t len)
{
    struct rt_i2c_msg msg;
    msg.addr = addr;
    msg.flags = RT_I2C_WR;
    /* RT-Thread 的写消息接口不是 const 修饰；底层驱动只读此缓冲区。 */
    msg.buf = (rt_uint8_t *)data;
    msg.len = len;
    return rt_i2c_transfer(s_bus, &msg, 1) == 1 ? RT_EOK : -RT_EIO;
}

static rt_err_t i2c_read(rt_uint8_t addr, rt_uint8_t *data, rt_uint16_t len)
{
    struct rt_i2c_msg msg;
    msg.addr = addr;
    msg.flags = RT_I2C_RD;
    msg.buf = data;
    msg.len = len;
    return rt_i2c_transfer(s_bus, &msg, 1) == 1 ? RT_EOK : -RT_EIO;
}

static rt_err_t sht40_read(rt_int32_t *temperature, rt_uint16_t *humidity)
{
    rt_uint8_t command = 0xfd;
    rt_uint8_t data[6];
    rt_uint16_t raw_t;
    rt_uint16_t raw_h;
    rt_int64_t t;
    rt_int64_t h;
    /* 0xfd 是高精度单次测量命令，传感器转换至少需要约 8.2 ms。 */
    if (i2c_write(SHT40_ADDR, &command, 1) != RT_EOK)
        return -RT_ENOENT;
    rt_thread_mdelay(10);
    if (i2c_read(SHT40_ADDR, data, sizeof(data)) != RT_EOK)
        return -RT_EIO;
    if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5])
        return -RT_EIO;
    raw_t = ((rt_uint16_t)data[0] << 8) | data[1];
    raw_h = ((rt_uint16_t)data[3] << 8) | data[4];
    /* 使用 64 位中间量，避免 16 位原始值乘量程时溢出。 */
    t = -4500 + ((rt_int64_t)raw_t * 17500 + 32767) / 65535;
    h = -600 + ((rt_int64_t)raw_h * 12500 + 32767) / 65535;
    if (h < 0) h = 0;
    if (h > 10000) h = 10000;
    *temperature = (rt_int32_t)t;
    *humidity = (rt_uint16_t)h;
    return RT_EOK;
}

static rt_err_t bh1750_start(rt_uint8_t address)
{
    /* 先复位，再启动 0x10 高分辨率单次测量（典型转换时间 120 ms）。 */
    rt_uint8_t command = 0x01;
    if (i2c_write(address, &command, 1) != RT_EOK)
        return -RT_ENOENT;
    command = 0x10;
    return i2c_write(address, &command, 1);
}

static rt_err_t bh1750_read(rt_uint8_t address, rt_uint32_t *lux_x10)
{
    rt_uint8_t data[2];
    rt_uint16_t raw;
    if (i2c_read(address, data, sizeof(data)) != RT_EOK)
        return -RT_EIO;
    raw = ((rt_uint16_t)data[0] << 8) | data[1];
    /* 原始值 / 1.2 lux；乘 10 后为 raw * 25 / 3。 */
    *lux_x10 = ((rt_uint32_t)raw * 25 + 1) / 3;
    return RT_EOK;
}

static rt_err_t sgp30_init(void)
{
    const rt_uint8_t command[] = {0x20, 0x03};
    /* IAQ 初始化后需要约 15 s 预热，期间数据不能用于报警。 */
    if (i2c_write(SGP30_ADDR, command, sizeof(command)) != RT_EOK)
        return -RT_ENOENT;
    s_sgp30_start = rt_tick_get();
    s_sgp30_ready = RT_TRUE;
    return RT_EOK;
}

static rt_err_t sgp30_read(rt_uint16_t *eco2, rt_uint16_t *tvoc)
{
    const rt_uint8_t command[] = {0x20, 0x08};
    rt_uint8_t data[6];
    if (!s_sgp30_ready || i2c_write(SGP30_ADDR, command, sizeof(command)) != RT_EOK)
        return -RT_ENOENT;
    rt_thread_mdelay(12);
    if (i2c_read(SGP30_ADDR, data, sizeof(data)) != RT_EOK)
        return -RT_EIO;
    if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5])
        return -RT_EIO;
    *eco2 = ((rt_uint16_t)data[0] << 8) | data[1];
    *tvoc = ((rt_uint16_t)data[3] << 8) | data[4];
    return RT_EOK;
}

static void sensor_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);
    /* 所有 I2C 访问集中在此线程，避免多个业务线程并发占用总线。 */
    while (1)
    {
        struct gd32_sensor_snapshot next;
        rt_memset(&next, 0, sizeof(next));
        if (sht40_read(&next.temperature_x100, &next.humidity_x100) == RT_EOK)
            next.sht40_valid = RT_TRUE;
        if (!s_bh1750_present)
        {
            if (bh1750_start(BH1750_ADDR) == RT_EOK)
            {
                s_bh1750_address = BH1750_ADDR;
                s_bh1750_present = RT_TRUE;
            }
            else if (bh1750_start(BH1750_ALT_ADDR) == RT_EOK)
            {
                s_bh1750_address = BH1750_ALT_ADDR;
                s_bh1750_present = RT_TRUE;
            }
        }
        if (s_bh1750_present)
        {
            /* BH1750's high-resolution mode is one-shot: start a new
             * conversion each cycle before waiting for the result. */
            if (bh1750_start(s_bh1750_address) != RT_EOK)
            {
                s_bh1750_present = RT_FALSE;
            }
        }
        if (s_bh1750_present)
        {
            /* 0x10 is a one-shot high-resolution conversion (120 ms max). */
            rt_thread_mdelay(180);
            if (bh1750_read(s_bh1750_address, &next.lux_x10) == RT_EOK)
                next.bh1750_valid = RT_TRUE;
        }
        if (!s_sgp30_ready)
        {
            if (sgp30_init() == RT_EOK)
                rt_thread_mdelay(10);
        }
        if (s_sgp30_ready)
        {
            /* Keep measuring during warm-up, but do not publish the
             * sensor's initial fixed values as valid air-quality data. */
            if (sgp30_read(&next.eco2_ppm, &next.tvoc_ppb) != RT_EOK)
                s_sgp30_ready = RT_FALSE;
            else if ((rt_tick_get() - s_sgp30_start) < rt_tick_from_millisecond(15000))
            {
                next.sgp30_warming = RT_TRUE;
                next.eco2_ppm = 0;
                next.tvoc_ppb = 0;
            }
            else
                next.sgp30_valid = RT_TRUE;
        }
        next.tick = rt_tick_get();
        /* 发布快照时持锁；读线程只复制结构体，锁内不执行 I2C。 */
        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        s_snapshot = next;
        rt_mutex_release(&s_lock);
        rt_thread_mdelay(1000);
    }
}

rt_err_t gd32_sensor_snapshot_get(struct gd32_sensor_snapshot *snapshot)
{
    if (snapshot == RT_NULL || s_bus == RT_NULL)
        return -RT_ENOENT;
    /* snapshot 是调用者提供的内存，复制完成后才释放锁。 */
    /* A reader is a UI/alarm-facing API. Do not let a stalled sensor update
     * hold the caller forever and make the whole application appear frozen. */
    if (rt_mutex_take(&s_lock, rt_tick_from_millisecond(20)) != RT_EOK)
        return -RT_EBUSY;
    *snapshot = s_snapshot;
    rt_mutex_release(&s_lock);
    return RT_EOK;
}

static void sensor_data_cmd(int argc, char **argv)
{
    struct gd32_sensor_snapshot data;
    rt_uint32_t temp_abs;
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    if (gd32_sensor_snapshot_get(&data) != RT_EOK)
    {
        rt_kprintf("sensor service unavailable\n");
        return;
    }
    temp_abs = (rt_uint32_t)(data.temperature_x100 < 0 ?
                             -data.temperature_x100 : data.temperature_x100);
    rt_kprintf("SHT40: %s %s%u.%02u C %u.%02u %%\n",
               data.sht40_valid ? "ok" : "offline",
               data.temperature_x100 < 0 ? "-" : "",
               (unsigned)(temp_abs / 100U),
               (int)(temp_abs % 100U),
               data.humidity_x100 / 100, data.humidity_x100 % 100);
    rt_kprintf("BH1750: %s %u.%u lux\n", data.bh1750_valid ? "ok" : "offline",
               (unsigned)(data.lux_x10 / 10), (unsigned)(data.lux_x10 % 10));
    if (data.sgp30_valid)
        rt_kprintf("SGP30: ok eCO2=%u ppm TVOC=%u ppb\n",
                   data.eco2_ppm, data.tvoc_ppb);
    else
        rt_kprintf("SGP30: %s\n", data.sgp30_warming ? "warming" : "offline");
}
MSH_CMD_EXPORT_ALIAS(sensor_data_cmd, sensor_data,
                     read GD32 environmental sensors);

int gd32_sensor_service_init(void)
{
    if (s_bus != RT_NULL)
        return RT_EOK;
    s_bus = (struct rt_i2c_bus_device *)rt_device_find(SENSOR_I2C_BUS);
    if (s_bus == RT_NULL)
    {
        LOG_E("cannot find %s", SENSOR_I2C_BUS);
        return -RT_ENOENT;
    }
    /* 线程栈为静态数组，避免传感器服务反复启停造成堆碎片。 */
    rt_mutex_init(&s_lock, "sens", RT_IPC_FLAG_PRIO);
    rt_thread_init(&s_thread, "sensor", sensor_thread_entry, RT_NULL,
                   s_stack, sizeof(s_stack), 23, 10);
    rt_thread_startup(&s_thread);
    return RT_EOK;
}
