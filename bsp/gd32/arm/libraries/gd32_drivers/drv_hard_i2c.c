/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author            Notes
 * 2021-12-20     BruceOu           the first version
 * 2026-01-11     ShiHongchao       Fix the I2C master receive mode B software
 *                                  flow and add support for mode A
 */

#include "drv_hard_i2c.h"

#ifdef RT_USING_I2C

#define LOG_TAG              "drv.i2c"
#include <rtdbg.h>

#if !defined(BSP_USING_HARD_I2C0) && !defined(BSP_USING_HARD_I2C1) && !defined(BSP_USING_HARD_I2C2) && !defined(BSP_USING_HARD_I2C3) && !defined(BSP_USING_HARD_I2C4) && !defined(BSP_USING_HARD_I2C5)
#error "Please define at least one BSP_USING_I2Cx"
/* this driver can be disabled at menuconfig → RT-Thread Components → Device Drivers */
#endif

#if defined(BSP_USING_HARD_I2C0)
struct rt_i2c_bus_device i2c0;
#endif /* BSP_USING_I2C0 */

#if defined(BSP_USING_HARD_I2C1)
struct rt_i2c_bus_device i2c1;
#endif /* BSP_USING_I2C1 */

#if defined(BSP_USING_HARD_I2C2)
struct rt_i2c_bus_device i2c2;
#endif /* BSP_USING_I2C2 */

#if defined(BSP_USING_HARD_I2C3)
struct rt_i2c_bus_device i2c3;
#endif /* BSP_USING_I2C3 */

#if defined(BSP_USING_HARD_I2C4)
struct rt_i2c_bus_device i2c4;
#endif /* BSP_USING_I2C4 */

#if defined(BSP_USING_HARD_I2C5)
struct rt_i2c_bus_device i2c5;
#endif /* BSP_USING_I2C5 */

#if defined (SOC_SERIES_GD32F5xx)
#define i2c_flag_get_gd                      i2c_add_flag_get
#define I2C_FLAG_RBNE_GD                     I2C_ADD_FLAG_RBNE
#define i2c_data_receive_gd                  i2c_add_data_receive
#define I2C_STAT_GD                          I2C_ADD_STAT
#define I2C_STAT_TBE_GD                      I2C_ADD_STAT_TBE
#define i2c_data_transmit_gd                 i2c_add_data_transmit
#define I2C_STAT_TI_GD                       I2C_ADD_STAT_TI
#define i2c_address10_enable_gd              i2c_add_address10_enable
#define i2c_address10_disable_gd             i2c_add_address10_disable
#define i2c_master_addressing_gd             i2c_add_master_addressing
#define I2C_MASTER_RECEIVE_GD                I2C_ADD_MASTER_RECEIVE
#define i2c_transfer_byte_number_config_gd   i2c_add_transfer_byte_number_config
#define i2c_start_on_bus_gd                  i2c_add_start_on_bus
#define I2C_MASTER_TRANSMIT_GD               I2C_ADD_MASTER_TRANSMIT
#define I2C_FLAG_I2CBSY_GD                   I2C_ADD_FLAG_I2CBSY
#define I2C_FLAG_TC_GD                       I2C_ADD_FLAG_TC
#define I2C_FLAG_TI_GD                       I2C_ADD_FLAG_TI
#define i2c_stop_on_bus_gd                   i2c_add_stop_on_bus
#define I2C_FLAG_STPDET_GD                   I2C_ADD_FLAG_STPDET
#define i2c_timing_config_gd                 i2c_add_timing_config
#define i2c_master_clock_config_gd           i2c_add_master_clock_config
#define i2c_enable_gd                        i2c_add_enable
#define i2c_flag_clear_gd                    i2c_add_flag_clear

#else

