# mqttka — WiFi 保活休眠 MQTT 测试固件

按《TXW82x 低功耗开发指南》模式 1（`SYSTEM_SLEEP_TYPE_SRAM_WIFI`）实现的「MQTT 长连接 + 保活休眠 + 断点恢复唤醒 + 远程抓拍」专用固件。本仓库唯一构建目标：`NE102_MQTTKA=1`（`custom/app/ne102_config.h`）。

**交付物**：固件 `bin/mqttka_keepalive_test.bin`；操作手册 `../../doc/NE102 MQTT保活休眠测试操作卡.md`；FAE 材料 `../../doc/NE102 FAE 唤醒原因问题包.md`；性能基准 `../../tools/ka_bench.py`。

## 能力（均上板验证）

- **保活循环**：纯 STA 启动 → `AT+KAGO` 连 MQTT → `AT+KASLP` 进 90s 分段休眠；每拍断点恢复、hb QoS1+PUBACK（唤醒→确认 350~550ms），会话死亡自动重连重发自愈。
- **事件协议**：定时唤醒 → `hb`（唯一的保活语义）；命令唤醒 → 只回命令事件（`capture` → photo → `sleep`；`wake`/`work` → `wake`+`awake` 退出）。已验证窗口内零 awake/hb 泄漏。
- **远程抓拍（无 SD 依赖）**：cmd `capture`/`shot` → 相机轨上电 → 1080p JPEG → PSRAM stash（启动预占 512KB）→ 单 MQTT 客户端（96KB 写缓冲，PSRAM 路由）NE301 JSON+Base64 单发到 `ne102/<mac6>/photo` → 自动回睡。NE301 接收器实测 SOI/EOI 校验通过。
- **质量档位**：`AT+KACAP=<skip>[,0,0,<target_len>]`——`target_len` 为 JPEG 体积调速器（SDK PID：qt+DQT）目标字节，skip 帧兼作预热。注入方式：`msi_do_cmd(g_jpg_msi[JPGID0], MSI_CMD_HARDWARE_JPEG, MSI_JPEG_SET_LEN, len)`，每拍首帧重试注入（编码器 msi 起流才创建）。

## 性能基准（2026-10-19，broker.emqx.io 公网，静态场景，ka_bench.py）

- **心跳维持**：唤醒→hb PUBACK 平均 412ms（402~502，23 样本）；空闲节拍 = 90s 停泊 + ~0.5s 窗口。
- **唤醒抓图**（KACAP 多配置平均）：发送→照片全程 **3.25~3.63s**（发送→事件 1.6~2.5s 射频+公网主导；拍照 404ms；发布 263ms@50KB / ~700ms@77KB 随体积线性）。
- 质量档位实测（1080p）：30K→46.8KB(skip3)/38.5KB(skip8)、50K→55.5KB、120K→92.1KB、200K→107.4KB（场景上限）。

## 实现要点（下一位维护者必读）

- **同步唤醒边（2026-10-22 重构）**：`ka_enter_sleep` 派发前 `os_sema_eat(&ka_resume_sema)`，`ka_sleepcb` RESUME 步 `os_sema_up`，`ka_sleep_loop` 用 `os_sema_down(seg+60s)` 阻塞等真醒——取代旧的"10ms 轮询 200ms 窗口 + 二次 ka_cb_fresh=0 丢早到 cb"补延时方案（`system_sleep()` 异步返回的两个竞态被计数语义一并消化；park 被拒时 sr!=OK 短路不空等，超时回落 TIMER 判定保留）。与 app_power_test 的 `pt_sleep_sync` 同型（板证可跨停泊阻塞）。
- **唤醒原因双源**：框架 sleepcb 的 `args->resume.wkreason` 为本拍活值（定时=1/数据=8，RESUME 步随信号量送达）；`sys_wakeup_reason()` 读 dcfg+0x3a9，恢复后 ≤300ms 才翻新（读早了是上一拍的值）。**陈旧读值绝不参与协议决策**。
- **KV 已修**（2026-10-22）：`ne102_mqttka_init` 头部补 `flashdb_init()`（此前全仓库无人调用，syscfg_* 全部静默 "not inited"——KA cfg/park/trace/dbg 的"持久化"从未真正生效）；分区为固定 0x7F0000 槽位，避开尾部参数区。
- **融合式派发宽限**（唤醒后单循环）：命令派发 sema 即到即决（0.14~1.4s）；wkdet/hook latch（数据即决）或 sleepcb 定时族原因（~151ms）早退直发 hb；其余保持 2s 完整宽限。注意 `os_sema_down` 返回约定 **1=成功 0=超时**。
- **休眠上下文铁律**：不碰 PMU RC 计数器（`pmu_tmrao_get` 会破坏停泊定时器基线）；park 程序上下文只做单字节写；跨停泊状态放 `__dsleep_data`。
- **抓拍管线 once-guard**：AV 池/工作队列/互斥锁/jpg_mem/iic 子系统一 boot 初始化一次，deinit 只拆真正支持重建的 msi/sensor/isp/vpp（每轮净内存泄漏=0）。
- `CONFIG_PSRAM_AVHEAP_SIZE=4MB`（NE102 无录像，7.5MB 会挤爆通用堆）。

