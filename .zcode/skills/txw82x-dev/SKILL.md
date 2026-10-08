---
name: txw82x-dev
description: TXW82x 低功耗相机项目开发指南。当任务涉及本仓库的固件/业务开发、构建编译（cdk-make、Core→App）、CUSTOMER_ID 方案选择、config.cfg/pin_param.h 板级配置、低功耗休眠（system_sleep、lowPower_app、dsleep hook）、Wi-Fi/BLE/视频/音频/SD/USB 外设、或排查构建/启动/无图/功耗等问题时使用。包含构建命令、配置体系、目录导航、硬性规则、官方 MCP 与文档索引。
---

# TXW82x 低功耗相机项目开发指南

本仓库 = 泰芯官方 TXW82x FPV SDK（v2.7.1.7-44398，分支 `v2.7.1.7`）+ 自研低功耗相机产品。
自研业务代码与资料统一放 `custom/`（当前有 `custom/doc/`）；尽量不改动 `sdk/`、`project/` 内官方源码，确需改动须在提交说明中注明动机。

硬件基线：芯片以 `custom/doc/TXW828-C08FL数据手册_V1.8.pdf` 为准，板卡以 `custom/doc/ne102_schematic.pdf` 为准；与官方学习板（TXW828-E016F）引脚/配置不同，不能照搬学习板结论。

## 0. 硬性规则（改代码前必读）

1. **构建顺序固定 Core→App**：先 `txw82xCore` 再 `txw82xApp`；App 链接需要 Core 的镜像大小/BSS/CRC 回填。最终交付物是 `project/txw82xApp/APP.bin`。改了 Core 相关内容后必须完整重建两工程，否则 `APP.bin` 内嵌旧 Core。
2. **SDK 非全源码交付**：`libs/` 含预编译静态库（Wi-Fi/ISP/H.264/USB 等）。修改未生效时先确认符号来自源码还是 `libs/*.a`；不得复制/改名静态库绕过冲突，不得用其他 SDK 版本的库替换。
3. **新增 `.c` 文件必须注册进 CDK 工程**：把 `<File Name="相对路径">` 节点加入 `project/txw82xApp/txw82xApp.cdkproj`（XML，文件路径相对 `project/txw82xApp/`，如 `../../custom/app/xxx.c`），且属于 `FLASH` 配置。只把文件复制进目录不会参与编译。
4. **`pin_param.h` 是固件参数 ABI**：枚举只允许在末尾追加，禁止插入/删除/交换/重命名已有项，否则旧 `config.cfg` 与量产设备参数区整体错位。
5. **配置三层优先级**：`project/txw82xApp/project_config.h`（选方案 CUSTOMER_ID）→ `sdk/demo/**/**config.h`（方案级开关）→ `project/txw82xApp/sys_config.h`（仅兜底默认值）。不要把产品开关写进 `sys_config.h`。
6. **`config.cfg` 必须与本板原理图逐项核对**：构建后处理会把 `config.cfg` + `pin_param.h` 参数直接写入 `APP.bin`。仓库当前 `config.cfg` 是学习板（NO.25-439 双摄像头）配置，与本产品不匹配；切换时从 `project/txw82xApp/cfg/` 复制最接近板卡的 `.cfg` 再改。
7. **休眠前必须**：完成 Flash/SD 写入、停掉仍在访问掉电域内存的 DMA、维持电源保持 IO（如 SW1 自锁供电）。任一模块 suspend 失败框架会回滚本次休眠，不要强行继续。
8. **ISR/事件回调中禁止**阻塞、睡眠、动态分配大内存、文件/网络操作；回调只做状态更新和投递 work。
9. **Cache/DMA 所有权**：DMA 读 CPU 写的数据前 clean，CPU 读 DMA 写的数据前 invalidate；传输完成前禁止释放/改写 buffer；MSI 帧交给下游后不再写原帧。
10. **仅支持 Windows + 玄铁 CDK 环境**（本机 CDK 在 `D:\C-SKY\CDK`）；文本文件保持 LF。构建产物（`Obj/`、`Lst/`、`*.bin` 等已被 `.gitignore` 排除）不提交。
11. 提交时不添加 `Co-authored-by: Codex` trailer（见根目录 AGENTS.md）。
12. **代码注释必须使用英文**：所有新增/修改的代码注释（`.c/.h/.s`、Makefile、链接脚本、构建/下载脚本）一律用英文；中文仅用于文档（`custom/doc/`、各 `README.md`）。

