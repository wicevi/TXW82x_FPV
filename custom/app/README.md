# app/ — 板级上电 + 保活入口

| 文件 | 角色 |
|------|------|
| `app_lowpwr_camera.c/.h` | 板级上电（状态 LED、相机/TF 电源开关）+ 保活入口 `app_lowpwr_camera_init()` → `ne102_mqttka_init()`；对外导出板级电源 helper |
| `ne102_config.h` | CUSTOMER_ID=10 方案配置头（镜像 ipc_1080p 基线，`IPC_1080P_DEMO`→`NE102_PRODUCT`；`NE102_MQTTKA=1` 唯一构建） |

## 启动链

```
pre_main（SDK 原生：CPU1 启动）
→ main(): app_board_power_init（LED + 相机/TF 上电）
   → msi/Codec → wifi(纯 STA)/ble/net → sys_app_init → usr_app_init
   → app_lowpwr_camera_init() → ne102_mqttka_init()（AT+KA* 待机）
```

注意：CPU1 由 SDK 原生 pre_main 启动，**不要**再调用 `ne102_cpu1_ensure()`（双重 `sys_start_cpu1` 会导致 RPC 崩溃重启循环——2026-10-19 板上实锤）。

## 板级电源 helper（组件共用）

- `app_camera_rails_power(on)`：相机三轨时序（AVDD 2.8V → IOVDD 1.8V → 10ms → DVDD(CAM_EN PB6) → 20ms；关断逆序）。硬件要求 AVDD 先、DVDD 后。
- `ne102_board_tf_power(on)`：TF 卡电源开关（PIN_TF_PWR_EN PB7）。
- `ne102_boot_elapsed_ms()`：boot 起点毫秒打点。
