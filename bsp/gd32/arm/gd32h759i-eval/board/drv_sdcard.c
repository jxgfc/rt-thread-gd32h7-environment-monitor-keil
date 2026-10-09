/*
 * GD32H759I-EVAL SDIO0 块设备适配层。
 * SPDX-License-Identifier: Apache-2.0
 *
 * 使用 18_SDIO_SDCardTest 协议驱动，以 1-bit 轮询方式传输。
 * JP52 保持 LCD 桥接位（1-2），JP65 选择 SDIO（2-3）。
 * SDIO 只占用 PB13/D0、PC12/CLK、PD2/CMD，PC11 仍作为 LCD B4。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "drv_sdcard.h"
#include "sdcard.h"
#include "gd32h7xx_sdio.h"

#ifdef BSP_USING_SD_CARD

#define SD_SECTOR_SIZE       512U
#define SD_READY_TIMEOUT_MS  2000
#define SD_LOCK_TIMEOUT_MS   100
#define SD_CARD_LOCKED       (1UL << 25)
#define SD_READY_FOR_DATA    (1UL << 8)
#define SD_STATE_MASK       (15UL << 9)
#define SD_STATE_TRANSFER   (4UL << 9)

static struct rt_device sd_device;
static struct rt_mutex sd_lock;
static struct rt_device_blk_geometry sd_geometry;
static sd_card_info_struct sd_info;
static rt_bool_t lock_initialized;
static rt_bool_t device_registered;
static rt_bool_t card_ready;
static sd_error_enum last_error = SD_ERROR;

/* Filesystem operations must never wait forever for a card transaction.  A
 * bounded lock is especially important during hot-unplug/retry: an envlog
 * worker that owns the lock while a peripheral is absent must not prevent
 * the rest of the application from running. */
static rt_err_t sd_lock_take_bounded(void)
{
    return rt_mutex_take(&sd_lock, rt_tick_from_millisecond(SD_LOCK_TIMEOUT_MS));
}

/* All buffers supplied by DFS pass through this aligned sector.  In polling
 * mode the CPU accesses both FIFO and RAM, so no D-cache invalidate/clean is
 * needed and no caller-owned partial cache line is ever invalidated. */
rt_align(32)
static uint32_t sd_sector[SD_SECTOR_SIZE / sizeof(uint32_t)];

static rt_err_t sd_wait_ready(void)
{
    rt_tick_t started = rt_tick_get();
    uint32_t status = 0;

    /* CMD13 返回的卡状态必须同时满足 READY_FOR_DATA 且处于 TRANSFER；
     * 不能只看一个 bit，否则写卡仍处于 PROGRAMMING 状态时可能提前返回。 */
    do
    {
        last_error = sd_cardstatus_get(&status);
        if (last_error != SD_OK)
            return -RT_EIO;
        if (status & SD_CARD_LOCKED)
        {
            last_error = SD_LOCK_UNLOCK_FAILED;
            return -RT_EIO;
        }
        if ((status & SD_READY_FOR_DATA) &&
            (status & SD_STATE_MASK) == SD_STATE_TRANSFER)
            return RT_EOK;
        rt_thread_mdelay(1);
    } while ((rt_tick_t)(rt_tick_get() - started) <
             rt_tick_from_millisecond(SD_READY_TIMEOUT_MS));

    last_error = SD_DATA_TIMEOUT;
    return -RT_ETIMEOUT;
}

static rt_err_t sd_device_init(rt_device_t dev)
{
    (void)dev;
    return card_ready ? RT_EOK : -RT_EIO;
}

static rt_err_t sd_device_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev;
    (void)oflag;
    return card_ready ? RT_EOK : -RT_EIO;
}

static rt_err_t sd_device_close(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}

static rt_bool_t sd_range_valid(rt_off_t pos, rt_size_t sectors)
{
    rt_uint64_t start;
    if (pos < 0 || !card_ready)
        return RT_FALSE;
    /* 用 64 位计算尾端，避免 pos + sectors 在 32 位平台上溢出。 */
    start = (rt_uint64_t)pos;
    return start < sd_geometry.sector_count &&
           (rt_uint64_t)sectors <= sd_geometry.sector_count - start;
}

