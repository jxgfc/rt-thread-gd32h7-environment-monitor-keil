/* SPDX-License-Identifier: Apache-2.0 */
/*
 * HMI 页面层：只在 LVGL 线程上下文中读快照、更新控件和提交异步请求。
 * 传感器采集、RTC 写入、SD 读写均由后台服务完成，避免触摸回调阻塞
 * LVGL 主循环；所有静态数组均按固定屏幕尺寸分配，避免运行时碎片。
 */
#include <rtthread.h>
#ifdef PKG_USING_LVGL
#include <lvgl.h>
#include <sys/time.h>
#include "hmi_pages.h"
#include "env_log.h"

LV_FONT_DECLARE(ui_font_14);
LV_FONT_DECLARE(ui_font_18);
#define BG 0x0d1720
#define CARD 0x182936
#define TEXT 0xebf1f6
#define MUTED 0x90a7b5
#define GREEN 0x41dcc4
#define AMBER 0xf2bd68
#define RED 0xff827b

static lv_obj_t *pages[4], *nav[4], *chart, *chart_empty, *chart_range;
static lv_obj_t *chart_from, *chart_to, *metrics[5], *setting_tabs[7];
static lv_chart_series_t *series;
static lv_coord_t chart_values[GD32_ENV_TREND_POINTS];
/* Large snapshots stay out of the 4 KiB LVGL thread stack. */
static struct gd32_env_trend trend;
static struct gd32_env_history history;
static struct gd32_env_thresholds draft;
static struct gd32_env_log_status status;
static lv_obj_t *threshold_panel, *clock_panel, *setting_hint;
static lv_obj_t *threshold_name[2], *threshold_value[2], *threshold_unit[2];
static lv_obj_t *clock_fields[6], *clock_value, *clock_full;
static lv_obj_t *history_labels[GD32_ENV_HISTORY_ROWS][6], *history_hint, *history_page;
static lv_obj_t *confirm_overlay, *history_previous, *history_next, *history_clear;
static lv_obj_t *history_detail_overlay, *history_detail_text;
static unsigned active_page, metric, setting_group, clock_field;
static lv_obj_t *storage_panel, *interval_value, *interval_unit_buttons[3], *interval_presets[5];
static lv_obj_t *trend_panel, *trend_interval_value;
static lv_obj_t *trend_interval_unit_buttons[3], *trend_interval_presets[5];
/* The backend stores seconds.  The UI keeps a human-friendly amount/unit pair. */
static rt_uint32_t interval_amount = 10;
static unsigned interval_unit = 1; /* 0=seconds, 1=minutes, 2=hours */
static rt_uint32_t trend_interval_amount = 10;
static unsigned trend_interval_unit = 1; /* 0=seconds, 1=minutes, 2=hours */
static int date_fields[6];
static rt_uint32_t requested_page, history_revision = (rt_uint32_t)-1;
static rt_uint32_t trend_revision = (rt_uint32_t)-1;
static rt_bool_t setting_notice;
static rt_bool_t time_pending;
static rt_uint32_t time_request_revision;
static const char *const tab_names[] = {"\351\246\226\351\241\265", "\346\233\262\347\272\277", "\350\256\276\347\275\256", "\345\216\206\345\217\262"};
static const char *const metric_names[] = {"\346\270\251\345\272\246", "\346\271\277\345\272\246", "\345\205\211\347\205\247", "eCO2", "TVOC"};

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, rt_uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, const char *text, rt_uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, &ui_font_14, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_text(obj, text);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h,
                        const char *text, lv_event_cb_t cb, unsigned id)
{
    lv_obj_t *obj = panel(parent, x, y, w, h, CARD);
    lv_obj_t *caption = label(obj, 0, 0, text, TEXT);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x304b5b), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(obj, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_center(caption);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, (void *)(rt_ubase_t)id);
    return obj;
}

static void selected(lv_obj_t *obj, rt_bool_t yes)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(yes ? 0x275349 : CARD), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(obj, 0), lv_color_hex(yes ? GREEN : TEXT), 0);
}

static void enabled(lv_obj_t *obj, rt_bool_t yes)
{
    if (yes) lv_obj_clear_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}

static void decimal(char *text, rt_size_t size, rt_int32_t raw)
{
    rt_uint32_t magnitude = raw < 0 ? 0U - (rt_uint32_t)raw : (rt_uint32_t)raw;
    rt_snprintf(text, size, "%s%lu.%lu", raw < 0 ? "-" : "",
                (unsigned long)(magnitude / 100), (unsigned long)((magnitude % 100) / 10));
}

static rt_int32_t metric_value(const struct gd32_env_record *r)
{
    /* 曲线内部统一使用整数缩放值；光照在这里降为 lux 以匹配 Y 轴显示。 */
    switch (metric)
    {
    case 0: return r->temperature_x100;
    case 1: return r->humidity_x100;
    case 2: return (rt_int32_t)(r->lux_x10 / 10U);
    case 3: return r->eco2_ppm;
    default: return r->tvoc_ppb;
    }
}

static void trend_period_text(char *text, rt_size_t size)
{
    rt_uint32_t seconds = trend.interval_seconds;
    if (seconds == 0U) seconds = 600U;
    if (seconds % 3600U == 0U)
        rt_snprintf(text, size, "%lu\345\260\217\346\227\266", (unsigned long)(seconds / 3600U));
    else if (seconds % 60U == 0U)
        rt_snprintf(text, size, "%lu\345\210\206\351\222\237", (unsigned long)(seconds / 60U));
    else
        rt_snprintf(text, size, "%lu\347\247\222", (unsigned long)seconds);
}

static void trend_description(char *text, rt_size_t size)
{
    char period[24];
    trend_period_text(period, sizeof(period));
    rt_snprintf(text, size, "\346\234\200\350\277\221 %u\347\202\271 \302\267 \346\257\217 %s \351\207\207\346\240\267",
                GD32_ENV_TREND_POINTS, period);
}

static void metric_text(char *text, rt_size_t size, rt_int32_t raw)
{
    if (metric < 2) decimal(text, size, raw);
    else rt_snprintf(text, size, "%ld", (long)raw);
}

