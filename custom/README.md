# custom/ — 自研业务目录

本目录承载 **MQTT 远程保活休眠测试固件**（本仓库唯一构建目标）的全部自研代码与板级适配。SDK 官方内容尽量不改（现存例外见下）。开发规则总纲见 `.zcode/skills/txw82x-dev/SKILL.md`。

## 目录结构

| 目录 | 用途 |
|------|------|
| `app/` | 板级上电（状态 LED + 相机/TF 电源开关）+ 保活入口：`app_lowpwr_camera_init()` → `ne102_mqttka_init()`；`ne102_config.h`（CUSTOMER_ID=10 方案配置，`NE102_MQTTKA=1`） |
| `app/app_power_test/` | 功耗/休眠档位测试示例（CUSTOMER_ID=11/POWER_TEST_DEMO，当前工作树构建目标）：LED 存活 + `AT+PWRS` 各档休眠 + `AT+PWRDT` DTIM 回调计时唤醒 + `AT+PWRHK` 回调探针（见其 README，含 system_sleep 异步返回教训） |
| `component/mqttka/` | **核心**：保活休眠循环、事件协议、远程抓拍、质量档位、AT+KA*（见其 README） |
| `component/capture/` | 相机轨（sensor→MIPI→ISP→VPP→JPEG msi、跳帧、PSRAM stash、循环上下电；`stash_only` 位供断电后落盘路径） |
| `component/wake_capture/` | **唤醒抓拍周期测试**（功耗测试固件扩展，AT+PWCAP/PWCURL）：睡→唤醒抓拍→PSRAM 暂存→相机下电→SD 落盘/HTTP 上传→再睡→空闲醒 |
| `component/netctl/` | STA 凭据判定与网络轨标记（保活构建走 boot 原生 STA） |
| `component/led/` | 状态 LED（PC0）：清醒 1Hz / 唤醒窗口快闪 / 休眠灭 / 故障慢闪 |
| `component/fae_demo/` | FAE 唤醒原因/生命周期复现（AT+FAE，独立接线 `fae_demo_init()`）：入眠前读取实证 + jiffies 跨停泊失效等 SDK 结论（见其 README 与 FAE 问题包修订段） |
| `pmu/` | PMU 协处理器（N32L403KBQ7）独立 GCC 工程，与 TXW 固件分开构建烧录（不走本仓 CDK 链） |
| `cfg/` | 板级配置备份（`NE102_V*.cfg`，生效需复制到 `project/txw82xApp/config.cfg`） |
| `doc/` | 文档：保活操作卡、FAE 问题包、`PROJECT_STATUS.md`（进展日志）、板级资料（系统IO/原理图/器件手册） |
| `tools/` | 烧录（burn.bat + CKLink；连不上先串口 `AT+RST`）、串口（serial_cmd.py）、MQTT 工具（console/bench/收图）、HTTP 接收端（http_recv.py）、自动验证（wc_test1.py 全套 + wc_rtcc.py RTCC 单腿） |

## 板级适配清单（保留勿删）

- `project/txw82xApp/pin_param.h`：板级引脚 ABI（CAM_EN/TF_PWR_EN 等）
- `project/txw82xApp/config.cfg`：板级生效配置——**TF 卡 SDIO 单线**（仅 DAT0：CLK=PB9/CMD=PB8/DAT0=PB10）+ 相机/TF 电源开关引脚
- `project/txw82xApp/makecode.ini`：XIP 四线 60MHz 启动配置
- `custom/cfg/NE102_V1.0_*.cfg`：板级参数备份
- `sdk/lib/video/isp/param/isp_param_gc20c3.c`：GC20C3 sensor IQ 表
- `custom/app/app_lowpwr_camera.c`：相机电源时序（AVDD 先、DVDD 后，硬件要求）
- `sdk/lib/video/vpp/vpp_dev.c`：mdet 缓冲泄漏修复 + `yuvbuf` 去 static（scale 链接依赖）

**注释语言**：代码注释一律英文；中文仅用于文档。**编译须知**：`custom/` 下新增 `.c` 必须注册进 `project/txw82xApp/txw82xApp.cdkproj` 的 FLASH 配置。

## 构建与烧录

`./build.sh txw`（Core→App，交付物 `project/txw82xApp/APP.bin`）→ `custom/tools/burn.bat`（CKLink；设备停泊中会失联，先发 MQTT `wake` 唤醒再烧）。

## 文档维护规则（对所有 agent 强制）

1. **组件 README**：涉及的组件目录维护 `README.md`（用途/入口/依赖/当前状态/已知问题/变更记录）。
2. **进展日志**：在 `custom/doc/PROJECT_STATUS.md` 表格顶部追加一行。
3. **组件索引**：增删组件或状态变化时更新本文件目录结构表。
4. 只记录事实与结论；文档更新与代码改动放进同一次提交。