## 1. 构建与固件

**快捷方式**：仓库根目录 `./build.sh`（或 `build.bat`，纯英文输出）——`txw`（默认：Core→App+归档）/`core`/`app`（alias `txw-app`）/`pmu`/`all`，清理 `clean`/`clean-core`/`clean-app`/`clean-pmu`；无参数进交互菜单（直接回车=默认 TXW Core→App）。构建成功自动归档到 `archive/<时间戳>/` + `archive/latest/`（含 git 版本、md5 的 build-info.txt）。可用环境变量覆盖：`CDK_MAKE`、`PMU_GCC_PATH`、`ARCHIVE_DIR`。

cdk-make 是**基于工作区**的工具：`-w` 传工作区文件 `.cdkws`，`-p` 传**工程名**（不是 .cdkproj 路径，传路径会报 "File does not exist"）。

```bash
# Git Bash（本机 CDK 安装在 D:\C-SKY\CDK；若路径不同按实际替换）
/d/C-SKY/CDK/cdk-make.exe -w '.\project\txw82x.cdkws' -p txw82xCore -d build -c FLASH
/d/C-SKY/CDK/cdk-make.exe -w '.\project\txw82x.cdkws' -p txw82xApp  -d build -c FLASH
```

```powershell
# PowerShell 等价形式
& 'D:\C-SKY\CDK\cdk-make.exe' -p '.\project\txw82xCore\txw82xCore.cdkproj' -d build -c FLASH
& 'D:\C-SKY\CDK\cdk-make.exe' -p '.\project\txw82xApp\txw82xApp.cdkproj' -d build -c FLASH
```

- 清理：把 `-d build` 换成 `-d clean`（两工程都执行）。切换方案/链接脚本/静态库/大量宏后必须完整清理重建。
- Core 后处理（`BuildBIN.sh`）生成 `txw82xcore_crc.bin` 并复制为 `project/txw82xApp/txw82xcore.bin`，同时回填 App 链接脚本 `utilities/gcc_csky.ld`——不要手工固定 Core 预留大小。
- App 后处理合并 Core/参数/`psram.bin` 输出 `APP.bin`；若打印 `!!!txw82xcore code is distroyed!`，先清理重建 Core 再重建 App。
- 构建成功的判据：返回码 0 + `APP.bin` 时间戳更新 + `project.map` 无区域溢出 + 无未定义符号，不能只看 ELF 已生成。
- IDE 方式：CDK 打开 `project/txw82x.cdkws`，`Debug` 配置映射两工程 `FLASH`，同样先 Core 后 App。
- 烧录：详见 NE102 文档 §9。TXLink-Lite 二线（**J1：PA9=DAT、PA10=CLK，VRET 接 3V3**，数据手册为准——原理图 TXLink 标注线序与手册相反，连不上先对调）；USB 烧录走 Type-C（PD13/PD12，需官方下载工具）；CKLink 调试烧录同用 PA9/PA10。**NE102 前提：PMU 固件先运行给主控域上电，否则任何方式都连不上 TXW**。调试串口 921600。

## 2. 目录与配置导航

```text
custom/                  自研业务代码与资料（本项目增量都在这里）
└─ doc/                  项目文档（索引见 §6）
project/txw82x.cdkws     CDK 双工程工作区
project/txw82xApp/       CPU0 应用工程（E804FD 硬浮点，-Os）
  main.c                 系统入口：sys_app_init() 选 Demo，usr_app_init() 用户扩展
  project_config.h       CUSTOMER_ID 方案选择（当前默认 5）
  sys_config.h           系统默认宏（仅兜底，勿直接改）
  config.cfg             当前生效板级引脚配置；cfg/ 下有多种板卡备份
  pin_param.h            引脚参数枚举（ABI，只追加）
  device.c               设备表 attach（dev_get(HG_*_DEVID) 的来源）
  syscfg.c/h             运行时参数 sys_cfgs（结构只尾部追加）
  wifi.c ble.c network.c events.c mount.c atcmd.c
project/txw82xCore/      CPU1 无线 Core 工程（E804D，-O2）
sdk/include/             公共 API 头文件（hal/ osal/ dev/ lib/）
sdk/driver/              开放驱动源码
sdk/lib/                 开放组件（video/net/fs/audio/lvgl/…）
sdk/app/                 可复用应用组件（lowPower_app/ spook/ video_app/ …）
sdk/demo/                产品方案（见下表）
libs/                    预编译静态库（勿改）
```

