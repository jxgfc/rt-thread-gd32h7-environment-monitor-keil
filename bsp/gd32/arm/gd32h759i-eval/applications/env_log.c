/* SPDX-License-Identifier: Apache-2.0 */
/* SD access is confined to envlog. UI calls only exchange bounded RAM copies. */
#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include <dfs.h>
#include <dfs_fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include "env_log.h"
#include "alarm.h"
#include "sensor_service.h"
#include "../board/drv_sdcard.h"
#include "../board/drv_rtc.h"

#ifdef BSP_USING_ENV_LOG

/*
 * envlog 是本工程唯一允许访问 SD/FAT 的线程服务。LVGL、报警线程和 shell
 * 只能通过事件、快照或请求函数与它交互，避免多个线程同时操作文件系统。
 * 其中 EVENT_TIME 的优先级最高：校时周期不进入 SD 驱动，防止大容量 FAT
 * 扫描导致“点击保存后屏幕和串口都像卡死”的现象。
 */
#define ENV_LOG_PATH       "/envlog.csv"
#define ENV_DEFAULT_INTERVAL 600U /* 默认每 10 分钟写一条 CSV。 */
#define ENV_RETRY         (60U * RT_TICK_PER_SECOND) /* SD 未就绪时的重试周期。 */
#define ENV_MOUNT_RETRY_MAX (15U * 60U * RT_TICK_PER_SECOND) /* 挂载失败时最多 15 分钟重试一次。 */
#define ENV_FRESH         (5U * RT_TICK_PER_SECOND) /* 传感器快照超过此值视为过期。 */
#define EVENT_SAVE        1U  /* 手动/定时保存一条记录。 */
#define EVENT_HISTORY     2U  /* 读取历史页。 */
#define EVENT_CLEAR       4U  /* 清空本设备日志。 */
#define EVENT_SETTINGS    8U  /* 保存阈值和间隔配置。 */
#define EVENT_TIME        16U /* 写入 RTC，必须优先于 SD 操作。 */
#define EVENT_INTERVAL    32U /* 重新计算定时保存的截止 tick。 */
#define EVENT_TREND_INTERVAL 64U /* 重新计算曲线采样截止 tick。 */
#define EVENT_ALL         127U
#define CONFIG_MAGIC      0x454E5631UL
#define CONFIG_V1_WORDS   10
#define CONFIG_V2_WORDS   11
#define CONFIG_V3_WORDS   13
#define CONFIG_WORDS      14
#define CONFIG_VERSION    4U
#define ENV_DEFAULT_LUX_LOW_X10  0U
#define ENV_DEFAULT_LUX_HIGH_X10 1000000U /* 100,000 lux */
#define ENV_ARCHIVE_LINES_PER_SLICE 64U

/* CSV 的列顺序是上位机解析器和固件共同的稳定协议，新增字段需同步两端。 */
static const char s_header[] =
    "timestamp,temperature_x100,humidity_x100,lux_x10,eco2_ppm,tvoc_ppb,valid_flags\n";
static const char *s_config_paths[2] = {"/ecfg0.bin", "/ecfg1.bin"};
static struct rt_thread s_log_thread;
rt_align(RT_ALIGN_SIZE)
static rt_uint8_t s_log_stack[4096];
static struct rt_mutex s_lock;
static struct rt_event s_events;
static struct gd32_env_log_status s_status;
static struct gd32_env_history s_history;
/* 144 个桶保存最近的曲线；每个桶的时长跟随 trend_interval_seconds。 */
static struct gd32_env_record s_bins[GD32_ENV_TREND_POINTS];
static struct gd32_env_thresholds s_thresholds = {
    1000, 3500, 2000, 8000, 1000, 500,
    ENV_DEFAULT_LUX_LOW_X10, ENV_DEFAULT_LUX_HIGH_X10
};
static rt_uint32_t s_settings_revision;
static rt_uint32_t s_history_request_id;
static rt_uint32_t s_requested_page;
static rt_uint32_t s_config_sequence;
static volatile rt_uint32_t s_requested_time;
static volatile rt_uint32_t s_time_request_id;
static int s_config_slot = -1;
static rt_bool_t s_started;
static rt_bool_t s_mounted;
static rt_bool_t s_need_unmount;
/* SD 卡尚未挂载时，用户在 HMI 修改的值优先于旧配置文件。 */
static rt_bool_t s_user_settings;
/* 配置文件延迟加载期间按字段合并，避免一次 HMI 修改覆盖其它旧设置。 */
static rt_bool_t s_user_thresholds;
static rt_bool_t s_user_log_interval;
static rt_bool_t s_user_trend_interval;
/* Archive recovery is intentionally lazy.  Scanning a large CSV while the
 * card is mounted can monopolize the FAT driver during boot. */
static rt_bool_t s_archive_recovery_pending;
static rt_bool_t s_config_load_pending;
/* Archive recovery is resumable.  A large CSV must not hold the SD/FAT
 * driver in one worker slice while the user is opening the history page. */
static rt_bool_t s_archive_active;
static rt_uint32_t s_archive_total;
static rt_uint32_t s_archive_now;
static struct gd32_env_record s_archive_recent[GD32_ENV_HISTORY_ROWS];
static rt_uint8_t s_archive_recent_count;
static rt_bool_t s_archive_recent_valid;
/* The final CSV line is repaired once after mounting.  Appending a normal
 * newline terminated row does not require rescanning the tail on every save. */
static rt_bool_t s_csv_tail_checked;

/* The single reader belongs to the worker, never shared with an LVGL callback. */
struct csv_reader
{
    int fd;                    /* 只由 envlog 线程持有。 */
    rt_size_t position;        /* 当前缓冲区读取位置。 */
    rt_size_t length;          /* 缓冲区有效字节数。 */
    char buffer[512];          /* 分块读取，避免一次性申请整个历史文件。 */
};
static struct csv_reader s_reader;
static const char *s_diag_phase = "not-started";
static rt_tick_t s_diag_phase_tick;
static rt_uint32_t s_diag_loops;
/* Worker-owned: trace the first valid calendar/clock change, including date
 * commands which do not send EVENT_TIME. Never print inside a snapshot lock. */
static rt_bool_t s_diag_trace_cycle;

static void log_stage(const char *phase)
{
    /* 阶段字符串用于现场诊断；更新采用短临界区，绝不在中断关闭期间打印。 */
    rt_base_t level = rt_hw_interrupt_disable();
    s_diag_phase = phase;
    s_diag_phase_tick = rt_tick_get();
    rt_hw_interrupt_enable(level);
    if (s_diag_trace_cycle)
        rt_kprintf("TIME SD: %s\n", phase);
}

rt_uint32_t gd32_env_log_diag_request_id(void)
{
    return s_time_request_id;
}

void gd32_env_log_diag_print(void)
{
    const char *phase;
    rt_tick_t age;
    rt_uint32_t loops, events, request_id;
    rt_bool_t pending, busy;
    rt_base_t level, critical;
    rt_ubase_t owner;
    int state;

    if (!s_started) return;
    /* Do not take elog/RTC/SD/heap locks: this must work while the worker
     * is blocked on one of them. Never dereference a possibly damaged owner. */
    level = rt_hw_interrupt_disable();
    phase = s_diag_phase;
    age = rt_tick_get() - s_diag_phase_tick;
    loops = s_diag_loops;
    events = s_events.set;
    request_id = s_time_request_id;
    pending = s_status.time_pending;
    busy = s_status.busy;
    state = s_status.state;
    critical = rt_critical_level();
    owner = (rt_ubase_t)s_lock.owner;
    rt_hw_interrupt_enable(level);
    rt_kprintf("ELOG: phase=%s age=%lu loops=%lu events=%02lx\n",
               phase, (unsigned long)age, (unsigned long)loops, (unsigned long)events);
    rt_kprintf("ELOG: request=%lu pending=%d state=%d busy=%d owner=%08lx irq=%ld critical=%ld\n",
               (unsigned long)request_id, pending, state, busy,
               (unsigned long)owner, (long)level, (long)critical);
}

static void status_state(enum gd32_env_log_state state)
{
    /* 状态只描述存储服务本身，RTC 或传感器故障不会把 SD 标成 IO_ERROR。 */
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_status.state = state;
    s_status.revision++;
    rt_mutex_release(&s_lock);
}

static void action_begin(void)
{
    /* busy 仅供 HMI 显示“正在处理”，不能作为线程同步手段。 */
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_status.busy = RT_TRUE;
    rt_mutex_release(&s_lock);
}

static void action_end(rt_err_t result)
{
    /* 所有 SD 动作必须成对调用 begin/end，保证失败时也能清除 busy。 */
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_status.busy = RT_FALSE;
    s_status.last_action_result = result;
    s_status.revision++;
    if (result == -RT_EIO)
        s_status.errors++;
    rt_mutex_release(&s_lock);
    if (result == -RT_EIO)
    {
        status_state(GD32_ENV_LOG_IO_ERROR);
        s_need_unmount = s_mounted;
        s_mounted = RT_FALSE;
    }
}

/* Complete a history request even when the archive cannot be opened or read.
 * action_end() only updates the global logging status; leaving
 * s_history.busy set here would make the HMI show "processing" forever. */
static void history_request_finish(rt_err_t result)
{
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_history.busy = RT_FALSE;
    s_history.result = result;
    s_history.revision++;
    rt_mutex_release(&s_lock);
}

static rt_err_t card_sync(void)
{
    /* 写文件后同步块设备，尽量缩短掉电丢失窗口；仍不能替代可靠供电。 */
    rt_device_t device = rt_device_find("sd0");
    if (device == RT_NULL)
        return -RT_EIO;
    return rt_device_control(device, RT_DEVICE_CTRL_BLK_SYNC, RT_NULL);
}