static rt_ssize_t sd_device_read(rt_device_t dev, rt_off_t pos,
                                void *buffer, rt_size_t sectors)
{
    rt_size_t done = 0;
    rt_uint8_t *out = (rt_uint8_t *)buffer;
    (void)dev;
    if (sectors == 0)
        return 0;
    if (buffer == RT_NULL)
        return -RT_EINVAL;
    if (sd_lock_take_bounded() != RT_EOK)
        return -RT_EBUSY;
    if (!sd_range_valid(pos, sectors))
    {
        rt_mutex_release(&sd_lock);
        return -RT_EIO;
    }

    /* 底层 API 一次按扇区传输；失败时返回已完成扇区数，DFS 可据此
     * 识别部分完成。若首扇区失败则返回负错误码。 */
    for (done = 0; done < sectors; ++done)
    {
        /* The H7 example accepts a sector index for both SDSC and SDHC.
         * It converts SDSC addresses to bytes internally. */
        last_error = sd_block_read(sd_sector, (rt_uint32_t)pos + done,
                                   SD_SECTOR_SIZE);
        if (last_error != SD_OK)
        {
            card_ready = RT_FALSE;
            break;
        }
        rt_memcpy(out + done * SD_SECTOR_SIZE, sd_sector, SD_SECTOR_SIZE);
    }
    rt_mutex_release(&sd_lock);
    return done ? (rt_ssize_t)done : -RT_EIO;
}

static rt_ssize_t sd_device_write(rt_device_t dev, rt_off_t pos,
                                 const void *buffer, rt_size_t sectors)
{
    rt_size_t done = 0;
    const rt_uint8_t *in = (const rt_uint8_t *)buffer;
    (void)dev;
    if (sectors == 0)
        return 0;
    if (buffer == RT_NULL)
        return -RT_EINVAL;
    if (sd_lock_take_bounded() != RT_EOK)
        return -RT_EBUSY;
    if (!sd_range_valid(pos, sectors))
    {
        rt_mutex_release(&sd_lock);
        return -RT_EIO;
    }
    /* 写保护在软件层提前拒绝，避免无意义地发起写命令。 */
    if (sd_info.card_csd.perm_write_protect || sd_info.card_csd.tmp_write_protect)
    {
        last_error = SD_WP_VIOLATION;
        rt_mutex_release(&sd_lock);
        return -RT_EIO;
    }

    for (done = 0; done < sectors; ++done)
    {
        rt_memcpy(sd_sector, in + done * SD_SECTOR_SIZE, SD_SECTOR_SIZE);
        last_error = sd_block_write(sd_sector, (rt_uint32_t)pos + done,
                                    SD_SECTOR_SIZE);
        if (last_error != SD_OK)
        {
            card_ready = RT_FALSE;
            break;
        }
    }
    rt_mutex_release(&sd_lock);
    return done ? (rt_ssize_t)done : -RT_EIO;
}

