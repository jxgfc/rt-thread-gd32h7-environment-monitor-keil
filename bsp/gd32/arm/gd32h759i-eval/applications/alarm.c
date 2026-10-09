/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <rthw.h>
#include <board.h>
#include "gd32h7xx_rcu.h"
#include "gd32h7xx_gpio.h"
#include "alarm.h"
#include "env_log.h"

#ifdef BSP_USING_ENV_LOG

/* 报警线程是 GPIO 的唯一写入者。其他线程只能通过 env_log 的状态位间接请求
 * 报警，避免多个上下文同时改写 PC6/PE2/PA6 造成波形抖动。 */
static struct rt_thread s_alarm_thread;
rt_align(RT_ALIGN_SIZE)
/* The snapshot/IPC diagnostic path can be several hundred bytes deep on
 * ARMCC. Keep enough headroom so an alarm cannot corrupt adjacent objects. */
static rt_uint8_t s_alarm_stack[1536];
/* A shell test temporarily owns the outputs. The alarm thread keeps running
 * so a test can never block LVGL, the logger, or the shell. */
static volatile rt_uint8_t s_test_mode;
static volatile rt_tick_t s_test_until;

/* Polarity belongs to the driver module. PC6 can be used either as a static
 * gate for the connected three-pin module or as the passive tone output. */
#if GD32_ALARM_BUZZER_ENABLE
static void passive_write(rt_bool_t on)
{
    /* 参数 on 表示“逻辑有效”，这里集中处理低电平有效模块的反相。 */
    if ((on != RT_FALSE) != (GD32_ALARM_PC6_ACTIVE_LOW != 0))
        gpio_bit_set(GD32_ALARM_PASSIVE_PORT, GD32_ALARM_PASSIVE_PIN);
    else
        gpio_bit_reset(GD32_ALARM_PASSIVE_PORT, GD32_ALARM_PASSIVE_PIN);
}
#endif

