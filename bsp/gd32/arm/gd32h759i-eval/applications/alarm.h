/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GD32_ENV_ALARM_H
#define GD32_ENV_ALARM_H

#include <rtthread.h>
#include <gd32h7xx.h>

/*
 * 报警输出说明
 *
 * 传感器线程只更新 env_log 中的 alarm_flags，真正的 GPIO 操作统一由本文件
 * 对应的 alarm 线程执行。这样 LVGL 回调、SD 卡文件操作不会直接操作 GPIO，
 * 也不会因为 FAT/SDIO 阻塞而丢失 LED 与蜂鸣器的闪烁节拍。
 * LED2 与蜂鸣器共用 250 ms 开 / 250 ms 关的节奏；GD32_ALARM_USE_ACTIVE
 * 只改变蜂鸣器输出引脚，不改变阈值判断和 LED 行为。
 */

/* Board wiring: LED2 is PA6 on the GD32H759I-EVAL-V2.x.
 *
 * PC6 is retained for the original passive-buzzer connection for wiring
 * compatibility.  The replacement active buzzer is controlled by PE2 when
 * GD32_ALARM_USE_ACTIVE is enabled.  PE2 (JP7 pin 1) is a digital enable only: use an
 * active-buzzer module or a transistor/MOSFET driver and do not connect a
 * high-current load directly to the MCU pin. */
#define GD32_ALARM_LED_PORT       GPIOA
#define GD32_ALARM_LED_PIN        GPIO_PIN_6
#define GD32_ALARM_PASSIVE_PORT   GPIOC
#define GD32_ALARM_PASSIVE_PIN    GPIO_PIN_6
#define GD32_ALARM_ACTIVE_PORT    GPIOE
#define GD32_ALARM_ACTIVE_PIN     GPIO_PIN_2

/* 蜂鸣器默认启用。该选项只决定是否驱动输出，不代表 MCU 引脚可以直接带动
 * 裸蜂鸣器；实际硬件应使用带限流/驱动级的模块。 */
/* Alarm output is enabled by default. The output driver is deliberately
 * current-limited in software (short alarm windows); use a transistor or a
 * buzzer module, never connect a bare buzzer directly to an MCU pin. Set this
 * to 0 while checking the LED and threshold logic without an external load. */
#ifndef GD32_ALARM_BUZZER_ENABLE
#define GD32_ALARM_BUZZER_ENABLE  1
#endif

/* 编译期选择输出通道：0 保留 PC6 无源蜂鸣器，1 使用 PE2 有源模块。修改后
 * 必须重新编译；运行时不会自动探测模块类型。 */
/* Select the alarm output at build time.  Keep the original passive-buzzer
 * path as the default for wiring compatibility; set this to 1 when an active
 * buzzer module is fitted to PE2. PC5 is connected to the Ethernet PHY's
 * RXD1 output and must not be used, even when the Ethernet driver is off.
 * PE2 is also TRACE_CK: use SWD debugging without parallel trace. */
#ifndef GD32_ALARM_USE_ACTIVE
#define GD32_ALARM_USE_ACTIVE     0
#endif

/* 低电平有效的三线模块需要在空闲时保持高电平；GD32_ALARM_PC6_ACTIVE_LOW
 * 只负责转换“逻辑有效”与 GPIO 电平的关系。 */
/* The connected three-pin buzzer module triggers on a low level.
 * Keep PC6 high while idle, including when PE2 is selected. Set to 0 for a
 * high-level-trigger driver instead. Gate mode uses this level directly;
 * passive mode applies it to each half-cycle. During MCU reset the pin is high
 * impedance; an external pull-up to 3.3 V is needed if the module lacks one. */
#ifndef GD32_ALARM_PC6_ACTIVE_LOW
#define GD32_ALARM_PC6_ACTIVE_LOW 1
#endif

/* PC6_GATE_MODE=0 时，PC6 输出约 500 Hz 方波驱动无源蜂鸣器；设为 1 时，
 * PC6 只输出静态有效电平，适用于模块自带振荡器的有源/门控输入。 */
/* Keep the original PC6 passive-buzzer path as the default. It outputs a
 * continuous 500 Hz square wave during an alarm. Set this to 1 only when the
 * three-pin OUT/VCC/GND module has its own oscillator and expects a static
 * low-level gate; in that mode PC6 is held active for the complete alarm. */
#ifndef GD32_ALARM_PC6_GATE_MODE
#define GD32_ALARM_PC6_GATE_MODE 0
#endif

/* 报警节拍，LED2 和蜂鸣器在同一个 on/off 窗口内更新。两个值都必须非零，
 * 否则可能出现忙循环或无法结束的报警状态。 */
/* Alarm cadence. LED2 and the buzzer are synchronized: both are active during
 * the beep window and inactive during the quiet window. Both values must be
 * non-zero. */
#ifndef GD32_ALARM_BEEP_ON_MS
#define GD32_ALARM_BEEP_ON_MS      250U
#endif
#ifndef GD32_ALARM_BEEP_OFF_MS
#define GD32_ALARM_BEEP_OFF_MS     250U
#endif

int gd32_alarm_init(void);

#endif
