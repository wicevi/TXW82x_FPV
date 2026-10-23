# app_power_test — 功耗/休眠档位测试示例（CUSTOMER_ID=11 / POWER_TEST_DEMO）

独立于保活固件的测试示例：LED 存活指示 + 各休眠档位测试指令 + dsleep
回调探针。用户搭建骨架（`power_test_config.h`、cdkproj 注册、`app_lowpwr_camera_init()`
分发），休眠框架逻辑在 `app_power_test.c`；唤醒抓拍周期测试独立成组件
`custom/component/wake_capture/`（由本文件 init 注册）。

## 入口

- `app_power_test_init()`（由 `app_lowpwr_camera_init()` 在 `POWER_TEST_DEMO` 下分发）
  ：打印启动唤醒原因 → `lowpower_app_init(pt_modules)` 注册电源轨模块（相机/TF
  轨 suspend 关断、resume 恢复）→ `pt_resume_sema` 创建 → 512KB PSRAM 抓拍
  暂存预占（必须在首次 capture_init 切 AV 池之前）→ `wake_capture_init()` →
  LED 闪线程 → AT 命令注册。
- 配网用系统自带 `AT+SSID` / `AT+KEY`，本示例不自建。

## 同步休眠封装 pt_sleep_sync（2026-10-22）

`system_sleep()` 是**异步**的（`:368` RPC 派发后 ~2-3ms 即返回，真正 park 在
~120ms 后），所以"返回后继续跑的代码"全部落在入眠前窗口——这是 PWRDT 8 轮
排障的根因，也让旧的"+0/+200ms 轮询采样"语义混乱。封装（导出于
`app_power_test.h`，wake_capture 也用它入睡）：

- `pt_sleep_park(type, sleep_ms)`：组参 + 派发 RPC，立即返回（入眠前窗口开始）；
- `pt_sleep_wait(timeout_ms)`：阻塞在 `pt_resume_sema` 上，该信号量由
  **sleepcb RESUME 步**投递（mqttka 板证唯一从不早从不错的唤醒信号；计数
  语义天然消化"恢复回调先于循环拿到控制权"的竞态）；
- `pt_sleep_sync()` = park+wait 一口call，返回即"真睡过并真醒完"（完整断点
  恢复链已走完）；park 被框架拒绝时立即返回错误不空等；
- `pt_sleep_wake_reason()`：wait 返回后取 sleepcb 活值原因；
- `pt_led_hold_set(1/0)`：外部周期（PWCAP）用它压暗 LED 并暂停心跳闪。

PWRS/PWRDT 路径已统一改走 park→（入眠前窗口诊断采样：entry 原因读、
dtim RC 采样）→wait（=唤醒可靠边）→ 事后处理（disarm/原因/报告）。报告的
原因三时点标签相应改为 `entry / resumed / cb`（旧 `+0/+200` 语义被入眠前
窗口污染，见 FAE 问题包 10-22 修订）。超时=期限+60s 余量（恢复链 ~200ms、
DTIM 量化 ~1s，留足）；RTCC(5) 重启永不返回，LIGHT(4) 若无 RESUME 步则超时
后照常出报告（不悬挂）。

## AT 命令