#define i2c_flag_get_gd                      i2c_flag_get
#define I2C_FLAG_RBNE_GD                     I2C_FLAG_RBNE
#define i2c_data_receive_gd                  i2c_data_receive
#define I2C_STAT_GD                          I2C_STAT
#define I2C_STAT_TBE_GD                      I2C_STAT_TBE
#define i2c_data_transmit_gd                 i2c_data_transmit
#define I2C_STAT_TI_GD                       I2C_STAT_TI
#define i2c_address10_enable_gd              i2c_address10_enable
#define i2c_address10_disable_gd             i2c_address10_disable
#define i2c_master_addressing_gd             i2c_master_addressing
#define I2C_MASTER_RECEIVE_GD                I2C_MASTER_RECEIVE
#define i2c_transfer_byte_number_config_gd   i2c_transfer_byte_number_config
#define i2c_start_on_bus_gd                  i2c_start_on_bus
#define I2C_MASTER_TRANSMIT_GD               I2C_MASTER_TRANSMIT
#define I2C_FLAG_I2CBSY_GD                   I2C_FLAG_I2CBSY
#define I2C_FLAG_TC_GD                       I2C_FLAG_TC
#define I2C_FLAG_TI_GD                       I2C_FLAG_TI
#define i2c_stop_on_bus_gd                   i2c_stop_on_bus
#define I2C_FLAG_STPDET_GD                   I2C_FLAG_STPDET
#define i2c_timing_config_gd                 i2c_timing_config
#define i2c_master_clock_config_gd           i2c_master_clock_config
#define i2c_enable_gd                        i2c_enable
#define i2c_flag_clear_gd                    i2c_flag_clear

#endif

#if defined (SOC_SERIES_GD32F5xx)
#define IS_I2C_LEGACY(periph)  ((periph) == I2C0 || (periph) == I2C1 || (periph) == I2C2)
#elif defined (SOC_SERIES_GD32F4xx)
#define IS_I2C_LEGACY(periph)  (1)
#elif defined (SOC_SERIES_GD32H7xx)
#define IS_I2C_LEGACY(periph)  (0)
#endif

static const struct gd32_i2c_bus gd_i2c_config[] = {
#ifdef BSP_USING_HARD_I2C0
    {
    I2C0,    /* uart peripheral index */

    RCU_I2C0, RCU_GPIOB, RCU_GPIOB,    /* periph clock, scl gpio clock, sda gpio clock */

    GPIOB, GPIO_AF_4, GPIO_PIN_8,    /* scl port, scl alternate, scl pin */
    GPIOB, GPIO_AF_4, GPIO_PIN_9,    /* sda port, sda alternate, sda pin */

        &i2c0,
        "hwi2c0",
    },
#endif

#ifdef BSP_USING_HARD_I2C1
    {
    I2C1,    /* uart peripheral index */

    RCU_I2C1, RCU_GPIOH, RCU_GPIOB,    /* periph clock, scl gpio clock, sda gpio clock */

    GPIOH, GPIO_AF_4, GPIO_PIN_4,    /* scl port, scl alternate, scl pin */
    GPIOB, GPIO_AF_4, GPIO_PIN_11,    /* sda port, sda alternate, sda pin */

        &i2c1,
        "hwi2c1",
    },
#endif

#ifdef BSP_USING_HARD_I2C2
    {
    I2C2,    /* uart peripheral index */

    RCU_I2C2, RCU_GPIOA, RCU_GPIOC,    /* periph clock, scl gpio clock, sda gpio clock */

    GPIOA, GPIO_AF_4, GPIO_PIN_8,    /* scl port, scl alternate, scl pin */
    GPIOC, GPIO_AF_4, GPIO_PIN_9,    /* sda port, sda alternate, sda pin */

        &i2c2,
        "hwi2c2",
    },
#endif

#ifdef BSP_USING_HARD_I2C3
    {
    I2C3,    /* uart peripheral index */

    RCU_I2C3, RCU_GPIOF, RCU_GPIOF,    /* periph clock, scl gpio clock, sda gpio clock */

    GPIOF, GPIO_AF_4, GPIO_PIN_14,    /* scl port, scl alternate, scl pin */
    GPIOF, GPIO_AF_4, GPIO_PIN_15,    /* sda port, sda alternate, sda pin */

        &i2c3,
        "hwi2c3",
    },
#endif
#ifdef BSP_USING_HARD_I2C4
    {
    I2C4,    /* uart peripheral index */

    RCU_I2C4, RCU_GPIOG, RCU_GPIOG,    /* periph clock, scl gpio clock, sda gpio clock */

    GPIOG, GPIO_AF_6, GPIO_PIN_7,    /* scl port, scl alternate, scl pin */
    GPIOG, GPIO_AF_6, GPIO_PIN_8,    /* sda port, sda alternate, sda pin */

        &i2c4,
        "hwi2c4",
    },
#endif
#ifdef BSP_USING_HARD_I2C5
    {
    I2C5,    /* uart peripheral index */

    RCU_I2C5, RCU_GPIOF, RCU_GPIOF,    /* periph clock, scl gpio clock, sda gpio clock */

    GPIOF, GPIO_AF_4, GPIO_PIN_11,    /* scl port, scl alternate, scl pin */
    GPIOF, GPIO_AF_4, GPIO_PIN_12,    /* sda port, sda alternate, sda pin */

        &i2c5,
        "hwi2c5",
    }
#endif
};

