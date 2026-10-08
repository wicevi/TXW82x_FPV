# pmu — NE102 电源管理协处理器固件（N32L403KBQ7，独立 GCC 工程）

## 用途
NE102 板上常电域 PMU 的独立固件工程。主控 TXW828 可被 PMU 整体断电；本固件负责电源时序、PIR/按键事件、IR-CUT/补光控制及与主控的串口协议。**注意：这是独立 MCU 工程，与 TXW82x 固件（txw82xApp）分开构建、分开烧录，不注册进 CDK 工程。**

## 入口与构建
- 源码：`src/main.c`（应用骨架，引脚定义在 `inc/main.h`，依据 `custom/doc/NE102 系统IO与连接关系.md` §4）
- 构建（GCC arm-none-eabi，参考官方 STANDBY/GCC 例程）：

  ```bash
  cd custom/pmu
  make GCC_PATH="E:/STM32CubeIDE_1.18.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344/tools/bin"
  # 产物：build/pmu_ne102.{elf,hex,bin}
  ```

  arm-none-eabi-gcc 在 PATH 时可省略 GCC_PATH；本机工具链为 STM32CubeIDE 自带 GNU Arm 13.3。
- 下载（J-Link SWD，器件名 N32L403KB）：`make flash`（或 `jlink/flash.bat [JLink.exe路径]`）。
  J-Link 若报未知器件，先安装 `../doc/n32l40x/tools/JLink_tool_adds_Nations_chip_V1.6.0.zip`。
- 调试串口：USART1 PA9(TX)/PA10(RX) 115200（**注意 USART1 在 AF4，不是 STM32 习惯的 AF7**）。
- SWD：PA13=TMS/SWDIO、PA14=TCK/SWCLK（TP 测试点 T7/T8）。

## 依赖
- `lib/Nations.N32L40x_Library.2.3.0/`：官方库 vendored 子集（CMSIS core/device + 全部标准外设驱动，2.4MB，含版本记录）。完整库在 `E:\n32_workspace\n32l40x_docs\`（团队共享盘），本目录子集已够用，勿混用其他版本。
- `ld/n32l40x_flash_kb.ld`：NE102 专用链接脚本——**N32L403KB = 128K Flash + 24K SRAM**（数据手册表 1-1）；官方默认脚本 RAM 标 32K 对本型号偏大，勿改回。

## 当前状态
可用（最小 bring-up 固件，2026-09-29）
已验证：GCC 13.3 完整编译链接通过（Flash 5.5K/128K，RAM ~3K/24K）
未验证：上板烧录运行、与 TXW828 联调

**当前固件行为（刻意最小化，完整 PMU 逻辑后续再写）**：
1. 启动后按顺序上电主控域：`PMU_DC_EN`（PA4）拉高使能 DC-DC——**同时开出 3V3 主轨（U6）与 1.2V 核心轨（U7）**，延时 5ms 后 `PMU_PWR_3V3`（PA5）拉高把 3V3 导通至 SYS_3V3；
2. 其余所有与 TXW828 相连/共用的引脚**保持高阻**（复位默认浮空输入，代码中显式重新声明）：PB1 复位线、PB6/PB7 命令串口、PA11 IRCUT、PB5 补光 PWM、PA1 IR_EN；
3. SysTick 1ms 时基，每秒经调试串口（USART1 PA9/PA10 115200，仅接 T2/T3 测试点，不与主控共用）打印 `[PMU] uptime HH:MM:SS (N s)`。

## 已知问题 / TODO
- PB1（PMU_RST_SYS）当前高阻浮空——主控复位完全依赖 TXW PB14 侧上拉，主控能否正常释放复位待上板确认
- PB6/PB7（PMU↔TXW828 命令串口）对应的外设与 AF 编号未定——需查数据手册引脚复用表（PDF 表格提取困难，建议直接翻 PDF 第 3 章或用 N32Cube 工具确认）
- 主控上电完整时序（复位钳住→3V3→1V2→释放复位）待联调时实现；此前版本骨架中的时序函数已按"最小固件"要求移除，逻辑保留在文档《NE102 系统IO与连接关系.md》§6
- PA2 光敏 ADC、PB5 补光 PWM、STANDBY/STOP2 低功耗策略未实现（低功耗模式选型见 `../doc/n32l40x/低功耗应用笔记`）
- STANDBY 下 GPIO 输出高阻：PA5/PA4 浮空会切断主控电源，进 STANDBY 前必须先解决电源保持（或改用 STOP 模式）
- 与主控的命令协议未定义（与 TXW82x 侧 `custom/component/` 联动设计）

## 变更记录
| 日期 | 摘要 |
|------|------|
| 2026-09-29 | 初始创建：vendored 官方库 2.3.0 子集、Makefile（GCC）、KB 专用链接脚本（24K RAM）、NE102 引脚骨架、J-Link 下载脚本；编译验证通过 |
| 2026-09-29 | 固件改为最小 bring-up 版（应用户要求）：PA4（DC_EN，**同开 3V3+1V2 两轨**）→ 延时 → PA5（PWR_3V3，导通 SYS_3V3）顺序上电主控域 + 其余 TXW 相连/共用引脚全部高阻 + SysTick 每秒打印 uptime；移除上电时序/按键/PIR/STANDBY 演示逻辑 |
