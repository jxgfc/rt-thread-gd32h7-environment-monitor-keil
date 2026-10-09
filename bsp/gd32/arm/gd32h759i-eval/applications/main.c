/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-08-20     BruceOu      first implementation
 * 2025-08-20     WangShun     change the LED pins
 */

#include <stdio.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <board.h>
#include "drv_tli_lcd.h"

/*
 * 应用层入口只负责启动各个独立服务：LCD、传感器、LVGL、环境日志和报警。
 * 传感器采集及 SD 卡访问均在各自线程中完成，main 线程本身不做阻塞式 I/O，
 * 这样即使 SD 卡未插入或文件系统暂时不可用，串口 shell 和屏幕仍能启动。
 */

#ifdef BSP_USING_LVGL
extern int gd32_lvgl_app_init(void);
#endif
#ifdef BSP_USING_SENSOR_SERVICE
extern int gd32_sensor_service_init(void);
#endif
#ifdef BSP_USING_ENV_LOG
#include "env_log.h"
#include "alarm.h"

/* Match the Cortex-M7 context layout used by cpuport.c and context_rvds.S. */
#if (defined(__CC_ARM) && defined(__TARGET_FPU_VFP)) || \
    (defined(__clang__) && defined(__VFP_FP__) && !defined(__SOFTFP__)) || \
    (defined(__ICCARM__) && defined(__ARMVFP__)) || \
    (defined(__GNUC__) && defined(__VFP_FP__) && !defined(__SOFTFP__))
#define APP_DIAG_FPU_CONTEXT 1
#else
#define APP_DIAG_FPU_CONTEXT 0
#endif

struct app_diag_thread
{
    char *name;                 /* RT-Thread 线程名，用于启动后的诊断查找。 */
    rt_thread_t thread;
    rt_ubase_t stack_low;       /* 启动时缓存的栈底，故障诊断时作为可信边界。 */
    rt_ubase_t stack_high;      /* 栈顶地址（不包含），用于校验保存的 SP。 */
};

static struct app_diag_thread s_diag_threads[] =
{
    {"LVGL", RT_NULL, 0, 0},
    {"tshell", RT_NULL, 0, 0},
    {"envlog", RT_NULL, 0, 0}
};

static void app_diag_cache_threads(void)
{
    /* 线程创建完成后只缓存一次栈范围；故障现场不要再读取可能已损坏的 TCB。 */
    unsigned index;
    for (index = 0; index < sizeof(s_diag_threads) / sizeof(s_diag_threads[0]); index++)
    {
        struct app_diag_thread *item = &s_diag_threads[index];
        item->thread = rt_thread_find(item->name);
        if (item->thread != RT_NULL)
        {
            /* Cache trusted bounds before the fault; never trust a later
             * corrupted stack_addr/stack_size when inspecting a saved SP. */
            item->stack_low = (rt_ubase_t)item->thread->stack_addr;
            item->stack_high = item->stack_low + item->thread->stack_size;
        }
    }
}

