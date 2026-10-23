# NE102 FAE 问题包：休眠唤醒原因（wk_reason）相关三问

日期：2026-10-19 ｜ 提问方：NE102 自研固件（泰芯 TXW82x FPV SDK 基础上开发）

> ## 2026-10-22 修订（自测仪器化后的重新定性，详见 fae_demo README）
>
> 1. **问题一定性修正**：`sys_wakeup_reason()` 的"+0ms 旧值"不是唤醒后刷新慢，
>    而是 **`system_sleep()` 异步返回**——RPC 派发后即返回，随后代码（含 +0ms
>    读取）在真正入眠前 ~123ms 执行，读到上一拍残留；真值在恢复链完成时
>    （sleepcb，复活后 ~180ms）落位。实测：`call->ret=3ms`、
>    `rsn0->suspcb=123ms`、`park_rc=10205ms`（10s 周期）。
> 2. **问题二撤回**：wakeup_hook 在停泊期有派发（hook_latch=0x01 板证）；
>    旧"从不执行"证据的锁存字节在 `__dsleep_data` 段，每次 park entry 从
>    flash 重载会抹掉 hook 写入。仍请厂家确认 wakeup_hook 的**准确触发条件**
>    （哪些唤醒源/哪些停泊档位会调用）。
> 3. **新发现请关注**：`os_jiffies()` 跨停泊**不补偿停泊时长**（10s 停泊只留
>    ~60ms），跨睡眠计时只能依赖 RC（`pmu_tmrao_get`，恢复链补偿后有效）。

## 1. 环境

| 项 | 值 |
|---|---|
| SDK | TXW82x FPV SDK v2.7.1.7-45228（分支 v2.7.1.7，Core→App 官方构建链） |
| 测试固件 | `custom/component/mqttka/`（NE102_MQTTKA=1），APP.bin MD5 fe6842f1 |
| 芯片/板 | TXW82x（CPU0 E804FD + CPU1 E804D），工程板 MAC 尾 433ba8 |
| 休眠方式 | `system_sleep(SYSTEM_SLEEP_TYPE_SRAM_WIFI, &sp)`，sleep_ms=90s 分段，断点恢复（非复位唤醒） |
| 每次入眠前 | `dsleep_set_usr_wkdet_cb(NULL, cb)` + `dsleep_set_wakeup_hook(hook)`（见 §2 代码） |
| 网络 | STA 关联路由器，MQTT（kawaii）长连接 broker.emqx.io:1883，keepalive 120s |

## 2. 我们的用法（已自证无误）

```c
/* 注册：每次 system_sleep 前调用（低功耗程序上下文，仅一个字节写入） */
static int32_t ka_park_wakeup_hook(uint8_t wk_reason)
{
    ka_wk_latch = wk_reason;                 /* __dsleep_data 常电区字节 */
    return DSLEEP_ACTION_DO_NOTHING;         /* 不拦截，走默认唤醒 */
}
    dsleep_set_usr_wkdet_cb(NULL, ka_wkdet);
    ka_wk_latch = 0xFF;                      /* 哨兵 */
    dsleep_set_wakeup_hook(ka_park_wakeup_hook);
    system_sleep(SYSTEM_SLEEP_TYPE_SRAM_WIFI, &sp);
```

**注册落盘证明**（固件启动后从 `bgn_dsleep`（`struct dsleep_priv *`，lmac_dsleep.h:505）直接读回）：

```
MQTTKA: dcfg=0x20001128 hook@0x41c=0x100d23c4 (want 0x100d23c4) wkdet@0x410=0x100d2ad0 (want 0x100d2ad0) ready@0x428=0x0
```

hook 指针与 wkdet 指针均精确写入 dcfg 结构——**排除我们侧注册错误**。

## 3. 问题一：`dsleep_set_wakeup_hook` 的回调从不执行

**现象**：上述注册下，多次 90s 停泊的全部唤醒（定时唤醒 + 数据唤醒），哨兵 `ka_wk_latch` 每拍保持 0xFF——`wakeup_hook` 从未被调用。曾尝试：① 每次入眠前重新注册；② 同时 arm `ieee80211_conf_set_heartbeat_int()`；③ 同时注册非空 `dsleep_set_ready_hook`——均无效。