/**
  * @brief  This function initializes the i2c pin.
  * @param  i2c
  * @retval None
  */
static void gd32_i2c_gpio_init(const struct gd32_i2c_bus *i2c)
{
    /* enable I2C and GPIO clock */
    rcu_periph_clock_enable(i2c->scl_gpio_clk);
    rcu_periph_clock_enable(i2c->sda_gpio_clk);
    rcu_periph_clock_enable(i2c->per_clk);

    /* configure I2C_SCL as alternate function push-pull */
    gpio_af_set(i2c->scl_port, i2c->scl_af, i2c->scl_pin);
    gpio_mode_set(i2c->scl_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, i2c->scl_pin);
#if defined (SOC_SERIES_GD32H7xx)
    gpio_output_options_set(i2c->scl_port, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, i2c->scl_pin);
    /* configure I2C_SDA as alternate function push-pull */
    gpio_af_set(i2c->sda_port, i2c->sda_af, i2c->sda_pin);
    gpio_mode_set(i2c->sda_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, i2c->sda_pin);
    gpio_output_options_set(i2c->sda_port, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, i2c->sda_pin);
#else
    gpio_output_options_set(i2c->scl_port, GPIO_OTYPE_OD, GPIO_OSPEED_50MHZ, i2c->scl_pin);
    /* configure I2C_SDA as alternate function push-pull */
    gpio_af_set(i2c->sda_port, i2c->sda_af, i2c->sda_pin);
    gpio_mode_set(i2c->sda_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, i2c->sda_pin);
    gpio_output_options_set(i2c->sda_port, GPIO_OTYPE_OD, GPIO_OSPEED_50MHZ, i2c->sda_pin);
#endif
}

#if defined(SOC_SERIES_GD32H7xx)

#define GD32_I2C_ERROR_FLAGS (I2C_FLAG_NACK | I2C_FLAG_BERR | I2C_FLAG_LOSTARB | \
                             I2C_FLAG_OUERR | I2C_FLAG_TIMEOUT)
#define GD32_I2C_SPIN_LIMIT  2000000U

/* Missing sensors must return an error, including when SysTick is unavailable.
 * Each byte/condition has a tick deadline and an independent finite poll limit. */
static rt_err_t gd32_i2c_wait(rt_uint32_t periph, rt_uint32_t flag,
                            rt_bool_t asserted, rt_tick_t timeout)
{
    rt_tick_t start = rt_tick_get();
    rt_uint32_t polls = GD32_I2C_SPIN_LIMIT;
    rt_uint32_t status;
    do
    {
        status = I2C_STAT(periph);
        if (status & GD32_I2C_ERROR_FLAGS)
            return -RT_EIO;
        if (((status & flag) != 0U) == asserted)
            return RT_EOK;
        if (flag != I2C_FLAG_STPDET && (status & I2C_FLAG_STPDET))
            return -RT_EIO;
    } while (--polls != 0U && (rt_tick_t)(rt_tick_get() - start) < timeout);
    return -RT_ETIMEOUT;
}

static rt_bool_t gd32_i2c_scl_released(const struct gd32_i2c_bus *config)
{
    rt_uint32_t attempts;
    for (attempts = 0; attempts < 100U; attempts++)
    {
        if (gpio_input_bit_get(config->scl_port, config->scl_pin) != RESET)
            return RT_TRUE;
        rt_hw_us_delay(5);
    }
    return RT_FALSE;
}

/* This BSP uses a single master. Release a slave interrupted mid-byte, then
 * restore alternate functions. A shorted/held-low line is never waited forever. */