## 已知边界

- `dsleep_set_wakeup_hook`/enter/exit hook：注册生效但库内不可达/死代码（反汇编定案，FAE）。
- 分辨率缩放（w/h 字段）：6 种路径均无产出，阻塞待 FAE 最小序列；字段保留、默认 0,0=原生零影响。
- park 内 UDP 心跳（`AT+KAPK`）：机制结论为否定（tx_ether 明文被 AP 丢弃），仅留作复现工具。
- 公网 broker 偶发会话死亡（自愈 ~9s）；本地 broker 消除。

## AT 指令

| 指令 | 说明 |
|---|---|
| `AT+KASSID=<ssid>[,<key>]` | 路由器凭据（存 syscfg） |
| `AT+KAMQ=<host>[,<port>[,<id>[,<sub>[,<pub>[,<ka_s>]]]]]` | MQTT 参数（缺省 broker.emqx.io:1883，主题按 MAC 生成） |
| `AT+KAGO` / `AT+KASLP=<s>[,<n>[,<type>]]` / `AT+KASTOP` | 连接 / 进保活休眠（0=自动 90s；type 1=SRAM_WIFI 默认 2=WIFI_DSLEEP 3=SRAM_ONLY 4=LIGHT 5=RTCC）/ 退出循环 |
| `AT+KASHOT` / `AT+KACAP=<skip>[,<w>,<h>[,<len>]]` | 本地抓拍 / 抓拍参数（跳帧、分辨率预留、体积目标） |
| `AT+KAPUB=<text>` / `AT+KAQ` | 发布文本 / 状态遥测 |
| `AT+KAPK` / `AT+KAPKT` / `AT+KADBG` | park 心跳实验（否定结论）/ 停泊调试打印 |

## 依赖

capture（抓拍管线+stash）、netctl（STA 凭据判定+boot 标记）、led、syscfg KV、sleepdata、kawaii MQTT、lowPower_app（ka_rails 模块表）。

## 入口

`ne102_mqttka_init()` ← `app_lowpwr_camera_init()`（`custom/app/`，纯保活编排；该调用 2026-10-22 曾在 power-test 重构中丢失、NE102_PRODUCT 分支空置导致 mqttka 固件无法启动，已补回）。

## 等待点清单（2026-10-22 全量审计：无残留异步补偿等待）

| 位置 | 等待 | 性质 |
|---|---|---|
| `ka_enter_sleep` | `os_sleep_ms(5)` 派发前排空 UART FIFO | 硬件时序，保留 |
| `ka_sleep_loop` 入口 | `KA_QUIET_MS=3s` 静默窗（KASLP 入口与**远程抓拍重入各跑一次**，非每拍——2026-10-23 补正：抓拍结束重新进循环时此窗再跑一遍，即"上传完还要等 ~3s 才睡"的来源） | **协议在途消费**：发起/`capture` 命令的 QoS1 重复投递、照片 PUBACK 尾巴，睡后才到会数据唤醒污染下一拍——必须醒着消化；有 stop/wake 早退 |
| `ka_sleep_loop` 入口 | `ka_rx_sema` 16×1ms 排空陈旧派发信号 | 信号卫生，保留 |
| 唤醒后 | ~~200ms sleepcb 轮询~~ → `ka_resume_sema` 阻塞 | **已修**（2026-10-22） |
| 唤醒后·数据唤醒 | `KA_SETTLE_DATA_MS=2s` 派发宽限（`ka_rx_sema` 50ms 步进，派发即断/定时族原因早退） | **协议等待**：唤醒包还在 socket 缓冲，kawaii 需调度时间解析出 ka_cmd——非休眠异步补偿，保留；旧 cb 补读死分支已清 |
| hb 未确认路径 | `os_sleep_ms(300)` 重连退避 | 网络协议，保留 |
| `AT+KAGO` 流程 | `KA_IP_WAIT_MS=50s` IP 等待 / `ka_wait_puback` | 网络协议，保留 |

## 变更记录

- 2026-10-22：**四修复 + 全量运行回归通过（本地 broker 9991）**：① `flashdb_init()` 补调（KV 持久化此前从未生效，回归实证 KAMQ 跨重启存活）；② 唤醒等待改 `ka_resume_sema`（回归实测定时拍 7~9ms 返回、远程拍 rsn=8/live=8/cb=8 +6ms，替换 200ms 轮询+二次清标志）；③ `NE102_PRODUCT` 入口调用补回（随老编排层丢失）；④ **AT 注册块重建**——`ne102_atcmd_*` 处理函数一直导出但注册块随老编排层丢失，全部 AT+KA* 曾静默失效（串口只剩系统 AT+RST 响应），已按 mqttka.h 名单重建 12 条注册。回归后切回功耗测试构建（板=树）。等待点全量审计见上节清单。