static rt_bool_t thresholds_valid(const struct gd32_env_thresholds *t)
{
    /* 限制输入范围既保护传感器物理范围，也防止 UI 数值换算溢出。 */
    return t != RT_NULL &&
        t->temperature_low_x100 >= -4000 && t->temperature_high_x100 <= 12500 &&
        t->temperature_low_x100 < t->temperature_high_x100 &&
        t->humidity_low_x100 < t->humidity_high_x100 && t->humidity_high_x100 <= 10000 &&
        t->eco2_high_ppm >= 400 && t->eco2_high_ppm <= 60000 &&
        t->tvoc_high_ppb >= 1 && t->tvoc_high_ppb <= 60000 &&
        t->lux_low_x10 < t->lux_high_x10 &&
        t->lux_high_x10 <= 2000000U;
}

static rt_uint32_t words_checksum(const rt_uint32_t *words, int count)
{
    /* 配置协议使用按 32 位 word 累积的 FNV-1a 变体，兼容已有 v1/v2 文件。 */
    rt_uint32_t result = 2166136261UL;
    int index;
    for (index = 0; index < count - 1; index++)
        result = (result ^ words[index]) * 16777619UL;
    return result;
}

static void words_to_thresholds(const rt_uint32_t *w, struct gd32_env_thresholds *t)
{
    t->temperature_low_x100 = (rt_int32_t)w[3];
    t->temperature_high_x100 = (rt_int32_t)w[4];
    t->humidity_low_x100 = (rt_uint16_t)w[5];
    t->humidity_high_x100 = (rt_uint16_t)w[6];
    t->eco2_high_ppm = (rt_uint16_t)w[7];
    t->tvoc_high_ppb = (rt_uint16_t)w[8];
    t->lux_low_x10 = w[9];
    t->lux_high_x10 = w[10];
}

static rt_err_t config_read(int slot, rt_uint32_t *words, rt_bool_t *upgrade)
{
    /* 双槽读取只接受完整长度、版本和校验均正确的副本。 */
    int fd, length, extra;
    rt_uint32_t version, interval;
    char tail;
    struct gd32_env_thresholds t;
    if (upgrade != RT_NULL) *upgrade = RT_FALSE;
    /* read() 只覆盖文件实际长度；先清零，避免旧版本迁移时使用未初始化字。 */
    rt_memset(words, 0, CONFIG_WORDS * sizeof(*words));
    log_stage("config-open-read");
    fd = open(s_config_paths[slot], O_RDONLY, 0);
    if (fd < 0)
        /* DFS v1 stores negative errors in the RT-Thread errno slot. */
        return rt_get_errno() == -ENOENT ? -RT_ENOENT : -RT_EIO;
    log_stage("config-read");
    length = read(fd, words, CONFIG_WORDS * sizeof(rt_uint32_t));
    extra = read(fd, &tail, 1);
    if (close(fd) != 0 || length < 0 || extra < 0)
        return -RT_EIO;
    if (extra != 0 ||
        (length != CONFIG_V1_WORDS * sizeof(rt_uint32_t) &&
         length != CONFIG_V2_WORDS * sizeof(rt_uint32_t) &&
         length != CONFIG_V3_WORDS * sizeof(rt_uint32_t) &&
         length != CONFIG_WORDS * sizeof(rt_uint32_t)) ||
        words[0] != CONFIG_MAGIC)
        return -RT_EINVAL;
    version = words[1];
    if (length == CONFIG_V1_WORDS * sizeof(rt_uint32_t))
    {
        /* Version 1 stored only thresholds. Normalize it to the current format. */
        if (version != 1 || words[9] != words_checksum(words, CONFIG_V1_WORDS))
            return -RT_EINVAL;
        words[9] = ENV_DEFAULT_LUX_LOW_X10;
        words[10] = ENV_DEFAULT_LUX_HIGH_X10;
        words[11] = ENV_DEFAULT_INTERVAL;
        words[12] = ENV_DEFAULT_INTERVAL;
        words[1] = CONFIG_VERSION;
        words[13] = words_checksum(words, CONFIG_WORDS);
        if (upgrade != RT_NULL) *upgrade = RT_TRUE;
    }
    else if (length == CONFIG_V2_WORDS * sizeof(rt_uint32_t))
    {
        /* Version 2 added the interval at word 9.  Keep that value while
         * inserting the two new light limits in words 9 and 10. */
        if (version != 2 || words[10] != words_checksum(words, CONFIG_V2_WORDS))
            return -RT_EINVAL;
        interval = words[9];
        words[9] = ENV_DEFAULT_LUX_LOW_X10;
        words[10] = ENV_DEFAULT_LUX_HIGH_X10;
        words[11] = interval;
        words[12] = ENV_DEFAULT_INTERVAL;
        words[1] = CONFIG_VERSION;
        words[13] = words_checksum(words, CONFIG_WORDS);
        if (upgrade != RT_NULL) *upgrade = RT_TRUE;
    }
    else if (length == CONFIG_V3_WORDS * sizeof(rt_uint32_t))
    {
        /* Version 3 already has light limits and the SD period; add the
         * independent trend period while preserving all existing values. */
        if (version != 3 || words[12] != words_checksum(words, CONFIG_V3_WORDS))
            return -RT_EINVAL;
        words[12] = ENV_DEFAULT_INTERVAL;
        words[1] = CONFIG_VERSION;
        words[13] = words_checksum(words, CONFIG_WORDS);
        if (upgrade != RT_NULL) *upgrade = RT_TRUE;
    }
    else if (version != CONFIG_VERSION || words[13] != words_checksum(words, CONFIG_WORDS))
        return -RT_EINVAL;
    if (words[11] < 1U || words[11] > 86400U ||
        words[12] < 1U || words[12] > 86400U ||
        words[5] > 10000 || words[6] > 10000 || words[7] > 60000 ||
        words[8] > 60000 || words[9] > 2000000U || words[10] > 2000000U)
        return -RT_EINVAL;
    words_to_thresholds(words, &t);
    return thresholds_valid(&t) ? RT_EOK : -RT_EINVAL;
}

static rt_err_t config_load(void)
{
    /* 选择序号较新的有效槽；序号用有符号差值比较以支持回绕。 */
    rt_uint32_t words[2][CONFIG_WORDS];
    rt_err_t result[2];
    rt_bool_t upgrade[2];
    int selected = -1;
    result[0] = config_read(0, words[0], &upgrade[0]);
    result[1] = config_read(1, words[1], &upgrade[1]);
    if (result[0] == -RT_EIO || result[1] == -RT_EIO)
        return -RT_EIO;
    if (result[0] == RT_EOK)
        selected = 0;
    if (result[1] == RT_EOK && (selected < 0 ||
        (rt_int32_t)(words[1][2] - words[0][2]) > 0))
        selected = 1;
    s_config_slot = selected;
    s_config_sequence = selected < 0 ? 0 : words[selected][2];
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    /* 配置加载可能晚于 UI 启动；只覆盖尚未被用户改过的字段。 */
    if (selected >= 0)
    {
        if (!s_user_thresholds)
            words_to_thresholds(words[selected], &s_thresholds);
        if (!s_user_log_interval)
            s_status.log_interval_seconds = words[selected][11];
        if (!s_user_trend_interval &&
            s_status.trend_interval_seconds != words[selected][12])
            rt_memset(s_bins, 0, sizeof(s_bins));
        if (!s_user_trend_interval)
            s_status.trend_interval_seconds = words[selected][12];
        s_settings_revision++;
        s_status.settings_pending = upgrade[selected] || s_user_settings;
    }
    else
        s_status.settings_pending = RT_TRUE;
    s_status.revision++;
    rt_mutex_release(&s_lock);
    return RT_EOK;
}

