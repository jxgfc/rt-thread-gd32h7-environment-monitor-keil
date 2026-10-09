# 基于 GD32H7 的多功能环境监测与本地数据记录仪系统

这是一个可直接用 Keil MDK 打开的 GD32H759I-EVAL 完整工程。工程基于 RT-Thread 5.3.0 和 LVGL 8.3.11，实现温湿度、光照、eCO₂、TVOC 采集，中文 LCD 触摸界面，阈值声光报警，RTC 时间设置，以及 FAT32 SD 卡本地 CSV 记录。

当前版本使用官方 `29_TLI_Touch_Draw` 对应的 TLI RGB 并行 LCD，不使用 SPI LCD；以太网和 TCP 上传功能未启用。

## 开发环境

- Keil MDK-ARM 5
- ARM Compiler 5.06 update 5（build 528）
- GigaDevice `GD32H7xx_DFP 1.5.0`
- GD32H759I-EVAL 开发板与板载 GD-Link

Keil、器件支持包和 GD-Link 下载器不包含在 Git 仓库中，需要在电脑上单独安装。

## 编译和烧录

1. 安装上面的 Keil 和 GD32H7xx DFP。
2. 克隆仓库后进入目录：

   ```powershell
   git clone <你的仓库地址>
   cd gd32h7-environment-monitor-keil\bsp\gd32\arm\gd32h759i-eval
   ```

3. 双击 `project.uvprojx`，选择目标 `rtthread`。
4. 在 Keil 中执行 `Project -> Rebuild all target files`。
5. 连接板载 GD-Link，使用 SWD 下载按钮烧录。

工程中的相对路径已经保留，不能只上传 `bsp/gd32/arm/gd32h759i-eval` 子目录。Keil 工程需要仓库根目录中的 `components`、`include`、`libcpu`、`src`，以及 `bsp/gd32/arm/libraries/gd32_drivers`。

工程使用 `board/linker_scripts/link.sct`，Keil 构建完成后会通过 `fromelf` 生成 `rtthread.bin`。下载前确认 Keil 的 Device 仍为 `GD32H759IM`，Flash 算法使用 GD32H7xx 3840 KB 配置。

## 主要功能

- SHT40：温度和湿度；BH1750：光照；SGP30：等效 CO₂ 和 TVOC。
- 480×272 RGB565 TLI LCD、GT911 电容触摸、中文首页／曲线／设置／历史页面。
- 温度、湿度、光照上下限，eCO₂ 和 TVOC 上限；LED2 PA6 与蜂鸣器间断报警。
- SD 保存间隔和曲线采样间隔分别可设置，范围 1 秒至 24 小时。
- 默认 PC6 无源蜂鸣器，约 500 Hz、250 ms 响／250 ms 停；备用 PE2 有源通道由 `applications/alarm.h` 编译宏选择。
- FAT16/FAT32 卡保存 `envlog.csv`，配置使用 `ecfg0.bin` 和 `ecfg1.bin` 双槽校验。

## 主要接线

| 功能 | 引脚或跳线 |
| --- | --- |
| 传感器 I2C1 | PH4 SCL、PB11 SDA，3.3 V，共地 |
| GT911 | PH7 SCL、PF9 SDA、PF6 RST、PH6 INT |
| SDIO 单线 | PB13 D0、PC12 CLK、PD2 CMD，JP65=2-3 |
| LCD | 采用官方 TLI 接线，JP52=1-2 |
| LED2 | PA6，JP66=2-3 |
| 默认无源蜂鸣器 | PC6，低电平有效模块 |
| USART0 | PA9/PA10，115200 8-N-1，JP68=2-3 |
| RTC 电池 | VBAT，JP0=2-3 |

蜂鸣器信号必须接带驱动的模块或外部驱动级，不能让 MCU GPIO 直接带动大电流裸负载。PF10、PB8/PB9、PC11 已被 LCD 占用，不能重复接线。

## 许可

本工程保留 RT-Thread、GD32、LVGL、CMSIS、FatFs 和字体源码的原版权与许可文件。工程代码用于学习和项目复现，重新分发时请保留对应源码许可声明。