static rt_err_t sd_device_control(rt_device_t dev, int cmd, void *args)
{
    rt_err_t result = RT_EOK;
    (void)dev;
    /* 读写、同步和几何信息访问共用同一把锁，防止卡状态被并发修改。 */
    if (sd_lock_take_bounded() != RT_EOK)
        return -RT_EBUSY;

    if (cmd == RT_DEVICE_CTRL_BLK_GETGEOME)
    {
        if (args == RT_NULL)
            result = -RT_EINVAL;
        else
            rt_memcpy(args, &sd_geometry, sizeof(sd_geometry));
    }
    else if (cmd == RT_DEVICE_CTRL_BLK_SYNC)
    {
        result = card_ready ? sd_wait_ready() : -RT_EIO;
        if (result != RT_EOK)
            card_ready = RT_FALSE;
    }
    else
        result = -RT_ENOSYS;

    rt_mutex_release(&sd_lock);
    return result;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops sd_ops =
{
    sd_device_init, sd_device_open, sd_device_close,
    sd_device_read, sd_device_write, sd_device_control
};
#endif

int gd32_sdcard_init(void)
{
    rt_err_t result;
    rt_err_t wait_result = RT_EOK;
    const char *failed_stage = "sd_init";
    /* 初始化允许在卡拔出后再次调用；但若设备仍被挂载，不能重置底层卡。 */
    if (!lock_initialized)
    {
        result = rt_mutex_init(&sd_lock, "sd0", RT_IPC_FLAG_PRIO);
        if (result != RT_EOK)
            return result;
        lock_initialized = RT_TRUE;
    }
    result = sd_lock_take_bounded();
    if (result != RT_EOK)
    {
        rt_kprintf("sd0: init lock timeout (%d ms)\n", SD_LOCK_TIMEOUT_MS);
        return result;
    }
    if (card_ready)
    {
        rt_mutex_release(&sd_lock);
        return RT_EOK;
    }
    /* An open filesystem must be unmounted before reinitializing the card. */
    if (device_registered && sd_device.ref_count != 0)
    {
        rt_mutex_release(&sd_lock);
        return -RT_EBUSY;
    }

    /* Keep the failing phase in the startup message.  Error 22 is generated
     * by the data FIFO path, so this distinguishes card discovery from the
     * SCR/bus-width transfer without changing the protocol driver. */
    rt_kprintf("SD INIT: lock acquired\n");
    failed_stage = "sd_init";
    rt_kprintf("SD INIT: sd_init begin\n");
    last_error = sd_init();
    rt_kprintf("SD INIT: sd_init done result=%d\n", last_error);
    if (last_error == SD_OK)
    {
        failed_stage = "card_info";
        rt_kprintf("SD INIT: card_info begin\n");
        last_error = sd_card_information_get(&sd_info);
        rt_kprintf("SD INIT: card_info done result=%d\n", last_error);
    }
    if (last_error == SD_OK)
    {
        failed_stage = "card_select";
        rt_kprintf("SD INIT: card_select begin\n");
        last_error = sd_card_select_deselect(sd_info.card_rca);
        rt_kprintf("SD INIT: card_select done result=%d\n", last_error);
    }
    if (last_error == SD_OK)
    {
        failed_stage = "bus_mode";
        rt_kprintf("SD INIT: bus_mode begin\n");
        last_error = sd_bus_mode_config(SDIO_BUSMODE_1BIT, SD_SPEED_DEFAULT);
        rt_kprintf("SD INIT: bus_mode done result=%d\n", last_error);
    }
    if (last_error == SD_OK)
    {
        failed_stage = "transfer_mode";
        rt_kprintf("SD INIT: transfer_mode begin\n");
        last_error = sd_transfer_mode_config(SD_POLLING_MODE);
        rt_kprintf("SD INIT: transfer_mode done result=%d\n", last_error);
    }
    if (last_error == SD_OK)
    {
        failed_stage = "wait_ready";
        rt_kprintf("SD INIT: wait_ready begin\n");
        wait_result = sd_wait_ready();
        rt_kprintf("SD INIT: wait_ready done result=%d sd_error=%d\n",
                   wait_result, last_error);
    }
    if (last_error != SD_OK || wait_result != RT_EOK)
    {
        rt_mutex_release(&sd_lock);
        rt_kprintf("sd0: unavailable, stage=%s, SD error=%d, SDIO_STAT=0x%08lx (1-bit PB13/PC12/PD2)\n",
                   failed_stage, last_error,
                   (unsigned long)SDIO_STAT(SDIO));
        return -RT_EIO;
    }

    sd_geometry.bytes_per_sector = SD_SECTOR_SIZE;
    sd_geometry.block_size = SD_SECTOR_SIZE;
    if (sd_info.card_type == SDIO_HIGH_CAPACITY_SD_CARD)
        sd_geometry.sector_count = ((rt_uint64_t)sd_info.card_csd.c_size + 1U) * 1024U;
    else
        sd_geometry.sector_count = (rt_uint64_t)sd_info.card_capacity / SD_SECTOR_SIZE;
    if (sd_geometry.sector_count == 0)
    {
        last_error = SD_PARAMETER_INVALID;
        rt_mutex_release(&sd_lock);
        return -RT_EIO;
    }

    if (!device_registered)
    {
        sd_device.type = RT_Device_Class_Block;
#ifdef RT_USING_DEVICE_OPS
        sd_device.ops = &sd_ops;
#else
        sd_device.init = sd_device_init;
        sd_device.open = sd_device_open;
        sd_device.close = sd_device_close;
        sd_device.read = sd_device_read;
        sd_device.write = sd_device_write;
        sd_device.control = sd_device_control;
#endif
        result = rt_device_register(&sd_device, "sd0", RT_DEVICE_FLAG_RDWR |
                                    RT_DEVICE_FLAG_REMOVABLE | RT_DEVICE_FLAG_STANDALONE);
        if (result != RT_EOK)
        {
            rt_mutex_release(&sd_lock);
            return result;
        }
        device_registered = RT_TRUE;
    }
    card_ready = RT_TRUE;
    rt_mutex_release(&sd_lock);
    rt_kprintf("sd0: ready, %lu MiB, SDIO0 1-bit polling\n",
               (unsigned long)(sd_geometry.sector_count / 2048U));
    return RT_EOK;
}

rt_bool_t gd32_sdcard_is_ready(void)
{
    return card_ready;
}

int gd32_sdcard_last_error(void)
{
    return (int)last_error;
}

#ifdef RT_USING_FINSH
static void sdcard_status(void)
{
    rt_kprintf("sd0: %s, registered=%d, last SD error=%d\n",
               card_ready ? "ready" : "offline", device_registered, last_error);
    if (device_registered)
        rt_kprintf("capacity=%lu MiB, sector=512 bytes\n",
                   (unsigned long)(sd_geometry.sector_count / 2048U));
    rt_kprintf("SDIO0 1-bit: D0=PB13 CLK=PC12 CMD=PD2; JP52=1-2 JP65=2-3\n");
}
MSH_CMD_EXPORT_ALIAS(sdcard_status, sdcard, Show SD card status and wiring);
#endif

#endif /* BSP_USING_SD_CARD */
