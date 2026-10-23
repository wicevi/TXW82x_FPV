# fae_demo — 唤醒原因/生命周期最小复现固件（交 FAE）

## 用途
单文件最小复现《NE102 FAE 唤醒原因问题包.md》中的问题，并输出**休眠生命周期
时间戳**（何时真的休眠、何时唤醒、软件何时恢复执行），供厂家 FAE 上板复现：
1. `sys_wakeup_reason()`"+0ms 读取"实为**入眠前读取**（`system_sleep()` 异步
   返回，+0ms 代码在真正入眠前 ~123ms 执行，读到的是上一拍残留）；真值在恢复
   链完成时（sleepcb，复活后 ~180ms）才落位
2. ~~wakeup_hook 从不执行~~ **已撤回**（2026-10-22 板证 hook 有派发，
   hook_latch=0x01；旧证据被 `__dsleep_data` 重载机制污染）
3. `dsleep_set_enter_hook` / `dsleep_set_exit_hook` 全镜像零调用点（维持）

## 使用方法

> 2026-10-22：曾短暂并入功耗测试固件（CUSTOMER_ID=11）实测，现已按用户裁定
> 移除接线。demo 提供 `fae_demo_init()` / `fae_demo_running()` 入口，需要时在
> 任意 app 初始化处调用即可挂上 AT+FAE（勿与 AT+PWRS/PWRDT 同时运行）。

```
AT+FAE=30        ← 单周期：30s SRAM_WIFI 休眠一次并出报告（默认 cycles=1）
AT+FAE=30,3      ← 连续 3 周期
AT+FAE=0         ← 停止（当前周期结束后退出）
AT+FAE?          ← 证据计数器（唤醒次数/hook 锁存/dcfg 字段 dump）
```

与 AT+PWRS/PWRDT 运行时互斥（共享 sleepcb/hook 槽位）。**注意**：FAE 周期运行
期间 AT 不被处理（板子看似无响应），因此默认单周期；多周期模式慎用。

设备休眠中用 PC 发 TCP 包唤醒：
```
echo "wake" | nc <device_ip> <any_port>
```

## 生命周期报告（2026-10-22 仪器，实测 10s 周期 × 多轮）

```
FAE life: call->ret=3ms ret->rsn0=0ms rsn0->suspcb=123ms (rsn0 PRE-PARK if >0)
        | park_rc=10205ms jiffies_gap=64ms (jiffies LOSE the park time)
        | run->cb=-6ms cb->report=389ms
FAE: software running again (j=50701 rc=1647497738) - physical wake is the WK/cpu0 sleep_exit line above
```

| 字段 | 实测 | 含义 |
|---|---|---|
| call->ret | 2~3ms | `system_sleep()` 返回耗时（异步：只发了 RPC） |
| **rsn0->suspcb** | **123~124ms** | **"+0ms"读取发生在真正入眠前 123ms**（入眠窗口实锤） |
| park_rc | ≈目标+205ms | 真实停泊时长（RC 时基，含入眠尾+唤醒链开销） |
| jiffies_gap | 55~64ms | **os_jiffies 不补偿停泊时长**（跨停泊计时禁止用 jiffies） |
| run->cb | ≈-6ms | 首个 RESUME sleepcb 比任务首个后停泊迭代早 ~6ms |
| cb->report | ~389ms | sleepcb 到报告打印 |
| （串口墙钟） | — | `cpu0 sleep_exit` 行 → `software running again` 行的 elapsed 差 = 物理唤醒→软件恢复执行 |

## 判定标准（修订版）

| 现象 | 预期 | 说明 |
|---|---|---|
| rsn0->suspcb > 0 | **是** | "+0ms"读数在入眠前执行（问题 1 的正确机理） |
| rsn +0ms ≠ rsn +500ms（跨拍类型变化时） | 是 | +0=入眠前旧值，+500=恢复链后真值 |
| cb reason 与 rsn +500ms 一致 | **是** | sleepcb 是可靠信源 |
| hook_latch ≠ 0xEE | 是（已撤回旧判定） | hook 实际有派发（0x01=timer）；0xEE 只说明无写入存活 |

## 已知陷阱（对 FAE 同样适用）

1. **hook 状态别用 `__dsleep_data`**：每次 park entry 从 flash 重载，双向写入
   都会被抹（本 demo 的 latch 展示此效应；工程代码用普通 .bss）。
2. **`system_sleep()` 异步返回**：返回后 ~120ms 才真正入眠——返回路径上的任何
   dsleep 配置撤销（如 `dsleep_set_wakeup_hook(NULL)`）会在入眠前自毁。
3. **os_jiffies 跨停泊不可用**：停泊时长丢失（实测 10s 停泊只留 ~60ms），跨
   睡眠计时只能用 RC（`pmu_tmrao_get`，恢复链补偿后有效）。

## 无 MQTT 依赖

TCP 包直接通过 wkdet 过滤器唤醒设备（过滤器放行任何带载荷的 TCP），不需要 MQTT broker。
