# wake_capture — 唤醒抓拍周期测试（功耗测试固件扩展）

一句话：`AT+PWCAP=1,10` = 睡 10s → 被唤醒（定时/远程数据均可）→ 抓 1 张
JPEG 暂存 PSRAM → **相机立刻下电** → 按配置落 SD / HTTP 上传 → 立刻再睡 →
下次唤醒空闲。用户 2026-10-22 需求的最小产品形态闭环（PRD §唤醒抓拍场景的
功耗/时序测量载体）。支持两条唤醒路径：**SRAM_WIFI 断点续跑**（默认）与
**RTCC 重启冷启动**（`type=5`）。

独立成组件（不塞进 app_power_test.c，防文件膨胀）；入睡用 app_power_test
导出的 `pt_sleep_sync()`（SRAM 路径）/ `pt_sleep_park()`（RTCC 路径）。

## 周期时序（wc_task 常驻任务 + 启动信号量，按请求分派三流程）

### 路径 A：SRAM_WIFI 断点续跑（type=1，默认）

```
park#1 (SRAM_WIFI, RC 定时兜底)          ← pt_sleep_sync 阻塞到真醒
  ↓ 任意源唤醒（timer/remote-data/link）
wc_capture_persist():
  capture: rails up → sensor/MIPI/ISP/VPP/JPEG → 跳帧(skip) → 1 张
           → consumer stash_only=1：帧只进 PSRAM 暂存，不碰 SD
  ↓ ne102_capture_deinit() ← 相机轨/管线下电，暂存按设计存活
  persist（相机已断电）:
    SD  : fatfs_sd0_init → 写 0:/DCIM/WK_xxx.JPG → fsync → 读回 CRC 校验
          → sys_mount_device(unmount) + sdhost_deinit_for_sleep
    HTTP: wc_http_post(url, stash)（全步骤 6s 有界；失败只记日志）
park#2（"立马再次休眠"；抓拍意图一次性，唤醒#2 即空闲）
  ↓
报告（AT+PWCAP? 可事后查询）
```

### 路径 B：RTCC 重启冷启动（type=5）

```
AT 时刻: wc_rtcc_arm() → 抓拍意图写入 syscfg KV（flash，键 ne102_pwc）
         → pt_sleep_park(RTCC, secs) → 停泊（唤醒=重启，任务永不返回）
  ↓ RTCC 到点 → 芯片重启（冷启动）
boot 时刻: wake_capture_init() 检出 KV 意图 → 立即消费（delete，one-shot：
         中途崩溃也不会在后续 boot 反复触发）→ wc_boot_cycle():
         wc_capture_persist(wait_ip=1)（HTTP 腿前有界等待开机后取到的 IP，
         上限 15s；抓拍本身不等网络，boot→JPEG 时序可测）
  ↓ 报告（仅串口日志——再睡的重启会抹掉 RAM 里的 wc.r）
再睡: pt_sleep_park(RTCC, secs)（意图已消费）→ 下一次开机 = 空闲冷启动
```

功耗要点（用户明确要求）：相机只在抓拍窗口带电；JPEG 落 PSRAM 暂存后即可
下电释放，SD/网络 IO 全部在相机断电后进行。TF 卡在 park#2 的 suspend 断轨
前已完成卸载。

## AT 命令（仅 RTCC 意图持久化到 KV，其余重启回默认）

| 命令 | 说明 |
|------|------|
| `AT+PWCAP=<mode 0..3>,<secs 5..3600>[,<skip 0..10>[,<type 1\|5>]]` | 跑一个周期：mode 1=SD 2=HTTP 3=两者（0=**解除挂起的 RTCC 意图**）；secs 为 park/再睡的 RC 兜底窗口（期间远程数据包会提前唤醒并照常抓拍——产品场景）；skip=AE 跳帧数默认 3；type 1=SRAM_WIFI 断点续跑（默认）5=RTCC 重启冷启动（唤醒=重启，抓拍在下一次开机执行） |
| `AT+PWCAP?` | busy/配置/RTCC 意图状态 + 上次周期完整报告（RTCC 重启后 RAM 报告丢失，看串口日志） |
| `AT+PWCURL=<url>` | HTTP 目标 `http://<ip\|host>[:port]/path`（默认 `http://192.168.110.50:8080/upload`，RAM 保存；RTCC 路径会随意图一起写进 KV） |

互斥：与 AT+PWRS/AT+PWRDT 互相 busy 检测（同窗口一个 park）。LED：整个周期
`pt_led_hold_set(1)` 压暗（电流测量友好）。

## 文件

