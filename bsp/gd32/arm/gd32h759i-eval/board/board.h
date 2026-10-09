/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-08-20     BruceOu      first implementation
 */
#ifndef __BOARD_H__
#define __BOARD_H__

#include <stdint.h>
#include "gd32h7xx.h"
#include "drv_usart.h"
#include "drv_gpio.h"

#include "gd32h7xx_exti.h"

#define EXT_SDRAM_BEGIN    (0xC0000000U) /* the begining address of external SDRAM */
#define EXT_SDRAM_END      (EXT_SDRAM_BEGIN + (32U * 1024 * 1024)) /* the end address of external SDRAM */

/* <o> Internal AXI SRAM memory size[Kbytes] <8-832>*/
/* <i>GD32H7xx_DFP 1.5.0 defines GD32H759IM AXI SRAM as 832 KiB.
 * Older demo project metadata still says 896 KiB.  Using that value places
 * the RT-Thread heap tail outside AXI SRAM and faults during heap init.
 */
#ifdef __ICCARM__
/* Use *.icf ram symbal, to avoid hardcode.*/
extern char __ICFEDIT_region_RAM_end__;
#define GD32_SRAM_END          &__ICFEDIT_region_RAM_end__
#else
#define GD32_SRAM_SIZE         832
#define GD32_SRAM_END          (0x24000000 + GD32_SRAM_SIZE * 1024)
#endif

/* The TLI framebuffer is deliberately outside the linker-managed objects,
 * at the same address used by the vendor 29_TLI_Touch_Draw example.  Keep
 * the RT-Thread heap above it so allocations can never overwrite the pixels.
 */
#define GD32_TLI_FRAMEBUFFER_START  ((uintptr_t)0x24005000U)
#define GD32_TLI_FRAMEBUFFER_BYTES  ((uintptr_t)(480U * 272U * 2U))
#define GD32_TLI_FRAMEBUFFER_END    (GD32_TLI_FRAMEBUFFER_START + GD32_TLI_FRAMEBUFFER_BYTES)

#ifdef __ARMCC_VERSION
extern int Image$$RW_IRAM1$$ZI$$Limit;
#define GD32_IMAGE_ZI_END ((uintptr_t)&Image$$RW_IRAM1$$ZI$$Limit)
#define HEAP_BEGIN    ((void *)((GD32_IMAGE_ZI_END > GD32_TLI_FRAMEBUFFER_END) ? \
                                GD32_IMAGE_ZI_END : GD32_TLI_FRAMEBUFFER_END))
#elif __ICCARM__
#pragma section="HEAP"
#define HEAP_BEGIN    (__segment_end("HEAP"))
#else
extern int __bss_end;
#define GD32_IMAGE_ZI_END ((uintptr_t)&__bss_end)
#define HEAP_BEGIN    ((void *)((GD32_IMAGE_ZI_END > GD32_TLI_FRAMEBUFFER_END) ? \
                                GD32_IMAGE_ZI_END : GD32_TLI_FRAMEBUFFER_END))
#endif

#define HEAP_END          GD32_SRAM_END

#endif /* __BOARD_H__ */