| 命令 | 作用 |
|------|------|
| `AT+PWR?` | 状态：blink/sleeping/wakes/IP/最近 sleepcb 原因 |
| `AT+PWRS=<type 1..5>,<secs 5..3600>` | 一次性休眠测试（走 pt_sleep 同步封装）：1=SRAM_WIFI（断点恢复）2=WIFI_DSLEEP 3=SRAM_ONLY 4=LIGHT 5=RTCC（唤醒=重启，报告下次开机才见） |
| `AT+PWRDT=<secs>[,<pad_secs>]` | **DTIM 回调计时唤醒**（详见下节）：RC 期限加 pad 秒（默认 10）防其先到，由 DTIM 回调自己判时并触发唤醒 |
| `AT+PWCAP=<mode 0..3>,<secs>[,<skip>[,<type 1\|3\|5>]]` | **唤醒抓拍周期**（wake_capture 组件，见其 README）：mode 1=存SD 2=HTTP上传 3=两者（0=解除挂起的 RTCC 意图）；type 1=SRAM_WIFI 断点续跑（默认，可被数据包提前唤醒）/ **3=SRAM_ONLY（无 WiFi 保活值班——"仅存SD不上传"低功耗场景**，停泊不维持关联、无远程唤醒）/ 5=RTCC 重启冷启动（意图入 KV，下次开机抓拍）；睡→唤醒抓拍→相机下电→落盘/上传→再睡→空闲醒 |
| `AT+PWCURL=<url>` | PWCAP 的 HTTP 上传目标 `http://host[:port]/path`（接收端 `custom/tools/http_recv.py`） |
| `AT+PWDWK=<0\|1>` | **保活停泊数据包唤醒过滤器**（dsleep wkdet，mqttka 板证模板移植）：1=布防，停泊中"目的 IP=本机单播"的 **UDP 任意包**或 **TCP 带载荷包**即唤醒（纯 TCP ACK 忽略——否则 NAT/broker 保活探测每 6min 误醒 ~4 次，mqttka 板证）；广播/组播/beacon 天然不匹配。每次入眠前自动刷新 IP 缓存并重注册；唤醒报告新增 `wkdet=0x..`（08=数据包触发，FF=纯定时）。验证：`AT+PWDWK=1`→`AT+PWRS=1,60`→停泊中 PC 发 UDP→应提前醒且 rsn=remote-data |
| `AT+PWRLED=<0\|1>` | LED 开关（测功耗前关掉） |
| `AT+PWRDBG=<0\|1>` | libdsleep 库调试打印（Bingo RC 自校准行等），不持久化 |
| `AT+PWRHK=<0\|1>` | dsleep 全套回调探针（wakeup/enter/exit/rx/ready/loop + sleepcb 全步，序号+RC 时间戳；hook 内只记录不打印，唤醒窗口统一 dump）。探针版 sleepcb 同样投递 resume 信号量 |
| `AT+PWRLOG=<0..7>` | 运行时打印级别（print_level+CoreSetting 双写，不持久化）。默认级别隐藏 SDK 的 KERN_DEBUG 追踪（如 sdhost 卡探测序列）；排存储等问题时开 7 |

互斥：PWRS/PWRDT 与 PWCAP 互相检测对方 busy（同窗口只允许一个 park）。

## AT+PWRDT 机制（2026-10-22 定案：SLEEP_EXIT 路线）

**目标**：不依赖入眠时设定的 RC 定时唤醒，由 DTIM 间隔回调自己计时，到点唤醒。
本命令就是为测 `DSLEEP_ACTION_SLEEP_EXIT` 而设（用户裁定；RC 定时唤醒由 AT+PWRS 覆盖）。

- 入睡（SRAM_WIFI）时 `sleep_ms = 目标 + pad`（默认 +10s），保证入眠期限先不到。
- **wakeup_hook 停泊期每个 DTIM 窗口触发**（实测 ~1.02s/窗口），hook 内：首窗
  锁存 RC 基准（hook 侧锁存，app 侧入眠前读 RC 会破坏停泊——mqttka 2026-10-15
  老坑），此后每窗用 RC 时钟（免除法交叉乘法）判时；到点**直接返回
  `DSLEEP_ACTION_SLEEP_EXIT`**——按 libdsleep 反汇编，该返回值走标准完整恢复
  路径（与常规唤醒同路）。
- 唤醒窗口打印报告：wakeup 调用数、窗口间隔 min/avg/max、hook 判定耗时 vs
  目标、误差，以及**恢复时延**（RC 时基）：`exit->app0`（system_sleep 返回瞬间，
  RC 补偿前为冻结值，显示 `(frozen)`）、`exit->cb`（hook 判定→框架恢复完成，
  实测 179ms）、`exit->report`（→报告打印，187ms）。实测 10s→11 调用
  err=+284ms、30s→31 调用 err=+749ms（均为一个窗口的量化误差），唤醒原因
  timer。**恢复时延与唤醒源无关**：SLEEP_EXIT 按反汇编走与常规唤醒相同的完整
  断点恢复链（PSRAM 校验恢复→~40 个 dev_resume→CPU1 重启→sleepcb 四步），
  远程数据唤醒同样付这笔账，只是其决策来自包内容不需等待。
