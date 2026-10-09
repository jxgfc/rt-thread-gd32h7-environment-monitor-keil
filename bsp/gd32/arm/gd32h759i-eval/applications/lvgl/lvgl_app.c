/* 主界面与触摸输入：所有 LVGL 对象只能在 LVGL 线程中创建/更新。 */
#include <rtthread.h>
#include <rthw.h>

/*
 * Include rtthread.h before testing PKG_USING_LVGL.  The package options are
 * emitted by rtconfig.h, which is included from rtthread.h rather than being
 * compiler pre-defines.
 */
#ifdef PKG_USING_LVGL
#include <lvgl.h>
#include "sensor_service.h"
#include "drv_gt911.h"
#include "env_log.h"
#include "hmi_pages.h"
#include <sys/time.h>

rt_err_t lv_port_disp_init(void);
int gd32_lvgl_thread_start(void);

/* Subset fonts keep Chinese text in flash without a large runtime font cache.
 * String literals use UTF-8 byte escapes for ARMCC5's locale-dependent lexer. */
LV_FONT_DECLARE(ui_font_14);
LV_FONT_DECLARE(ui_font_18);

#define UI_BG       0x0d1720
#define UI_CARD     0x182936
#define UI_TEXT     0xebf1f6
#define UI_MUTED    0x90a7b5
#define UI_GREEN    0x41dcc4
#define UI_AMBER    0xf2bd68

enum sensor_state
{
    SENSOR_OFFLINE,
    SENSOR_READY,
    SENSOR_WARMING,
    SENSOR_STALE,
    SENSOR_UNAVAILABLE,
    SENSOR_WAITING
};

struct metric_card
{
    lv_obj_t *status;
    lv_obj_t *value;
    lv_obj_t *unit;
};

static struct metric_card s_cards[4];
static lv_obj_t *s_tvoc;
static lv_obj_t *s_device_button;
static lv_obj_t *s_device_label;
static lv_obj_t *s_footer;
static lv_obj_t *s_home;
static lv_obj_t *s_clock;
static lv_obj_t *s_storage;
static lv_obj_t *s_overlay;
static lv_obj_t *s_device_states[3];
static lv_obj_t *s_device_hint;

/* 这些辅助函数只负责创建控件并设置样式，不执行 I2C/文件系统操作，
 * 避免把可能阻塞的硬件访问放进 LVGL 重绘路径。 */
static lv_obj_t *make_panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                            lv_coord_t width, lv_coord_t height, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                            const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static uint32_t state_color(enum sensor_state state)
{
    if (state == SENSOR_READY)
        return UI_GREEN;
    if (state == SENSOR_WAITING)
        return UI_MUTED;
    return UI_AMBER;
}

static void set_status(lv_obj_t *label, enum sensor_state state)
{
    static const char * const names[] =
    {
        "\346\234\252\350\277\236\346\216\245", "\345\234\250\347\272\277", "\351\242\204\347\203\255\344\270\255", "\345\267\262\350\277\207\346\234\237", "\346\234\252\345\220\257\345\212\250", "\347\255\211\345\276\205\344\270\255"
    };
    lv_label_set_text(label, names[state]);
    lv_obj_set_style_text_color(label, lv_color_hex(state_color(state)), 0);
}

