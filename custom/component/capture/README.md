# capture（相机轨）

## 用途
保活固件的相机流水线：sensor→MIPI→ISP→VPP→JPEG 硬编码→消费者 msi（跳帧 + PSRAM stash + 可选写卡），支持完整下电/再上电循环（远程抓拍即用即关）。mqttka 的远程抓拍与质量档位都建立在本组件上。

## 入口
- `ne102_capture_init()` / `ne102_capture_deinit()`：拉起/拆卸相机轨（vpp→isp→mipi 逆序 + 传感器三电源轨下电）。幂等、可循环。
- `ne102_capture_shot(count)`：非阻塞武装（先丢 `cfg.skip_frames` 帧等 AE，再取 `count` 张）。
- `ne102_capture_wait_done(timeout_ms)`：有界等待。
- `ne102_capture_stash_reserve(bytes)`：boot 期预占 PSRAM stash（无 SD 直传路径）。
- `ne102_capture_get_last_jpg()`：取最后一张（stash 常驻，上传用）。
- 配置：`struct ne102_capture_cfg {skip_frames, burst, width/height(预留), target_len, stash_only}`——`target_len` 为 JPEG 体积调速器目标（0=SDK 默认 50KB），首帧注入 `MSI_JPEG_SET_LEN`（子命令须经 `MSI_CMD_HARDWARE_JPEG` 顶层封装）；`stash_only=1` 时消费者**不写 SD**（帧只进 PSRAM stash，由调用方在 `ne102_capture_deinit()` 断电后自行落盘/上传——wake_capture 的功耗形态；默认 0=mqttka 原行为写 0:/DCIM）。
- 回调：`ne102_capture_set_done_cb(fn)`。

## 依赖
- SDK：`auto_jpg_msi_init`（AUTO_JPG 生产者）、`mipi_csi/isp/vpp` 驱动、jpg_mutex/jpg_mem/iic 子系统
- custom：`app_camera_rails_power()`（板级电源时序）、`ne102_boot_elapsed_ms()`
- 引脚：PIN_CAM_EN(PB6) + 片内 VCAM/VCAM2 LDO

## 实现要点
- **once-guard**：AV 池/工作队列/互斥锁/jpg_mem/iic 子系统一 boot 一次（无销毁 API），deinit 只拆可重建的 msi/sensor/isp/vpp——每轮净内存泄漏 0（20+ 轮实测）。
- 分辨率缩放（w/h）：6 种路径实测无编码产出，阻塞待 FAE 最小序列；默认 0,0=原生零影响，缩放分支超时安全退出。

## 当前状态
**可用（上板验证）**：1080p 冷启动图片就绪 ~800ms、循环重初始化 112ms、连拍帧间隔 ~55ms、跳帧 AE 稳定；mqttka 远程抓拍全程 3.3~3.6s（含射频+公网）；质量档位（target_len）单调可用。