static void gd32_i2c_bus_clear(const struct gd32_i2c_bus *config)
{
    rt_uint32_t clocks;
    gpio_bit_set(config->scl_port, config->scl_pin);
    gpio_bit_set(config->sda_port, config->sda_pin);
    gpio_mode_set(config->scl_port, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, config->scl_pin);
    gpio_mode_set(config->sda_port, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, config->sda_pin);
    for (clocks = 0; clocks < 9U; clocks++)
    {
        if (!gd32_i2c_scl_released(config) ||
            gpio_input_bit_get(config->sda_port, config->sda_pin) != RESET)
            break;
        gpio_bit_reset(config->scl_port, config->scl_pin);
        rt_hw_us_delay(5);
        gpio_bit_set(config->scl_port, config->scl_pin);
        rt_hw_us_delay(5);
    }
    /* STOP: release SDA only after SCL has actually risen. */
    gpio_bit_reset(config->scl_port, config->scl_pin);
    gpio_bit_reset(config->sda_port, config->sda_pin);
    rt_hw_us_delay(5);
    gpio_bit_set(config->scl_port, config->scl_pin);
    if (gd32_i2c_scl_released(config))
        rt_hw_us_delay(5);
    gpio_bit_set(config->sda_port, config->sda_pin);
    rt_hw_us_delay(5);
    gd32_i2c_gpio_init(config);
}

static void gd32_i2c_abort(const struct gd32_i2c_bus *config)
{
    rt_uint32_t periph = config->i2c_periph;
    rt_uint32_t control = I2C_CTL0(periph);
    rt_uint32_t timing = I2C_TIMING(periph);
    rt_uint32_t timeout_reg = I2C_TIMEOUT(periph);
    rt_uint32_t polls = 10000U;
    rt_bool_t lost_arbitration = (I2C_STAT(periph) & I2C_FLAG_LOSTARB) != 0U;

    if (!lost_arbitration && i2c_flag_get(periph, I2C_FLAG_I2CBSY))
    {
        i2c_stop_on_bus(periph);
        while (i2c_flag_get(periph, I2C_FLAG_I2CBSY) && --polls != 0U)
        {
        }
    }
    i2c_disable(periph);
    if (!lost_arbitration &&
        (gpio_input_bit_get(config->sda_port, config->sda_pin) == RESET ||
         gpio_input_bit_get(config->scl_port, config->scl_pin) == RESET))
        gd32_i2c_bus_clear(config);

    /* Reset stale START, RELOAD, RX/TX data and error state after an abort. */
    i2c_deinit(periph);
    I2C_TIMING(periph) = timing;
    I2C_TIMEOUT(periph) = timeout_reg;
    I2C_CTL0(periph) = control;
}

static rt_ssize_t gd32_i2c_master_xfer(struct rt_i2c_bus_device *bus,
                                     struct rt_i2c_msg msgs[], rt_uint32_t num)
{
    const struct gd32_i2c_bus *config;
    rt_uint32_t periph;
    rt_uint32_t i;
    rt_uint32_t offset;
    rt_uint32_t chunk;
    rt_uint32_t count;
    rt_uint32_t remaining;
    rt_bool_t continuation;
    rt_bool_t reload;
    rt_err_t result;
    rt_tick_t timeout;
    struct rt_i2c_msg *msg;

    RT_ASSERT(bus != RT_NULL);
    if (num == 0U)
        return 0;
    if (msgs == RT_NULL)
        return -RT_EINVAL;
    config = (const struct gd32_i2c_bus *)bus->priv;
    periph = config->i2c_periph;
    timeout = bus->timeout ? bus->timeout : rt_tick_from_millisecond(20);
    if (timeout == 0U)
        timeout = 1U;

    /* Validate before putting anything on the wire. BYTENUM controls ACK/NACK;
     * slave-only NACKEN and legacy ACK/STOP reception sequences do not apply. */
    for (i = 0U; i < num; i++)
    {
        msg = &msgs[i];
        if ((msg->len != 0U && msg->buf == RT_NULL) ||
            msg->addr > ((msg->flags & RT_I2C_ADDR_10BIT) ? 0x3ffU : 0x7fU) ||
            (msg->flags & ~(RT_I2C_RD | RT_I2C_ADDR_10BIT | RT_I2C_NO_START | RT_I2C_NO_STOP)))
            return -RT_EINVAL;
        if (msg->flags & RT_I2C_NO_START)
        {
            if (i == 0U || msg->len == 0U || msgs[i - 1U].len == 0U ||
                msg->addr != msgs[i - 1U].addr ||
                ((msg->flags ^ msgs[i - 1U].flags) & (RT_I2C_RD | RT_I2C_ADDR_10BIT)))
                return -RT_EINVAL;
        }
    }

