# custom/ — 自研业务目录

本目录承载低功耗相机产品的全部自研代码与资料。SDK 官方内容（`sdk/`、`project/`、`libs/` 等）尽量不改；本目录即"我们的产品"。开发规则总纲见 `.zcode/skills/txw82x-dev/SKILL.md`。

## 目录结构

| 目录 | 用途 | 约定 |
|------|------|------|
| `app/` | 产品应用层（程序入口） | 应用入口与编排：`app_lowpwr_camera_init()` 由官方 `usr_app_init()` 调用，在此编排各组件初始化与低功耗注册；文件按 SDK 风格 `app_*.c` 命名 |
| `pmu/` | PMU 独立固件工程 | NE102 电源协处理器 N32L403KBQ7 的独立 GCC 工程（自带 Makefile/库/链接脚本，**不走 CDK 构建**，与 TXW82x 固件分开烧录）；见 `pmu/README.md` |
| `component/` | 业务功能组件 | 每个组件一个子目录（可多对 `.c/.h`），对外入口收敛为一个 `<组件>_init()`；目录内维护 `README.md` |
| `lib/` | 可复用纯软件库 | 协议解析、算法、数据结构等**不直接操作硬件**的代码，供 component/driver 调用 |
| `driver/` | 板级/外设驱动 | NE102 特有、SDK 未提供的外设驱动（电源时序、PIR、LED、按键、传感器等），每个器件一个子目录 |
| `cfg/` | 板级配置备份 | 命名 `NE102_V<版本>_<yyyymmdd>.cfg`；生效配置仍需复制到 `project/txw82xApp/config.cfg` |
| `doc/` | 文档资料 | 参考资料（索引见 skill §6）+ `PROJECT_STATUS.md` 项目进展日志 |
| `tools/` | 脚本工具 | 构建/分析/烧录辅助脚本，注明运行方式与依赖 |

**注释语言**：代码注释一律使用英文（含 Makefile/链接脚本/下载脚本）；中文仅用于本文档、`custom/doc/` 及各组件 `README.md`。

**编译须知**：`custom/` 下新增 `.c` 必须注册进 `project/txw82xApp/txw82xApp.cdkproj` 的 `FLASH` 配置（XML 中 `<File Name="相对路径"/>`，路径相对 `project/txw82xApp/`，如 `../../custom/component/xxx/xxx.c`），否则不参与构建。新增头文件搜索路径同样在 cdkproj 的 IncludePath 中配置。

**初始化挂接**：产品应用入口是 `custom/app/app_lowpwr_camera.c` 的 `app_lowpwr_camera_init()`，由 `project/txw82xApp/main.c` 官方预留的 `usr_app_init()` 钩子调用；`app_lowpwr_camera_init()` 再按依赖顺序编排 `custom/component/` 各组件的 init。init 必须快速返回，耗时逻辑放独立 task/workqueue。

## 组件索引（agent 维护：增删组件或状态变化时更新此表）

| 组件 | 路径 | 职责 | 状态 | 文档 |
|------|------|------|------|------|
| app_lowpwr_camera | `app/` | 低功耗相机产品应用入口（编排层）：`app_lowpwr_camera_init()` 由 `usr_app_init()` 调用，编排各组件初始化与低功耗注册 | 开发中 | [README.md](app/README.md) |
| pmu（独立工程） | `pmu/` | NE102 电源协处理器 N32L403KBQ7 固件（GCC/Makefile + J-Link，非 CDK） | 可用（骨架） | [README.md](pmu/README.md) |
| burn.bat 等工具 | `tools/` | 自主烧录（CDK 配置直调）/ 烧录+校验 / 全片读回 / GDB-RSP 调试客户端 | 可用 | [README.md](tools/README.md) |

状态取值：`规划中 / 开发中 / 可用 / 阻塞 / 废弃`。

## 文档维护规则（对所有 agent 强制）

每次完成开发任务后必须同步更新文档，让下一个 agent 不读代码就能了解项目进展与组件状态：

1. **组件 README**：本次涉及的应用/组件/驱动/库目录（如 `custom/app/`、`custom/component/<组件>/`、`custom/driver/<器件>/`、`custom/lib/<库>/`）维护 `README.md`，不存在则按下方模板创建，存在则更新"当前状态/已知问题/变更记录"。
2. **进展日志**：在 `custom/doc/PROJECT_STATUS.md` 表格**顶部**追加一行本次记录（日期倒序）。
3. **组件索引**：新增/删除组件或状态变化时，更新上方组件索引表。
4. 只记录事实与结论（做了什么、为什么、当前状态），不复制 SDK 文档内容；文档更新与代码改动放进同一次提交。

### 组件 README.md 模板

```markdown
# <组件名>

## 用途
一句话说明组件做什么、给谁用。

## 入口
- 初始化函数：`xxx_init()`（由 `usr_app_init()` / 上层组件调用）
- 关键文件：`xxx.c`（核心逻辑）、`xxx_hw.c`（硬件访问）…

## 依赖
硬件引脚/外设、SDK 组件、其他 custom 组件。

## 当前状态
<可用 / 开发中 / 阻塞 + 原因>
已验证：…；未验证：…

## 已知问题
- …

## 变更记录
| 日期 | 摘要 |
|------|------|
| 2026-xx-xx | 初始创建 |
```
