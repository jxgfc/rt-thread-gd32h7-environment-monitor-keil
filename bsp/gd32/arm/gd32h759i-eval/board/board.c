/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-08-20     BruceOu      first implementation
 */
#include <stdint.h>
#include <rthw.h>
#include <rtthread.h>
#include <board.h>
#include "gd32h7xx_rcu.h"

/**
  * @brief  This function is executed in case of error occurrence.
  * @param  None
  * @retval None
  */
void Error_Handler(void)
{
    /* USER CODE BEGIN Error_Handler */
    /* User can add his own implementation to report the HAL error return state */
    while (1)
    {
    }
    /* USER CODE END Error_Handler */
}

/** System Clock Configuration
*/
void SystemClock_Config(void)
{
    SysTick_Config(SystemCoreClock / RT_TICK_PER_SECOND);
}

/* SysTick_Config selects the core clock and starts the down-counter before
 * any board drivers run.  Only read VAL here: the RT-Thread tick keeps its
 * existing period and interrupts remain enabled.  Preemption may extend a
 * delay, but must never shorten an I2C timing interval.
 */
void rt_hw_us_delay(rt_uint32_t us)
{
    rt_uint32_t cycles_per_us = (SystemCoreClock + 999999U) / 1000000U;
    rt_uint32_t period = SysTick->LOAD + 1U;

    while (us != 0U)
    {
        /* Bound the multiplication even for a large delay argument. */
        rt_uint32_t part = us > 1000U ? 1000U : us;
        rt_uint32_t remaining = part * cycles_per_us;
        rt_uint32_t previous = SysTick->VAL;

        while (remaining != 0U)
        {
            rt_uint32_t current = SysTick->VAL;
            rt_uint32_t elapsed = previous >= current ? previous - current :
                                  previous + period - current;

            if (elapsed >= remaining)
                break;
            remaining -= elapsed;
            previous = current;
        }
        us -= part;
    }
}

/*
 * This deliberately uses the vendor board COM wiring directly and does not
 * depend on RT-Thread, the heap, or the serial device layer.  It gives a
 * reliable first byte stream when diagnosing a board that appears silent.
 */
static void gd32_early_uart0_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_USART0);

    gpio_af_set(GPIOA, GPIO_AF_7, GPIO_PIN_9 | GPIO_PIN_10);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP,
                  GPIO_PIN_9 | GPIO_PIN_10);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP,
                            GPIO_OSPEED_100_220MHZ,
                            GPIO_PIN_9 | GPIO_PIN_10);

    usart_deinit(USART0);
    usart_baudrate_set(USART0, 115200U);
    usart_word_length_set(USART0, USART_WL_8BIT);
    usart_stop_bit_set(USART0, USART_STB_1BIT);
    usart_parity_config(USART0, USART_PM_NONE);
    usart_receive_config(USART0, USART_RECEIVE_ENABLE);
    usart_transmit_config(USART0, USART_TRANSMIT_ENABLE);
    usart_enable(USART0);
}

static void gd32_early_uart0_puts(const char *text)
{
    rt_uint32_t timeout;

    while (text != RT_NULL && *text != '\0')
    {
        timeout = 1000000U;
        while (usart_flag_get(USART0, USART_FLAG_TBE) == RESET)
        {
            if (timeout == 0U)
                return;
            --timeout;
        }
        usart_data_transmit(USART0, (uint32_t)(uint8_t)*text++);
    }

    timeout = 1000000U;
    while (usart_flag_get(USART0, USART_FLAG_TC) == RESET)
    {
        if (timeout == 0U)
            return;
        --timeout;
    }
}

static void gd32_early_uart0_hex(const char *name, rt_uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[11];
    unsigned int i;

    for (i = 0; i < 8; ++i)
        text[i] = digits[(value >> (28U - 4U * i)) & 0xFU];
    text[8] = '\r';
    text[9] = '\n';
    text[10] = '\0';
    gd32_early_uart0_puts(name);
    gd32_early_uart0_puts(text);
}

/* Capture before clearing once per boot. BOR can also be set on normal
 * power-up; the flags alone do not identify an external load as the cause. */