static void update_chart(void)
{
    rt_int32_t low = 0, high = 0;
    rt_uint8_t valid = metric < 2 ? GD32_ENV_VALID_SHT40 :
                       metric == 2 ? GD32_ENV_VALID_LIGHT : GD32_ENV_VALID_GAS;
    unsigned i, count = 0;
    char a[24], b[24], period[24], text[112];
    static const char *const units[] = {"\302\260C", "%", "lux", "ppm", "ppb"};
    /* 趋势数据由 env_log 服务复制；没有有效数据时保留空图提示。 */
    if (gd32_env_trend_get(&trend) != RT_EOK) return;
    if (trend_revision == trend.revision) return;
    trend_revision = trend.revision;
    for (i = 0; i < GD32_ENV_TREND_POINTS; i++)
    {
        if (trend.points[i].valid_flags & valid)
        {
            rt_int32_t value = metric_value(&trend.points[i]);
            if (count == 0 || value < low) low = value;
            if (count == 0 || value > high) high = value;
            count++;
        }
    }
    for (i = 0; i < GD32_ENV_TREND_POINTS; i++)
    {
        if (trend.points[i].valid_flags & valid)
        {
            rt_int64_t value = (rt_int64_t)metric_value(&trend.points[i]) - low;
            chart_values[i] = high == low ? 500 :
                (lv_coord_t)(value * 1000 / ((rt_int64_t)high - low));
        }
        else chart_values[i] = LV_CHART_POINT_NONE;
    }
    if (count)
    {
        metric_text(a, sizeof(a), low);
        metric_text(b, sizeof(b), high);
        trend_period_text(period, sizeof(period));
        rt_snprintf(text, sizeof(text), "\346\234\200\344\275\216 %s / \346\234\200\351\253\230 %s %s \302\267 %s", a, b, units[metric], period);
        lv_label_set_text(chart_range, text);
        lv_obj_add_flag(chart_empty, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        trend_description(text, sizeof(text));
        lv_label_set_text(chart_range, text);
        lv_label_set_text(chart_empty, trend.rtc_set ? "\346\232\202\346\227\240\346\234\211\346\225\210\346\225\260\346\215\256\357\274\214\347\274\272\346\265\213\344\277\235\347\225\231\347\251\272\347\231\275" : "\350\257\267\345\205\210\345\234\250\350\256\276\347\275\256\351\241\265\346\240\241\345\207\206\346\227\266\351\227\264");
        lv_obj_clear_flag(chart_empty, LV_OBJ_FLAG_HIDDEN);
    }
    if (trend.rtc_set)
    {
        struct tm tm;
        time_t value = trend.points[0].timestamp;
        if (localtime_r(&value, &tm))
        {
            rt_snprintf(text, sizeof(text), "%02d-%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
            lv_label_set_text(chart_from, text);
        }
        value = trend.points[GD32_ENV_TREND_POINTS - 1].timestamp;
        if (localtime_r(&value, &tm))
        {
            rt_snprintf(text, sizeof(text), "%02d-%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
            lv_label_set_text(chart_to, text);
        }
    }
    else
    {
        lv_label_set_text(chart_from, "--");
        lv_label_set_text(chart_to, "\347\216\260\345\234\250");
    }
    lv_chart_refresh(chart);
}

static void metric_event(lv_event_t *event)
{
    unsigned i;
    metric = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    for (i = 0; i < 5; i++) selected(metrics[i], i == metric);
    trend_revision = (rt_uint32_t)-1;
    update_chart();
}

static int month_days(int year, int month)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    /* 调用方已将月份限制在 1..12；这里只处理 2000..2099，闰年规则
     * 与 RTC 驱动保持一致（该范围内无需世纪例外）。 */
    return days[month - 1] + (month == 2 && (year % 4 == 0));
}

static void clock_display(void)
{
    char text[56];
    rt_snprintf(text, sizeof(text), "%04d-%02d-%02d  %02d:%02d:%02d",
                date_fields[0], date_fields[1], date_fields[2],
                date_fields[3], date_fields[4], date_fields[5]);
    lv_label_set_text(clock_full, text);
    rt_snprintf(text, sizeof(text), clock_field == 0 ? "%04d" : "%02d", date_fields[clock_field]);
    lv_label_set_text(clock_value, text);
    lv_obj_center(clock_value);
}

static void clock_load(void)
{
    struct tm tm;
    time_t now = status.timestamp ? status.timestamp : 1767225600U;
    rt_memset(&tm, 0, sizeof(tm));
    if (!localtime_r(&now, &tm)) return;
    date_fields[0] = tm.tm_year + 1900;
    date_fields[1] = tm.tm_mon + 1;
    date_fields[2] = tm.tm_mday;
    date_fields[3] = tm.tm_hour;
    date_fields[4] = tm.tm_min;
    date_fields[5] = tm.tm_sec;
    clock_display();
}

static rt_int32_t threshold_get(unsigned row)
{
    if (setting_group == 0) return row ? draft.temperature_high_x100 : draft.temperature_low_x100;
    if (setting_group == 1) return row ? draft.humidity_high_x100 : draft.humidity_low_x100;
    if (setting_group == 2) return row ? (rt_int32_t)draft.lux_high_x10 : (rt_int32_t)draft.lux_low_x10;
    return row ? draft.tvoc_high_ppb : draft.eco2_high_ppm;
}

static void threshold_display(void)
{
    unsigned row;
    for (row = 0; row < 2; row++)
    {
        char text[24];
        rt_int32_t value = threshold_get(row);
        if (setting_group < 2) decimal(text, sizeof(text), value);
        else if (setting_group == 2) rt_snprintf(text, sizeof(text), "%ld", (long)(value / 10));
        else rt_snprintf(text, sizeof(text), "%ld", (long)value);
        lv_label_set_text(threshold_value[row], text);
        lv_label_set_text(threshold_name[row], setting_group == 3 ?
                          (row ? "TVOC\344\270\212\351\231\220" : "eCO2\344\270\212\351\231\220") : (row ? "\346\212\245\350\255\246\344\270\212\351\231\220" : "\346\212\245\350\255\246\344\270\213\351\231\220"));
        lv_label_set_text(threshold_unit[row], setting_group == 0 ? "\302\260C" :
                          setting_group == 1 ? "%" :
                          setting_group == 2 ? "lux" : row ? "ppb" : "ppm");
    }
}

static void setting_group_event(lv_event_t *event)
{
    unsigned i;
    setting_group = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    for (i = 0; i < 7; i++) selected(setting_tabs[i], i == setting_group);
    lv_obj_add_flag(storage_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(trend_panel, LV_OBJ_FLAG_HIDDEN);
    if (setting_group == 4)
    {
        lv_obj_add_flag(threshold_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(clock_panel, LV_OBJ_FLAG_HIDDEN);
        clock_load();
    }
    else if (setting_group == 5)
    {
        lv_obj_add_flag(threshold_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(clock_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(storage_panel, LV_OBJ_FLAG_HIDDEN);
    }
    else if (setting_group == 6)
    {
        lv_obj_add_flag(threshold_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(clock_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(trend_panel, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(threshold_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(clock_panel, LV_OBJ_FLAG_HIDDEN);
        threshold_display();
    }
    setting_notice = RT_FALSE;
}

static void adjust_event(lv_event_t *event)
{
    unsigned id = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    unsigned row = id / 2;
    rt_int32_t value = threshold_get(row), minimum, maximum;
    rt_int32_t step = setting_group == 0 ? 50 :
                      setting_group == 1 ? 100 :
                      setting_group == 2 ? 100 : row ? 10 : 50;
    minimum = setting_group == 0 ? -4000 :
              setting_group == 3 && row == 0 ? 400 : 0;
    maximum = setting_group == 0 ? 12500 :
              setting_group == 1 ? 10000 :
              setting_group == 2 ? 2000000 : 60000;
    value += id % 2 ? step : -step;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    if (setting_group == 0)
    {
        if (row) draft.temperature_high_x100 = value;
        else draft.temperature_low_x100 = value;
    }
    else if (setting_group == 1)
    {
        if (row) draft.humidity_high_x100 = (rt_uint16_t)value;
        else draft.humidity_low_x100 = (rt_uint16_t)value;
    }
    else if (setting_group == 2)
    {
        if (row) draft.lux_high_x10 = (rt_uint32_t)value;
        else draft.lux_low_x10 = (rt_uint32_t)value;
    }
    else
    {
        if (row) draft.tvoc_high_ppb = (rt_uint16_t)value;
        else draft.eco2_high_ppm = (rt_uint16_t)value;
    }
    threshold_display();
    lv_label_set_text(setting_hint, "\345\217\202\346\225\260\345\267\262\344\277\256\346\224\271\357\274\214\347\202\271\345\207\273\344\277\235\345\255\230\345\220\216\347\224\237\346\225\210");
    setting_notice = RT_TRUE;
}

static void clock_field_event(lv_event_t *event)
{
    unsigned i;
    clock_field = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    for (i = 0; i < 6; i++) selected(clock_fields[i], i == clock_field);
    clock_display();
}

static void clock_adjust_event(lv_event_t *event)
{
    static const int lower[] = {2000,1,1,0,0,0};
    static const int upper[] = {2099,12,31,23,59,59};
    int delta = (unsigned)(rt_ubase_t)lv_event_get_user_data(event) ? 1 : -1;
    /* 修改年/月后立即重新限制日期，避免出现 2 月 31 日等非法组合。 */
    int max = clock_field == 2 ? month_days(date_fields[0], date_fields[1]) : upper[clock_field];
    date_fields[clock_field] += delta;
    if (date_fields[clock_field] < lower[clock_field]) date_fields[clock_field] = lower[clock_field];
    if (date_fields[clock_field] > max) date_fields[clock_field] = max;
    max = month_days(date_fields[0], date_fields[1]);
    if (date_fields[2] > max) date_fields[2] = max;
    clock_display();
    lv_label_set_text(setting_hint, "\346\227\266\351\227\264\345\267\262\344\277\256\346\224\271\357\274\214\347\202\271\345\207\273\344\277\235\345\255\230\345\220\216\347\224\237\346\225\210");
    setting_notice = RT_TRUE;
}

static rt_uint32_t interval_seconds(void)
{
    rt_uint64_t seconds = interval_amount;
    if (interval_unit == 1) seconds *= 60U;
    else if (interval_unit == 2) seconds *= 3600U;
    /* 后端统一保存秒数，UI 的秒/分/时选择仅影响这里的换算。 */
    if (seconds < 1U) seconds = 1U;
    if (seconds > 86400U) seconds = 86400U;
    return (rt_uint32_t)seconds;
}

static void interval_load(rt_uint32_t seconds)
{
    if (seconds == 0U) seconds = 600U;
    if (seconds % 3600U == 0U && seconds / 3600U <= 24U)
    {
        interval_unit = 2;
        interval_amount = seconds / 3600U;
    }
    else if (seconds % 60U == 0U)
    {
        interval_unit = 1;
        interval_amount = seconds / 60U;
    }
    else
    {
        interval_unit = 0;
        interval_amount = seconds;
    }
    if (interval_amount == 0U) interval_amount = 1U;
}

static void interval_display(void)
{
    char text[24];
    static const unsigned presets[3][5] = {
        {1, 5, 10, 30, 60},
        {1, 5, 10, 30, 60},
        {1, 2, 4, 8, 24}
    };
    unsigned i;
    rt_snprintf(text, sizeof(text), "%lu", (unsigned long)interval_amount);
    lv_label_set_text(interval_value, text);
    lv_obj_center(interval_value);
    for (i = 0; i < 3; i++) selected(interval_unit_buttons[i], i == interval_unit);
    for (i = 0; i < 5; i++)
    {
        rt_snprintf(text, sizeof(text), "%u", presets[interval_unit][i]);
        lv_label_set_text(lv_obj_get_child(interval_presets[i], 0), text);
    }
}

static void interval_unit_event(lv_event_t *event)
{
    interval_unit = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    if (interval_unit > 2U) interval_unit = 1U;
    if (interval_unit == 0U && interval_amount > 86400U) interval_amount = 86400U;
    if (interval_unit == 1U && interval_amount > 1440U) interval_amount = 1440U;
    if (interval_unit == 2U && interval_amount > 24U) interval_amount = 24U;
    interval_display();
    lv_label_set_text(setting_hint, "\345\255\230\345\202\250\345\215\225\344\275\215\345\267\262\345\210\207\346\215\242\357\274\214\347\202\271\345\207\273\344\277\235\345\255\230\345\220\216\347\224\237\346\225\210");
    setting_notice = RT_TRUE;
}

static void interval_event(lv_event_t *event)
{
    unsigned id = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    static const unsigned presets[3][5] = {
        {1, 5, 10, 30, 60},
        {1, 5, 10, 30, 60},
        {1, 2, 4, 8, 24}
    };
    unsigned maximum = interval_unit == 0U ? 86400U : interval_unit == 1U ? 1440U : 24U;
    if (id >= 2 && id < 7) interval_amount = presets[interval_unit][id - 2];
    else if (id == 0 && interval_amount > 1U) interval_amount--;
    else if (id == 1 && interval_amount < maximum) interval_amount++;
    interval_display();
    lv_label_set_text(setting_hint, "\351\227\264\351\232\224\345\267\262\344\277\256\346\224\271\357\274\214\347\202\271\345\207\273\344\277\235\345\255\230\345\220\216\347\224\237\346\225\210");
    setting_notice = RT_TRUE;
}

/* 曲线采样周期使用与 SD 保存周期相同的秒/分钟/小时编辑方式，
 * 但状态和提交接口完全独立，修改曲线周期不会改变 CSV 写入计划。 */
static rt_uint32_t trend_interval_seconds(void)
{
    rt_uint64_t seconds = trend_interval_amount;
    if (trend_interval_unit == 1U) seconds *= 60U;
    else if (trend_interval_unit == 2U) seconds *= 3600U;
    if (seconds < 1U) seconds = 1U;
    if (seconds > 86400U) seconds = 86400U;
    return (rt_uint32_t)seconds;
}

static void trend_interval_load(rt_uint32_t seconds)
{
    if (seconds == 0U) seconds = 600U;
    if (seconds % 3600U == 0U && seconds / 3600U <= 24U)
    {
        trend_interval_unit = 2;
        trend_interval_amount = seconds / 3600U;
    }
    else if (seconds % 60U == 0U)
    {
        trend_interval_unit = 1;
        trend_interval_amount = seconds / 60U;
    }
    else
    {
        trend_interval_unit = 0;
        trend_interval_amount = seconds;
    }
    if (trend_interval_amount == 0U) trend_interval_amount = 1U;
}

static void trend_interval_display(void)
{
    char text[24];
    static const unsigned presets[3][5] = {
        {1, 5, 10, 30, 60},
        {1, 5, 10, 30, 60},
        {1, 2, 4, 8, 24}
    };
    unsigned i;
    rt_snprintf(text, sizeof(text), "%lu", (unsigned long)trend_interval_amount);
    lv_label_set_text(trend_interval_value, text);
    lv_obj_center(trend_interval_value);
    for (i = 0; i < 3; i++) selected(trend_interval_unit_buttons[i], i == trend_interval_unit);
    for (i = 0; i < 5; i++)
    {
        rt_snprintf(text, sizeof(text), "%u", presets[trend_interval_unit][i]);
        lv_label_set_text(lv_obj_get_child(trend_interval_presets[i], 0), text);
    }
}

static void trend_interval_unit_event(lv_event_t *event)
{
    trend_interval_unit = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    if (trend_interval_unit > 2U) trend_interval_unit = 1U;
    if (trend_interval_unit == 0U && trend_interval_amount > 86400U) trend_interval_amount = 86400U;
    if (trend_interval_unit == 1U && trend_interval_amount > 1440U) trend_interval_amount = 1440U;
    if (trend_interval_unit == 2U && trend_interval_amount > 24U) trend_interval_amount = 24U;
    trend_interval_display();
    lv_label_set_text(setting_hint, "\346\233\262\347\272\277\345\215\225\344\275\215\345\267\262\345\210\207\346\215\242\357\274\214\347\202\271\345\207\273\344\277\235\345\255\230\345\220\216\347\224\237\346\225\210");
    setting_notice = RT_TRUE;
}

static void trend_interval_event(lv_event_t *event)
{
    unsigned id = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    static const unsigned presets[3][5] = {
        {1, 5, 10, 30, 60},
        {1, 5, 10, 30, 60},
        {1, 2, 4, 8, 24}
    };
    unsigned maximum = trend_interval_unit == 0U ? 86400U : trend_interval_unit == 1U ? 1440U : 24U;
    if (id >= 2 && id < 7) trend_interval_amount = presets[trend_interval_unit][id - 2];
    else if (id == 0 && trend_interval_amount > 1U) trend_interval_amount--;
    else if (id == 1 && trend_interval_amount < maximum) trend_interval_amount++;
    trend_interval_display();
    lv_label_set_text(setting_hint, "\351\207\207\346\240\267\345\221\250\346\234\237\345\267\262\344\277\256\346\224\271\357\274\214\347\202\271\345\207\273\344\277\235\345\255\230\345\220\216\347\224\237\346\225\210");
    setting_notice = RT_TRUE;
}

static void save_event(lv_event_t *event)
{
    rt_err_t result;
    RT_UNUSED(event);
    if (setting_group == 4)
    {
        struct tm tm;
        time_t timestamp;
        rt_kprintf("TIME UI: save %04d-%02d-%02d %02d:%02d:%02d\n",
                   date_fields[0], date_fields[1], date_fields[2],
                   date_fields[3], date_fields[4], date_fields[5]);
        rt_memset(&tm, 0, sizeof(tm));
        tm.tm_year = date_fields[0] - 1900;
        tm.tm_mon = date_fields[1] - 1;
        tm.tm_mday = date_fields[2];
        tm.tm_hour = date_fields[3];
        tm.tm_min = date_fields[4];
        tm.tm_sec = date_fields[5];
        timestamp = mktime(&tm);
        rt_kprintf("TIME UI: queue timestamp=%lu\n", (unsigned long)timestamp);
        result = gd32_env_time_set_request((rt_uint32_t)timestamp);
        time_pending = result == RT_EOK;
        time_request_revision = status.time_revision;
        lv_label_set_text(setting_hint, result == RT_EOK ? "\346\255\243\345\234\250\346\240\241\346\227\266\357\274\214\350\257\267\347\250\215\345\200\231" : "\346\240\241\346\227\266\350\257\267\346\261\202\345\244\261\350\264\245\357\274\214\350\257\267\347\250\215\345\220\216\351\207\215\350\257\225");
        setting_notice = RT_TRUE;
        rt_kprintf("TIME UI: callback done, result=%d\n", result);
        gd32_env_log_diag_print();
    }
    else if (setting_group == 5)
    {
        result = gd32_env_log_interval_set(interval_seconds());
        lv_label_set_text(setting_hint, result == RT_EOK ? "\344\277\235\345\255\230\351\227\264\351\232\224\345\267\262\347\224\237\346\225\210\357\274\214\347\255\211\345\276\205\345\206\231\345\205\245SD\345\215\241" : "\344\277\235\345\255\230\345\244\261\350\264\245\357\274\214\350\257\267\347\250\215\345\220\216\351\207\215\350\257\225");
        setting_notice = result != RT_EOK;
    }
    else if (setting_group == 6)
    {
        result = gd32_env_trend_interval_set(trend_interval_seconds());
        lv_label_set_text(setting_hint, result == RT_EOK ? "\346\233\262\347\272\277\345\221\250\346\234\237\345\267\262\347\224\237\346\225\210\357\274\214\347\255\211\345\276\205\345\206\231\345\205\245SD\345\215\241" : "\346\233\262\347\272\277\345\221\250\346\234\237\344\277\256\346\224\271\345\244\261\350\264\245\357\274\214\350\257\267\347\250\215\345\220\216\351\207\215\350\257\225");
        setting_notice = result != RT_EOK;
    }
    else
    {
        result = gd32_env_thresholds_set(&draft);
        lv_label_set_text(setting_hint, result == RT_EOK ? "\351\230\210\345\200\274\345\267\262\347\224\237\346\225\210\357\274\214\347\255\211\345\276\205\345\206\231\345\205\245SD\345\215\241" : "\344\277\235\345\255\230\345\244\261\350\264\245\357\274\232\350\257\267\346\243\200\346\237\245\344\270\212\344\270\213\351\231\220\350\214\203\345\233\264");
        setting_notice = result != RT_EOK;
    }
}

static void restore_event(lv_event_t *event)
{
    RT_UNUSED(event);
    /* 保存后立即撤销也使用最新快照，不等待下一次页面定时刷新。 */
    (void)gd32_env_log_get_status(&status);
    if (setting_group == 4) clock_load();
    else if (setting_group == 5)
    {
        interval_load(status.log_interval_seconds);
        interval_display();
    }
    else if (setting_group == 6)
    {
        trend_interval_load(status.trend_interval_seconds);
        trend_interval_display();
    }
    else
    {
        (void)gd32_env_thresholds_get(&draft);
        threshold_display();
    }
    setting_notice = RT_FALSE;
}

static void history_request(rt_uint32_t page)
{
    /* 读历史是异步请求；页面只显示 busy 状态，不在触摸回调里读文件。 */
    if (gd32_env_history_request(page) == RT_EOK)
    {
        requested_page = page;
        lv_label_set_text(history_hint, "\346\255\243\345\234\250\350\257\273\345\217\226SD\345\215\241\357\274\214\350\257\267\347\250\215\345\200\231");
    }
    else lv_label_set_text(history_hint, "\350\257\273\345\217\226\350\257\267\346\261\202\345\244\261\350\264\245\357\274\214\350\257\267\347\250\215\345\220\216\351\207\215\350\257\225");
}

static void history_page_event(lv_event_t *event)
{
    unsigned id = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    if (id == 0)
    {
        if (requested_page) history_request(requested_page - 1);
    }
    else if (id == 1)
    {
        if ((requested_page + 1) * GD32_ENV_HISTORY_ROWS < history.total_rows)
            history_request(requested_page + 1);
    }
    else history_request(requested_page);
}

static void clear_confirm_event(lv_event_t *event)
{
    unsigned confirm = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    /* 清空请求同样异步执行，确认层先关闭，后台完成后通过 revision 刷新。 */
    if (confirm)
    {
        if (gd32_env_history_clear_request() == RT_EOK)
        {
            requested_page = 0;
            lv_label_set_text(history_hint, "\346\255\243\345\234\250\346\270\205\347\251\272\346\227\245\345\277\227\357\274\214\350\257\267\347\250\215\345\200\231");
        }
        else lv_label_set_text(history_hint, "\346\270\205\347\251\272\350\257\267\346\261\202\345\244\261\350\264\245\357\274\214\350\257\267\347\250\215\345\220\216\351\207\215\350\257\225");
    }
}

static void clear_event(lv_event_t *event)
{
    RT_UNUSED(event);
    lv_obj_clear_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(confirm_overlay);
}

static void detail_close_event(lv_event_t *event)
{
    RT_UNUSED(event);
    lv_obj_add_flag(history_detail_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void record_cells(const struct gd32_env_record *r, char text[6][32])
{
    struct tm tm;
    time_t timestamp = r->timestamp;
    unsigned i;
    for (i = 0; i < 6; i++) rt_snprintf(text[i], 32, "--");
    if (localtime_r(&timestamp, &tm))
        rt_snprintf(text[0], 32, "%02d-%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    if (r->valid_flags & GD32_ENV_VALID_SHT40)
    {
        decimal(text[1], 32, r->temperature_x100);
        decimal(text[2], 32, r->humidity_x100);
    }
    if (r->valid_flags & GD32_ENV_VALID_LIGHT)
    {
        if (r->lux_x10 < 1000000U) rt_snprintf(text[3], 32, "%lu", (unsigned long)(r->lux_x10 / 10U));
        else rt_snprintf(text[3], 32, "%luk", (unsigned long)(r->lux_x10 / 10000U));
    }
    if (r->valid_flags & GD32_ENV_VALID_GAS)
    {
        rt_snprintf(text[4], 32, "%u", r->eco2_ppm);
        rt_snprintf(text[5], 32, "%u", r->tvoc_ppb);
    }
}

static void history_row_event(lv_event_t *event)
{
    unsigned row = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    char values[6][32], text[256];
    struct tm tm;
    time_t timestamp;
    /* 只允许打开当前已读入的行；busy 期间避免读取正在替换的数组。 */
    if (row >= history.count || history.busy) return;
    record_cells(&history.rows[row], values);
    timestamp = history.rows[row].timestamp;
    rt_memset(&tm, 0, sizeof(tm));
    (void)localtime_r(&timestamp, &tm);
    rt_snprintf(text, sizeof(text),
                "%04d-%02d-%02d %02d:%02d:%02d\n\346\270\251\345\272\246  %s \302\260C     \346\271\277\345\272\246  %s %%\n\345\205\211\347\205\247  %s lux\neCO2  %s ppm     TVOC  %s ppb",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                values[1], values[2], values[3], values[4], values[5]);
    lv_label_set_text(history_detail_text, text);
    lv_obj_clear_flag(history_detail_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(history_detail_overlay);
}

static void update_history(void)
{
    unsigned row, col;
    char text[96], cells[6][32];
    if (gd32_env_history_get(&history) != RT_EOK) return;
    enabled(history_previous, !history.busy && history.page > 0);
    enabled(history_next, !history.busy && (history.page + 1) * GD32_ENV_HISTORY_ROWS < history.total_rows);
    enabled(history_clear, !history.busy && status.state == GD32_ENV_LOG_READY);
    if (history.busy)
    {
        lv_label_set_text(history_hint, "\346\255\243\345\234\250\345\244\204\347\220\206SD\345\215\241\357\274\214\350\257\267\347\250\215\345\200\231");
        return;
    }
    if (history_revision == history.revision) return;
    history_revision = history.revision;
    requested_page = history.page;
    for (row = 0; row < GD32_ENV_HISTORY_ROWS; row++)
    {
        if (row < history.count) record_cells(&history.rows[row], cells);
        for (col = 0; col < 6; col++)
            lv_label_set_text(history_labels[row][col], row < history.count ? cells[col] : "--");
    }
    rt_snprintf(text, sizeof(text), "%lu / %lu", (unsigned long)(history.page + 1),
                (unsigned long)(history.total_rows ? (history.total_rows + 3) / 4 : 1));
    lv_label_set_text(history_page, text);
    if (history.result != RT_EOK)
        lv_label_set_text(history_hint, "SD\345\215\241\346\234\252\345\260\261\347\273\252\346\210\226\350\257\273\345\217\226\345\244\261\350\264\245\357\274\214\350\257\267\346\217\222\345\215\241\345\220\216\345\210\267\346\226\260");
    else
    {
        rt_snprintf(text, sizeof(text), "\345\205\261 %lu \346\235\241 \302\267 \346\234\200\346\226\260\345\234\250\345\211\215 \302\267 \347\202\271\345\207\273\350\256\260\345\275\225\346\237\245\347\234\213\350\257\246\346\203\205", (unsigned long)history.total_rows);
        lv_label_set_text(history_hint, text);
    }
}

static void nav_event(lv_event_t *event)
{
    unsigned i;
    active_page = (unsigned)(rt_ubase_t)lv_event_get_user_data(event);
    for (i = 0; i < 4; i++)
    {
        if (i == active_page) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        selected(nav[i], i == active_page);
    }
    if (active_page == 1) { trend_revision = (rt_uint32_t)-1; update_chart(); }
    if (active_page == 2)
    {
        (void)gd32_env_log_get_status(&status);
        (void)gd32_env_thresholds_get(&draft);
        threshold_display();
        clock_load();
        interval_load(status.log_interval_seconds);
        interval_display();
        trend_interval_load(status.trend_interval_seconds);
        trend_interval_display();
        setting_notice = RT_FALSE;
    }
    if (active_page == 3) history_request(0);
}

void gd32_hmi_pages_update(void)
{
    /* 由主页每秒调用一次；只轮询服务状态，不做阻塞式 I/O。 */
    if (gd32_env_log_get_status(&status) != RT_EOK) return;
    if (active_page == 1) update_chart();
    if (active_page == 3) update_history();
    if (active_page == 2)
    {
        if (time_pending && status.time_revision != time_request_revision && !status.time_pending)
        {
            time_pending = RT_FALSE;
            lv_label_set_text(setting_hint, status.time_result == RT_EOK ?
                              "\346\227\266\351\227\264\345\267\262\346\240\241\345\207\206" : "\346\240\241\346\227\266\345\244\261\350\264\245\357\274\214\350\257\267\351\207\215\350\257\225");
        }
        if (!setting_notice)
            lv_label_set_text(setting_hint, setting_group == 6 ?
                (status.settings_pending ? "\346\233\262\347\272\277\345\221\250\346\234\237\345\267\262\347\224\237\346\225\210\357\274\214\345\276\205SD\345\215\241\344\277\235\345\255\230" : "\346\233\262\347\272\277\345\221\250\346\234\237\345\267\262\345\206\231\345\205\245SD\345\215\241") :
                setting_group == 5 ?
                (status.settings_pending ? "\351\227\264\351\232\224\345\267\262\347\224\237\346\225\210\357\274\214\345\276\205SD\345\215\241\344\277\235\345\255\230" : "\344\277\235\345\255\230\351\227\264\351\232\224\345\267\262\345\206\231\345\205\245SD\345\215\241") :
                setting_group == 4 ?
                (status.rtc_set ? "\351\200\211\346\213\251\345\271\264\346\234\210\346\227\245\346\227\266\345\210\206\347\247\222\357\274\214\346\214\211\345\212\240\345\207\217\350\260\203\346\225\264" : "\346\227\266\351\227\264\346\234\252\346\240\241\345\207\206\357\274\214\346\227\245\345\277\227\346\232\202\345\201\234\344\277\235\345\255\230") :
                status.settings_pending ? "\351\230\210\345\200\274\345\267\262\347\224\237\346\225\210\357\274\214\345\276\205SD\345\215\241\344\277\235\345\255\230" : "\351\230\210\345\200\274\345\267\262\344\277\235\345\255\230\345\210\260SD\345\215\241");
    }
}

void gd32_hmi_pages_init(lv_obj_t *screen, lv_obj_t *home)
{
    unsigned i, row, col;
    lv_obj_t *obj, *box;
    static const char *const groups[] = {"\346\270\251\345\272\246", "\346\271\277\345\272\246", "\345\205\211\347\205\247", "\346\260\224\344\275\223", "\346\227\266\351\227\264", "\345\255\230\345\202\250", "\346\233\262\347\272\277"};
    static const char *const fields[] = {"\345\271\264", "\346\234\210", "\346\227\245", "\346\227\266", "\345\210\206", "\347\247\222"};
    static const char *const headings[] = {"\346\227\266\351\227\264", "\302\260C", "%", "lux", "eCO2", "TVOC"};
    static const int xs[] = {5, 113, 178, 242, 315, 390};
    pages[0] = home;
    for (i = 1; i < 4; i++)
    {
        pages[i] = panel(screen, 0, 40, 480, 196, BG);
        lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (i = 0; i < 4; i++)
    {
        nav[i] = button(screen, 12 + i * 117, 236, 105, 34, tab_names[i], nav_event, i);
        selected(nav[i], i == 0);
    }
    for (i = 0; i < 5; i++)
    {
        metrics[i] = button(pages[1], 12 + i * 94, 0, 82, 28, metric_names[i], metric_event, i);
        selected(metrics[i], i == 0);
    }
    chart_range = label(pages[1], 16, 31, "\350\277\221 144\347\202\271 \302\267 \346\257\217 10\345\210\206\351\222\237 \351\207\207\346\240\267", MUTED);
    chart = lv_chart_create(pages[1]);
    lv_obj_set_pos(chart, 18, 54);
    lv_obj_set_size(chart, 444, 116);
    lv_obj_set_style_bg_color(chart, lv_color_hex(CARD), 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 6, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x304451), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 3, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, GD32_ENV_TREND_POINTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_div_line_count(chart, 3, 7);
    series = lv_chart_add_series(chart, lv_color_hex(GREEN), LV_CHART_AXIS_PRIMARY_Y);
    for (i = 0; i < GD32_ENV_TREND_POINTS; i++) chart_values[i] = LV_CHART_POINT_NONE;
    lv_chart_set_ext_y_array(chart, series, chart_values);
    chart_empty = label(pages[1], 98, 104, "\346\232\202\346\227\240\346\234\211\346\225\210\346\225\260\346\215\256\357\274\214\347\274\272\346\265\213\344\277\235\347\225\231\347\251\272\347\231\275", MUTED);
    chart_from = label(pages[1], 18, 175, "--", MUTED);
    chart_to = label(pages[1], 348, 175, "\347\216\260\345\234\250", MUTED);

    /* 七个设置标签都在 480 像素内，存储和曲线分别编辑、分别提交。 */
    for (i = 0; i < 7; i++)
    {
        setting_tabs[i] = button(pages[2], 5 + i * 68, 0, 64, 28, groups[i], setting_group_event, i);
        selected(setting_tabs[i], i == 0);
    }
    threshold_panel = panel(pages[2], 12, 34, 456, 102, BG);
    for (row = 0; row < 2; row++)
    {
        box = panel(threshold_panel, 0, row * 52, 456, 46, CARD);
        threshold_name[row] = label(box, 12, 13, row ? "\346\212\245\350\255\246\344\270\212\351\231\220" : "\346\212\245\350\255\246\344\270\213\351\231\220", MUTED);
        threshold_value[row] = label(box, 160, 10, "--", TEXT);
        lv_obj_set_style_text_font(threshold_value[row], &lv_font_montserrat_24, 0);
        threshold_unit[row] = label(box, 262, 15, "\302\260C", MUTED);
        for (i = 0; i < 2; i++)
        {
            obj = button(box, 324 + i * 66, 3, 56, 40, i ? "+" : "-", adjust_event, row * 2 + i);
            lv_obj_add_event_cb(obj, adjust_event, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(rt_ubase_t)(row * 2 + i));
        }
    }
    clock_panel = panel(pages[2], 12, 34, 456, 102, BG);
    for (i = 0; i < 6; i++)
    {
        clock_fields[i] = button(clock_panel, i * 76, 0, 70, 28, fields[i], clock_field_event, i);
        selected(clock_fields[i], i == 0);
    }
    box = panel(clock_panel, 153, 31, 150, 42, CARD);
    clock_value = label(box, 0, 0, "2026", TEXT);
    lv_obj_set_style_text_font(clock_value, &lv_font_montserrat_24, 0);
    lv_obj_center(clock_value);
    for (i = 0; i < 2; i++)
    {
        obj = button(clock_panel, i ? 311 : 83, 31, 62, 42, i ? "+" : "-", clock_adjust_event, i);
        lv_obj_add_event_cb(obj, clock_adjust_event, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(rt_ubase_t)i);
    }
    clock_full = label(clock_panel, 125, 79, "2026-01-01  00:00:00", MUTED);
    lv_obj_add_flag(clock_panel, LV_OBJ_FLAG_HIDDEN);
    storage_panel = panel(pages[2], 12, 34, 456, 102, BG);
    label(storage_panel, 10, 13, "\345\255\230\345\202\250\345\221\250\346\234\237", MUTED);
    box = panel(storage_panel, 145, 0, 96, 42, CARD);
    interval_value = label(box, 0, 0, "10", TEXT);
    lv_obj_set_style_text_font(interval_value, &lv_font_montserrat_24, 0);
    lv_obj_center(interval_value);
    {
        static const char *const unit_names[] = {"\347\247\222", "\345\210\206\351\222\237", "\345\260\217\346\227\266"};
        for (i = 0; i < 3; i++)
        {
            interval_unit_buttons[i] = button(storage_panel, 328 + i * 42, 1, 38, 40,
                                              unit_names[i], interval_unit_event, i);
            selected(interval_unit_buttons[i], i == interval_unit);
        }
    }
    for (i = 0; i < 2; i++)
    {
        obj = button(storage_panel, i ? 249 : 78, 0, 56, 42, i ? "+" : "-", interval_event, i);
        lv_obj_add_event_cb(obj, interval_event, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(rt_ubase_t)i);
    }
    {
        static const char *const presets[] = {"1", "5", "10", "30", "60"};
        for (i = 0; i < 5; i++)
        {
            interval_presets[i] = button(storage_panel, i * 92, 48, 84, 30, presets[i], interval_event, i + 2);
        }
    }
    label(storage_panel, 10, 84, "\350\214\203\345\233\2641\347\247\222\350\207\26324\345\260\217\346\227\266\357\274\214SD\345\215\241\350\207\252\345\212\250\344\277\235\345\255\230", MUTED);
    lv_obj_add_flag(storage_panel, LV_OBJ_FLAG_HIDDEN);
    trend_panel = panel(pages[2], 12, 34, 456, 102, BG);
    label(trend_panel, 10, 13, "\346\233\262\347\272\277\345\221\250\346\234\237", MUTED);
    box = panel(trend_panel, 145, 0, 96, 42, CARD);
    trend_interval_value = label(box, 0, 0, "10", TEXT);
    lv_obj_set_style_text_font(trend_interval_value, &lv_font_montserrat_24, 0);
    lv_obj_center(trend_interval_value);
    {
        static const char *const unit_names[] = {"\347\247\222", "\345\210\206\351\222\237", "\345\260\217\346\227\266"};
        for (i = 0; i < 3; i++)
        {
            trend_interval_unit_buttons[i] = button(trend_panel, 328 + i * 42, 1, 38, 40,
                                                    unit_names[i], trend_interval_unit_event, i);
            selected(trend_interval_unit_buttons[i], i == trend_interval_unit);
        }
    }
    for (i = 0; i < 2; i++)
    {
        obj = button(trend_panel, i ? 249 : 78, 0, 56, 42, i ? "+" : "-", trend_interval_event, i);
        lv_obj_add_event_cb(obj, trend_interval_event, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(rt_ubase_t)i);
    }
    {
        static const char *const presets[] = {"1", "5", "10", "30", "60"};
        for (i = 0; i < 5; i++)
            trend_interval_presets[i] = button(trend_panel, i * 92, 48, 84, 30, presets[i], trend_interval_event, i + 2);
    }
    label(trend_panel, 10, 84, "\350\214\203\345\233\2641\347\247\222\350\207\26324\345\260\217\346\227\266\357\274\214\346\233\262\347\272\277\344\277\235\347\225\231144\347\202\271", MUTED);
    lv_obj_add_flag(trend_panel, LV_OBJ_FLAG_HIDDEN);
    button(pages[2], 12, 140, 216, 34, "\346\222\244\351\224\200\344\277\256\346\224\271", restore_event, 0);
    obj = button(pages[2], 240, 140, 228, 34, "\344\277\235\345\255\230", save_event, 0);
    selected(obj, RT_TRUE);
    setting_hint = label(pages[2], 14, 178, "\346\255\243\345\234\250\345\212\240\350\275\275\345\217\202\346\225\260", MUTED);

    box = panel(pages[3], 12, 0, 456, 140, CARD);
    for (col = 0; col < 6; col++) label(box, xs[col], 3, headings[col], MUTED);
    for (row = 0; row < GD32_ENV_HISTORY_ROWS; row++)
    {
        obj = panel(box, 0, 23 + row * 29, 456, 28, row % 2 ? 0x20333f : CARD);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(obj, history_row_event, LV_EVENT_CLICKED, (void *)(rt_ubase_t)row);
        for (col = 0; col < 6; col++)
        {
            history_labels[row][col] = label(obj, xs[col], 5, "--", TEXT);
            lv_obj_set_style_text_font(history_labels[row][col], &lv_font_montserrat_12, 0);
        }
    }
    history_previous = button(pages[3], 12, 145, 72, 30, "\344\270\212\344\270\200\351\241\265", history_page_event, 0);
    history_page = label(pages[3], 92, 151, "1 / 1", MUTED);
    history_next = button(pages[3], 185, 145, 72, 30, "\344\270\213\344\270\200\351\241\265", history_page_event, 1);
    button(pages[3], 270, 145, 84, 30, "\345\210\267\346\226\260", history_page_event, 2);
    history_clear = button(pages[3], 366, 145, 102, 30, "\346\270\205\347\251\272\350\256\260\345\275\225", clear_event, 0);
    history_hint = label(pages[3], 14, 179, "SD\345\215\241 CSV \302\267 \346\214\211\350\256\276\347\275\256\351\227\264\351\232\224\344\277\235\345\255\230", MUTED);

    confirm_overlay = panel(screen, 0, 0, 480, 272, BG);
    lv_obj_set_style_bg_opa(confirm_overlay, LV_OPA_80, 0);
    lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_CLICKABLE);
    box = panel(confirm_overlay, 28, 42, 424, 188, CARD);
    label(box, 20, 18, "\346\270\205\347\251\272\345\205\250\351\203\250\345\216\206\345\217\262\350\256\260\345\275\225\357\274\237", AMBER);
    label(box, 20, 53, "\345\260\206\345\210\240\351\231\244\346\234\254\350\256\276\345\244\207\347\232\204CSV\346\227\245\345\277\227\345\222\214\346\233\262\347\272\277\350\256\260\345\275\225\343\200\202\n\346\255\244\346\223\215\344\275\234\346\227\240\346\263\225\346\222\244\351\224\200\357\274\214\351\230\210\345\200\274\350\256\276\347\275\256\344\277\235\347\225\231\343\200\202", TEXT);
    button(box, 20, 124, 180, 42, "\345\217\226\346\266\210", clear_confirm_event, 0);
    obj = button(box, 218, 124, 186, 42, "\347\241\256\350\256\244\346\270\205\347\251\272", clear_confirm_event, 1);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x703c3a), 0);
    lv_obj_add_flag(confirm_overlay, LV_OBJ_FLAG_HIDDEN);

    history_detail_overlay = panel(screen, 0, 0, 480, 272, BG);
    lv_obj_set_style_bg_opa(history_detail_overlay, LV_OPA_80, 0);
    lv_obj_add_flag(history_detail_overlay, LV_OBJ_FLAG_CLICKABLE);
    box = panel(history_detail_overlay, 24, 24, 432, 224, CARD);
    label(box, 18, 12, "\345\216\206\345\217\262\350\256\260\345\275\225\350\257\246\346\203\205", GREEN);
    history_detail_text = label(box, 18, 48, "--", TEXT);
    lv_obj_set_style_text_line_space(history_detail_text, 12, 0);
    button(box, 300, 177, 112, 36, "\345\205\263\351\227\255", detail_close_event, 0);
    lv_obj_add_flag(history_detail_overlay, LV_OBJ_FLAG_HIDDEN);
    (void)gd32_env_log_get_status(&status);
    (void)gd32_env_thresholds_get(&draft);
    threshold_display();
    clock_load();
    interval_load(status.log_interval_seconds);
    interval_display();
    trend_interval_load(status.trend_interval_seconds);
    trend_interval_display();
}
#endif