### CUSTOMER_ID 方案表（`project_config.h`）

| ID | 方案 | 配置文件 | 备注 |
|----|------|----------|------|
| 5 | 720P IPC | `sdk/demo/ipc_720p_demo/` | 当前默认 |
| 7 | 720P Sleep | `sdk/demo/ipc_Sleep_720P/` | 低功耗 720P，STA |
| 9 | Battery Camera 1080P | `sdk/demo/battery_camera_1080p/` | 电池摄像机，低功耗参考首选 |
| 1/2/3 | AI 语音/视觉/闹钟 | `sdk/demo/ai_demo/` | |
| 4 | ISP Tuning | `sdk/demo/isp_tuning_demo/` | Sensor/ISP 调试 |
| 6 | LCD 720P | `sdk/demo/lcd_720P_demo/` | MIPI LCD + LVGL |
| 8 | 1080P IPC | `sdk/demo/ipc_1080p_demo/` | |

本项目（低功耗相机）基线建议取 ID 9 或 7；自研方案成熟后可参照"新产品方案"流程（§4）落到 `custom/`。

### 双核与内存速记

- CPU0 跑业务/网络/多媒体；CPU1（`txw82xCore`）跑 LMAC/BLE LL；RPC/邮箱通信，启动以日志 `CPU1 ready!` 为准（CPU0 会死等 `CoreSetting->cpu1_ready`）。
- 内存池：`os_malloc()`(SRAM) / `os_malloc_psram()`(PSRAM) / AV 专用池（`video_sram_init`/`video_psram_init`）；CPU1 的 heap/RX/SKB 由 CPU0 启动时分配。改 AV 池大小时要与 CPU1 SKB、lwIP、文件系统一起核算。
- OSAL 统一入口 `sdk/include/osal/`（task/mutex/sema/msgq/work/timer）；系统事件 `sdk/include/lib/common/sysevt.h`（回调只投递）。
- 运行时参数：`sys_cfgs` + `syscfg_save()`；结构前部有 magic/CRC，新字段只追加尾部。

## 3. 低功耗开发（本产品核心）

完整细节读 `custom/doc/TXW82x 低功耗开发指南.md`；API 声明在 `sdk/include/osal/sleep.h`、`sdk/include/lib/lmac/lmac_dsleep.h`。

### 休眠模式（`SYSTEM_SLEEP_TYPE`）

| 模式 | 枚举 | 场景 | 唤醒 | 实测功耗 |
|------|------|------|------|----------|
| 1 WiFi 保活 | `SYSTEM_SLEEP_TYPE_SRAM_WIFI` | 保持 WiFi 连接，唤醒后断点恢复 | 远程/IO/Timer | ~338μA |
| 3 SRAM 保持 | `SYSTEM_SLEEP_TYPE_SRAM_ONLY` | 断 WiFi，唤醒后断点恢复 | IO/Timer | ~91μA |
| 5 RTC | `SYSTEM_SLEEP_TYPE_RTCC` | 全重启，重新初始化 | IO/Timer | ~43μA |

```c
struct system_sleep_param {
    uint32 sleep_ms;        // 0 = 不用 Timer 唤醒
    uint8  wkup_io_sel[6];  // 最多 6 路唤醒 IO 引脚号
    uint8  wkup_io_en;      // bit0~5 对应 sel[0..5]，1 使能
    uint8  wkup_io_edge;    // bit0~5：0 上升沿(内打下拉)，1 下降沿(内打上拉)
};
int32 system_sleep(uint16 type, struct system_sleep_param *args);
```

### 关键 API 速查

