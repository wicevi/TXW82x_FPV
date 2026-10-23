# led（LED 状态机）

## 用途
NE102 状态灯（PC0，高电平点亮，Q3 NPN 驱动）的模式化控制，对应 PRD §5.1 的六态设计。**仅配置模式初始化**；快启模式不初始化 LED（PRD §4.4），只在模式分叉时直接灭一次 GPIO。

## 入口
- `ne102_led_init()`：配置 GPIO + 启动 100ms 周期 work
- `ne102_led_set(st)`：切换状态；`ne102_led_get()`

状态表（实现 agent 可后续优化节奏）：
| 状态 | 表现 | 使用点 |
|------|------|--------|
| ON | 常亮 | 启动中（board init 常亮→config init 接管） |
| AP_READY | 1Hz 慢闪 | 配置模式初始化完成（webservice 起后） |
| WEB_ACTIVE | 4Hz 快闪 | webservice 连接计数 >0 时自动切 |
| OTA | 双闪循环 | ⑤期 |
| ERROR | 2s 亮/1s 灭 | 预留（SD 缺失等） |
| SLEEP_FADE | 渐灭（一次性，结束转 OFF） | ④期进休眠前 |

## 依赖
hal/gpio、osal work（系统 workqueue，100ms 周期）。

## 当前状态
可用（②期上板验证：AP_READY/WEB_ACTIVE 随 Web 访问切换）
- WEB_ACTIVE 由 webservice 的连接计数驱动（accept 计数 +1 / 关闭 -1，>0 即快闪）
- OTA/SLEEP_FADE 状态已实现节奏，等⑤④期接线

## 已知问题
- 单 work 100ms 周期常驻（配置模式功耗略增，④期休眠时 LED work 随 suspend 停）

## 变更记录
| 日期 | 摘要 |
|------|------|
| 2026-10-10 | 初始创建（②期：替代原心跳闪烁，接入 webservice 计数） |
