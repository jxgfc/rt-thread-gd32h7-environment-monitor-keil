/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GD32_ENV_LOG_H
#define GD32_ENV_LOG_H

#include <rtthread.h>

/* 环境日志服务的公共数据协议：UI 只读写这些 RAM 快照，SD/FAT 访问始终由
 * envlog 线程串行执行。数值采用整数定点格式，避免在 Cortex-M7 上传递浮点
 * 时引入额外的 ABI/格式化开销。 */
#define GD32_ENV_TREND_POINTS 144
#define GD32_ENV_HISTORY_ROWS 4       /* 历史页一次显示的记录数。 */
#define GD32_ENV_VALID_SHT40  1U      /* bit0：温度和湿度有效。 */
#define GD32_ENV_VALID_LIGHT  2U      /* bit1：BH1750 光照有效。 */
#define GD32_ENV_VALID_GAS    4U      /* bit2：SGP30 eCO2/TVOC 有效。 */
#define GD32_ENV_ALARM_TEMP   1U      /* 温度超出上下限。 */
#define GD32_ENV_ALARM_HUM    2U      /* 湿度超出上下限。 */
#define GD32_ENV_ALARM_ECO2   4U      /* eCO2 超过上限。 */
#define GD32_ENV_ALARM_TVOC   8U      /* TVOC 超过上限。 */
#define GD32_ENV_ALARM_LIGHT  16U     /* 光照超出上下限。 */

enum gd32_env_log_state
{
    GD32_ENV_LOG_OFFLINE = 0,
    GD32_ENV_LOG_MOUNT_ERROR,
    GD32_ENV_LOG_READY,
    GD32_ENV_LOG_IO_ERROR
};

struct gd32_env_record
{
    rt_uint32_t timestamp;     /* RTC 秒；与 date 命令使用同一 UTC 日历约定。 */
    rt_int32_t temperature_x100; /* 摄氏度 ×100，例如 25.34°C 保存为 2534。 */
    rt_uint32_t lux_x10;       /* lux ×10，例如 123.4 lux 保存为 1234。 */
    rt_uint16_t humidity_x100; /* 相对湿度 % ×100。 */
    rt_uint16_t eco2_ppm;      /* eCO2，单位 ppm。 */
    rt_uint16_t tvoc_ppb;      /* TVOC，单位 ppb。 */
    rt_uint8_t valid_flags;    /* 各传感器有效位；无效字段不可当作 0 使用。 */
};

struct gd32_env_thresholds
{
    rt_int32_t temperature_low_x100;  /* 温度下限，°C ×100。 */
    rt_int32_t temperature_high_x100; /* 温度上限，°C ×100。 */
    rt_uint16_t humidity_low_x100;    /* 湿度下限，% ×100。 */
    rt_uint16_t humidity_high_x100;   /* 湿度上限，% ×100。 */
    rt_uint16_t eco2_high_ppm;        /* eCO2 只有上限，单位 ppm。 */
    rt_uint16_t tvoc_high_ppb;        /* TVOC 只有上限，单位 ppb。 */
    /* BH1750 value in lux*10.  The lower limit allows dark-environment
     * alarms while the upper limit catches excessive illumination. */
    rt_uint32_t lux_low_x10;
    rt_uint32_t lux_high_x10;
};

struct gd32_env_log_status
{
    enum gd32_env_log_state state;
    rt_uint32_t rows_saved;
    rt_uint32_t errors;
    rt_tick_t last_save_tick;
    rt_uint32_t total_rows; /* Complete valid CSV records on the card. */
    rt_uint32_t timestamp;
    rt_uint32_t alarm_flags;
    rt_uint32_t revision;
    rt_bool_t rtc_set;
    rt_bool_t settings_pending; /* RAM values active, not yet durable on SD. */
    rt_bool_t busy;             /* 当前是否正在执行 SD/FAT 动作。 */
    rt_err_t last_action_result;
    rt_bool_t time_pending; /* RTC update is queued or in progress. */
    rt_err_t time_result; /* Dedicated result, unaffected by SD operations. */
    rt_uint32_t time_revision; /* Increments when the latest RTC request finishes. */
    rt_uint32_t log_interval_seconds; /* CSV 保存周期，默认600秒。 */
    rt_uint32_t trend_interval_seconds; /* 曲线采样周期，默认600秒。 */
};

struct gd32_env_trend
{
    /* 按 trend_interval_seconds 分桶，按时间从旧到新排列；valid_flags=0 表示该时间段无数据。 */
    struct gd32_env_record points[GD32_ENV_TREND_POINTS];
    rt_uint32_t interval_seconds; /* 此快照使用的曲线周期，与 CSV 存储周期独立。 */
    rt_uint32_t revision;
    rt_bool_t rtc_set;
};

struct gd32_env_history
{
    struct gd32_env_record rows[GD32_ENV_HISTORY_ROWS]; /* 新记录在前。 */
    rt_uint32_t page; /* 从 0 开始；第 0 页是最新数据。 */
    rt_uint32_t total_rows;
    rt_uint32_t revision;
    rt_uint8_t count;
    rt_bool_t busy;
    rt_err_t result;
};

int gd32_env_log_init(void);
/* 调试快照：不等待服务互斥锁，故障现场也可调用。 */
rt_uint32_t gd32_env_log_diag_request_id(void);
void gd32_env_log_diag_print(void);
rt_err_t gd32_env_log_get_status(struct gd32_env_log_status *status);
/* 从最新传感器快照刷新报警位，不访问 SD。 */
rt_err_t gd32_env_log_alarm_refresh(void);
/* Getter 只在短互斥区内复制 RAM，UI 线程不会进入 SD/FAT。 */
rt_err_t gd32_env_thresholds_get(struct gd32_env_thresholds *thresholds);
/* 阈值先校验并立即生效，持久化写入由后台线程排队完成。 */
rt_err_t gd32_env_thresholds_set(const struct gd32_env_thresholds *thresholds);
rt_err_t gd32_env_trend_get(struct gd32_env_trend *trend);
rt_err_t gd32_env_history_get(struct gd32_env_history *history);
rt_err_t gd32_env_history_request(rt_uint32_t page);
/* 清空是破坏性操作，调用前必须由 UI 二次确认。 */
rt_err_t gd32_env_history_clear_request(void);
rt_err_t gd32_env_log_save_request(void);
/* 日历约定与 RTC/date 一致，范围 2000..2099；UI 调用只投递请求，不做 I/O。 */
rt_err_t gd32_env_time_set_request(rt_uint32_t timestamp);
/* 两种间隔均为 1..86400 秒；只修改 SD 周期不会清除曲线或改变采样计划。 */
rt_err_t gd32_env_log_interval_set(rt_uint32_t seconds);
/* 修改曲线周期会清空 RAM 时间桶后按新周期采样，不删除 SD 上已有 CSV。 */
rt_err_t gd32_env_trend_interval_set(rt_uint32_t seconds);

#endif