static void gd32_early_print_reset_cause(void)
{
    rt_uint32_t flags = 0U;

    if (rcu_flag_get(RCU_FLAG_BORRST) != RESET) flags |= (1U << 0);
    if (rcu_flag_get(RCU_FLAG_EPRST) != RESET) flags |= (1U << 1);
    if (rcu_flag_get(RCU_FLAG_PORRST) != RESET) flags |= (1U << 2);
    if (rcu_flag_get(RCU_FLAG_SWRST) != RESET) flags |= (1U << 3);
    if (rcu_flag_get(RCU_FLAG_FWDGTRST) != RESET) flags |= (1U << 4);
    if (rcu_flag_get(RCU_FLAG_WWDGTRST) != RESET) flags |= (1U << 5);
    if (rcu_flag_get(RCU_FLAG_LPRST) != RESET) flags |= (1U << 6);
    gd32_early_uart0_hex("RESET_FLAGS=0x", flags);
    rcu_all_reset_flag_clear();
}

/* RT-Thread otherwise discards diagnostics before a console device exists. */
void rt_hw_console_output(const char *text)
{
    char ch[2] = {0, 0};

    if (text == RT_NULL)
        return;
    while (*text != '\0')
    {
        if (*text == '\n' && ch[0] != '\r')
            gd32_early_uart0_puts("\r");
        ch[0] = *text++;
        gd32_early_uart0_puts(ch);
    }
}

static rt_err_t gd32_boot_exception(void *context)
{
    RT_UNUSED(context);

    /* The fault may occur while the serial driver is reconfiguring USART0. */
    gd32_early_uart0_init();
    gd32_early_uart0_puts("\r\nFAULT Cortex-M7\r\n");
    gd32_early_uart0_hex("CFSR=0x", SCB->CFSR);
    gd32_early_uart0_hex("HFSR=0x", SCB->HFSR);
    gd32_early_uart0_hex("BFAR=0x", SCB->BFAR);
    gd32_early_uart0_hex("MMFAR=0x", SCB->MMFAR);

    /* Keep the CPU port's register dump and fatal-fault handling. */
    return -RT_ERROR;
}

/**
 * This is the timer interrupt service routine.
 *
 */
void SysTick_Handler(void)
{
    /* enter interrupt */
    rt_interrupt_enter();

    rt_tick_increase();

    /* leave interrupt */
    rt_interrupt_leave();
}

/**
 * This function will initial GD32 board.
 */
void rt_hw_board_init(void)
{
    /* NVIC Configuration */
#define NVIC_VTOR_MASK              0x3FFFFF80
#ifdef  VECT_TAB_RAM
    /* Set the Vector Table base location at 0x10000000 */
    SCB->VTOR  = (0x10000000 & NVIC_VTOR_MASK);
#else  /* VECT_TAB_FLASH  */
    /* Set the Vector Table base location at 0x08000000 */
    SCB->VTOR  = (0x08000000 & NVIC_VTOR_MASK);
#endif

    /* Enable IChace */
    rt_hw_cpu_icache_enable();

    /* Enable DChace */
    rt_hw_cpu_dcache_enable();

    SystemClock_Config();

    gd32_early_uart0_init();
    rt_hw_exception_install(gd32_boot_exception);
    gd32_early_uart0_puts("\r\nBOOT USART0 PA9/PA10 115200\r\n");
    gd32_early_print_reset_cause();

#ifdef RT_USING_SERIAL
    gd32_early_uart0_puts("B1 before rt_hw_usart_init\r\n");
    rt_hw_usart_init();
    gd32_early_uart0_puts("B2 after rt_hw_usart_init\r\n");
#endif

#ifdef RT_USING_HEAP
    gd32_early_uart0_puts("B3 before rt_system_heap_init\r\n");
    gd32_early_uart0_hex("HEAP_BEGIN=0x", (rt_uint32_t)(uintptr_t)HEAP_BEGIN);
    gd32_early_uart0_hex("HEAP_END=0x", (rt_uint32_t)(uintptr_t)HEAP_END);
    rt_system_heap_init((void *)HEAP_BEGIN, (void *)HEAP_END);
    gd32_early_uart0_puts("B4 after rt_system_heap_init\r\n");
#endif

#ifdef RT_USING_CONSOLE
    gd32_early_uart0_puts("B5 before rt_console_set_device\r\n");
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
    gd32_early_uart0_puts("B6 after rt_console_set_device\r\n");
    if (rt_console_get_device() != RT_NULL)
        gd32_early_uart0_puts("BOOT RT console uart0\r\n");
    else
        gd32_early_uart0_puts("BOOT RT console switch failed\r\n");
#endif

#ifdef RT_USING_COMPONENTS_INIT
    gd32_early_uart0_puts("B7 before rt_components_board_init\r\n");
    rt_components_board_init();
    gd32_early_uart0_puts("B8 after rt_components_board_init\r\n");
#endif
    gd32_early_uart0_puts("B9 board init complete\r\n");
}