static void make_card(unsigned index, lv_coord_t x, lv_coord_t y,
                      const char *title, const char *source, uint32_t accent)
{
    lv_obj_t *panel = make_panel(s_home, x, y, 224, 76, UI_CARD);
    lv_obj_t *mark = make_panel(panel, 0, 12, 3, 16, accent);
    lv_obj_set_style_radius(mark, 1, 0);
    make_label(panel, 14, 4, &ui_font_14, UI_TEXT, title);
    s_cards[index].status = make_label(panel, 151, 4, &ui_font_14, UI_MUTED, "\347\255\211\345\276\205\344\270\255");
    lv_obj_set_width(s_cards[index].status, 59);
    lv_obj_set_style_text_align(s_cards[index].status, LV_TEXT_ALIGN_RIGHT, 0);
    if (index == 3)
    {
        make_label(panel, 14, 27, &lv_font_montserrat_12, UI_MUTED, "eCO2 / ppm");
        make_label(panel, 126, 27, &lv_font_montserrat_12, UI_MUTED, "TVOC / ppb");
        s_cards[index].value = make_label(panel, 14, 45, &lv_font_montserrat_24, UI_TEXT, "--");
        s_tvoc = make_label(panel, 126, 45, &lv_font_montserrat_24, UI_TEXT, "--");
        make_panel(panel, 111, 27, 1, 42, 0x304451);
    }
    else
    {
        s_cards[index].value = make_label(panel, 14, 25, &lv_font_montserrat_24, UI_TEXT, "--");
        s_cards[index].unit = make_label(panel, 150, 36, &ui_font_14, UI_MUTED,
                                        index == 0 ? "\302\260C" : index == 1 ? "%" : "lux");
        make_label(panel, 14, 58, &lv_font_montserrat_12, UI_MUTED, source);
    }
}