    i2c_flag_clear(periph, GD32_I2C_ERROR_FLAGS | I2C_FLAG_STPDET);
    /* TC with BUSY is a previous successful NO_STOP transfer owned by this
     * controller. It can accept a repeated START without waiting for idle. */
    if (!i2c_flag_get(periph, I2C_FLAG_TC))
    {
        result = gd32_i2c_wait(periph, I2C_FLAG_I2CBSY, RT_FALSE, timeout);
        if (result != RT_EOK)
            goto failed;
    }

    for (i = 0U; i < num; i++)
    {
        msg = &msgs[i];
        continuation = i + 1U < num && (msgs[i + 1U].flags & RT_I2C_NO_START);
        offset = 0U;
        remaining = msg->len;
        if (!(msg->flags & RT_I2C_NO_START))
        {
            I2C_CTL1(periph) = 0U;
            if (msg->flags & RT_I2C_ADDR_10BIT)
                i2c_address10_enable(periph);
            /* RT-Thread stores an unshifted 7-bit address. The GD32 API writes
             * SADDRESS verbatim, whose seven-bit field occupies bits 7:1. */
            i2c_master_addressing(periph,
                (msg->flags & RT_I2C_ADDR_10BIT) ? msg->addr : (msg->addr << 1),
                (msg->flags & RT_I2C_RD) ? I2C_MASTER_RECEIVE : I2C_MASTER_TRANSMIT);
        }

        do
        {
            chunk = remaining > 255U ? 255U : remaining;
            reload = remaining > chunk || continuation;
            if (reload)
                i2c_reload_enable(periph);
            else
                i2c_reload_disable(periph);
            i2c_transfer_byte_number_config(periph, chunk);
            if (offset == 0U && !(msg->flags & RT_I2C_NO_START))
                i2c_start_on_bus(periph);
            for (count = 0U; count < chunk; count++)
            {
                result = gd32_i2c_wait(periph,
                    (msg->flags & RT_I2C_RD) ? I2C_FLAG_RBNE : I2C_FLAG_TI,
                    RT_TRUE, timeout);
                if (result != RT_EOK)
                    goto failed;
                if (msg->flags & RT_I2C_RD)
                    msg->buf[offset++] = i2c_data_receive(periph);
                else
                    i2c_data_transmit(periph, msg->buf[offset++]);
            }
            remaining -= chunk;
            result = gd32_i2c_wait(periph, reload ? I2C_FLAG_TCR : I2C_FLAG_TC,
                                   RT_TRUE, timeout);
            if (result != RT_EOK)
                goto failed;
        } while (remaining != 0U);
    }
    if (!(msgs[num - 1U].flags & RT_I2C_NO_STOP))
    {
        i2c_stop_on_bus(periph);
        result = gd32_i2c_wait(periph, I2C_FLAG_STPDET, RT_TRUE, timeout);
        if (result != RT_EOK)
            goto failed;
        i2c_flag_clear(periph, I2C_FLAG_STPDET);
    }
    return num;

failed:
    gd32_i2c_abort(config);
    return result;
}

#else /* Legacy F4/F5 implementations are unchanged. */

/**
  * @brief  read data.
  * @param  i2c_periph
  * @param  *p_buffer
  * @param  data_byte
  * @retval None
  */
