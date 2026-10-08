# app/ — 低功耗相机产品应用层（app_lowpwr_camera）

| 文件 | 角色 |
|------|------|
| `app_lowpwr_camera.c/.h` | 产品应用入口（编排层）：板级电源时序 + XIP 运行时提速 + 组件初始化编排 |

## 用途

自研低功耗相机（NE102 板）的应用层入口，承担三件事：

1. **XIP 运行时提速**（`ll_xip_clock_init(60)`，在 `app_board_power_init()` 开头 = `main()` 第一句，启动 [3ms] 即生效）；
2. **板级电源时序**（`app_board_power_init()`，在 `main()` 最先执行）；
3. **组件编排**（`app_lowpwr_camera_init()`，由官方 `usr_app_init()` 钩子调用，为后续业务预留）。

## 入口

- `app_board_power_init()`：`main()` 开头、`sys_app_init()` 之前调用——**第一句是 XIP 提速（见下）**，随后是电源开关时序（demo 的 SD/相机初始化在 `sys_app_init()` 内部执行，电源开关必须先行）；
- `app_lowpwr_camera_init()`：`usr_app_init()` 调用——启动 LED 心跳，为后续业务预留；
- 构建注册：`txw82xApp.cdkproj` 顶层 `custom` 虚拟目录（FLASH 配置）。

## XIP 运行时提速（关键修复，2026-10-08；当天二次优化：修正位置上移）

镜像头的 `SPI_CLK_MHZ=0x1c`（冷启动修复所需，见 `project/txw82xApp/makecode.ini`）会被 ROM 写进 fw_info 结构体（0x2006be00+0x18），`SystemInit` 的 `ll_xip_clock_init(0)` 以它为源 → **运行时 XIP 也只有 ~26MHz，取指带宽 1-3MB/s，全固件爬行**（表现为 1080p 编码 4.7fps、"CPU 100%"）。修复：`ll_xip_clock_init(60)` 运行时提速（boot 头保持 0x1c，烧录/启动不受影响；`ll_qspi_clock_auto_adjust` 自动调延迟，60MHz 实测稳定）。效果：主码流 1080p 4.7→**17.9fps**、子码流 720p **17.9fps**。

**位置演进**：最初放在 `app_lowpwr_camera_init()`（`usr_app_init()` 钩子）——但 `usr_app_init()` 排在 `sys_wifi_init()`/`sys_app_init()`（整个 demo init）**之后**，启动主体 [3→664ms] 一直在跑 28MHz XIP。上移到 `app_board_power_init()` 开头（`main()` 第一句）后 [3ms] 即生效，全链路再快 ~31ms（sensor 就绪 548→513ms）。注意：header 时钟本身只覆盖 reset→main 入口（1c 下实测 **~2ms**），对启动速度无实质影响，1c 档永久保留（详见 NE102 文档 §10"flash 启动速率定案"）。

## 板级电源时序（app_board_power_init）

| 开关 | 引脚 | 参数名 | 时机 | 说明 |
|------|------|--------|------|------|
| CAM_EN | PB6 | `PIN_CAM_EN`（pin_param 尾部新增） | ① 先拉低 | 相机电源总控（sensor 1V2 DVDD 轨） |
| VCAM/VCAM2 | 片内 LDO | 代码直调 `pmu_vcam_ldo_en/pmu_vcam2_ldo_en` | ② LDO 先上（AVDD 2.8V → IOVDD 1.8V） | VCAM2=1V8_CSI 是 sensor IOVDD + 相机 I2C 上拉轨，**必须开** |
| CAM_EN | PB6 | — | ③ 10ms 后拉高（DVDD 最后上）+ 20ms 稳定 | 手册：三轨任意顺序可行，实测早期竞争窗口需间隔 |
| TF_PWR_EN | PB7 | `PIN_TF_PWR_EN` | 最后拉高 + 10ms | TF 卡供电 |
| 状态 LED | PC0 | — | 常亮→心跳 | 高电平点亮（Q3 NPN 驱动） |

电源未配置（config.cfg 无该键 → 0xFF）时自动跳过，固件可移植回学习板。

## 状态 LED 心跳

| LED 状态 | 含义 |
|----------|------|
| 常灭 | 固件未运行（PMU 供电/烧录问题） |
| 常亮 | 已进 main()，demo 初始化未完成/卡住 |
| 2Hz 闪烁 | 系统正常运行 |

## 依赖

- demo：**CUSTOMER_ID=8（1080P IPC）**，NE102 适配（GC20C3=1、VCAM2_EN=1、SD 屏蔽 `NE102_SD_EN=0`）
- `pin_param.h` 尾部追加 `PIN_CAM_EN`（ABI 安全）
- 待接入：`sdk/app/lowPower_app/` 低功耗模块表

## 当前状态

**bring-up 完成（2026-10-08 上板验证）**：双码流 1080p@17.9fps / 720p@17.9fps、CPU 57%、零丢帧。固件四项关键修复全部就位（冷启动 SPI_CLK、sensor 探测重试、AVHEAP、XIP 提速——详见 NE102 文档 §9.7/§10）。

## 已知问题 / 待办

- GC20C3 出图后需 ISP 画质调参（新 sensor/镜头）
- 低功耗模块表未实现
- TF 卡录像未验证（SD 初始化被 `NE102_SD_EN=0` 屏蔽中）