**反汇编证据**（csky-elfabiv2-objdump，最终链接镜像 txw82xApp.elf）：
- `dsleep_ioctl` case DSLEEP_IOCTL_SET_WAKEUP_HOOK（cmd=0x22）把指针存 `bgn_dsleep`+0x41c（与上面 dump 一致）；
- 该指针**全镜像唯一读取/调用点**在停泊程序 `dsleep_wakeup`（.ds_text @0x04003914）：`ld.w r3,(r2,0x41c); bnez → ld.b r0,(r2,0x3a9); jsr r3`，并按返回值处理 SLEEP_CONTINUE/EXIT；
- 入口 `wakeup_Handler`（@0x04003bd4）由停泊主程序在**全镜像唯一 `doze` 前**装入 AON 向量（0x20000004）；
- 交叉：`dsleep_set_ready_hook`/`set_loop_hook` 的调用点与 wakeup_hook 同在 `dsleep_wakeup`——我们早期 KAPK 实验（注册了 ready_hook + UDP 心跳）时 ready_hook 曾以 DTIM 节拍被调用，证明该路径在某些配置下可达，但**触发条件未能通过配置复现**。

**问题**：模式1（SRAM_WIFI）停泊在什么条件下会执行 `wakeup_Handler → dsleep_wakeup` 路径？指南《TXW82x 低功耗开发指南》§2.2.6 描述"IO / Timer 唤醒后立即调用"，与实测不符。

## 4. 问题二：`sys_wakeup_reason()` 读值滞后一拍，何时写入无契约

**现象**：恢复后立即读 `sys_wakeup_reason()` 得到**上一次停泊的原因**；本拍原因在恢复后 ≤300ms 内才写入。

**时间线实测**（同一唤醒窗口内，`+0x3a9` 即 `sys_wakeup_reason` 的取数字节）：

```
MQTTKA: wake #2 rsn=1(timer) wkbyte0=1 ...        # +0ms：旧值（上拍 timer）
MQTTKA: wkbyte +300ms=8 (sys=8)                   # +300ms：翻新为 8（本拍实为 capture 数据唤醒）
MQTTKA: wkbyte +settle=8 (sys=8)
MQTTKA: rx topic=ne102/433ba8/cmd payload=capture  # 命令随后派发，证明本拍确为数据唤醒
```

另一定时拍对照：`wkbyte0=1 → +300ms=1`（本拍确为 timer，无翻转）。

**问题**：`wk_reason` 字节（dcfg+0x3a9）的写入时机是否有官方契约/事件？当前我们只能"等 300ms 再读"或依赖 sleepcb（见下），是否有推荐的权威获取方式？

## 5. 问题三：框架 sleepcb 的 `args->resume.wkreason` 是否为受支持的接口

**实测：该字段工作正常且为本拍活值**（官方 demo lowPower_app.c:38 中此字段被注释掉，故请确认支持状态）：

```c
static int32_t ka_sleepcb(uint16_t type, struct sys_sleepcb_param *args, void *priv)
{
    if (args->action == SYS_SLEEPCB_ACTION_RESUME) {
        ka_cb_wkreason = args->resume.wkreason;   /* 实测：定时拍=1，capture 数据拍=8，与命令到达精确吻合 */
    }
    return RET_OK;
}
    sys_register_sleepcb(ka_sleepcb, NULL);
```

串口实录（act=0 为 RESUME，枚举 SYS_SLEEPCB_ACTION_RESUME=0）：

```
[22463] sleepcb act=0 step=0 wkreason=1     # 定时拍 resume，LMAC 步
[22464] sleepcb act=0 step=1 wkreason=1
[22465] sleepcb act=0 step=2 wkreason=1
[22465] sleepcb act=0 step=3 wkreason=1
...
[24846] sleepcb act=0 step=0 wkreason=8     # capture 数据唤醒拍，全步均报 8(WK_DATA)
```

**问题**：确认 `resume.wkreason` 为受支持语义（即"本拍唤醒原因"），我们将以它为主用源。

## 6. 附带发现：enter/exit hook 无调用点

反汇编显示 `dsleep_set_enter_hook`/`dsleep_set_exit_hook` 对应字段（dcfg+0x418/+0x420）在**最终镜像中没有任何读取/调用点**（纯死代码）。请确认这两个接口在当前库版本是否实现。

## 7. 我们当前的工程解（不阻塞等答复）

唤醒后：优先 wkdet/hook latch（若命中=数据）→ 否则 50ms 步进等待 ≤400ms 直至 `sys_wakeup_reason()` 变为定时类（1/2/3）或值发生变化 → 定时类跳过派发宽限直接发心跳（窗口 ~400ms），数据类进入 2s 事件驱动派发宽限。实测定时心跳窗口 2352ms→351~540ms，capture 事件协议（capture→photo→sleep，无 awake/hb 泄漏）保持正确。

## 8. 最小复现固件

配套 `custom/component/fae_demo/fae_demo.c`（单文件，AT+FAE 命令驱动，TCP 唤醒不需要 MQTT）。使用方法与预期输出见 `custom/component/fae_demo/README.md`。烧录同一固件后：

```
AT+FAE=30        ← 30s 休眠循环
（PC 端）echo wake | nc <ip> <port>   ← TCP 唤醒
AT+FAE?           ← 查看证据（rsn 三时点采样 + hook 锁存 + dcfg dump）
```