| 用途 | API |
|------|-----|
| 保持 IO 电平（防浮空/维持自锁供电） | `dsleep_set_user_gioa()` ~ `dsleep_set_user_giof()`（按端口掩码） |
| 外部 DCDC 休眠供电 | `dsleep_set_ext_dcdc(en)` |
| 常电区静态数据 | `__dsleep_data` 修饰变量 |
| 常电区动态申请 | `sys_sleepdata_request(SYSTEM_SLEEPDATA_ID_USER, size)`（用户只能用 `_USER`） |
| 休眠计时（RC Timer） | `pmu_tmrao_get()`，用 `dsleep_get_rc_hz()` 换算 ms |
| 唤醒过滤 | `dsleep_set_wakeup_hook()`，返回 `DSLEEP_ACTION_SLEEP_CONTINUE` 可放弃本次唤醒 |
| 唤醒窗口收包 | `dsleep_set_rx_hook()`（中断上下文，尽快返回） |
| RF 就绪发包 | `dsleep_set_ready_hook()` + `dsleep_tx_ether()`（周期保活） |
| 系统休眠回调 | `sys_register_sleepcb()`，APP 阶段为 `SYS_SLEEPCB_APP` |

前提：工程启用 `CONFIG_SLEEP`，且 `project/txw82xApp/wifi.c` 已调用 `bgn_dsleep_init()`（RTC 时钟源默认内部 RC，需高精度时用 flags `BIT(0)` 使能外部晶振）。

### lowPower_app 应用层框架（`sdk/app/lowPower_app/`）

产品休眠需按依赖暂停/恢复文件系统、Sensor、MIPI、ISP、VPP、音频等模块，统一走该框架：

- 模块用 `struct lowPower_module_ops { name, suspend, resume, priv[4] }` 描述，按初始化依赖顺序排数组。
- **suspend 逆序执行（数组尾→头），resume 正序执行（头→尾）**；suspend 失败自动回滚已挂起模块。
- 一次性调用 `lowpower_app_init(ops_array, count)` 注册（再次调用会清空重注册）；resume 由独立任务异步执行，耗时的重新挂载/初始化放 resume 回调里是安全的。
- 参考实现：`sdk/demo/battery_camera_1080p/battery_camera_sleep_1080p_cb.c`（7 模块：prev→sd→mipi→isp→vpp→audio→post；`post` 的 suspend 最先执行，负责切外围电源电平 + `dsleep_set_user_giox()` 保持电源脚）。
- 唤醒包过滤：`dsleep_set_usr_wkdet_cb()` 的检测回调当前仅判断"任意 TCP 包"（`data[9]==0x06`）即唤醒；产品需收紧为特定端口/内容时在此回调中继续解析。

### 唤醒 IO 与实测

- 6 路唤醒源 IO0~IO5；**PA0~PA14 被全部 6 路覆盖，兼容性最好**（PB 只有 PB6~PB15 且部分源缺 PB；PD 仅 IO1/3/5 支持 PD0~PD13）。
- 板上 SW1 按键 = PB7（下降沿）；USB 插入检测脚可作唤醒 IO。
- 开发板测试：`AT+SLEEP=5,80000,23,1`（RTC 模式 80s + PB7 下降沿唤醒），RTC 模式休眠电流应落在 35~45μA。
- SW1 是"按住接通、软件自锁"型电源开关：休眠期间必须用 `dsleep_set_user_gio*()` 保持电源使能 IO 输出，否则休眠即断电。

## 4. 业务代码落点（custom/ 约定）

```text
custom/
├─ README.md      # 目录总览 + 组件索引表 + 文档维护规则与 README 模板（agent 维护）
├─ app/           # 产品应用层（程序入口）：app_lowpwr_camera.c 的 app_lowpwr_camera_init() 由 usr_app_init() 调用，编排各组件与低功耗注册；文件按 SDK 风格 app_*.c 命名
├─ pmu/           # 独立子工程：NE102 电源协处理器 N32L403KBQ7 固件（GCC/Makefile + J-Link，不走 CDK；构建方式见 pmu/README.md）
├─ component/     # 业务功能组件：每组件一个子目录，对外入口收敛为一个 <组件>_init()，目录内维护 README.md
├─ lib/           # 可复用纯软件库：协议/算法/数据结构，不直接操作硬件
├─ driver/        # NE102 板级/外设驱动：SDK 未提供的器件（电源时序、PIR、LED、按键…），每器件一个子目录
├─ cfg/           # 板级配置备份：命名 NE102_V<版本>_<yyyymmdd>.cfg；生效配置仍是 project/txw82xApp/config.cfg
├─ doc/           # 资料与文档（索引见 §6）+ PROJECT_STATUS.md 进展日志
└─ tools/         # 自研脚本/工具
```