static rt_err_t config_save(void)
{
    /* 写入另一槽并读回校验，保留上一份有效配置，降低断电损坏风险。 */
    struct gd32_env_thresholds t;
    rt_uint32_t words[CONFIG_WORDS], verify[CONFIG_WORDS], revision;
    rt_uint32_t interval, trend_interval;
    int fd, slot = s_config_slot == 0 ? 1 : 0;
    rt_err_t result = RT_EOK;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    t = s_thresholds;
    revision = s_settings_revision;
    interval = s_status.log_interval_seconds;
    trend_interval = s_status.trend_interval_seconds;
    rt_mutex_release(&s_lock);
    words[0] = CONFIG_MAGIC;
    words[1] = CONFIG_VERSION;
    words[2] = s_config_sequence + 1;
    words[3] = (rt_uint32_t)t.temperature_low_x100;
    words[4] = (rt_uint32_t)t.temperature_high_x100;
    words[5] = t.humidity_low_x100;
    words[6] = t.humidity_high_x100;
    words[7] = t.eco2_high_ppm;
    words[8] = t.tvoc_high_ppb;
    words[9] = t.lux_low_x10;
    words[10] = t.lux_high_x10;
    words[11] = interval;
    words[12] = trend_interval;
    words[13] = words_checksum(words, CONFIG_WORDS);
    /* Alternate checksum-protected slots: never truncate the latest good copy.
     * FAT rename cannot replace an existing destination atomically. */
    log_stage("config-open-write");
    fd = open(s_config_paths[slot], O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
        return -RT_EIO;
    log_stage("config-write-sync");
    if (write(fd, words, sizeof(words)) != sizeof(words) || fsync(fd) != 0)
        result = -RT_EIO;
    log_stage("config-close-sync");
    if (close(fd) != 0 || card_sync() != RT_EOK)
        result = -RT_EIO;
    if (result == RT_EOK && (config_read(slot, verify, RT_NULL) != RT_EOK ||
        rt_memcmp(words, verify, sizeof(words)) != 0))
        result = -RT_EIO;
    if (result == RT_EOK)
    {
        s_config_slot = slot;
        s_config_sequence = words[2];
        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        if (revision == s_settings_revision)
        {
            s_status.settings_pending = RT_FALSE;
            s_user_settings = RT_FALSE;
            s_user_thresholds = RT_FALSE;
            s_user_log_interval = RT_FALSE;
            s_user_trend_interval = RT_FALSE;
        }
        s_status.revision++;
        rt_mutex_release(&s_lock);
    }
    return result;
}

static rt_bool_t rtc_now(rt_uint32_t *seconds)
{
    /* RTC 驱动和时间有效标志都通过后才允许生成日志时间戳。 */
    time_t now = 0;
    rt_device_t rtc = rt_device_find("rtc");
    if (!gd32_rtc_time_is_set() || rtc == RT_NULL ||
        rt_device_control(rtc, RT_DEVICE_CTRL_RTC_GET_TIME, &now) != RT_EOK ||
        now < (time_t)946684800 || (rt_uint64_t)now > 4102444799ULL)
        return RT_FALSE;
    *seconds = (rt_uint32_t)now;
    return RT_TRUE;
}

static rt_err_t rtc_set_requested(void)
{
    /* 校时请求由 UI 投递、envlog 执行；因此 RTC 写入不会在 LVGL 回调里阻塞。 */
    rt_device_t rtc;
    rt_uint32_t request_id;
    time_t timestamp;
    rt_err_t result;
    rt_base_t level;
    log_stage("rtc-device-find");
    rtc = rt_device_find("rtc");
    /* The UI request path is deliberately independent from the environment
     * mutex.  A pending SD/FAT operation must never prevent the worker from
     * receiving a clock update.  These two words are single-core atomics. */
    log_stage("rtc-request-snapshot");
    level = rt_hw_interrupt_disable();
    timestamp = (time_t)s_requested_time;
    request_id = s_time_request_id;
    rt_hw_interrupt_enable(level);
    log_stage("rtc-set");
    rt_kprintf("TIME worker: RTC set begin, timestamp=%lu\n", (unsigned long)timestamp);
    result = rtc == RT_NULL ? -RT_ENOENT :
             rt_device_control(rtc, RT_DEVICE_CTRL_RTC_SET_TIME, &timestamp);
    rt_kprintf("TIME worker: RTC set returned, result=%d\n", result);
    log_stage("rtc-result-lock");
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (request_id == s_time_request_id)
    {
        s_status.time_pending = RT_FALSE;
        s_status.time_result = result;
        s_status.time_revision++;
        s_status.revision++;
    }
    rt_mutex_release(&s_lock);
    return result;
}

static void record_snapshot(struct gd32_env_record *record, rt_uint32_t timestamp)
{
    /* 先清零再填充，保证失效传感器对应的 CSV 列为空而不是残留旧值。 */
    struct gd32_sensor_snapshot data;
    rt_memset(record, 0, sizeof(*record));
    record->timestamp = timestamp;
    if (gd32_sensor_snapshot_get(&data) != RT_EOK ||
        (rt_tick_t)(rt_tick_get() - data.tick) > ENV_FRESH)
        return;
    record->temperature_x100 = data.temperature_x100;
    record->humidity_x100 = data.humidity_x100;
    record->lux_x10 = data.lux_x10;
    record->eco2_ppm = data.eco2_ppm;
    record->tvoc_ppb = data.tvoc_ppb;
    record->valid_flags = (data.sht40_valid ? GD32_ENV_VALID_SHT40 : 0) |
                         (data.bh1750_valid ? GD32_ENV_VALID_LIGHT : 0) |
                         (data.sgp30_valid && !data.sgp30_warming ? GD32_ENV_VALID_GAS : 0);
}

static rt_uint32_t alarm_flags(const struct gd32_env_record *r,
                              const struct gd32_env_thresholds *t)
{
    /* 只有 valid_flags 标记有效的测量值才参与越界判断，避免传感器离线误报警。 */
    rt_uint32_t flags = 0;
    if (r->valid_flags & GD32_ENV_VALID_SHT40)
    {
        if (r->temperature_x100 < t->temperature_low_x100 ||
            r->temperature_x100 > t->temperature_high_x100)
            flags |= GD32_ENV_ALARM_TEMP;
        if (r->humidity_x100 < t->humidity_low_x100 || r->humidity_x100 > t->humidity_high_x100)
            flags |= GD32_ENV_ALARM_HUM;
    }
    if (r->valid_flags & GD32_ENV_VALID_GAS)
    {
        if (r->eco2_ppm > t->eco2_high_ppm)
            flags |= GD32_ENV_ALARM_ECO2;
        if (r->tvoc_ppb > t->tvoc_high_ppb)
            flags |= GD32_ENV_ALARM_TVOC;
    }
    if ((r->valid_flags & GD32_ENV_VALID_LIGHT) &&
        (r->lux_x10 < t->lux_low_x10 || r->lux_x10 > t->lux_high_x10))
        flags |= GD32_ENV_ALARM_LIGHT;
    return flags;
}

/* Re-evaluate the alarm state immediately after a threshold change.  The
 * logger worker normally refreshes this once per second, but it may be inside
 * an SD/FAT operation when the settings page is saved.  Keeping the update
 * here bounded to one sensor snapshot makes the alarm output responsive and
 * avoids waiting for the next storage cycle. */
static void alarm_refresh_from_sensor(void)
{
    /* 阈值保存后立即刷新一次，报警线程无需等待下一次 SD 采样周期。 */
    struct gd32_sensor_snapshot data;
    struct gd32_env_record record;
    struct gd32_env_thresholds thresholds;

    /* This is called from LVGL/shell contexts, so a stalled sensor worker
     * must not make a threshold save wait forever.  A failed or stale sample
     * leaves the last alarm decision unchanged until envlog's next cycle. */
    if (gd32_sensor_snapshot_get(&data) != RT_EOK ||
        (rt_tick_t)(rt_tick_get() - data.tick) > ENV_FRESH)
        return;
    rt_memset(&record, 0, sizeof(record));
    record.temperature_x100 = data.temperature_x100;
    record.humidity_x100 = data.humidity_x100;
    record.lux_x10 = data.lux_x10;
    record.eco2_ppm = data.eco2_ppm;
    record.tvoc_ppb = data.tvoc_ppb;
    record.valid_flags = (data.sht40_valid ? GD32_ENV_VALID_SHT40 : 0) |
                         (data.bh1750_valid ? GD32_ENV_VALID_LIGHT : 0) |
                         (data.sgp30_valid && !data.sgp30_warming ? GD32_ENV_VALID_GAS : 0);
    if (rt_mutex_take(&s_lock, rt_tick_from_millisecond(20)) != RT_EOK)
        return;
    thresholds = s_thresholds;
    s_status.alarm_flags = alarm_flags(&record, &thresholds);
    s_status.revision++;
    rt_mutex_release(&s_lock);
}

rt_err_t gd32_env_log_alarm_refresh(void)
{
    if (!s_started)
        return -RT_ENOENT;
    alarm_refresh_from_sensor();
    return RT_EOK;
}

static rt_uint32_t trend_period_seconds(rt_uint32_t interval)
{
    /* 配置异常时回到默认值，避免除零或时间桶溢出。 */
    return (interval >= 1U && interval <= 86400U) ? interval : ENV_DEFAULT_INTERVAL;
}

static void trend_add(const struct gd32_env_record *record, rt_uint32_t now,
                      rt_uint32_t period)
{
    /* 环形数组按 timestamp 覆盖旧桶；窗口长度由当前曲线周期决定。 */
    rt_uint32_t bin, current_bin;
    period = trend_period_seconds(period);
    if (!record->timestamp || record->timestamp > now)
        return;
    bin = record->timestamp / period;
    current_bin = now / period;
    if (current_bin - bin >= GD32_ENV_TREND_POINTS)
        return;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    /* UI 可能在采样/扫描期间改周期：拒绝旧周期结果，不把旧桶写进新曲线。 */
    if (period == s_status.trend_interval_seconds &&
        record->timestamp >= s_bins[bin % GD32_ENV_TREND_POINTS].timestamp)
    {
        s_bins[bin % GD32_ENV_TREND_POINTS] = *record;
        s_status.revision++;
    }
    rt_mutex_release(&s_lock);
}

static rt_bool_t parse_uint(const char *text, rt_uint32_t *value, int base)
{
    rt_uint32_t result = 0;
    unsigned digit;
    if (*text == '\0')
        return RT_FALSE;
    while (*text)
    {
        if (*text >= '0' && *text <= '9')
            digit = (unsigned)(*text - '0');
        else if (base == 16 && *text >= 'a' && *text <= 'f')
            digit = (unsigned)(*text - 'a') + 10;
        else if (base == 16 && *text >= 'A' && *text <= 'F')
            digit = (unsigned)(*text - 'A') + 10;
        else
            return RT_FALSE;
        if (digit >= (unsigned)base || result > (0xffffffffUL - digit) / (unsigned)base)
            return RT_FALSE;
        result = result * (unsigned)base + digit;
        text++;
    }
    *value = result;
    return RT_TRUE;
}

static rt_bool_t parse_timestamp(char *text, rt_uint32_t *timestamp)
{
    /* CSV 使用 YYYY-MM-DD HH:MM:SS；timegm 以 UTC 解释，避免时区重复换算。 */
    struct tm calendar, normalized;
    rt_uint32_t parts[6];
    const int offsets[6] = {0, 5, 8, 11, 14, 17};
    time_t value;
    int index;
    if (rt_strlen(text) != 19 || text[4] != '-' || text[7] != '-' ||
        text[10] != ' ' || text[13] != ':' || text[16] != ':')
        return RT_FALSE;
    text[4] = text[7] = text[10] = text[13] = text[16] = '\0';
    for (index = 0; index < 6; index++)
        if (!parse_uint(text + offsets[index], &parts[index], 10))
            return RT_FALSE;
    if (parts[0] < 2000 || parts[0] > 2099 || parts[1] < 1 || parts[1] > 12 ||
        parts[2] < 1 || parts[2] > 31 || parts[3] > 23 || parts[4] > 59 || parts[5] > 59)
        return RT_FALSE;
    rt_memset(&calendar, 0, sizeof(calendar));
    calendar.tm_year = parts[0] - 1900;
    calendar.tm_mon = parts[1] - 1;
    calendar.tm_mday = parts[2];
    calendar.tm_hour = parts[3];
    calendar.tm_min = parts[4];
    calendar.tm_sec = parts[5];
    value = timegm(&calendar);
    if (gmtime_r(&value, &normalized) == RT_NULL || normalized.tm_year != parts[0] - 1900 ||
        normalized.tm_mon != parts[1] - 1 || normalized.tm_mday != parts[2])
        return RT_FALSE;
    *timestamp = (rt_uint32_t)value;
    return RT_TRUE;
}

static rt_bool_t parse_record(char *line, struct gd32_env_record *record)
{
    /* 解析器严格校验列数、范围及无效列为空，坏行只影响历史显示，不中断线程。 */
    char *fields[7], *cursor = line;
    rt_uint32_t numbers[5] = {0}, flags, temp;
    int index;
    rt_bool_t negative;
    fields[0] = line;
    for (index = 1; index < 7; index++)
    {
        while (*cursor && *cursor != ',') cursor++;
        if (*cursor != ',') return RT_FALSE;
        *cursor++ = '\0';
        fields[index] = cursor;
    }
    rt_memset(record, 0, sizeof(*record));
    if (!parse_timestamp(fields[0], &record->timestamp) ||
        !parse_uint(fields[6], &flags, 16) || flags > 7)
        return RT_FALSE;
    for (index = 0; index < 5; index++)
    {
        unsigned valid_bit = index < 2 ? GD32_ENV_VALID_SHT40 :
                             (index == 2 ? GD32_ENV_VALID_LIGHT : GD32_ENV_VALID_GAS);
        if (!(flags & valid_bit))
        {
            if (fields[index + 1][0] != '\0') return RT_FALSE;
            continue;
        }
        cursor = fields[index + 1];
        negative = index == 0 && *cursor == '-';
        if (negative) cursor++;
        if (!parse_uint(cursor, &temp, 10)) return RT_FALSE;
        if ((index == 0 && temp > (negative ? 4000U : 12500U)) ||
            (index == 1 && temp > 10000U) || (index >= 3 && temp > 65535U))
            return RT_FALSE;
        numbers[index] = negative ? (rt_uint32_t)(-(rt_int32_t)temp) : temp;
    }
    record->temperature_x100 = (rt_int32_t)numbers[0];
    record->humidity_x100 = (rt_uint16_t)numbers[1];
    record->lux_x10 = numbers[2];
    record->eco2_ppm = (rt_uint16_t)numbers[3];
    record->tvoc_ppb = (rt_uint16_t)numbers[4];
    record->valid_flags = (rt_uint8_t)flags;
    return RT_TRUE;
}

static int csv_open(void)
{
    /* 历史读取和归档恢复共用一个 reader，调用者必须在 envlog 线程内串行使用。 */
    s_reader.fd = open(ENV_LOG_PATH, O_RDONLY, 0);
    s_reader.position = s_reader.length = 0;
    return s_reader.fd;
}

static rt_err_t csv_repair_tail(void);

/* 1 complete line, 0 EOF (ignore an interrupted final write), -1 I/O error. */
static int csv_line(char *line, rt_size_t capacity)
{
    /* 按行读取并限制单行长度；最后一条没有换行的掉电残行会被忽略。 */
    rt_size_t used = 0;
    rt_bool_t overflow = RT_FALSE;
    while (1)
    {
        char ch;
        if (s_reader.position == s_reader.length)
        {
            int result = read(s_reader.fd, s_reader.buffer, sizeof(s_reader.buffer));
            if (result <= 0)
                return result < 0 ? -1 : 0;
            s_reader.position = 0;
            s_reader.length = result;
            rt_thread_mdelay(1); /* A large archive must not monopolize the CPU. */
        }
        ch = s_reader.buffer[s_reader.position++];
        if (ch == '\n')
        {
            if (overflow)
            {
                used = 0;
                overflow = RT_FALSE;
                continue;
            }
            line[used] = '\0';
            return 1;
        }
        if (ch == '\r') continue;
        if (used + 1 < capacity)
            line[used++] = ch;
        else
            overflow = RT_TRUE;
    }
}

static rt_err_t archive_recover_pending(rt_uint32_t now)
{
    /* 大文件恢复按 ENV_ARCHIVE_LINES_PER_SLICE 分片，每片结束后让出 CPU。 */
    char line[192];
    struct gd32_env_record record;
    rt_uint32_t processed = 0;
    rt_uint32_t trend_period;
    int result;

    if (!s_archive_recovery_pending)
        return RT_EOK;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    trend_period = trend_period_seconds(s_status.trend_interval_seconds);
    rt_mutex_release(&s_lock);
    if (!s_archive_active)
    {
        log_stage("archive-open");
        if (csv_open() < 0)
        {
            if (rt_get_errno() != -ENOENT)
            {
                s_archive_recovery_pending = RT_FALSE;
                history_request_finish(-RT_EIO);
                return -RT_EIO;
            }
            /* An absent log is a valid empty history. */
            s_archive_total = 0;
            s_archive_recent_count = 0;
            s_archive_recent_valid = RT_TRUE;
            s_csv_tail_checked = RT_TRUE;
            s_archive_recovery_pending = RT_FALSE;
            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
            s_status.total_rows = 0;
            s_history.total_rows = 0;
            s_history.count = 0;
            s_history.revision++;
            rt_mutex_release(&s_lock);
            return RT_EOK;
        }
        s_archive_active = RT_TRUE;
        s_archive_total = 0;
        s_archive_now = now;
        s_archive_recent_count = 0;
        s_archive_recent_valid = RT_FALSE;
        log_stage("archive-read");
    }

    /* Process a bounded number of records.  The next worker cycle continues
     * from s_reader, so the LVGL and shell threads get scheduling time even
     * when envlog.csv contains many years of data. */
    while (processed < ENV_ARCHIVE_LINES_PER_SLICE &&
           (result = csv_line(line, sizeof(line))) > 0)
    {
        if (parse_record(line, &record))
        {
            s_archive_total++;
            if (s_archive_now) trend_add(&record, s_archive_now, trend_period);
            if (s_archive_recent_count < GD32_ENV_HISTORY_ROWS)
                s_archive_recent[s_archive_recent_count++] = record;
            else
            {
                rt_memmove(&s_archive_recent[0], &s_archive_recent[1],
                           (GD32_ENV_HISTORY_ROWS - 1U) * sizeof(s_archive_recent[0]));
                s_archive_recent[GD32_ENV_HISTORY_ROWS - 1U] = record;
            }
        }
        processed++;
    }
    if (result > 0)
    {
        /* Keep the request marked busy and ask the worker to continue after
         * its normal event wait. */
        (void)rt_event_send(&s_events, EVENT_HISTORY);
        return -RT_EBUSY;
    }
    if (close(s_reader.fd) != 0 || result < 0)
    {
        s_archive_active = RT_FALSE;
        s_archive_recovery_pending = RT_FALSE;
        history_request_finish(-RT_EIO);
        return -RT_EIO;
    }
    s_archive_active = RT_FALSE;
    /* A power loss can leave one partial final row.  Repair it once while
     * recovering the archive; write_record() then only seeks to EOF. */
    if (csv_repair_tail() != RT_EOK)
    {
        s_archive_recovery_pending = RT_FALSE;
        history_request_finish(-RT_EIO);
        return -RT_EIO;
    }
    s_csv_tail_checked = RT_TRUE;
    s_archive_recent_valid = RT_TRUE;
    s_archive_recovery_pending = RT_FALSE;
    log_stage("archive-status");
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_status.total_rows = s_archive_total;
    s_history.total_rows = s_archive_total;
    s_history.count = s_archive_recent_count;
    {
        rt_uint8_t recent_index;
        for (recent_index = 0; recent_index < s_archive_recent_count; recent_index++)
            s_history.rows[recent_index] =
                s_archive_recent[s_archive_recent_count - recent_index - 1U];
    }
    s_history.revision++;
    rt_mutex_release(&s_lock);
    log_stage("archive-done");
    return RT_EOK;
}

static rt_err_t history_read(void)
{
    /* CSV 按时间从旧到新追加；这里按目标页反向填充，让 UI 始终看到新记录在前。 */
    struct gd32_env_history page;
    struct gd32_env_record record;
    char line[192];
    rt_uint32_t request_id, first, last, index = 0;
    int result = 0;
    rt_memset(&page, 0, sizeof(page));
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    page.page = s_requested_page;
    page.total_rows = s_status.total_rows;
    request_id = s_history_request_id;
    rt_mutex_release(&s_lock);
    if (!s_mounted)
        page.result = -RT_ENOENT;
    else if (page.page > 0 && page.page > (page.total_rows ? page.total_rows - 1 : 0) / GD32_ENV_HISTORY_ROWS)
        page.result = -RT_EINVAL;
    else if (page.total_rows && page.page == 0 && s_archive_recent_valid)
    {
        /* Archive recovery already retained the newest page.  Avoid scanning
         * the complete CSV a second time just to display page zero. */
        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        page.count = s_history.count;
        rt_memcpy(page.rows, s_history.rows, sizeof(page.rows));
        rt_mutex_release(&s_lock);
    }
    else if (page.total_rows)
    {
        last = page.total_rows - page.page * GD32_ENV_HISTORY_ROWS;
        first = last > GD32_ENV_HISTORY_ROWS ? last - GD32_ENV_HISTORY_ROWS : 0;
        log_stage("history-open");
        if (csv_open() < 0)
            page.result = -RT_EIO;
        else
        {
            log_stage("history-read");
            while (index < last && (result = csv_line(line, sizeof(line))) > 0)
            {
                if (!parse_record(line, &record)) continue;
                if (index >= first)
                {
                    /* The page buffer is zero based even when reading an
                     * older page. Newest records remain first within it. */
                    /* CSV records are oldest first.  Within the selected
                     * range, reverse the absolute record index so row zero
                     * is always the newest record.  Do not subtract `first`
                     * again: that would underflow on the oldest row of a
                     * full page and corrupt the stack-backed page object. */
                    {
                        rt_uint32_t row = last - index - 1U;
                        /* Page zero normally comes from the recovery cache.
                         * Keep this fallback bounded as well, so a recovery
                         * state transition can never write past page.rows. */
                        if (row < GD32_ENV_HISTORY_ROWS)
                        {
                            page.rows[row] = record;
                            if (page.count < GD32_ENV_HISTORY_ROWS)
                                page.count++;
                        }
                    }
                }
                index++;
            }
            if (close(s_reader.fd) != 0 || result < 0 || index != last)
                page.result = -RT_EIO;
        }
    }
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (request_id == s_history_request_id)
    {
        page.revision = s_history.revision + 1;
        s_history = page;
    }
    rt_mutex_release(&s_lock);
    return page.result;
}

/* Remove only an unfinished final line. Adding a newline would turn a torn
 * row into a record that recovery did not count, shifting every history page.
 * The worker owns s_reader, so its buffer is available here after close. */
static rt_err_t discard_unfinished_tail(int fd, off_t *size)
{
    /* 只截掉最后一个没有换行的残行，不把掉电半行伪装成合法测量值。 */
    off_t end = *size, start, repaired = 0;
    int index, length;
    while (end > 0)
    {
        length = end > (off_t)sizeof(s_reader.buffer) ? sizeof(s_reader.buffer) : (int)end;
        start = end - length;
        if (lseek(fd, start, SEEK_SET) != start || read(fd, s_reader.buffer, length) != length)
            return -RT_EIO;
        for (index = length - 1; index >= 0; index--)
        {
            if (s_reader.buffer[index] == '\n')
            {
                repaired = start + index + 1;
                break;
            }
        }
        if (repaired) break;
        end = start;
        rt_thread_mdelay(1);
    }
    if (repaired != *size && ftruncate(fd, repaired) != 0)
        return -RT_EIO;
    *size = repaired;
    return lseek(fd, repaired, SEEK_SET) == repaired ? RT_EOK : -RT_EIO;
}

static rt_err_t csv_repair_tail(void)
{
    /* 挂载后的首次扫描修复尾部，后续正常追加均写入换行完整的记录。 */
    struct stat file_stat;
    off_t size;
    int fd;

    fd = open(ENV_LOG_PATH, O_RDWR, 0);
    if (fd < 0)
        return rt_get_errno() == -ENOENT ? RT_EOK : -RT_EIO;
    if (fstat(fd, &file_stat) != 0)
    {
        close(fd);
        return -RT_EIO;
    }
    size = file_stat.st_size;
    {
        rt_err_t result = discard_unfinished_tail(fd, &size);
        if (close(fd) != 0)
            result = -RT_EIO;
        return result;
    }
}

static rt_err_t write_record(const struct gd32_env_record *data)
{
    /* 将定点数转换为 CSV 实际存储格式；无效传感器列保持为空。 */
    struct tm calendar;
    time_t timestamp = data->timestamp;
    off_t append_at;
    char row[192], fields[5][16];
    int fd, length, index;
    rt_bool_t refresh_history = RT_FALSE;
    rt_err_t result = RT_EOK;
    if (!s_mounted) return -RT_ENOENT;
    if (!timestamp || gmtime_r(&timestamp, &calendar) == RT_NULL) return -RT_EINVAL;
    rt_memset(fields, 0, sizeof(fields));
    if (data->valid_flags & GD32_ENV_VALID_SHT40)
    {
        rt_snprintf(fields[0], sizeof(fields[0]), "%ld", (long)data->temperature_x100);
        rt_snprintf(fields[1], sizeof(fields[1]), "%u", data->humidity_x100);
    }
    if (data->valid_flags & GD32_ENV_VALID_LIGHT)
        rt_snprintf(fields[2], sizeof(fields[2]), "%lu", (unsigned long)data->lux_x10);
    if (data->valid_flags & GD32_ENV_VALID_GAS)
    {
        rt_snprintf(fields[3], sizeof(fields[3]), "%u", data->eco2_ppm);
        rt_snprintf(fields[4], sizeof(fields[4]), "%u", data->tvoc_ppb);
    }
    length = rt_snprintf(row, sizeof(row), "%04d-%02d-%02d %02d:%02d:%02d,%s,%s,%s,%s,%s,%02x\n",
        calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday,
        calendar.tm_hour, calendar.tm_min, calendar.tm_sec,
        fields[0], fields[1], fields[2], fields[3], fields[4], data->valid_flags);
    if (length <= 0 || length >= sizeof(row)) return -RT_EINVAL;
    log_stage("csv-open-write");
    fd = open(ENV_LOG_PATH, O_RDWR | O_CREAT, 0);
    if (fd < 0) return -RT_EIO;
    log_stage("csv-position");
    append_at = lseek(fd, 0, SEEK_END);
    if (append_at < 0)
        result = -RT_EIO;
    else
    {
        /* Mount recovery normally performs this step.  Keep a one-time
         * fallback for a save racing the first mount attempt. */
        if (!s_csv_tail_checked)
        {
            log_stage("csv-tail-repair");
            result = discard_unfinished_tail(fd, &append_at);
            s_csv_tail_checked = RT_TRUE;
        }
        if (result == RT_EOK && !append_at)
        {
            log_stage("csv-header-write");
            if (write(fd, s_header, sizeof(s_header) - 1) != sizeof(s_header) - 1)
                result = -RT_EIO;
        }
    }
    log_stage("csv-row-write");
    if (result == RT_EOK && write(fd, row, length) != length) result = -RT_EIO;
    log_stage("csv-fsync");
    if (result == RT_EOK && fsync(fd) != 0) result = -RT_EIO;
    log_stage("csv-close");
    if (close(fd) != 0) result = -RT_EIO;
    else
    {
        log_stage("csv-card-sync");
        if (card_sync() != RT_EOK) result = -RT_EIO;
    }
    log_stage("csv-status");
    if (result == RT_EOK)
    {
        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        s_status.rows_saved++;
        s_status.total_rows++;
        s_status.last_save_tick = rt_tick_get();
        s_status.revision++;
        s_history.total_rows = s_status.total_rows;
        if (s_history.page == 0 && !s_history.busy && s_history.result == RT_EOK)
        {
            for (index = GD32_ENV_HISTORY_ROWS - 1; index > 0; index--)
                s_history.rows[index] = s_history.rows[index - 1];
            s_history.rows[0] = *data;
            if (s_history.count < GD32_ENV_HISTORY_ROWS) s_history.count++;
        }
        else if (!s_history.busy)
        {
            /* A new row shifts the indices of older pages as well. */
            s_history.busy = RT_TRUE;
            refresh_history = RT_TRUE;
        }
        s_history.revision++;
        rt_mutex_release(&s_lock);
        if (refresh_history) rt_event_send(&s_events, EVENT_HISTORY);
    }
    return result;
}

static rt_err_t history_clear(void)
{
    /* 清空只重建 envlog.csv 的表头，不删除阈值配置和其他用户文件。 */
    int fd;
    rt_uint32_t request_id;
    rt_err_t result = RT_EOK;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    request_id = s_history_request_id;
    rt_mutex_release(&s_lock);
    if (!s_mounted)
        result = -RT_ENOENT;
    /* The action affects only this application's CSV; configuration is preserved. */
    if (result == RT_EOK)
    {
        fd = open(ENV_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0);
        if (fd < 0)
            result = -RT_EIO;
        else
        {
            if (write(fd, s_header, sizeof(s_header) - 1) != sizeof(s_header) - 1 || fsync(fd) != 0)
                result = -RT_EIO;
            if (close(fd) != 0 || card_sync() != RT_EOK) result = -RT_EIO;
        }
    }
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (result == RT_EOK)
    {
        s_archive_recovery_pending = RT_FALSE;
        s_archive_active = RT_FALSE;
        s_archive_recent_valid = RT_TRUE;
        s_archive_recent_count = 0;
        s_status.total_rows = 0;
        s_status.revision++;
        rt_memset(s_bins, 0, sizeof(s_bins));
        rt_memset(s_history.rows, 0, sizeof(s_history.rows));
        s_history.page = s_history.total_rows = s_history.count = 0;
    }
    if (request_id == s_history_request_id)
    {
        s_history.busy = RT_FALSE;
        s_history.result = result;
        s_history.revision++;
        s_requested_page = 0;
    }
    rt_mutex_release(&s_lock);
    return result;
}

static rt_err_t card_mount(rt_uint32_t now)
{
    /* SD 初始化和挂载只在 envlog 线程执行；失败后由定时重试，不复位 MCU。 */
    rt_err_t result;
    rt_tick_t started = rt_tick_get();
    RT_UNUSED(now);
    log_stage("sd-mount");
    rt_kprintf("SD MOUNT: begin mounted=%d need_unmount=%d\n",
               s_mounted, s_need_unmount);
    if (s_need_unmount)
    {
        /* All worker descriptors are closed before this call. If a shell command
         * still owns a file, DFS refuses; retry later instead of resetting SD. */
        rt_kprintf("SD MOUNT: unmount begin\n");
        result = dfs_unmount("/");
        rt_kprintf("SD MOUNT: unmount done result=%d elapsed=%lu ms\n",
                   result, (unsigned long)((rt_tick_get() - started) * 1000U /
                                           RT_TICK_PER_SECOND));
        if (result != 0)
        {
            rt_kprintf("SD MOUNT: unmount refused, retry later\n");
            return -RT_EBUSY;
        }
        s_need_unmount = RT_FALSE;
    }
    rt_kprintf("SD MOUNT: driver init begin\n");
    result = gd32_sdcard_init();
    rt_kprintf("SD MOUNT: driver init done result=%d ready=%d last_error=%d elapsed=%lu ms\n",
               result, gd32_sdcard_is_ready(), gd32_sdcard_last_error(),
               (unsigned long)((rt_tick_get() - started) * 1000U /
                               RT_TICK_PER_SECOND));
    if (result != RT_EOK)
    {
        status_state(GD32_ENV_LOG_OFFLINE);
        return -RT_ENOENT;
    }
    rt_kprintf("SD MOUNT: filesystem mount begin\n");
    result = dfs_mount("sd0", "/", "elm", 0, RT_NULL);
    rt_kprintf("SD MOUNT: filesystem mount done result=%d errno=%d elapsed=%lu ms\n",
               result, rt_get_errno(),
               (unsigned long)((rt_tick_get() - started) * 1000U /
                               RT_TICK_PER_SECOND));
    if (result != 0)
    {
        rt_kprintf("envlog: filesystem mount failed, errno=%d (FAT16/FAT32 required)\n",
                   rt_get_errno());
        status_state(GD32_ENV_LOG_MOUNT_ERROR);
        return -RT_ERROR;
    }
    rt_kprintf("envlog: filesystem mounted on / (elapsed=%lu ms)\n",
               (unsigned long)((rt_tick_get() - started) * 1000U /
                               RT_TICK_PER_SECOND));
    s_mounted = RT_TRUE;
    s_csv_tail_checked = RT_FALSE;
    s_archive_recovery_pending = RT_TRUE;
    s_archive_active = RT_FALSE;
    s_archive_recent_valid = RT_FALSE;
    s_archive_recent_count = 0;
    s_config_load_pending = RT_TRUE;
    status_state(GD32_ENV_LOG_READY);
    rt_kprintf("envlog: archive recovery deferred until history request\n");
    return RT_EOK;
}

static void log_entry(void *parameter)
{
    /* 主循环每秒醒来一次：先处理校时/用户请求，再按截止时间采样、写趋势和 CSV。 */
    rt_tick_t next_trend = rt_tick_get();
    /* Do not make the first valid RTC read an implicit SD write.  This is
     * especially important after a shell/UI clock change: the RTC path must
     * complete without entering FAT/SD code. */
    rt_tick_t next_save = rt_tick_get() + ENV_DEFAULT_INTERVAL * RT_TICK_PER_SECOND;
    rt_tick_t next_mount = rt_tick_get(), next_config_load = rt_tick_get();
    rt_tick_t mount_retry = ENV_RETRY;
    rt_tick_t previous_tick = rt_tick_get(), clock_tick;
    rt_uint32_t events, now, previous_time = 0;
    rt_uint32_t interval = ENV_DEFAULT_INTERVAL, requested_interval;
    rt_uint32_t trend_interval = ENV_DEFAULT_INTERVAL, requested_trend_interval;
    rt_uint32_t mount_failures = 0;
    rt_bool_t time_set, had_time = RT_FALSE, pending, time_changed;
    rt_bool_t trend_due, save_due;
    struct gd32_env_record record;
    rt_err_t result;
    RT_UNUSED(parameter);
    while (1)
    {
        events = 0;
        log_stage("event-wait");
        (void)rt_event_recv(&s_events, EVENT_ALL, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                            rt_tick_from_millisecond(1000), &events);
        s_diag_loops++;
        /* Keep the interactive clock path independent from every SD action.
         * Event bits can be coalesced by RT-Thread, so defer any save,
         * history, clear, or settings work that was queued at the same time
         * and let the next worker cycle handle it after the RTC update. */
        if (events & EVENT_TIME)
        {
            rt_uint32_t deferred = events & (EVENT_SAVE | EVENT_HISTORY |
                                              EVENT_CLEAR | EVENT_SETTINGS |
                                              EVENT_INTERVAL | EVENT_TREND_INTERVAL);
            if (deferred)
                (void)rt_event_send(&s_events, deferred);
            events &= ~deferred;
        }
        s_diag_trace_cycle = (events & EVENT_TIME) != 0;
        log_stage("event-received");
        if (events & EVENT_TIME)
            rt_kprintf("TIME worker: event received, bits=%02lx\n", (unsigned long)events);
        time_changed = RT_FALSE;
        if (events & EVENT_TIME)
        {
            /* RTC failures must not mark a working SD card as faulty. */
            time_changed = rtc_set_requested() == RT_EOK;
        }
        now = 0;
        log_stage("rtc-read");
        if (events & EVENT_TIME) rt_kprintf("TIME worker: readback begin\n");
        time_set = rtc_now(&now);
        if (events & EVENT_TIME)
            rt_kprintf("TIME worker: readback done, valid=%d timestamp=%lu\n",
                       time_set, (unsigned long)now);
        clock_tick = rt_tick_get();
        if (time_set && had_time)
        {
            rt_int64_t elapsed = (rt_tick_t)(clock_tick - previous_tick) / RT_TICK_PER_SECOND;
            rt_int64_t clock_delta = (rt_int64_t)now - previous_time - elapsed;
            /* Also detect calibration through the shell's standard date command. */
            if (clock_delta < -3 || clock_delta > 3) time_changed = RT_TRUE;
        }
        if (time_set && (!had_time || time_changed))
            s_diag_trace_cycle = RT_TRUE;
        if (s_diag_trace_cycle)
            rt_kprintf("TIME SD: cycle time=%lu previous=%lu valid=%d changed=%d mounted=%d\n",
                       (unsigned long)now, (unsigned long)previous_time,
                       time_set, time_changed, s_mounted);
        previous_time = now;
        previous_tick = clock_tick;
        log_stage("sensor-snapshot");
        record_snapshot(&record, now);
        log_stage("status-lock");
        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        if (s_status.rtc_set != time_set ||
            s_status.timestamp / trend_period_seconds(s_status.trend_interval_seconds) !=
            now / trend_period_seconds(s_status.trend_interval_seconds))
            s_status.revision++;
        s_status.timestamp = now;
        s_status.rtc_set = time_set;
        s_status.alarm_flags = alarm_flags(&record, &s_thresholds);
        if (time_changed)
        {
            rt_memset(s_bins, 0, sizeof(s_bins));
            s_status.revision++;
        }
        rt_mutex_release(&s_lock);
        if (s_mounted && !gd32_sdcard_is_ready()) action_end(-RT_EIO);
        /* A clock-setting cycle is deliberately SD-free.  If the card was
         * not mounted yet, defer the mount/archive scan to the next cycle as
         * well; entering the FAT/SD driver during the interactive save can
         * operation look stuck on a large FAT volume. */
        if (!s_mounted && !(events & EVENT_TIME) && !time_changed &&
            (had_time || !time_set) &&
            (rt_int32_t)(rt_tick_get() - next_mount) >= 0)
        {
            action_begin();
            result = card_mount(now);
            action_end(result);
            if (result == RT_EOK)
            {
                mount_failures = 0;
                mount_retry = ENV_RETRY;
                next_config_load = rt_tick_get() + rt_tick_from_millisecond(5000);
            }
            else
            {
                /* A missing or electrically unstable card must not be probed
                 * every second. Back off up to 15 minutes, while preserving
                 * automatic recovery when the card is inserted later. */
                if (mount_failures < 4U) mount_failures++;
                mount_retry = ENV_RETRY << mount_failures;
                if (mount_retry > ENV_MOUNT_RETRY_MAX)
                    mount_retry = ENV_MOUNT_RETRY_MAX;
                rt_kprintf("envlog: SD mount failed result=%d, retry in %lu s (attempt=%lu)\n",
                           result,
                           (unsigned long)(mount_retry / RT_TICK_PER_SECOND),
                           (unsigned long)mount_failures);
            }
            next_mount = rt_tick_get() + mount_retry;
        }
        /*
         * Do not start SD archive recovery in the same cycle as a clock
         * change. Recovery is incremental, but its first slice still enters
         * the FAT/SD driver. A time-setting request is an
         * interactive operation; keeping the clock write independent of SD
         * recovery prevents the shell and LVGL from appearing dead when a
         * card is inserted. The next history request refreshes the view.
         */
        if (s_mounted && s_config_load_pending && !(events & EVENT_TIME) &&
            !time_changed && (rt_int32_t)(rt_tick_get() - next_config_load) >= 0)
        {
            action_begin();
            log_stage("config-load");
            result = config_load();
            rt_kprintf("envlog: config load result=%d\n", result);
            action_end(result);
            if (result == RT_EOK)
                s_config_load_pending = RT_FALSE;
            else
            {
                /* 瞬态 SD/FAT 错误不能让默认配置覆盖卡上的旧配置；保留
                 * pending，待重新挂载后再读双槽。 */
                s_config_load_pending = RT_TRUE;
                next_config_load = rt_tick_get() + ENV_RETRY;
            }
        }
        /* 先加载配置再取快照，避免重启后按旧周期多采样或漏掉迁移保存。 */
        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        requested_interval = s_status.log_interval_seconds;
        requested_trend_interval = s_status.trend_interval_seconds;
        pending = s_status.settings_pending;
        rt_mutex_release(&s_lock);
        if (interval != requested_interval || (events & EVENT_INTERVAL))
        {
            interval = requested_interval;
            next_save = rt_tick_get() + interval * RT_TICK_PER_SECOND;
        }
        if (trend_interval != requested_trend_interval || (events & EVENT_TREND_INTERVAL))
        {
            trend_interval = requested_trend_interval;
            /* 清桶与周期写入已在同一锁内完成；这里仅重排曲线采样。 */
            next_trend = rt_tick_get() + trend_interval * RT_TICK_PER_SECOND;
        }
        if (events & EVENT_CLEAR)
        {
            action_begin();
            result = history_clear();
            action_end(result);
            /* Do not immediately refill the cleared file with a scheduled row. */
            next_save = rt_tick_get() + interval * RT_TICK_PER_SECOND;
            next_trend = rt_tick_get() + trend_interval * RT_TICK_PER_SECOND;
            /* A request already queued before clear must not hide a clear error. */
            events &= ~EVENT_HISTORY;
        }
        if (events & EVENT_HISTORY)
        {
            action_begin();
            /* Archive recovery is incremental.  While it is active the
             * worker requeues EVENT_HISTORY and returns to its event wait,
             * allowing LVGL, touch, and the shell to continue running. */
            result = archive_recover_pending(now);
            if (result == RT_EOK)
                result = history_read();
            action_end(result);
        }
        if (s_mounted && !s_config_load_pending && !s_archive_active && !time_changed &&
            !(events & EVENT_TIME) && pending)
        {
            action_begin();
            log_stage("config-save");
            result = config_save();
            action_end(result);
        }
        /* 历史归档仍持有分块 reader 时，不能进入 write_record()；两者
         * 共用 s_reader 缓冲区，等归档下一轮完成后再补采样/保存。 */
        trend_due = time_set && !s_archive_active && (!had_time || time_changed ||
                    (rt_int32_t)(rt_tick_get() - next_trend) >= 0);
        /* Do not append a CSV row as a side effect of changing the clock. */
        save_due = time_set && !s_archive_active && !time_changed && !(events & EVENT_TIME) &&
                   ((rt_int32_t)(rt_tick_get() - next_save) >= 0 ||
                    (events & EVENT_SAVE));
        if (!(events & EVENT_CLEAR) && (trend_due || save_due))
        {
            /* SD recovery may have taken time. Re-read time and freshness now. */
            log_stage("sample-rtc-read");
            if (rtc_now(&now))
            {
                log_stage("sample-sensor");
                record_snapshot(&record, now);
                if (trend_due)
                {
                    log_stage("sample-trend");
                    trend_add(&record, now, trend_interval);
                    /* Keep the live chart aligned to the selected interval. */
                    {
                        rt_uint32_t period = trend_period_seconds(trend_interval);
                        next_trend = rt_tick_get() +
                            (period - now % period) * RT_TICK_PER_SECOND;
                    }
                }
                if (save_due)
                {
                    if (s_mounted)
                    {
                        action_begin();
                        if (s_diag_trace_cycle) rt_kprintf("TIME worker: CSV write begin\n");
                        log_stage("csv-format");
                        result = write_record(&record);
                        if (s_diag_trace_cycle)
                            rt_kprintf("TIME worker: CSV write done, result=%d\n", result);
                        action_end(result);
                    }
                    else if (events & EVENT_SAVE)
                        action_end(-RT_ENOENT);
                }
            }
            else if (events & EVENT_SAVE)
                action_end(-RT_EINVAL);
            if (save_due) next_save = rt_tick_get() + interval * RT_TICK_PER_SECOND;
        }
        else if ((events & EVENT_SAVE) && !time_set)
            action_end(-RT_EINVAL);
        had_time = time_set;
        log_stage("cycle-done");
        if (s_diag_trace_cycle) rt_kprintf("TIME worker: request finished\n");
        s_diag_trace_cycle = RT_FALSE;
    }
}

rt_err_t gd32_env_log_get_status(struct gd32_env_log_status *status)
{
    /* 返回的是一致的 RAM 快照；调用者可以在 UI 线程安全显示，不会触碰文件。 */
    if (!s_started || status == RT_NULL) return -RT_ENOENT;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    *status = s_status;
    rt_mutex_release(&s_lock);
    return RT_EOK;
}

rt_err_t gd32_env_thresholds_get(struct gd32_env_thresholds *thresholds)
{
    /* 读取当前生效阈值，不代表配置已经写入 SD。 */
    if (!s_started || thresholds == RT_NULL) return -RT_ENOENT;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    *thresholds = s_thresholds;
    rt_mutex_release(&s_lock);
    return RT_EOK;
}

rt_err_t gd32_env_thresholds_set(const struct gd32_env_thresholds *thresholds)
{
    /* 设置立即写入内存；后台线程会在下一次采样时重新计算报警并负责
     * 持久化。不要在 LVGL 回调里同步读取传感器：I2C 设备异常时读取
     * 快照可能等待传感器互斥锁，进而冻结界面和串口。 */
    if (!s_started) return -RT_ENOENT;
    if (!thresholds_valid(thresholds)) return -RT_EINVAL;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_thresholds = *thresholds;
    s_settings_revision++;
    s_user_settings = RT_TRUE;
    s_user_thresholds = RT_TRUE;
    s_status.settings_pending = RT_TRUE;
    s_status.revision++;
    rt_mutex_release(&s_lock);
    return rt_event_send(&s_events, EVENT_SETTINGS);
}

rt_err_t gd32_env_trend_get(struct gd32_env_trend *trend)
{
    /* 趋势返回 144 个可配置周期的时间桶，缺少采样的时间桶保留为无效点。 */
    rt_uint32_t current, bin, period;
    int index;
    if (!s_started || trend == RT_NULL) return -RT_ENOENT;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    period = trend_period_seconds(s_status.trend_interval_seconds);
    current = s_status.timestamp / period;
    rt_memset(trend, 0, sizeof(*trend));
    trend->interval_seconds = period;
    trend->rtc_set = s_status.rtc_set;
    trend->revision = s_status.revision;
    if (trend->rtc_set)
    {
        for (index = 0; index < GD32_ENV_TREND_POINTS; index++)
        {
            bin = current - (GD32_ENV_TREND_POINTS - 1U) + index;
            if (s_bins[bin % GD32_ENV_TREND_POINTS].timestamp / period == bin)
                trend->points[index] = s_bins[bin % GD32_ENV_TREND_POINTS];
            trend->points[index].timestamp = bin * period;
        }
    }
    rt_mutex_release(&s_lock);
    return RT_EOK;
}

rt_err_t gd32_env_history_get(struct gd32_env_history *history)
{
    /* 历史页读取结果带 revision，UI 可据此丢弃过期异步请求。 */
    if (!s_started || history == RT_NULL) return -RT_ENOENT;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    *history = s_history;
    rt_mutex_release(&s_lock);
    return RT_EOK;
}

rt_err_t gd32_env_history_request(rt_uint32_t page)
{
    /* 只登记页码并投递事件；实际扫描在 envlog 线程分片完成。 */
    if (!s_started) return -RT_ENOENT;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_requested_page = page;
    s_history_request_id++;
    s_history.busy = RT_TRUE;
    s_history.revision++;
    rt_mutex_release(&s_lock);
    return rt_event_send(&s_events, EVENT_HISTORY);
}

rt_err_t gd32_env_history_clear_request(void)
{
    /* 清空同样异步执行，避免 LVGL 事件回调直接阻塞在 FAT 写入。 */
    if (!s_started) return -RT_ENOENT;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_history.busy = RT_TRUE;
    s_history_request_id++;
    s_requested_page = 0;
    s_history.revision++;
    rt_mutex_release(&s_lock);
    return rt_event_send(&s_events, EVENT_CLEAR);
}

rt_err_t gd32_env_log_save_request(void)
{
    /* 手动保存只发事件；传感器采样、CSV 格式化和同步由 worker 完成。 */
    if (!s_started) return -RT_ENOENT;
    return rt_event_send(&s_events, EVENT_SAVE);
}

rt_err_t gd32_env_time_set_request(rt_uint32_t timestamp)
{
    /* 不拿 s_lock 的原因：此函数可直接运行在 LVGL 回调中，必须快速返回。 */
    rt_base_t level;
    rt_err_t result;
    rt_uint32_t request_id;
    if (!s_started) return -RT_ENOENT;
    if (timestamp < 946684800UL || timestamp > 4102444799UL) return -RT_EINVAL;
    /* Do not wait on s_lock here: this function is called directly from an
     * LVGL event callback and must return even if SD/FAT recovery is busy. */
    level = rt_hw_interrupt_disable();
    s_requested_time = timestamp;
    s_time_request_id++;
    request_id = s_time_request_id;
    s_status.time_pending = RT_TRUE;
    s_status.revision++;
    rt_hw_interrupt_enable(level);
    result = rt_event_send(&s_events, EVENT_TIME);
    if (result != RT_EOK)
    {
        level = rt_hw_interrupt_disable();
        if (request_id == s_time_request_id)
            s_status.time_pending = RT_FALSE;
        rt_hw_interrupt_enable(level);
    }
    return result;
}

rt_err_t gd32_env_log_interval_set(rt_uint32_t seconds)
{
    /* 间隔以秒保存，HMI 的单位换算在 UI 层完成；范围在这里统一校验。 */
    if (!s_started) return -RT_ENOENT;
    if (seconds < 1U || seconds > 86400U)
        return -RT_EINVAL;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_status.log_interval_seconds = seconds;
    s_settings_revision++;
    s_user_settings = RT_TRUE;
    s_user_log_interval = RT_TRUE;
    s_status.settings_pending = RT_TRUE;
    s_status.revision++;
    rt_mutex_release(&s_lock);
    return rt_event_send(&s_events, EVENT_SETTINGS | EVENT_INTERVAL);
}

rt_err_t gd32_env_trend_interval_set(rt_uint32_t seconds)
{
    /* 曲线周期独立于 CSV 周期；改动后清空旧桶，避免不同时间尺度混在一起。 */
    rt_bool_t changed;
    if (!s_started) return -RT_ENOENT;
    if (seconds < 1U || seconds > 86400U)
        return -RT_EINVAL;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    changed = s_status.trend_interval_seconds != seconds;
    if (changed) rt_memset(s_bins, 0, sizeof(s_bins));
    s_status.trend_interval_seconds = seconds;
    s_settings_revision++;
    s_user_settings = RT_TRUE;
    s_user_trend_interval = RT_TRUE;
    s_status.settings_pending = RT_TRUE;
    s_status.revision++;
    rt_mutex_release(&s_lock);
    return rt_event_send(&s_events, EVENT_SETTINGS | (changed ? EVENT_TREND_INTERVAL : 0U));
}

#ifdef RT_USING_FINSH
static void envlog_alarm_print(int argc, char **argv)
{
    struct gd32_env_log_status status;
    struct gd32_env_thresholds thresholds;
    struct gd32_sensor_snapshot sensor;
    rt_err_t status_result;
    rt_err_t threshold_result;
    rt_err_t sensor_result;
    rt_uint32_t temp_abs;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    /* Keep the diagnostic result independent of the logger's SD activity. */
    (void)gd32_env_log_alarm_refresh();
    status_result = gd32_env_log_get_status(&status);
    threshold_result = gd32_env_thresholds_get(&thresholds);
    sensor_result = gd32_sensor_snapshot_get(&sensor);

    rt_kprintf("alarm diagnostic (read-only)\n");
    rt_kprintf("  status: envlog=%d sensor=%d thresholds=%d\n",
               status_result, sensor_result, threshold_result);

    if (sensor_result == RT_EOK)
    {
        temp_abs = (rt_uint32_t)(sensor.temperature_x100 < 0 ?
                                 -sensor.temperature_x100 : sensor.temperature_x100);
        rt_kprintf("  sensor: SHT40=%s temp=%s%lu.%02luC hum=%lu.%02lu%%\n",
                   sensor.sht40_valid ? "ok" : "offline",
                   sensor.temperature_x100 < 0 ? "-" : "",
                   (unsigned long)(temp_abs / 100U),
                   (unsigned long)(temp_abs % 100U),
                   (unsigned long)(sensor.humidity_x100 / 100U),
                   (unsigned long)(sensor.humidity_x100 % 100U));
        rt_kprintf("           BH1750=%s lux=%lu.%lu  SGP30=%s eCO2=%u TVOC=%u\n",
                   sensor.bh1750_valid ? "ok" : "offline",
                   (unsigned long)(sensor.lux_x10 / 10U),
                   (unsigned long)(sensor.lux_x10 % 10U),
                   sensor.sgp30_valid ? "ok" :
                       (sensor.sgp30_warming ? "warming" : "offline"),
                   (unsigned)sensor.eco2_ppm, (unsigned)sensor.tvoc_ppb);
    }

    if (threshold_result == RT_EOK)
    {
        rt_kprintf("  limits: temp=%ld..%ldC(0.01C) hum=%u..%u%%(0.01%%)\n",
                   (long)thresholds.temperature_low_x100,
                   (long)thresholds.temperature_high_x100,
                   (unsigned)thresholds.humidity_low_x100,
                   (unsigned)thresholds.humidity_high_x100);
        rt_kprintf("          eCO2<=%uppm TVOC<=%uppb light=%lu..%lu lux(0.1lux)\n",
                   (unsigned)thresholds.eco2_high_ppm,
                   (unsigned)thresholds.tvoc_high_ppb,
                   (unsigned long)thresholds.lux_low_x10,
                   (unsigned long)thresholds.lux_high_x10);
    }

    if (status_result == RT_EOK)
    {
        rt_kprintf("  alarm: flags=0x%08lx expected_led2=%s\n",
                   (unsigned long)status.alarm_flags,
                   status.alarm_flags ? "ON" : "OFF");
        rt_kprintf("         rtc=%s timestamp=%lu revision=%lu\n",
                   status.rtc_set ? "set" : "unset",
                   (unsigned long)status.timestamp,
                   (unsigned long)status.revision);
    }

    rt_kprintf("  build: LED2=PA6 buzzer_enable=%d mode=%s passive=PC6 active=PE2 idle=%s gate=%d\n",
               GD32_ALARM_BUZZER_ENABLE,
               GD32_ALARM_USE_ACTIVE ? "active" : "passive",
               GD32_ALARM_PC6_ACTIVE_LOW ? "high" : "low",
               GD32_ALARM_PC6_GATE_MODE);
}

static void envlog_cmd(int argc, char **argv)
{
    struct gd32_env_log_status status;
    if (argc == 2 && !rt_strcmp(argv[1], "alarm"))
    {
        envlog_alarm_print(0, RT_NULL);
        return;
    }
    if (argc == 2 && !rt_strcmp(argv[1], "save"))
    {
        rt_kprintf("envlog: save request %d\n", gd32_env_log_save_request());
        return;
    }
    if (gd32_env_log_get_status(&status) != RT_EOK)
    {
        rt_kprintf("envlog: unavailable\n");
        return;
    }
    rt_kprintf("envlog: state=%d total=%lu saved=%lu errors=%lu rtc_set=%d pending=%d busy=%d result=%d\n",
        status.state, (unsigned long)status.total_rows, (unsigned long)status.rows_saved,
        (unsigned long)status.errors, status.rtc_set, status.settings_pending,
        status.busy, status.last_action_result);
    rt_kprintf("envlog save: request one CSV row; automatic period %lu seconds\n",
               (unsigned long)status.log_interval_seconds);
    rt_kprintf("trend: independent sample period %lu seconds; %u points\n",
               (unsigned long)status.trend_interval_seconds, GD32_ENV_TREND_POINTS);
    rt_kprintf("time: pending=%d result=%d revision=%lu timestamp=%lu\n",
               status.time_pending, status.time_result,
               (unsigned long)status.time_revision, (unsigned long)status.timestamp);
}
MSH_CMD_EXPORT_ALIAS(envlog_cmd, envlog,
                     "Environmental logging status, save, or alarm diagnostics");
MSH_CMD_EXPORT_ALIAS(envlog_alarm_print, alarm, Show read-only alarm diagnostics);
#endif

int gd32_env_log_init(void)
{
    /* 服务使用静态线程、互斥锁和事件对象，初始化失败时逐项回收已创建对象。 */
    rt_err_t result;
    if (s_started) return RT_EOK;
    rt_memset(&s_status, 0, sizeof(s_status));
    rt_memset(&s_history, 0, sizeof(s_history));
    s_history.result = -RT_ENOENT;
    s_status.state = GD32_ENV_LOG_OFFLINE;
    s_status.log_interval_seconds = ENV_DEFAULT_INTERVAL;
    s_status.trend_interval_seconds = ENV_DEFAULT_INTERVAL;
    s_user_settings = RT_FALSE;
    s_user_thresholds = RT_FALSE;
    s_user_log_interval = RT_FALSE;
    s_user_trend_interval = RT_FALSE;
    result = rt_mutex_init(&s_lock, "elog", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK) return result;
    result = rt_event_init(&s_events, "elogev", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&s_lock);
        return result;
    }
    result = rt_thread_init(&s_log_thread, "envlog", log_entry, RT_NULL,
                           s_log_stack, sizeof(s_log_stack), 25, 10);
    if (result == RT_EOK)
    {
        s_started = RT_TRUE;
        result = rt_thread_startup(&s_log_thread);
        if (result != RT_EOK)
        {
            s_started = RT_FALSE;
            rt_thread_detach(&s_log_thread);
        }
    }
    if (result != RT_EOK)
    {
        rt_event_detach(&s_events);
        rt_mutex_detach(&s_lock);
    }
    return result;
}