static uint8_t gd32_i2c_read(rt_uint32_t i2c_periph, rt_uint8_t *p_buffer, rt_uint16_t data_byte)
{
    if (data_byte == 0) return 1;

#ifdef BSP_USING_RECEIVING_A
    /*
        In single-byte reception, disable ACK because the master needs to send
        NACK after receiving the first byte,indicating no more data will be
        received, then immediately send the stop condition
    */
    if(data_byte == 1)
    {
        /* disable acknowledge */
        i2c_ack_config(i2c_periph, I2C_ACK_DISABLE);
        /* send a stop condition to I2C bus */
        i2c_stop_on_bus(i2c_periph);
    }
#endif

    /* while there is data to be read */
    while(data_byte)
    {
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32F4xx)
        if(IS_I2C_LEGACY(i2c_periph))
        {
#ifdef BSP_USING_RECEIVING_A
            /*
                After receiving the second-to-last byte, ACK should be disabled
                and STOP should be set, to ensure that NACK is sent after receiving
                the last byte and the stop condition is transmitted
            */
            if(2 == data_byte)
            {
                    /* wait until BTC bit is set */
                    while(!i2c_flag_get(i2c_periph, I2C_FLAG_RBNE));
                    /* disable acknowledge */
                    i2c_ack_config(i2c_periph, I2C_ACK_DISABLE);
                    /* send a stop condition to I2C bus */
                    i2c_stop_on_bus(i2c_periph);
            }
#elif defined(BSP_USING_RECEIVING_B)
            /*
                For 3-byte reception: Wait for byte transfer completion, then
                disable ACK so NACK is automatically sent after receiving the
                last byte
                For 2-byte reception: Wait for byte transfer completion, then
                send stop condition to ensure direct stop after receiving the
                last byte instead of sending ACK
            */
            if(3 == data_byte)
            {
                    /* wait until BTC bit is set */
                    while(!i2c_flag_get(i2c_periph, I2C_FLAG_BTC));
                    /* disable acknowledge */
                    i2c_ack_config(i2c_periph, I2C_ACK_DISABLE);
            }
            else if(2 == data_byte)
            {
                    /* wait until BTC bit is set */
                    while(!i2c_flag_get(i2c_periph, I2C_FLAG_BTC));
                    /* send a stop condition to I2C bus */
                    i2c_stop_on_bus(i2c_periph);
            }
#else
#error "Please select the receiving scheme."
#endif
            /* wait until RBNE bit is set */
            if(i2c_flag_get(i2c_periph, I2C_FLAG_RBNE))
            {
                    /* read a byte from the EEPROM */
                    *p_buffer = i2c_data_receive(i2c_periph);
                    /* point to the next location where the byte read will be saved */
                    p_buffer++;
                    /* decrement the read bytes counter */
                    data_byte--;
            }
        }else
#endif
        {
            /* wait until the RBNE bit is set */
            while(!i2c_flag_get_gd(i2c_periph, I2C_FLAG_RBNE_GD));

            /* read a byte */
            *p_buffer = i2c_data_receive_gd(i2c_periph);
            /* point to the next location where the byte read will be saved */
            p_buffer++;
            /* decrement the read bytes counter */
            data_byte--;
        }
    }
    return 0;
}

/**
  * @brief  write data.
  * @param  i2c_periph
  * @param  *p_buffer
  * @param  data_byte
  * @retval None
  */
static uint8_t gd32_i2c_write(rt_uint32_t i2c_periph, uint8_t *p_buffer, uint16_t data_byte)
{
    if (data_byte == 0) return 1;

    while(data_byte)
    {
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32F4xx)
        if(IS_I2C_LEGACY(i2c_periph))
        {
            /* data transmission */
            i2c_data_transmit(i2c_periph, *p_buffer);
            /* point to the next byte to be written */
            p_buffer++;
            /* decrement the write bytes counter */
            data_byte--;
            /* wait until the TI bit is set */
            while(!i2c_flag_get(i2c_periph, I2C_FLAG_BTC));
        }
        else
#endif
        {
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32H7xx)
            /* wait until the transmit data buffer is empty */
            I2C_STAT_GD(i2c_periph) |= I2C_STAT_TBE_GD;
            while(!i2c_flag_get(i2c_periph, I2C_FLAG_TBE));

            while(data_byte)
            {
                /* wait until the TI bit is set */
                while(!i2c_flag_get(i2c_periph, I2C_FLAG_TI_GD));
                /* data transmission */
                i2c_data_transmit(i2c_periph, *p_buffer);
                /* point to the next byte to be written */
                p_buffer++;
                /* decrement the write bytes counter */
                data_byte--;
            }
#endif
        }
    }

    if(data_byte != 0)
    {
        return 1;
    }
    return 0;
}

/**
  * @brief
  * @param
  * @param
  * @param
  * @retval
  */