- `测试指南.md`：**上手看这个**——环境/指令/六条测试流程/判定基线（2026-10-22 实测数据）/排障表。
- `wake_capture.c/.h`：周期编排 + AT + SD 保存/读回 CRC 校验（wc_sd_save，
  静态 4KB 校验缓冲，CRC32 表惰性初始化）。
- `wc_http.c/.h`：最小 HTTP/1.1 POST 客户端（lwip sockets；DNS 可用 IP 直连
  优先；状态行解析；每步 6s 上界；`wc_http_result.status` <0 = 传输层失败）。
- 接收端：`custom/tools/http_recv.py [port]`（本机跑，存 `wc_recv/`，打印
  大小与 JPEG SOI 校验）。
- 自动验证脚本：`custom/tools/wc_test1.py`（等 COM7 空闲后自动回归
  PWRS/PWRDT/PWCAP×3+RTCC 腿并拉起接收端，串口闪断自动重连，日志
  `wc_test1.log`）；`wc_rtcc.py` 单独重跑 RTCC 腿（日志 `wc_rtcc.log`）。

## 依赖

- `../capture/`（ne102_capture_*，stash_only 位为本组件新增，mqttka 路径
  默认 0 行为不变）；512KB PSRAM 暂存由 `app_power_test_init()` 预占（必须在
  首次 capture_init 切 7.5MB AV 池前，capture.c 2026-10-16 板证坑）。
- `app_power_test.h`（pt_sleep_sync/wake_reason/led_hold）。
- SDK：fatfs（osal_file/fatfs_sd0_init/sys_mount_device/sdhost_deinit_for_sleep，
  电池 demo 的 sd suspend/resume 模式）、lwip sockets、libatcmd。

## 当前状态

**✅ 上板全链路验证通过（2026-10-22，串口日志 wc_test1.log / wc_rtcc.log）**：
- SRAM 路径 `AT+PWCAP=1,10`：唤醒→抓拍 44KB(1920×1080)→相机下电 **461ms**→**SD 落盘（含读回 CRC 校验+卸载）280~310ms**→再睡→空闲醒，**清醒窗口 ~1.1s**；连跑 3 轮 WK_001~003.JPG 全部 crc ok；wake1=1(timer)
- `AT+PWCAP=2,10`：抓拍 460ms + **HTTP 200，350ms**，PC 接收端收到完整 JPEG（SOI/DQT/SOF/EOI 全齐），清醒窗口 1180ms
- `AT+PWCAP=3,10`：both 模式 46928B/458ms + HTTP 200/130ms，清醒窗口 997ms
- RTCC 路径 `AT+PWCAP=1,8,,5`：KV 意图保存→RTCC 停泊→重启（boot 原因=1 timer）→boot +102ms 检出意图→**+105ms 开抓、+201ms 管线起(1920×1080)、+477ms 照片暂存**→再睡 RTCC→空闲开机（意图一次性确认）
- 附带验证：PWRS/PWRDT 走同步封装回归通过（PWRS err=+0ms、PWRDT err=+260ms，与重构前一致）

调试中定位并修复的五个根因（详见 PROJECT_STATUS 10-22 行）：
1. power_test_config.h PSRAM 池 7.5MB（ipc_1080p 拷贝值）+512KB 暂存超出 8MB → av_zalloc 断言崩溃 → 改 4MB（mqttka 已证几何）
2. `flashdb_init()` 全仓库无人调用 → syscfg_* KV 全部 "not inited"（**mqttka 的 KV 持久化同病，latent bug**）→ wake_capture_init 补调
3. power_test_config.h sensor 启用 GC2053（demo 值，本板无此件）而 GC20C3 未启用 → 探测 12s 全 NAK、几何 0x0 → 改 GC20C3=1
4. `vfs_fatfs_register()` 没人调（demo 里在 `app_sd_init()`，本组件直调 `fatfs_sd0_init()` 跳过了它）→ vfs 不认识 "fatfs" 类型，挂载 -19（用 `AT+PWRLOG=7` 打开 DEBUG 级才看见 sdhost 探测全过、挂载层失败）→ wc_sd_save 内每 boot 注册一次
5. `osal_fwrite` 返回**字节数**而非 nmemb 数（sdfunc 按 truthy 判断）→ 本组件 `!= 1` 误判写失败 → 改 `< len`

## 已知问题 / 设计边界