static void alarm_outputs_init(void)
{
    /* 先开 GPIO 时钟，再预置输出锁存器，最后切换为推挽输出，避免上电瞬间
     * 产生一次不可控的蜂鸣器脉冲。 */
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOE);
    /* Set the output latches before changing direction to avoid a turn-on pulse. */
    gpio_bit_reset(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
#if GD32_ALARM_BUZZER_ENABLE
    passive_write(RT_FALSE);
    gpio_bit_reset(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
#endif
    gpio_mode_set(GD32_ALARM_LED_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                  GD32_ALARM_LED_PIN);
    gpio_output_options_set(GD32_ALARM_LED_PORT, GPIO_OTYPE_PP,
                            GPIO_OSPEED_60MHZ, GD32_ALARM_LED_PIN);
#if GD32_ALARM_BUZZER_ENABLE
    /* Each output starts at its inactive level, including the unused pin. */
    gpio_mode_set(GD32_ALARM_PASSIVE_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                  GD32_ALARM_PASSIVE_PIN);
    gpio_output_options_set(GD32_ALARM_PASSIVE_PORT, GPIO_OTYPE_PP,
                            GPIO_OSPEED_12MHZ, GD32_ALARM_PASSIVE_PIN);
    /* PE2 is the active-buzzer enable.  In active mode it is held at the
     * enable level for the complete alarm interval, so no tone waveform is
     * put on the pin. */
    gpio_mode_set(GD32_ALARM_ACTIVE_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                  GD32_ALARM_ACTIVE_PIN);
    gpio_output_options_set(GD32_ALARM_ACTIVE_PORT, GPIO_OTYPE_PP,
                            GPIO_OSPEED_12MHZ, GD32_ALARM_ACTIVE_PIN);
    gpio_bit_reset(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
    passive_write(RT_FALSE);
    gpio_bit_reset(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
#else
    /* No waveform and no push-pull/open-drain output in this diagnostic
     * build. Weak bias is not a substitute for disconnecting the module. */
    gpio_mode_set(GD32_ALARM_PASSIVE_PORT, GPIO_MODE_INPUT,
                  GD32_ALARM_PC6_ACTIVE_LOW ? GPIO_PUPD_PULLUP : GPIO_PUPD_PULLDOWN,
                  GD32_ALARM_PASSIVE_PIN);
    gpio_mode_set(GD32_ALARM_ACTIVE_PORT, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN,
                  GD32_ALARM_ACTIVE_PIN);
#endif
}

static void alarm_outputs_off(void)
{
    /* 报警结束和 shell 测试结束都走同一关闭路径，保证两个蜂鸣器引脚回到安全态。 */
    gpio_bit_reset(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
#if GD32_ALARM_BUZZER_ENABLE
    passive_write(RT_FALSE);
    gpio_bit_reset(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
#endif
}

#ifdef RT_USING_FINSH
static void alarm_test_cmd(int argc, char **argv)
{
    const rt_uint32_t duration = 1500U;

    /* 该命令只设置测试状态，由 alarm 线程实际驱动输出；shell 回调不会忙等。 */
    if (argc != 2 || (rt_strcmp(argv[1], "led") != 0 &&
                      rt_strcmp(argv[1], "buzzer") != 0 &&
                      rt_strcmp(argv[1], "off") != 0))
    {
        rt_kprintf("usage: alarm_test led|buzzer|off\n");
        return;
    }
    if (rt_strcmp(argv[1], "off") == 0)
    {
        s_test_mode = 0U;
        alarm_outputs_off();
        rt_kprintf("alarm_test: outputs off\n");
        return;
    }
    if (rt_strcmp(argv[1], "buzzer") == 0 && !GD32_ALARM_BUZZER_ENABLE)
    {
        rt_kprintf("alarm_test: buzzer disabled at build time\n");
        return;
    }
    s_test_until = rt_tick_get() + rt_tick_from_millisecond(duration);
    s_test_mode = rt_strcmp(argv[1], "led") == 0 ? 1U : 2U;
    rt_kprintf("alarm_test: %s for %lu ms\n", argv[1], (unsigned long)duration);
}
MSH_CMD_EXPORT_ALIAS(alarm_test_cmd, alarm_test,
                     Test LED2 or the alarm buzzer for a short time);
#endif

static void alarm_thread_entry(void *parameter)
{
    struct gd32_env_log_status status;
#if GD32_ALARM_BUZZER_ENABLE
    rt_bool_t active = RT_FALSE;
    rt_bool_t beep_on = RT_FALSE;
    rt_tick_t beep_due = 0;
#if !GD32_ALARM_USE_ACTIVE && !GD32_ALARM_PC6_GATE_MODE
    rt_bool_t tone = RT_FALSE;
#endif
#if !GD32_ALARM_USE_ACTIVE && !GD32_ALARM_PC6_GATE_MODE
    rt_bool_t test_tone = RT_FALSE;
#endif
#endif
    RT_UNUSED(parameter);

    /* 每轮先处理临时测试请求，最后消费 envlog 发布的报警状态。
     * 测试模式有明确的截止 tick，异常退出 shell 也不会让输出永久保持有效。 */
    while (1)
    {
        /* envlog evaluates alarm_flags when it publishes each sensor sample.
         * The alarm thread only consumes that RAM status; it must never enter
         * the sensor or SD service and wait behind a faulty peripheral. */
        rt_uint8_t test_mode = s_test_mode;
        if (test_mode != 0U)
        {
            if ((rt_int32_t)(rt_tick_get() - s_test_until) >= 0)
            {
                s_test_mode = 0U;
                alarm_outputs_off();
                continue;
            }
            if (test_mode == 1U)
            {
                gpio_bit_set(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
#if GD32_ALARM_BUZZER_ENABLE
                passive_write(RT_FALSE);
                gpio_bit_reset(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
#endif
            }
            else
            {
                gpio_bit_reset(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
#if GD32_ALARM_BUZZER_ENABLE
#if GD32_ALARM_USE_ACTIVE
                gpio_bit_set(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
#elif GD32_ALARM_PC6_GATE_MODE
                passive_write(RT_TRUE);
#else
                test_tone = !test_tone;
                passive_write(test_tone);
#endif
#endif
            }
#if GD32_ALARM_BUZZER_ENABLE && !GD32_ALARM_USE_ACTIVE && !GD32_ALARM_PC6_GATE_MODE
            rt_thread_mdelay(1);
#else
            rt_thread_mdelay(20);
#endif
            continue;
        }
        /* alarm_flags 是按位标志：任意传感器越界都会触发同一组声光输出。 */
        rt_bool_t next_active = RT_FALSE;
        if (gd32_env_log_get_status(&status) == RT_EOK)
            next_active = status.alarm_flags != 0U;
#if GD32_ALARM_BUZZER_ENABLE
        /* 从无报警进入报警时立即开启 on 窗口；之后按固定节拍翻转。 */
        if (next_active && !active)
        {
            beep_on = RT_TRUE;
            beep_due = rt_tick_get() +
                       rt_tick_from_millisecond(GD32_ALARM_BEEP_ON_MS);
#if !GD32_ALARM_USE_ACTIVE && !GD32_ALARM_PC6_GATE_MODE
            tone = RT_FALSE;
#endif
        }
        active = next_active;
        if (!active)
        {
            gpio_bit_reset(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
            beep_on = RT_FALSE;
            passive_write(RT_FALSE);
            gpio_bit_reset(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
            rt_thread_mdelay(100);
            continue;
        }

        /* Keep the visual and audible warning synchronized. The LED is on
         * during the beep window and off during the quiet window. */
        if ((rt_int32_t)(rt_tick_get() - beep_due) >= 0)
        {
            beep_on = beep_on ? RT_FALSE : RT_TRUE;
            beep_due = rt_tick_get() + rt_tick_from_millisecond(
                beep_on ? GD32_ALARM_BEEP_ON_MS : GD32_ALARM_BEEP_OFF_MS);
#if !GD32_ALARM_USE_ACTIVE && !GD32_ALARM_PC6_GATE_MODE
            tone = RT_FALSE;
#endif
        }
        if (beep_on)
            gpio_bit_set(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
        else
            gpio_bit_reset(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
#if GD32_ALARM_USE_ACTIVE
        /* 有源模块只需要稳定的有效电平，不能在 PE2 上输出无源蜂鸣器方波。 */
        passive_write(RT_FALSE);
        if (beep_on)
            gpio_bit_set(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
        else
            gpio_bit_reset(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
        rt_thread_mdelay(20);
        continue;
#elif GD32_ALARM_PC6_GATE_MODE
        /* 模块自带振荡器时，PC6 只作为门控信号。 */
        passive_write(beep_on);
        gpio_bit_reset(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
        rt_thread_mdelay(20);
        continue;
#else
        /* 裸无源蜂鸣器需要方波：线程每约 1 ms 翻转一次 PC6，形成约 500 Hz。 */
        /* Bare passive buzzer: generate approximately 500 Hz in the beep
         * window, and hold its inactive level during the quiet window. */
        if (beep_on)
            tone = tone ? RT_FALSE : RT_TRUE;
        else
            tone = RT_FALSE;
        passive_write(tone);
        gpio_bit_reset(GD32_ALARM_ACTIVE_PORT, GD32_ALARM_ACTIVE_PIN);
        rt_thread_mdelay(beep_on ? 1 : 20);
#endif
#else
        if (next_active)
            gpio_bit_set(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
        else
            gpio_bit_reset(GD32_ALARM_LED_PORT, GD32_ALARM_LED_PIN);
        rt_thread_mdelay(100);
#endif
    }
}

int gd32_alarm_init(void)
{
    rt_err_t result;
    /* 线程使用静态 TCB/栈，不从堆分配，避免存储服务异常时报警也无法启动。 */
    alarm_outputs_init();
    result = rt_thread_init(&s_alarm_thread, "alarm", alarm_thread_entry, RT_NULL,
                            s_alarm_stack, sizeof(s_alarm_stack), 24, 10);
    if (result != RT_EOK) return result;
    result = rt_thread_startup(&s_alarm_thread);
    if (result == RT_EOK)
    {
#if !GD32_ALARM_BUZZER_ENABLE
        rt_kprintf("alarm: LED2 PA6 only; buzzer disabled, PC6/PE2 inputs\n");
#elif GD32_ALARM_USE_ACTIVE
        rt_kprintf("alarm: LED2 PA6, active buzzer PE2, PC6 idle=%s\n",
                   GD32_ALARM_PC6_ACTIVE_LOW ? "high" : "low");
#else
 #if GD32_ALARM_PC6_GATE_MODE
        rt_kprintf("alarm: LED2 PA6, low-level buzzer gate PC6, idle=%s\n",
                   GD32_ALARM_PC6_ACTIVE_LOW ? "high" : "low");
 #else
        rt_kprintf("alarm: LED2 PA6, passive buzzer PC6, idle=%s, tone=500Hz (PE2 low)\n",
                   GD32_ALARM_PC6_ACTIVE_LOW ? "high" : "low");
 #endif
#endif
    }
    return result;
}

#else
int gd32_alarm_init(void) { return -RT_ENOSYS; }
#endif
