/*
 * GD32H759I-EVAL RTC 驱动。
 *
 * 官方 22_RTC_Calendar 例程使用内部 IRC32K 时钟；本工程不依赖外部
 * 32.768 kHz 晶振，因此沿用相同的时钟和分频配置。
 * same clock and prescalers here.  RTC_BKP0 is a persistent initialization
 * marker; a valid 00:00:00 time is therefore never mistaken for "uninitialised".
 */
#include <board.h>
#include <rtdevice.h>
#include <rthw.h>
#include <sys/time.h>
#include "drv_rtc.h"

#define DBG_TAG "drv.rtc"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifdef RT_USING_RTC

#define GD32_RTC_MAGIC          ((rt_uint32_t)0x52544331U) /* "RTC1" */
#define GD32_RTC_SET_MAGIC      ((rt_uint32_t)0x53455431U) /* "SET1", BKP1 */
#define GD32_RTC_PRESCALER_S    ((rt_uint16_t)0x13FU)      /* IRC32K, official demo */
#define GD32_RTC_PRESCALER_A    ((rt_uint16_t)0x63U)
#define GD32_RTC_DEFAULT_TIME   ((time_t)1767225600LL)    /* 2026-01-01 00:00:00 UTC */

static rt_rtc_dev_t s_rtc;
static struct rt_mutex s_rtc_lock;
static rt_bool_t s_rtc_ready;

static rt_uint8_t rtc_bcd_to_bin(rt_uint8_t value)
{
    /* RTC 寄存器使用 BCD，接口层统一转换为二进制数值。 */
    return (rt_uint8_t)((value & 0x0FU) + ((value >> 4) * 10U));
}