static rt_ssize_t gd32_i2c_master_xfer(struct rt_i2c_bus_device *bus, struct rt_i2c_msg msgs[], rt_uint32_t num)
{
    static struct rt_i2c_msg *msg;

    rt_uint32_t i,w_total_byte=0,r_total_byte=0;
    rt_err_t ret = RT_ERROR;

    RT_ASSERT(bus != RT_NULL);

    struct gd32_i2c_bus *gd32_i2c = (struct gd32_i2c_bus *)bus->priv;

    for(i = 0; i < num; i++)
    {
        msg = &msgs[i];

        if(msg->flags & RT_I2C_RD)
        {
            r_total_byte += msg->len;
        }else{
            w_total_byte += msg->len;
        }
    }

    for(i = 0; i < num; i++)
    {
        msg = &msgs[i];
        if (!(msg->flags & RT_I2C_NO_START))
        {
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32F4xx)
            if(IS_I2C_LEGACY(gd32_i2c->i2c_periph))
            {

                if(msg->flags & RT_I2C_RD)
                {
                    if(i2c_flag_get(gd32_i2c->i2c_periph, I2C_FLAG_I2CBSY))
                    {
                            i2c_stop_on_bus(gd32_i2c->i2c_periph);
                    }
                    /* enable acknowledge */
                    i2c_ack_config(gd32_i2c->i2c_periph, I2C_ACK_ENABLE);
                    /* i2c master sends start signal only when the bus is idle */
                    while(i2c_flag_get(gd32_i2c->i2c_periph, I2C_FLAG_I2CBSY));
#ifdef BSP_USING_RECEIVING_B
                    /*  */
                    if(msg->len == 2)
                    {
                        i2c_ackpos_config(gd32_i2c->i2c_periph, I2C_ACKPOS_NEXT);
                    }
#endif
                    /* send the start signal */
                    i2c_start_on_bus(gd32_i2c->i2c_periph);
                     /* i2c master sends START signal successfully */
                    while(!i2c_flag_get(gd32_i2c->i2c_periph, I2C_FLAG_SBSEND));

                    i2c_master_addressing(gd32_i2c->i2c_periph, msg->addr, I2C_RECEIVER);

                    while(!i2c_flag_get(gd32_i2c->i2c_periph, I2C_FLAG_ADDSEND));
#ifdef BSP_USING_RECEIVING_B
                    if(msg->len <= 2)
                    {
                        i2c_ack_config(gd32_i2c->i2c_periph, I2C_ACK_DISABLE);
                    }
#endif
                    /* address flag set means i2c slave sends ACK */
                    i2c_flag_clear(gd32_i2c->i2c_periph, I2C_FLAG_ADDSEND);
#ifdef BSP_USING_RECEIVING_B
                    if(msg->len == 1)
                    {
                        i2c_stop_on_bus(gd32_i2c->i2c_periph);
                    }
#endif

               }else {
                    /* configure slave address */
                    while(i2c_flag_get(gd32_i2c->i2c_periph, I2C_FLAG_I2CBSY));
                    //i2c_transfer_byte_number_config(gd32_i2c->i2c_periph, w_total_byte);
                    /* send a start condition to I2C bus */
                    i2c_start_on_bus(gd32_i2c->i2c_periph);
                    while(!i2c_flag_get(gd32_i2c->i2c_periph, I2C_FLAG_SBSEND));

                    i2c_master_addressing(gd32_i2c->i2c_periph, msg->addr, I2C_TRANSMITTER);
                    while(!i2c_flag_get(gd32_i2c->i2c_periph, I2C_FLAG_ADDSEND));

                    i2c_flag_clear(gd32_i2c->i2c_periph, I2C_FLAG_ADDSEND);
               }
            }else
#endif
            {
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32H7xx)
                if(msg->flags & RT_I2C_ADDR_10BIT)
                {
                        /* enable 10-bit addressing mode in master mode */
                    i2c_address10_enable_gd(gd32_i2c->i2c_periph);
                }else {
                        /* disable 10-bit addressing mode in master mode */
                    i2c_address10_disable_gd(gd32_i2c->i2c_periph);
                }

                if(msg->flags & RT_I2C_RD)
                {
                     /* configure slave address */
                    i2c_master_addressing_gd(gd32_i2c->i2c_periph, msg->addr, I2C_MASTER_RECEIVE_GD);

                    i2c_transfer_byte_number_config_gd(gd32_i2c->i2c_periph, r_total_byte);
                     /* send a start condition to I2C bus */
                    i2c_start_on_bus_gd(gd32_i2c->i2c_periph);

                }else {
                     /* configure slave address */
                    i2c_master_addressing_gd(gd32_i2c->i2c_periph, msg->addr, I2C_MASTER_TRANSMIT_GD);
                    while(i2c_flag_get_gd(gd32_i2c->i2c_periph, I2C_FLAG_I2CBSY_GD));
                    i2c_transfer_byte_number_config_gd(gd32_i2c->i2c_periph, w_total_byte);
                     /* send a start condition to I2C bus */
                    i2c_start_on_bus_gd(gd32_i2c->i2c_periph);
                }
#endif
            }
        }

        if(msg->flags & RT_I2C_RD)
        {
            if(gd32_i2c_read(gd32_i2c->i2c_periph, msg->buf, msg->len) != 0)
            {
                LOG_E("i2c bus read failed,i2c bus stop!");
                goto out;
            }
        }else {
            if(gd32_i2c_write(gd32_i2c->i2c_periph, msg->buf, msg->len) != 0)
            {
                LOG_E("i2c bus write failed,i2c bus stop!");
                goto out;
            }
       }
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32H7xx)
        if(!IS_I2C_LEGACY(gd32_i2c->i2c_periph))
        {
            if(r_total_byte != 0)
            {
                while(!i2c_flag_get_gd(gd32_i2c->i2c_periph, I2C_FLAG_TC_GD));
            }
        }