- 兜底：RC 冻结假设下的计数兜底（3 倍目标秒数+20 窗口，同样返回 SLEEP_EXIT）。
- 曾短暂使用 ready_hook+改写 `tmrao_wkup` 方案（10s/30s 亦通过，err +293/+744ms），
  后按用户裁定回归 SLEEP_EXIT 本意；两路线等价可用，数据存档于 PROJECT_STATUS。

### 三个关键教训（复用价值高）

1. **`system_sleep()` 异步返回**：`:368` 行发出 RPC 后即返回，任务还会继续跑
   几毫秒才真正挂起。**在返回后立刻拆 hook（或做任何 dsleep 配置撤销）等于在
   入眠窗口内自毁**——表现为入眠日志 `:368→:379` 之间出现自己的 cmd=3x NULL
   五连、停泊期全部 hook 零调用。正确做法：唤醒（sleepcb RESUME 后）再拆。
   本次 8 轮二分才定位（fae_demo 的 "wakeup_hook never ran" 记录是同一类陷阱）。
2. **hook 状态用普通 .bss，不用 `__dsleep_data`**：`__dsleep_data` 段每次 park
   entry 从 flash 重载（app 侧写不入 hook、hook 写也被下次重载抹掉——fae_demo
   的 hook_latch=0xEE "从未执行"结论即被此机制污染）；普通 .bss 双向可见
   （探针计数器两方向均已板证）。
3. **入眠前窗口内 dsleep 重配置 → LVD 复位（2026-10-22）**：用户日志
   `AT+PWRDBG=0`（RPC `cmd=26`）执行后瞬间复位，boot 报
   `lvd fault / srp_lvd_vcc_vol_low`。测试运行期间 PWRDBG/PWRHK 的 dsleep
   ioctl 可落进 ~120ms 入眠前窗口（AT 线程此时仍活着），污染正在启动的停泊
   程序 → 电源轨序列异常 → VCC 塌陷。与教训 1、mqttka"RC 计数器"规则同族：
   **入眠前窗口内不许碰 dsleep 库状态**。修复：`AT+PWRDBG`/`AT+PWRHK` 加
   busy 守卫（PWRS/PWRDT/PWCAP 任一运行中即拒绝）；PWRLED/PWRLOG 只动
   GPIO/打印级别，无风险不加。若守卫后仍复现 LVD → 转供电完整性排查
   （唤醒瞬间 RF+PSRAM 恢复电流尖峰：储能电容/供电线径/电源限流）。

## 依赖

- `lowPower_app`（电源轨模块表）、`libatcmd`、`libdsleep`（hook/ioctl/tmrao_wkup）。
- LED/相机/TF 轨配置复用 `app_lowpwr_camera.c`（不重复配置）。

## 当前状态

- ✅ 上板验证：SLEEP_EXIT 路线 10s/30s 双测通过（err +284/+749ms），连续两轮，
  唤醒后功能正常。
- 2026-10-22 同步封装版：PWRS/PWRDT 改走 pt_sleep_park/wait **回归通过**
  （PWRS err=+0ms、PWRDT err=+260ms，行为与重构前一致）；PWCAP 唤醒抓拍
  周期全链路通过（SRAM 路径清醒窗口 ~1s、HTTP 200、RTCC 冷启动 +477ms 出图，
  见 wake_capture/README.md）。

## 已知问题

- `dur:` 行（§2.2.5 时长）整数截断显示（`ticks/hz*1000` 先除后乘），精度 1s 级；
  精确值看 `dtim:` 行的 hook-elapsed。

## 变更记录

- 2026-10-21：首版（PWRS/PWRLED/PWRDBG/PWRHK）+ §2.2.5 时长接入 + PWRDT
  （8 轮排障定位 system_sleep 异步返回 disarm 自毁 + `__dsleep_data` 重载两教训）。
- 2026-10-22：PWRDT 按用户裁定定案 SLEEP_EXIT 路线（判定逻辑回 wakeup_hook、
  到点返回 DSLEEP_ACTION_SLEEP_EXIT；去掉 tmrao_wkup 改写与 2s IPC 延迟残留），
  10s/30s 双测通过。
- 2026-10-22：**pt_sleep 同步封装**（park/wait/sync + wake_reason + led_hold，
  sleepcb RESUME 投递信号量），PWRS/PWRDT 统一走封装、报告原因标签改
  entry/resumed/cb；接入 wake_capture 组件（PWCAP/PWCURL）+ PSRAM 暂存预占。
