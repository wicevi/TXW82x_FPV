# netctl（网络轨）

## 用途
快启抓拍模式的"第二条轨道"：CPU1（WiFi 协议栈+RF）不再随 pre_main 启动（`system0.c` 的 `NE102_DEFER_CPU1`），由本组件按需拉起整个网络栈——立即上传分叉即启 / 批量达标清仓再启 / 仅本地存储永不启（PRD §3.2/§5.4 两级判定中的"执行者"）。

**快启轨网络策略（2026-10-10 用户评审定案）**：
- **仅 STA**：网络轨只为连路由器上传服务，`wifi_mode` 运行时强制 STA（**不持久化**，配置模式的 AP/Web 不受影响）；耗时主要在连接路由器+协议连接+上传。
- **未配网即跳过**：用户从未配置路由器 → 整条网络轨跳过（CPU1 不启动、返回 `NE102_NETCTL_NO_CRED_RET`），抓拍回落纯存储路径。

## 入口
- `netctl_start()`：凭据检查 → `sys_start_cpu1(0x10001000)`（内部忙等 cpu1_ready）→ 强制 STA → `sys_wifi_init()` + `sys_network_init()`。幂等。连接/DHCP 异步完成（③期上传组件等 `SYSEVT_WIFI_CONNECTTED`/`SYSEVT_LWIP_DHCPC_DONE` 事件）。
- `netctl_state()`：OFF / NO_CRED / SYS（配置模式走系统路径）/ STA。
- `ne102_cpu1_ensure()`：只启动 CPU1（main.c 的配置模式分叉在 msi/Codec init 前调用）。
- `netctl_cpu1_started()`：CPU1 是否已运行。
- 调试：`AT+NENET`（`?` 查询 cpu1/state/cred；未配网回 SKIP）。

## 凭据判定（上板发现的关键坑）
"已配网" = `sys_cfgs.ssid` 非空 **且不是本机自动 AP 名**。预编译 wifi 库会在每次配置模式启动时把自动生成的热点名（`82X_`+MAC 后 3 字节大写 hex，如 `82X_433BA8`）写入 `sys_cfgs.ssid` 并持久化——单纯判空会把出厂设备误判为"已配网"。判定逻辑精确比对 MAC 生成的自动名（MAC 不可用时按前缀+长度兜底）。②期 Web 配置落地后建议改为独立 STA 凭据字段（config 组件），彻底摆脱该字段的双义性。

## 依赖
- project：`sys_start_cpu1`（system0.c，无公开头文件声明、本组件 extern）、`sys_wifi_init/sys_network_init`（`__init` 为空宏，可运行时调用）
- custom：mode_router（配置模式不重复初始化）

## 当前状态
可用（阶段①上板验证通过，2026-10-10）
- 已验证：**真实路由器 STA 全链路**（Camthink_hi，WPA2）：CPU1 晚启动 → 强制 STA → 关联 → 4-Way/组握手 → `wifi connected` 事件自动起 DHCP → `dhcp done ip:192.168.31.161` → ping 网关 0% 丢包（5-31ms）；NENET→拿 IP 共 10.3s（其中 wifi connected 事件→DHCP 启动有 ~7s 去抖，③期上传链路优化点）；**无凭据跳过**（CPU1 完全不启）；**STA 强制**（was mode 2，not persisted）；CPU1 晚启动 **46ms ready**
- **信号敏感性实测**（重要）：rssi -83/-73 时 assoc 响应收不到（auth 通、assoc_req 无响应循环，重试可挤入但 EAPOL 反复重握手、DHCP OFFER 丢失）；rssi -33/-37 一次通过。弱信号表现属链路质量非固件逻辑——测试时设备需离路由器较近；正式产品关注天线/结构设计
- 未验证：休眠唤醒后重连（④期）；AP+STA 双模并发（②期，见下）

## 已知问题
- `netctl_stop()`（iface 停止/下电）未实现，④期随休眠模块表补
- CPU1 启动后无法停止（一次启动常驻），仅本地存储模式的省电收益依赖"根本不启动"
- **红线（上板教训）**：任何 RPC 类挂接（Codec/audio coder 等）必须在 CPU1 就绪之后——首版 CPU1 在 Codec_init 后启动导致 audio_codec_thread 野指针 crash loop，已在 main.c 把分叉提前到 msi/Codec_init 之前并关闭 AAC 挂接
- 自动 AP 名判定依赖预编译库的命名规则（`82X_` 前缀），SDK 升级时需复核
- `wifi connected` 事件→DHCP 启动有 ~7s 去抖（NENET→IP 总 10.3s 的主要构成），③期上传链路查明并优化（PRD 预算：唤醒→图到服务器 ≤5s）
- **配置模式 AP+STA 双模（2026-10-10 用户评审后实现）**：配置模式初始化完成后运行时切 APSTA + `syscfg_flush(1)` 双接口同起（AP 服务 Web/DHCPD，STA 扫描/连接）；出厂态清 STA ssid 防自连；AT+SCAN 已 APSTA 感知（扫 vif1）。上板验证：CSA 信道同步 + STA 自动连 + DHCP + ping 通 + 并发扫描。快启侧不变（跳过 wifi，netctl 强制 STA）。

## 变更记录
| 日期 | 摘要 |
|------|------|
| 2026-10-10 | 真实路由器 STA 全链路上板通过（关联/4-Way/DHCP/ping）；记录信号敏感性与 7s 事件去抖；配置模式 APSTA 定为②期项 |
| 2026-10-10 | STA-only 语义落地（用户评审）：凭据检查+运行时强制 STA+无凭据跳过；上板发现自动 AP 名污染 syscfg.ssid 并修复判定 |
| 2026-10-10 | 上板验证通过（46ms CPU1 晚启动数据）；记录 RPC 挂接顺序红线 |
| 2026-10-10 | 初始创建（阶段①：CPU1 晚启动 + 网络 init 幂等封装 + AT 冒烟口） |