static rt_uint8_t rtc_bin_to_bcd(rt_uint8_t value)
{
    return (rt_uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static int rtc_bcd_in_range(rt_uint8_t value, rt_uint8_t maximum)
{
    return ((value & 0x0FU) <= 9U) && ((value >> 4) <= 9U) &&
           rtc_bcd_to_bin(value) <= maximum;
}

static int rtc_date_is_valid(const rtc_parameter_struct *now)
{
    static const rt_uint8_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    rt_uint8_t month;
    rt_uint8_t date;
    rt_uint8_t days;

    /* 先检查 BCD 半字节，再按月份天数检查完整日期。 */
    if (!rtc_bcd_in_range(now->year, 99U) ||
        !rtc_bcd_in_range(now->month, 12U) ||
        !rtc_bcd_in_range(now->date, 31U))
        return 0;

    month = rtc_bcd_to_bin(now->month);
    date = rtc_bcd_to_bin(now->date);
    if (month == 0U || date == 0U)
        return 0;
    days = month_days[month - 1U];
    /* Hardware represents 2000..2099: divisibility by four is sufficient. */
    if (month == 2U && (rtc_bcd_to_bin(now->year) % 4U) == 0U)
        days++;

    /* Do not reject midnight: all six fields are independently validated. */
    return date <= days && now->display_format == RTC_24HOUR &&
           rtc_bcd_in_range(now->hour, 23U) &&
           rtc_bcd_in_range(now->minute, 59U) &&
           rtc_bcd_in_range(now->second, 59U);
}

/* TIME followed by DATE reads one locked shadow-register snapshot.  All
 * callers hold s_rtc_lock; never validate one read and convert a later one. */
static int rtc_read_calendar(rtc_parameter_struct *now)
{
    if (rtc_register_sync_wait() != SUCCESS)
        return 0;
    rtc_current_time_get(now);
    return rtc_date_is_valid(now);
}

static rt_err_t gd32_rtc_set_timestamp(time_t timestamp)
{
    struct tm current = {0};
    rtc_parameter_struct init = {0};

    /* 日志时间戳按 UTC 保存；RTC 写入 gmtime，界面显示时再 localtime。 */
    if (gmtime_r(&timestamp, &current) == RT_NULL ||
        current.tm_year < 100 || current.tm_year > 199)
        return -RT_EINVAL;

    init.year = rtc_bin_to_bcd((rt_uint8_t)(current.tm_year - 100));
    init.month = rtc_bin_to_bcd((rt_uint8_t)(current.tm_mon + 1));
    init.date = rtc_bin_to_bcd((rt_uint8_t)current.tm_mday);
    init.day_of_week = rtc_bin_to_bcd((rt_uint8_t)(current.tm_wday == 0 ? 7 : current.tm_wday));
    init.hour = rtc_bin_to_bcd((rt_uint8_t)current.tm_hour);
    init.minute = rtc_bin_to_bcd((rt_uint8_t)current.tm_min);
    init.second = rtc_bin_to_bcd((rt_uint8_t)current.tm_sec);
    init.factor_asyn = GD32_RTC_PRESCALER_A;
    init.factor_syn = GD32_RTC_PRESCALER_S;
    init.am_pm = RTC_AM;
    init.display_format = RTC_24HOUR;

    if (s_rtc_ready)
        rt_kprintf("TIME RTC: calendar write begin\n");
    if (rtc_init(&init) != SUCCESS)
    {
        rt_kprintf("TIME RTC: calendar write failed, STAT=%08lx CTL=%08lx\n",
                   (unsigned long)RTC_STAT, (unsigned long)RTC_CTL);
        return -RT_ERROR;
    }
    if (s_rtc_ready)
        rt_kprintf("TIME RTC: calendar written, sync begin\n");
    if (rtc_register_sync_wait() != SUCCESS)
    {
        rt_kprintf("TIME RTC: sync failed, STAT=%08lx CTL=%08lx\n",
                   (unsigned long)RTC_STAT, (unsigned long)RTC_CTL);
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_err_t gd32_rtc_get_secs(time_t *timestamp)
{
    rtc_parameter_struct now;
    struct tm current = {0};
    rt_err_t result = -RT_ERROR;

    if (timestamp == RT_NULL || !s_rtc_ready)
        return -RT_EINVAL;

    /* TIME/DATE 必须从同一次同步快照读取，避免跨秒拼出非法日期。 */
    rt_mutex_take(&s_rtc_lock, RT_WAITING_FOREVER);
    if (RTC_BKP0 == GD32_RTC_MAGIC && rtc_read_calendar(&now))
    {
        current.tm_year = rtc_bcd_to_bin(now.year) + 100;
        current.tm_mon = rtc_bcd_to_bin(now.month) - 1;
        current.tm_mday = rtc_bcd_to_bin(now.date);
        current.tm_hour = rtc_bcd_to_bin(now.hour);
        current.tm_min = rtc_bcd_to_bin(now.minute);
        current.tm_sec = rtc_bcd_to_bin(now.second);
        current.tm_isdst = 0;
        *timestamp = timegm(&current);
        result = RT_EOK;
    }
    rt_mutex_release(&s_rtc_lock);
    return result;
}

static rt_err_t gd32_rtc_set_secs(time_t *timestamp)
{
    rt_err_t result;
    if (timestamp == RT_NULL || !s_rtc_ready)
        return -RT_EINVAL;

    rt_kprintf("TIME RTC: lock begin\n");
    /* 写 RTC 与更新备份域标志使用同一把锁，避免读到半成品。 */
    rt_mutex_take(&s_rtc_lock, RT_WAITING_FOREVER);
    rt_kprintf("TIME RTC: lock acquired\n");
    result = gd32_rtc_set_timestamp(*timestamp);
    if (result == RT_EOK)
    {
        RTC_BKP0 = GD32_RTC_MAGIC;
        RTC_BKP1 = GD32_RTC_SET_MAGIC;
    }
    else
        RTC_BKP1 = 0U;
    rt_mutex_release(&s_rtc_lock);
    rt_kprintf("TIME RTC: set complete, result=%d\n", result);
    return result;
}

rt_bool_t gd32_rtc_time_is_set(void)
{
    rtc_parameter_struct now;
    rt_bool_t result;
    if (!s_rtc_ready)
        return RT_FALSE;
    rt_mutex_take(&s_rtc_lock, RT_WAITING_FOREVER);
    result = RTC_BKP0 == GD32_RTC_MAGIC && RTC_BKP1 == GD32_RTC_SET_MAGIC &&
             rtc_read_calendar(&now);
    rt_mutex_release(&s_rtc_lock);
    return result;
}

static rt_err_t gd32_rtc_init(void)
{
    rt_bool_t needs_setup;
    rtc_parameter_struct now;

    /* 设备框架可能重复调用 init；硬件初始化只允许成功执行一次。 */
    if (s_rtc_ready)
        return RT_EOK;

    rcu_periph_clock_enable(RCU_PMU);
    pmu_backup_write_enable();

    /* Match Projects/22_RTC_Calendar: use the internal 32 kHz oscillator. */
    rcu_osci_on(RCU_IRC32K);
    if (rcu_osci_stab_wait(RCU_IRC32K) != SUCCESS)
    {
        LOG_E("IRC32K oscillator failed");
        return -RT_ERROR;
    }

    /* The RTC APB clock must be enabled before reading status/backup data. */
    rcu_periph_clock_enable(RCU_RTC);

    needs_setup = (RTC_BKP0 != GD32_RTC_MAGIC) ||
                  ((RCU_BDCTL & RCU_BDCTL_RTCSRC) != RCU_RTCSRC_IRC32K) ||
                  ((RTC_CTL & RTC_CTL_BPSHAD) != 0U) ||
                  !rtc_read_calendar(&now);

    if (needs_setup)
    {
        /* Reset only when the persistent marker/source is invalid. */
        rcu_bkp_reset_enable();
        rcu_bkp_reset_disable();
        rcu_rtc_clock_config(RCU_RTCSRC_IRC32K);
        /* Backup reset clears RTCEN together with the clock source. */
        rcu_periph_clock_enable(RCU_RTC);
    }

    if (rtc_register_sync_wait() != SUCCESS)
        return -RT_ERROR;

    if (needs_setup)
    {
        if (gd32_rtc_set_timestamp(GD32_RTC_DEFAULT_TIME) != RT_EOK)
            return -RT_ERROR;
        RTC_BKP0 = GD32_RTC_MAGIC;
        RTC_BKP1 = 0U;
        LOG_I("RTC default 2026-01-01; set date before recording timed logs");
    }

    s_rtc_ready = RT_TRUE;
    return RT_EOK;
}

static const struct rt_rtc_ops s_rtc_ops =
{
    .init = gd32_rtc_init,
    .get_secs = gd32_rtc_get_secs,
    .set_secs = gd32_rtc_set_secs,
};

static int rt_hw_rtc_init(void)
{
    if (rt_mutex_init(&s_rtc_lock, "rtc", RT_IPC_FLAG_PRIO) != RT_EOK)
        return -RT_ERROR;
    /* RT-Thread 5.x no longer calls every registered device's init callback
     * automatically.  Initialise the backup domain before registering the
     * device so date/time and FatFs timestamps work immediately. */
    if (gd32_rtc_init() != RT_EOK)
    {
        LOG_E("RTC hardware initialization failed");
        rt_mutex_detach(&s_rtc_lock);
        return -RT_ERROR;
    }
    s_rtc.ops = &s_rtc_ops;
    return rt_hw_rtc_register(&s_rtc, "rtc", RT_DEVICE_FLAG_RDWR, RT_NULL);
}
INIT_DEVICE_EXPORT(rt_hw_rtc_init);

#endif /* RT_USING_RTC */
