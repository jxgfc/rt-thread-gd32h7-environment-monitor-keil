/*
 * 传感器服务公共接口。
 * 采集线程写入快照，调用者通过 gd32_sensor_snapshot_get() 获得一致副本；
 * 不要直接访问服务内部变量，也不要在业务线程中绕过服务访问 I2C。
 */
#ifndef __GD32_SENSOR_SERVICE_H__
#define __GD32_SENSOR_SERVICE_H__

#include <rtthread.h>

/* Override this at compile time if the modules are wired to another bus. */
#ifndef GD32_SENSOR_I2C_BUS
#define GD32_SENSOR_I2C_BUS "hwi2c1"
#endif

struct gd32_sensor_snapshot
{
    /* 有效标志与数值分开保存：传感器离线时数值可能为上一次/零值。 */
    rt_bool_t sht40_valid;
    rt_bool_t bh1750_valid;
    rt_bool_t sgp30_valid;
    rt_bool_t sgp30_warming; /* Initialization succeeded; readings are not ready yet. */
    rt_int32_t temperature_x100; /* 温度，单位 0.01 ℃，可为负数。 */
    rt_uint16_t humidity_x100;   /* 相对湿度，单位 0.01 %RH。 */
    rt_uint32_t lux_x10;          /* 光照，单位 0.1 lux。 */
    rt_uint16_t eco2_ppm;         /* eCO2，单位 ppm。 */
    rt_uint16_t tvoc_ppb;         /* TVOC，单位 ppb。 */
    rt_tick_t tick;               /* 生成此快照时的系统 tick；0 表示尚未发布。 */
};

int gd32_sensor_service_init(void);
rt_err_t gd32_sensor_snapshot_get(struct gd32_sensor_snapshot *snapshot);

#endif