- 分层依赖单向：`app/`（编排）→ `component/`（业务）→ `driver/`/`lib/` → SDK；不要反向引用（driver/lib 不得调 component/app）。
- 产品应用入口是 `custom/app/app_lowpwr_camera.c` 的 `app_lowpwr_camera_init()`，由 `project/txw82xApp/main.c` 官方预留的 `usr_app_init()` 钩子调用；各业务组件 init 由 `app_lowpwr_camera_init()` 按依赖顺序编排。init 必须快速返回，耗时逻辑放独立 task/workqueue。
- 新文件记得注册进 `txw82xApp.cdkproj`（规则见 §0.3）。头文件搜索路径如需新增，也在 cdkproj 的 IncludePath 中配置。
- 新建正式产品方案的标准步骤：复制最接近 Demo 的配置和骨架 → 建独立 `<product>_config.h`（只覆盖需要的宏）→ `project_config.h` 加新 CUSTOMER_ID 分支 → `sys_app_init()` 加唯一初始化入口 → 注册 CDK 工程 → 选配 `config.cfg` → 先冒烟（Core ready/heap/Sensor/ISP）再逐步加编码/网络/录卡 → 完整 Core→App 构建，留档 map 与启动日志。
- 同一固件不得同时定义多个顶层 Demo 宏。

### 文档维护（任务完成后强制执行）

每次完成开发任务后必须同步更新文档，让下一个 agent 不读代码就能了解项目进展与组件状态（细则与 README 模板见 `custom/README.md`）：

1. 涉及的应用/组件/驱动/库目录（`custom/app/`、`custom/component/<组件>/`、`custom/driver/<器件>/`、`custom/lib/<库>/`）维护 `README.md`（不存在就创建）：用途、入口、依赖、当前状态、已知问题、变更记录；
2. `custom/doc/PROJECT_STATUS.md` 表格顶部追加本次工作记录（日期/摘要/影响范围/下一步）；
3. 组件增删或状态变化时，同步 `custom/README.md` 组件索引表；
4. 只记事实与结论，不复制 SDK 文档内容；文档更新与代码改动同一次提交。

## 5. 官方 MCP 与在线资料

### taixin-documentation MCP（工作区已配置于 `.zcode/config.json`，HTTP + OAuth 登录）

| 工具 | 用途 |
|------|------|
| `search_taixin_sources(query, language)` | 搜索官网产品/文档/参数/应用资料 |
| `get_taixin_product_specs(model, language)` | 查具体型号规格（如 TXW828、TXW8301） |
| `find_taixin_downloads(query, language)` | 找数据手册/手册/SDK/下载项 |
| `compare_taixin_products(l, r, language)` | 对比两个具体型号 |

使用原则：**本地 `custom/doc/` 优先**（与仓库版本一致），本地没有或需最新版/其他料号资料时再用 MCP 或官网。查不到会返回 `unresolved`；MCP 只覆盖公开官网内容，不含密钥/价格/内部结论。使用说明：https://taixin-semi.com/en/mcp

### 高频在线文档（完整链接表见 `custom/doc/TXW82x_FPV_SDK开发文档.md` §21）

- 文档列表总入口：https://taixin-semi.com/zh/documentList?productScope=category%3Atxw82x
- TXW82x SDK 架构与配置 / 框架原理 / 视频应用功能使用说明（本地 `custom/doc/` 有同名镜像）
- TXSDK Wi-Fi / BLE / 网络应用 / OTA / AT 指令 / 主控交互 / USB / Cat.1 开发指南
- FLASH 常见问题 FAQ、烧录方法与异常排查、LCD FAQ、硬件设计指南

## 6. 本地文档索引（custom/doc/）