#else
int gd32_env_log_init(void) { return -RT_ENOSYS; }
rt_uint32_t gd32_env_log_diag_request_id(void) { return 0; }
void gd32_env_log_diag_print(void) { }
#define ENV_DISABLED_GET(name, type) \
    rt_err_t name(type *value) { RT_UNUSED(value); return -RT_ENOSYS; }
ENV_DISABLED_GET(gd32_env_log_get_status, struct gd32_env_log_status)
rt_err_t gd32_env_log_alarm_refresh(void) { return -RT_ENOSYS; }
ENV_DISABLED_GET(gd32_env_thresholds_get, struct gd32_env_thresholds)
ENV_DISABLED_GET(gd32_env_thresholds_set, const struct gd32_env_thresholds)
ENV_DISABLED_GET(gd32_env_trend_get, struct gd32_env_trend)
ENV_DISABLED_GET(gd32_env_history_get, struct gd32_env_history)
rt_err_t gd32_env_history_request(rt_uint32_t page) { RT_UNUSED(page); return -RT_ENOSYS; }
rt_err_t gd32_env_history_clear_request(void) { return -RT_ENOSYS; }
rt_err_t gd32_env_log_save_request(void) { return -RT_ENOSYS; }
rt_err_t gd32_env_time_set_request(rt_uint32_t timestamp) { RT_UNUSED(timestamp); return -RT_ENOSYS; }
rt_err_t gd32_env_log_interval_set(rt_uint32_t seconds) { RT_UNUSED(seconds); return -RT_ENOSYS; }
rt_err_t gd32_env_trend_interval_set(rt_uint32_t seconds) { RT_UNUSED(seconds); return -RT_ENOSYS; }
#endif