#endif

    }
    ret = i;

out:
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32F4xx)
    if(IS_I2C_LEGACY(gd32_i2c->i2c_periph))
    {

        if(!(msg->flags & RT_I2C_NO_STOP))
        {
            if(msg->flags & RT_I2C_RD)
            {
                while((I2C_CTL0(gd32_i2c->i2c_periph) & I2C_CTL0_STOP));
            }else{
                /* send a stop condition to I2C bus */
                i2c_stop_on_bus(gd32_i2c->i2c_periph);
                /* wait until stop condition generate */
                while((I2C_CTL0(gd32_i2c->i2c_periph) & I2C_CTL0_STOP));
                /* clear the STPDET bit */
            }
        }
    }else
#endif
    {
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32H7xx)
        if(!(msg->flags & RT_I2C_NO_STOP))
        {
            while(!i2c_flag_get_gd(gd32_i2c->i2c_periph, I2C_FLAG_TC_GD));
            /* send a stop condition to I2C bus */
            i2c_stop_on_bus_gd(gd32_i2c->i2c_periph);
            /* wait until stop condition generate */
            while(!i2c_flag_get_gd(gd32_i2c->i2c_periph, I2C_FLAG_STPDET_GD));
            /* clear the STPDET bit */
            i2c_flag_clear_gd(gd32_i2c->i2c_periph, I2C_FLAG_STPDET_GD);
        }
#endif
    }
    return ret;
}

#endif /* SOC_SERIES_GD32H7xx */

static const struct rt_i2c_bus_device_ops i2c_ops = {
    .master_xfer = gd32_i2c_master_xfer,
    .slave_xfer = RT_NULL,
    .i2c_bus_control = RT_NULL
};

/**
  * @brief  I2C initialization function
  * @param  None
  * @retval RT_EOK indicates successful initialization.
  */
int rt_hw_i2c_init(void)
{
    rt_size_t obj_num = sizeof(gd_i2c_config) / sizeof(gd_i2c_config[0]);
//    rt_err_t result;

    for(int i = 0; i < obj_num; i++)
    {

        gd32_i2c_gpio_init(&gd_i2c_config[i]);

        /* configure I2C timing. I2C speed clock=400kHz*/
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32F4xx)
        if(IS_I2C_LEGACY(gd_i2c_config[i].i2c_periph))
        {

            i2c_clock_config(gd_i2c_config[i].i2c_periph, 100000, I2C_DTCY_2);
            i2c_mode_addr_config(gd_i2c_config[i].i2c_periph, I2C_I2CMODE_ENABLE, I2C_ADDFORMAT_7BITS, 0xa0);
            i2c_enable(gd_i2c_config[i].i2c_periph);
            i2c_ack_config(gd_i2c_config[i].i2c_periph, I2C_ACK_ENABLE);
        }else
#endif
        {
#if defined (SOC_SERIES_GD32F5xx) || defined (SOC_SERIES_GD32H7xx)
            i2c_timing_config_gd(gd_i2c_config[i].i2c_periph, 0x1, 0x7, 0);
            i2c_master_clock_config_gd(gd_i2c_config[i].i2c_periph, 0x2D, 0x87);

            /* enable I2C1 */
            i2c_enable_gd(gd_i2c_config[i].i2c_periph);
#endif
        }

        gd_i2c_config[i].i2c_bus->ops = &i2c_ops;
        gd_i2c_config[i].i2c_bus->priv = (void *)&gd_i2c_config[i];
#if defined(SOC_SERIES_GD32H7xx)
        gd_i2c_config[i].i2c_bus->timeout = rt_tick_from_millisecond(20);
#endif

        rt_i2c_bus_device_register(gd_i2c_config[i].i2c_bus, gd_i2c_config[i].device_name);
    }

    return RT_EOK;
}

INIT_BOARD_EXPORT(rt_hw_i2c_init);

#endif /* RT_USING_I2C */