- **偶发上传腿楔死（2026-10-23 事故→当日根因锁定并修复）**：一轮 PWCAP=2 在 `camera dark` 后永久卡死（busy 锁死到复位）。**根因**：该 lwip 构建 **SO_SNDTIMEO/SO_RCVTIMEO 不生效**（recv/send 在慢链路上无限阻塞），叠加台架 WiFi 速率跌至 5~25KB/s（48KB 传 2~10s）。**修复三层**：① wc_http 全部 IO 改 select+非阻塞自管超时（连接 6s/发送 6s/响应 15s——不再赌 socket 选项）；② POST 跑独立 `wc_http` worker + 周期硬看门狗（SRAM 30s / RTCC boot 45s，超时弃置 -100 继续再睡）；③ WC_LEG 面包屑 + hb 心跳。修复后 5×PWCAP=2 压测：**5/5 周期正常再睡零卡死**，成功率随链路速率（2/5@慢链路；链路恢复 130KB/s 时应回到 ~100%）。**教训：本平台 socket 超时选项不可信，网络 IO 一律 select 自管。**
- **控制台 "FFJDV+" 乱码**：SDK `jpg_v3_msi.c` 的单字母进度标记（JD=编码块 DEBUG 级、JE=帧结束 ERR 级）与 vpp 的 V/F 标记交错。固件默认 print_level=6（INFO）已隐藏 DEBUG 级；`AT+PWRLOG=7` 可开回排查（JE 为 ERR 级仍会漏一个字符，无害）。
- 首个 SD 周期可能出现一条 `sd0 mount fail ret=-16(EBUSY)` 噪音：`dev_hotplug_in` 的异步挂载与 `sys_mount_device` 直接挂载竞争，两者其一成功即可（后续周期无此行），不影响落盘。
- **RTCC 停泊会让 CH340 串口闪断**（板电波动，Windows 句柄失效，串口工具会掉线重连一次）——wc_test1/wc_rtcc 脚本已带重连；手动观察属正常现象。
- RTCC 档板级语义已实测通过（arm→停泊→重启唤醒 boot rsn=1(timer)→意图消费→再睡→空闲开机）；CKLink 若在 RTCC 周期后连不上目标，串口发 `AT+RST` 软复位即可恢复烧录。
- 文件名 `WK_<唤醒源><跳帧数>_<序号>.JPG`（2026-10-23）：唤醒源单字母 T=定时族/D=远程数据/I=IO或PIR/L=链路丢失/X=其它，跳帧数即 `AT+PWCAP` 的 skip 参数（RTCC 冷启动用 boot 原因打标）；例 `WK_T3_001.JPG`、`WK_D3_002.JPG`。**防覆盖**：保存前 `osal_fexist` 探测，同名即递增跳过（RAM 计数器重启归零后自动跳过卡上已有文件，跨会话不覆盖旧图）；连续 5000 个同名碰撞报错放弃该腿。
- URL 不含用户名密码/https；host 走 DNS 时受 lwip DNS 超时（秒级）影响。
- RTCC 路径的报告只存在于串口日志（再睡重启抹 RAM）；`AT+PWCAP?` 查不到。
- 意图按"检出即消费"设计：若 arm 后、RTCC 到点前手动断电，下次开机仍会执行一次冷启动抓拍（`AT+PWCAP=0` 可解除）。

## 变更记录

- 2026-10-22：首版（用户需求：唤醒后抓拍，暂存内存即下电相机，SD/HTTP 按
  配置，清标签再睡，下次唤醒空闲；独立文件）。
- 2026-10-22：**RTCC 重启冷启动路径**（用户需求"相当于冷启动"）：意图入
  syscfg KV（ne102_pwc）跨重启、boot 期检出即消费、HTTP 腿前有界等 IP、
  再睡 RTCC 下次开机空闲；`AT+PWCAP=0` 解除挂起意图；type 参数进 AT。
- 2026-10-22：**全链路上板验证通过**（数据见"当前状态"）；排障修复三根因
  （PSRAM 池 7.5→4MB、flashdb_init 补调、GC2053→GC20C3，均在
  power_test_config.h / wake_capture_init）；wc_test1.py 加串口闪断重连、
  新增 wc_rtcc.py 单腿脚本。
- 2026-10-22：**SD 落盘补验通过**（用户插卡后三轮 WK_001~003 crc ok，SD 腿 280~310ms）；排障追加两根因（`vfs_fatfs_register` 未调用致挂载 -19、`osal_fwrite` 返回字节数被误判）+ 新增 `AT+PWRLOG` 打印级别开关（定位 SD 问题时的关键工具）。
- 2026-10-23：**上传腿硬看门狗 + 打印清理**（用户判定"卡在网络上传"）——POST 移入独立 `wc_http` worker，周期有界等待 30s（RTCC boot 45s），超时弃置（-100）继续再睡；固件默认 print_level=6 隐藏 SDK 单字母进度标记（"FFJDV+"乱码源），`AT+PWRLOG=7` 开回；另加 WC_LEG 面包屑与 hb 心跳（定位偶发楔死）。
