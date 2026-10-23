# NE102 MQTT 保活休眠测试操作卡（mqttka 固件）

> 固件：`NE102_MQTTKA=1`（`custom/app/ne102_config.h`），已在 2026-10-14 构建烧录就绪（APP.bin 1,323,024B）。
> 目的：验证低功耗指南模式 1（`SRAM_WIFI`）——MQTT 长连接跨保活休眠存活、断点恢复唤醒（**非复位**）、心跳与远程命令闭环。
> 串口 921600。设备启动后约 3s 出现 `MQTTKA: idle.` 横幅即为正常待机。

## 1. LED 状态对照

| 现象 | 含义 |
|---|---|
| 1Hz 慢闪 | 清醒待机（连上路由后） |
| 4Hz 快闪 | 休眠唤醒后的工作窗口（发心跳中） |
| 全灭 | 正处于保活休眠 |
| 2s 亮/1s 灭循环 | 故障（无凭据/无 IP/MQTT 连不上/连续 3 次心跳失败） |

## 2. PC 侧准备

公网 broker（broker.emqx.io）无需本地服务；如自建 broker：

```bash
# 订阅设备事件 + 交互下发命令（公网 broker 直接可用）
python custom/tools/mqttka_console.py <设备mac后6位hex>        # 如 0af1e2
# 或自建 broker：MQTT_HOST=192.168.110.11 MQTT_PORT=9991 python ...
```

也可用任意 MQTT 客户端：订阅 `ne102/<mac6>/evt`，发布 `ne102/<mac6>/cmd`。
mac6 从设备串口横幅读取（`MQTTKA: idle. mac-derived id=ne102_xxxxxx ...`）。

## 3. 操作序列

| 步骤 | 串口指令 | 预期 |
|---|---|---|
| 1 配路由 | `AT+KASSID=CAMTHINK_DEV,12345678.` | OK，立即重连；随后 STA 拿到 IP |
| 2 连 MQTT | `AT+KAGO` | `mqtt up broker.emqx.io:1883 ...` + evt 收到 `{"event":"online"}`，LED 转 1Hz |
| 3 进保活休眠 | `AT+KASLP=0`（0=自动 90s 段） | evt 收到 `{"event":"sleep"}`，LED 灭；**AT 串口在休眠期间无响应属正常** |
| 4 观察心跳循环 | 等 ~90s/次 | 每次唤醒 LED 快闪，evt 收到 `{"event":"hb",...}`；串口 `wake #n rsn=... tchk=±xxms` |
| 5 远程唤醒 | PC console 输入 `wake` | 设备退出循环，evt 收到 `{"event":"wake","by":"remote"}`，LED 恢复 1Hz |
| 6 再入休眠 | PC console 输入 `sleep`（ awake 期收到即入眠）或串口 `AT+KASLP` | 同步骤 3 |
| 7 自由发布 | `AT+KAPUB=hello` | evt 通道出现 hello |
| 8 查状态 | `AT+KAQ` | loop/mqtt/计数器/最近唤醒原因/tchk |
| 9 退出 | `AT+KASTOP` | 下个唤醒窗口退出（最多等一个睡眠段） |

## 4. 判定要点（2026-10-14 板测已验证的结论）

- **断点恢复（核心）**：✅ 已验证 6+ 连续周期无复位唤醒。evt 心跳带 `wk` 字段（`timer`=定时唤醒）；若 evt 突然永久静默且设备 AT 无响应→疑似复位，重启后 `AT+KAQ` 第二行 `btrc=1 ph=...` 给出临终周期状态（轨迹同时存 KV `ne102_katrace`，断电也留档）。
- **心跳送达**：心跳为 QoS1+等 PUBACK，broker 真收到才算成功；未确认会自动重连重发（evt 出现 `"rc":1` 的 hb 即自愈过一次）。首版 QoS0 曾出现"发送成功但 broker 永远收不到"的假活，勿再用 QoS0 判断会话存活。
- **时间同步**：跨休眠 **jiffies 冻结不补偿**（tchk≈-89970ms 属预期），kawaii 定时器随之冻结——不会误发 PINGREQ，92s 真实间隔 < 120s keepalive，broker 会话保持。
- **首次入眠**：偶发 ~3s 即醒（wk:"other"），下个周期自动正常，无碍。
- **串口**：唤醒瞬间与复位唤醒后的 boot 打印不可靠（板上已知），一切以 evt 流 + `AT+KAQ` 计数器 + KV 轨迹为准。
- **电流**（可选）：休眠段接功耗仪。注意：本板 `ulp_half_sleep` 深层省电未启用（等 FAE），停泊电流可能为几十 mA 而非手册 338μA——不影响逻辑验证，数据留档。

## 5. 故障速查

| 现象 | 首查 |
|---|---|
| `AT+KAGO` 后 ERROR 灯 | 无凭据（步骤1）或路由不可达；`AT+KAQ` 看 ip= |
| MQTT connect failed | PC 侧透明代理拦截历史问题（见 PROJECT_STATUS 10-13）；换端口/网络对比 |
| 唤醒即重启 | 看 `leftover sleep trace` 的 phase/wake_rsn；对比上次正常时的 rsn |
| 完全无唤醒 | `AT+KASLP` 段长最小 5s；休眠期间串口无响应是正常的，等唤醒窗口 |
| 心跳连续失败退出 | 故障灯；查路由器是否把静默客户端踢掉（旧 Camthink_hi 有 9s 失步史） |