static void app_diag_print_thread(const struct app_diag_thread *item)
{
    rt_ubase_t sp, pending;
    rt_uint32_t pc = 0, lr = 0, psr = 0;
    /* Cortex-M7 硬件异常帧固定为 r0-r3、r12、LR、PC、xPSR 共 8 个字。 */
    rt_uint32_t frame_words = 8, total_words;
    rt_uint8_t state, priority;
    rt_bool_t valid = RT_FALSE;
    rt_base_t level;

    if (item->thread == RT_NULL)
    {
        rt_kprintf("LIVE %s: absent\n", item->name);
        return;
    }

    /* Only copy fixed fields while IRQs are masked. No object lookup, IPC,
     * allocation or console output is allowed inside this short snapshot. */
    level = rt_hw_interrupt_disable();
    sp = (rt_ubase_t)item->thread->sp;
    pending = (rt_ubase_t)item->thread->pending_object;
    state = RT_SCHED_CTX(item->thread).stat & RT_THREAD_STAT_MASK;
    priority = RT_SCHED_PRIV(item->thread).current_priority;
    if (state != RT_THREAD_RUNNING && state != RT_THREAD_CLOSE &&
        item->stack_high >= item->stack_low && sp >= item->stack_low &&
        sp <= item->stack_high && (sp & 3U) == 0 &&
        item->stack_high - sp >= (16U + APP_DIAG_FPU_CONTEXT) * sizeof(rt_uint32_t))
    {
        const rt_uint32_t *saved = (const rt_uint32_t *)sp;
        rt_uint32_t fp = 0;
#if APP_DIAG_FPU_CONTEXT
        fp = saved[0];
        frame_words++; /* FPU flag precedes r4..r11. */
#endif
        if (fp <= 1U)
        {
            /* Optional s16..s31 precede the hardware r0..xPSR frame;
             * s0..s15/FPSCR/reserved follow that frame when FPU is active. */
            if (fp) frame_words += 16U;
            total_words = frame_words + 8U + (fp ? 18U : 0U);
            if (item->stack_high - sp >= total_words * sizeof(rt_uint32_t))
            {
                lr = saved[frame_words + 5U];
                pc = saved[frame_words + 6U];
                psr = saved[frame_words + 7U];
                valid = (psr & 0x01000000UL) != 0U;
            }
        }
    }
    rt_hw_interrupt_enable(level);
    rt_kprintf("LIVE %s: state=%u pri=%u wait=%08lx sp=%08lx\n",
               item->name, state, priority, (unsigned long)pending, (unsigned long)sp);
    rt_kprintf("LIVE %s: context=%d pc=%08lx lr=%08lx psr=%08lx\n",
               item->name, valid, (unsigned long)pc, (unsigned long)lr, (unsigned long)psr);
}

static void app_diag_print(rt_uint32_t request_id)
{
    unsigned index;
    /* 诊断输出只读快照，不主动访问 SD 或申请内存，避免放大原始故障。 */
    rt_kprintf("LIVE tick=%lu request=%lu PRIMASK=%lu BASEPRI=%lu critical=%u\n",
               (unsigned long)rt_tick_get(), (unsigned long)request_id,
               (unsigned long)__get_PRIMASK(), (unsigned long)__get_BASEPRI(),
               (unsigned)rt_critical_level());
    for (index = 0; index < sizeof(s_diag_threads) / sizeof(s_diag_threads[0]); index++)
        app_diag_print_thread(&s_diag_threads[index]);
    gd32_env_log_diag_print();
}
#endif

int main(void)
{
#ifdef BSP_USING_ENV_LOG
    rt_uint32_t seen_request = 0, request_id;
    unsigned samples_left = 6;
#endif
    /* main 运行在 RT-Thread 的 main 线程中；组件初始化已由系统启动流程完成。 */
    rt_kprintf("\nGD32H759I-EVAL application started\n");
    rt_kprintf("build: SD watchdog + alarm isolation v1: " __DATE__ " " __TIME__ "\n");
    /* Reset flags have already been captured and cleared once by board.c. */
    rt_kprintf("console: uart0 (USART0 PA9/PA10, 115200-8-N-1)\n");

#ifdef BSP_USING_TLI_LCD
    /* Registration is intentionally after RT-Thread component startup.  The
     * LCD device's open callback contains the TLI/PLL setup and must not be
     * allowed to delay creation of the serial shell. */
    if (gd32_tli_lcd_register() != RT_EOK)
        rt_kprintf("LCD device registration failed\n");
#endif

#ifdef BSP_USING_SENSOR_SERVICE
    if (gd32_sensor_service_init() != RT_EOK)
        rt_kprintf("sensor service start failed\n");
#endif

#ifdef BSP_USING_LVGL
    if (gd32_lvgl_app_init() != RT_EOK)
        rt_kprintf("LVGL start failed; serial shell remains available\n");
#endif

#ifdef BSP_USING_ENV_LOG
    if (gd32_env_log_init() != RT_EOK)
        rt_kprintf("environment log service start failed\n");
    if (gd32_alarm_init() != RT_EOK)
        rt_kprintf("alarm service start failed\n");
    app_diag_cache_threads();
#endif

    /* PF10 is LCD DE in the 29_TLI_Touch_Draw wiring.  Do not use it as an
     * LED while the RGB panel is enabled. */
    while (1)
    {
        rt_thread_mdelay(2000);
#ifdef BSP_USING_ENV_LOG
        request_id = gd32_env_log_diag_request_id();
        if (request_id != seen_request)
        {
            seen_request = request_id;
            samples_left = 6;
        }
        if (samples_left)
        {
            samples_left--;
            app_diag_print(request_id);
        }
#endif
    }
}