static void overlay_event(lv_event_t *event)
{
    RT_UNUSED(event);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void device_event(lv_event_t *event)
{
    RT_UNUSED(event);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void make_device_overlay(void)
{
    lv_obj_t *panel;
    lv_obj_t *close_button;
    lv_obj_t *label;
    unsigned i;
    static const char * const sensors[] =
    {
        "SHT40   \346\270\251\345\272\246 / \346\271\277\345\272\246", "BH1750  \347\216\257\345\242\203\345\205\211\347\205\247", "SGP30   \347\251\272\346\260\224\346\214\207\346\240\207"
    };
    s_overlay = make_panel(lv_scr_act(), 0, 0, 480, 272, 0x030a10);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    panel = make_panel(s_overlay, 24, 16, 432, 240, UI_CARD);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    make_label(panel, 18, 14, &ui_font_18, UI_TEXT, "\350\256\276\345\244\207\347\212\266\346\200\201");
    close_button = make_panel(panel, 348, 8, 72, 36, 0x29414f);
    lv_obj_add_flag(close_button, LV_OBJ_FLAG_CLICKABLE);
    label = make_label(close_button, 0, 0, &ui_font_14, UI_TEXT, "\345\205\263\351\227\255");
    lv_obj_center(label);
    lv_obj_add_event_cb(close_button, overlay_event, LV_EVENT_CLICKED, RT_NULL);
    for (i = 0; i < 3; i++)
    {
        lv_coord_t y = (lv_coord_t)(57 + i * 38);
        make_label(panel, 18, y, &ui_font_14, UI_TEXT, sensors[i]);
        s_device_states[i] = make_label(panel, 342, y, &ui_font_14, UI_MUTED, "\347\255\211\345\276\205\344\270\255");
        lv_obj_set_width(s_device_states[i], 72);
        lv_obj_set_style_text_align(s_device_states[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    make_panel(panel, 18, 167, 396, 1, 0x304451);
    s_device_hint = make_label(panel, 18, 178, &ui_font_14, UI_MUTED, "");
    make_label(panel, 18, 207, &ui_font_14, UI_MUTED, "eCO2 \344\270\272\347\255\211\346\225\210\344\274\260\347\256\227\345\200\274\357\274\214\351\235\236\347\233\264\346\216\245\346\265\213\351\207\217\343\200\202");
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void set_value(unsigned index, const char *text, enum sensor_state state)
{
    lv_label_set_text(s_cards[index].value, text);
    lv_obj_set_style_text_color(s_cards[index].value,
                                lv_color_hex(state == SENSOR_READY ? UI_TEXT : UI_MUTED), 0);
    if (s_cards[index].unit != RT_NULL)
        lv_obj_align_to(s_cards[index].unit, s_cards[index].value,
                         LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -4);
}

/* Snapshot access and all widget updates run in the LVGL thread. */
static void update_dashboard(lv_timer_t *timer)
{
    struct gd32_sensor_snapshot data;
    enum sensor_state states[3];
    rt_bool_t available = RT_FALSE;
    rt_bool_t stale;
    rt_bool_t waiting;
    rt_uint32_t magnitude;
    unsigned detected = 0;
    unsigned i;
    char text[40];
    const char *hint;
    RT_UNUSED(timer);
    /* 定时器每秒运行一次；快照由传感器线程写入，接口内部负责加锁复制。 */
    rt_memset(&data, 0, sizeof(data));
#ifdef BSP_USING_SENSOR_SERVICE
    available = gd32_sensor_snapshot_get(&data) == RT_EOK;
#endif
    waiting = available && data.tick == 0 && !data.sht40_valid &&
              !data.bh1750_valid && !data.sgp30_valid && !data.sgp30_warming;
    stale = available && !waiting &&
            (rt_tick_t)(rt_tick_get() - data.tick) > (rt_tick_t)(5U * RT_TICK_PER_SECOND);
    states[0] = data.sht40_valid ? SENSOR_READY : SENSOR_OFFLINE;
    states[1] = data.bh1750_valid ? SENSOR_READY : SENSOR_OFFLINE;
    states[2] = data.sgp30_valid ? SENSOR_READY :
                data.sgp30_warming ? SENSOR_WARMING : SENSOR_OFFLINE;
    for (i = 0; i < 3; i++)
    {
        if (!available)
            states[i] = SENSOR_UNAVAILABLE;
        else if (waiting)
            states[i] = SENSOR_WAITING;
        else if (stale)
            states[i] = SENSOR_STALE;
        if (states[i] == SENSOR_READY || states[i] == SENSOR_WARMING)
            detected++;
        set_status(s_device_states[i], states[i]);
    }
    set_status(s_cards[0].status, states[0]);
    set_status(s_cards[1].status, states[0]);
    set_status(s_cards[2].status, states[1]);
    set_status(s_cards[3].status, states[2]);

    if (states[0] == SENSOR_READY)
    {
        /* Unsigned negation also handles INT32_MIN without signed overflow. */
        magnitude = data.temperature_x100 < 0 ?
                    0U - (rt_uint32_t)data.temperature_x100 : (rt_uint32_t)data.temperature_x100;
        magnitude = (magnitude + 5U) / 10U;
        rt_snprintf(text, sizeof(text), "%s%u.%u", data.temperature_x100 < 0 ? "-" : "",
                     (unsigned)(magnitude / 10U), (unsigned)(magnitude % 10U));
        set_value(0, text, states[0]);
        magnitude = ((rt_uint32_t)data.humidity_x100 + 5U) / 10U;
        rt_snprintf(text, sizeof(text), "%u.%u", (unsigned)(magnitude / 10U),
                     (unsigned)(magnitude % 10U));
        set_value(1, text, states[0]);
    }
    else
    {
        set_value(0, "--", states[0]);
        set_value(1, "--", states[0]);
    }
    lv_label_set_text(s_cards[2].unit, "lux");
    if (states[1] == SENSOR_READY)
    {
        rt_uint32_t divisor = data.lux_x10 >= 10000000U ? 10000000U :
                               data.lux_x10 >= 1000000U ? 10000U : 10U;
        if (divisor != 10U || data.lux_x10 < 10000U)
            rt_snprintf(text, sizeof(text), "%u.%u", (unsigned)(data.lux_x10 / divisor),
                         (unsigned)((data.lux_x10 % divisor) / (divisor / 10U)));
        else
            rt_snprintf(text, sizeof(text), "%u", (unsigned)(data.lux_x10 / 10U));
        if (divisor != 10U)
            lv_label_set_text(s_cards[2].unit, divisor == 10000000U ? "Mlux" : "klux");
        set_value(2, text, states[1]);
    }
    else
        set_value(2, "--", states[1]);
    if (states[2] == SENSOR_READY)
    {
        rt_snprintf(text, sizeof(text), "%u", (unsigned)data.eco2_ppm);
        set_value(3, text, states[2]);
        rt_snprintf(text, sizeof(text), "%u", (unsigned)data.tvoc_ppb);
        lv_label_set_text(s_tvoc, text);
    }
    else
    {
        set_value(3, "--", states[2]);
        lv_label_set_text(s_tvoc, "--");
    }
    lv_obj_set_style_text_color(s_tvoc,
                                lv_color_hex(states[2] == SENSOR_READY ? UI_TEXT : UI_MUTED), 0);

    if (!available)
        hint = "\351\207\207\351\233\206\346\234\215\345\212\241\346\234\252\345\220\257\345\212\250\357\274\214\350\257\267\346\237\245\347\234\213\344\270\262\345\217\243";
    else if (waiting)
        hint = "\346\255\243\345\234\250\347\255\211\345\276\205\351\246\226\347\273\204\344\274\240\346\204\237\345\231\250\346\225\260\346\215\256";
    else if (stale)
        hint = "\346\225\260\346\215\256\345\267\262\350\277\207\346\234\237\357\274\214\347\255\211\345\276\205\351\207\207\351\233\206\346\201\242\345\244\215";
    else if (detected == 0)
        hint = "\344\274\240\346\204\237\345\231\250\346\234\252\350\277\236\346\216\245\357\274\214\350\257\267\346\243\200\346\237\245\344\276\233\347\224\265\345\222\214\346\216\245\347\272\277";
    else if (detected < 3)
        hint = "\351\203\250\345\210\206\350\256\276\345\244\207\346\234\252\350\277\236\346\216\245\357\274\214\347\202\271\345\207\273\345\217\263\344\270\212\350\247\222\346\237\245\347\234\213";
    else if (states[2] == SENSOR_WARMING)
        hint = "\347\251\272\346\260\224\344\274\240\346\204\237\345\231\250\351\242\204\347\203\255\344\270\255\357\274\214\350\257\267\347\250\215\345\200\231";
    else
        hint = "\346\225\260\346\215\256\345\267\262\346\233\264\346\226\260\357\274\214\347\202\271\345\207\273\345\217\263\344\270\212\350\247\222\346\237\245\347\234\213\350\256\276\345\244\207";
    lv_label_set_text(s_footer, hint);
    lv_label_set_text(s_device_hint, detected == 3 && !stale ?
                       (states[2] == SENSOR_WARMING ? "\351\242\204\347\203\255\345\256\214\346\210\220\345\220\216\346\230\276\347\244\272\347\251\272\346\260\224\346\214\207\346\240\207\343\200\202" : "\350\256\276\345\244\207\345\267\262\345\260\261\347\273\252\357\274\214\346\225\260\346\215\256\350\207\252\345\212\250\346\233\264\346\226\260\343\200\202") : hint);
    if (stale)
        lv_label_set_text(s_device_label, "\346\225\260\346\215\256\350\277\207\346\234\237  >");
    else
    {
        rt_snprintf(text, sizeof(text), "\350\256\276\345\244\207 %u/3  >", detected);
        lv_label_set_text(s_device_label, text);
    }
    lv_obj_set_style_text_color(s_device_label,
                                lv_color_hex(detected == 3 ? UI_GREEN : UI_AMBER), 0);
    lv_obj_center(s_device_label);
    {
        /* 日志状态只读展示；写卡由 env_log 线程完成，UI 不直接触碰 FatFs。 */
        struct gd32_env_log_status log;
        struct tm current;
        time_t now;
        char line[144];
        char period[48];
        if (gd32_env_log_get_status(&log) == RT_EOK)
        {
            const char *storage = log.state == GD32_ENV_LOG_READY ? "SD\345\215\241\345\260\261\347\273\252" :
                log.state == GD32_ENV_LOG_OFFLINE ? "\346\234\252\346\217\222SD\345\215\241" :
                log.state == GD32_ENV_LOG_MOUNT_ERROR ? "SD\345\215\241\346\214\202\350\275\275\345\244\261\350\264\245" : "SD\345\215\241\350\257\273\345\206\231\345\274\202\345\270\270";
            if (log.log_interval_seconds % 3600U == 0U)
                rt_snprintf(period, sizeof(period), "\346\257\217%lu\345\260\217\346\227\266\344\277\235\345\255\230",
                            (unsigned long)(log.log_interval_seconds / 3600U));
            else if (log.log_interval_seconds % 60U == 0U)
                rt_snprintf(period, sizeof(period), "\346\257\217%lu\345\210\206\351\222\237\344\277\235\345\255\230",
                            (unsigned long)(log.log_interval_seconds / 60U));
            else
                rt_snprintf(period, sizeof(period), "\346\257\217%lu\347\247\222\344\277\235\345\255\230",
                            (unsigned long)log.log_interval_seconds);
            now = log.timestamp;
            if (log.rtc_set && localtime_r(&now, &current))
            {
                rt_snprintf(line, sizeof(line), "%02d-%02d %02d:%02d:%02d",
                            current.tm_mon + 1, current.tm_mday,
                            current.tm_hour, current.tm_min, current.tm_sec);
                lv_label_set_text(s_clock, line);
            }
            else lv_label_set_text(s_clock, "\350\257\267\346\240\241\345\207\206\346\227\266\351\227\264");
            rt_snprintf(line, sizeof(line), "%s  |  %s%s%s%s%s", storage,
                        log.alarm_flags ? "\346\212\245\350\255\246\357\274\232" : (log.rtc_set ? period : "\346\240\241\346\227\266\345\220\216\345\274\200\345\247\213\350\256\260\345\275\225"),
                        log.alarm_flags & GD32_ENV_ALARM_TEMP ? "\346\270\251\345\272\246 " : "",
                        log.alarm_flags & GD32_ENV_ALARM_HUM ? "\346\271\277\345\272\246 " : "",
                        log.alarm_flags & (GD32_ENV_ALARM_ECO2 | GD32_ENV_ALARM_TVOC) ? "\346\260\224\344\275\223 " : "",
                        log.alarm_flags & GD32_ENV_ALARM_LIGHT ? "\345\205\211\347\205\247 " : "");
            lv_label_set_text(s_storage, line);
            lv_obj_set_style_text_color(s_storage, lv_color_hex(log.alarm_flags ? UI_AMBER : UI_MUTED), 0);
        }
    }
    gd32_hmi_pages_update();
}

/* Called by LVGL's RT-Thread package task after the display is initialized. */
void lv_user_gui_init(void)
{
    lv_obj_t *screen = lv_scr_act();
    rt_kprintf("UI: creating dashboard\n");
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    make_label(screen, 14, 12, &ui_font_18, UI_TEXT, "\347\216\257\345\242\203\347\233\221\346\265\213");
    s_clock = make_label(screen, 112, 14, &ui_font_14, UI_MUTED, "--");
    s_device_button = make_panel(screen, 340, 4, 128, 32, 0x20333f);
    lv_obj_add_flag(s_device_button, LV_OBJ_FLAG_CLICKABLE);
    s_device_label = make_label(s_device_button, 0, 0, &ui_font_14, UI_MUTED, "\350\256\276\345\244\207 0/3  >");
    lv_obj_add_event_cb(s_device_button, device_event, LV_EVENT_CLICKED, RT_NULL);
    s_home = make_panel(screen, 0, 40, 480, 196, UI_BG);
    make_card(0, 12, 0, "\346\270\251\345\272\246", "SHT40", 0xf2bd68);
    make_card(1, 244, 0, "\346\271\277\345\272\246", "SHT40", 0x65b9ff);
    make_card(2, 12, 80, "\345\205\211\347\205\247", "BH1750", 0x41dcc4);
    make_card(3, 244, 80, "\347\251\272\346\260\224\346\214\207\346\240\207", "SGP30", 0xb7a0f3);
    s_storage = make_label(s_home, 14, 158, &ui_font_14, UI_MUTED, "");
    s_footer = make_label(s_home, 14, 178, &ui_font_14, UI_MUTED, "");
    gd32_hmi_pages_init(screen, s_home);
    make_device_overlay();
    update_dashboard(RT_NULL);
    lv_timer_create(update_dashboard, 1000, RT_NULL);
    rt_kprintf("UI ready: dashboard and navigation created\n");
    /* Flush the visible page before the first input timer can initialize
     * GT911. A touch-controller fault must not leave a working LCD white. */
    lv_refr_now(RT_NULL);
    rt_kprintf("UI: initial refresh complete\n");
}

#ifdef BSP_USING_GT911_TOUCH
static struct
{
    rt_uint32_t polls;
    rt_uint32_t errors;
    rt_err_t last_error;
    rt_uint16_t x, y;
    rt_bool_t pressed;
    rt_bool_t registered;
} s_touch;
static volatile rt_bool_t s_touch_trace;

static void gt911_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    rt_bool_t pressed = RT_FALSE;
    rt_bool_t changed;
    rt_uint16_t x = 0, y = 0;
    rt_err_t result;
    rt_base_t level;
    RT_UNUSED(drv);

    /* 触摸驱动在这里惰性初始化；错误只上报 RELEASED，避免一次 I2C
     * 故障把 LVGL 输入状态卡在按下。 */
    result = gd32_gt911_read(&pressed, &x, &y);
    if (result == RT_EOK)
    {
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    level = rt_hw_interrupt_disable();
    changed = pressed != s_touch.pressed;
    s_touch.polls++;
    s_touch.last_error = result;
    if (result != RT_EOK) s_touch.errors++;
    s_touch.x = x;
    s_touch.y = y;
    s_touch.pressed = pressed;
    rt_hw_interrupt_enable(level);
    if (s_touch_trace && changed)
        rt_kprintf("touch: %s x=%u y=%u err=%d\n",
                   pressed ? "down" : "up", x, y, result);
}

#ifdef RT_USING_FINSH
static void touch(int argc, char **argv)
{
    rt_uint32_t polls, errors;
    rt_err_t last_error;
    rt_uint16_t x, y;
    rt_bool_t pressed, registered;
    rt_base_t level;

    if (argc == 2 && rt_strcmp(argv[1], "on") == 0)
        s_touch_trace = RT_TRUE;
    else if (argc == 2 && rt_strcmp(argv[1], "off") == 0)
        s_touch_trace = RT_FALSE;
    else if (argc != 1)
    {
        rt_kprintf("Usage: touch [on|off]\n");
        return;
    }
    /* The LVGL thread alone owns the I2C bus.  The shell only reads its
     * cached diagnostics, so it cannot consume a frame ahead of LVGL. */
    level = rt_hw_interrupt_disable();
    polls = s_touch.polls;
    errors = s_touch.errors;
    last_error = s_touch.last_error;
    x = s_touch.x;
    y = s_touch.y;
    pressed = s_touch.pressed;
    registered = s_touch.registered;
    rt_hw_interrupt_enable(level);
    rt_kprintf("touch: LVGL=%d polls=%lu errors=%lu last_error=%d trace=%d\n",
               registered, (unsigned long)polls, (unsigned long)errors,
               last_error, s_touch_trace);
    rt_kprintf("touch: pressed=%d x=%u y=%u; SCL=PH7 SDA=PF9 RST=PF6 INT=PH6\n",
               pressed, x, y);
}
MSH_CMD_EXPORT(touch, Show touch status or enable touch on/off tracing);
#endif
#endif

void lv_port_indev_init(void)
{
#ifdef BSP_USING_GT911_TOUCH
    /* LVGL 会保存驱动结构体指针，因此必须使用静态存储期对象。 */
    static lv_indev_drv_t indev_drv;
    /* gd32_gt911_read() initializes/retries from the first input poll. Keep
     * this registration free of hardware access so GUI creation comes first. */
    s_touch.last_error = -RT_EBUSY;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = gt911_read_cb;
    s_touch.registered = lv_indev_drv_register(&indev_drv) != RT_NULL;
    rt_kprintf("touch: LVGL pointer %s\n",
               s_touch.registered ? "registered" : "registration failed");
#endif
}

/* Kept as a public entry point for applications that do not use the package
 * supplied LVGL thread.  With PKG_USING_LVGL enabled the package calls
 * lv_user_gui_init() from its own thread, so this function only reports that
 * the task is managed by the package. */
int gd32_lvgl_app_init(void)
{
    return gd32_lvgl_thread_start();
}
#else
int gd32_lvgl_app_init(void)
{
    return RT_EOK;
}
#endif