| 文件 | 何时读 |
|------|--------|
| `TXW82x_FPV_SDK开发文档.md` | **主参考**。构建/配置/双核/内存/模块参考/烧录/调试，含全部官网链接表 |
| `TXW82x 低功耗开发指南.md` | 休眠模式/API/Hook/lowPower_app/功耗实测/唤醒 IO——本产品核心 |
| `TXW82x SDK 架构与配置说明.md` | 目录结构、方案 ID、配置生效方式、客户开发建议 |
| `TXW82x SDK 框架原理说明.md` | MSI 媒体流、TXMPlayer、LCD 显示流、VFS |
| `TXW82x SDK 视频应用功能使用说明.md` | 拍照/录像/RTSP 图传/录风者应用 |
| `TXSDK_USB开发指南.md` | USB Host/Device、类驱动开发、OTG |
| `Cat.1模组接入指南.md` | USB Cat.1 模组联网（RNDIS、网卡注册、宏开关） |
| `TXW828-C08FL数据手册_V1.8.pdf` | 目标芯片电气/引脚/规格（以受控版本为准） |
| `ne102_schematic.pdf` | 本产品板卡原理图（引脚/电源域核对） |
| `NE102 系统IO与连接关系.md` | **本板 IO 速查**：TXW828 引脚表、N32L403 协处理器、电源树、模块连接、SDK 差异清单（引脚问题先查这份再读原理图） |
| `n32l40x/` | PMU（N32L403）专区：数据手册 V2.2、低功耗应用笔记、GCC 环境应用笔记、JLink 器件支持包（tools/）——与 TXW 资料分开归档 |
| `PROJECT_STATUS.md` | 项目进展日志（agent 每次任务后在顶部追加记录，见 §4 文档维护） |

## 7. 排查路由（现象 → 首查）

| 现象 | 首查 |
|------|------|
| 修改源码不生效 | 文件是否在 cdkproj FLASH 配置；符号是否来自 `libs/*.a`；烧的是否本次 `APP.bin`；是否只改了 `cfg/` 备份没改 `config.cfg` |
| CPU1 功能与源码不符 / 无 `CPU1 ready!` | 完整重建 Core→App；核对 Core 镜像、共享 SRAM、`0x10001000` 入口、Core heap/RX/SKB |
| 无任何日志 | 供电/复位/晶振/波特率 921600；UART 引脚以 `config.cfg` 为准（勿照搬学习板文档） |
| 链接/后处理失败 | `project.map` 区域溢出；Core CRC；`sdktools/parameter.bincfg/psram.bin/makecode.ini`；勿手工扩链接区掩盖重叠 |
| MIPI 无图 | Sensor 电源/RESET/PWDN→MCLK→I2C 读 ID→lane 数/顺序/极性→CSI 帧中断，先见稳定帧再启 ISP/VPP |
| 休眠电流偏高 / 休眠即断电 | 电源保持 IO 是否 `dsleep_set_user_gio*`；SW1 自锁供电；未保持的 IO 会被复位防漏电；DMA/Flash 写入是否完成 |
| 唤醒后异常 | resume 顺序与依赖；`app_sleepcb` 当前不区分唤醒原因（所有唤醒都触发全量恢复）；首帧/首包/SD 重挂载 |
| WiFi 连上但不能联网 | 等 DHCP 完成事件再起业务；默认网卡/网关/DNS；STA 场景勿用固定延时 |
| TF 卡写超时 | SD 时钟/总线宽度对比；写入是否占用 MAIN workqueue（应走 `sd_workqueue`） |
| 屏显黑屏/花屏 | LCD FAQ：先纯色 framebuffer 验面板电源/复位/时序，再查 LCDC/图层/stride/Cache |

调试通则：保存 CPU0+CPU1 完整日志，**找首个错误**（后续超时多为上游失败的连锁）；记录料号/PCB/Sensor/Flash/固件版本；对照本次 `project.map`。

## 8. Agent 工作流清单

改动前：
- [ ] 确认目标符号来自开放源码还是 `libs/*.a`（grep 源码 + 查 map）
- [ ] 涉及引脚/电源域的改动，先对照 `ne102_schematic.pdf` 与 `config.cfg`
- [ ] 涉及休眠流程的改动，画出模块 suspend/resume 依赖再排数组

改动后：
- [ ] 新文件已注册 cdkproj；引脚新宏只追加 `pin_param.h` 尾部；`config.cfg` 同步更新
- [ ] 完整构建 Core→App，核对返回码/时间戳/map
- [ ] 上板冒烟：启动日志、`CPU1 ready!`、PSRAM/heap、再逐个外设
- [ ] 低功耗改动：验证休眠电流、1000 次循环唤醒、唤醒后首帧/首包

完成后（强制，便于下一个 agent 接手）：
- [ ] 涉及组件/驱动/库目录的 `README.md` 已更新（没有就按 `custom/README.md` 模板创建）
- [ ] `custom/doc/PROJECT_STATUS.md` 顶部已追加本次工作记录
- [ ] 组件增删/状态变化已同步 `custom/README.md` 组件索引表
