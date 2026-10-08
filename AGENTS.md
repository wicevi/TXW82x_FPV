# Repository instructions

- Do not add a `Co-authored-by: Codex <noreply@openai.com>` trailer to commits in this repository unless the user explicitly requests it.

## Project context（自研低功耗相机）

本仓库是泰芯官方 TXW82x FPV SDK（分支 `v2.7.1.7`）+ 自研低功耗相机产品。进行任何固件/业务开发前，先加载项目 skill `.zcode/skills/txw82x-dev/SKILL.md`（或由 `txw82x-dev` skill 自动触发），其中包含构建命令、配置体系、低功耗框架和排查路由。

- 自研业务代码与资料统一放 `custom/`（文档在 `custom/doc/`），尽量不改 `sdk/`、`project/` 内官方源码。
- 构建顺序固定 `txw82xCore` → `txw82xApp`，交付物为 `project/txw82xApp/APP.bin`；本机 CDK 位于 `D:\C-SKY\CDK`。
- SDK 部分模块以预编译静态库交付（`libs/`），修改前先确认符号来源；新增 `.c` 必须注册进 `txw82xApp.cdkproj` 的 `FLASH` 配置才会编译。
- `project/txw82xApp/pin_param.h` 是固件参数 ABI，只允许尾部追加。
- 官方文档 MCP（taixin-documentation）已在 `.zcode/config.json` 配置，本地 `custom/doc/` 没有的资料用它查官网。
- **代码注释一律使用英文**（对所有 agent 强制，含 `.c/.h/.s`、Makefile、链接脚本、构建/下载脚本）；中文仅用于文档（`custom/doc/`、各 `README.md`）。

## 文档维护（对所有 agent 强制）

完成任何开发任务后，必须同步更新文档，让下一个 agent 不读代码也能了解项目进展与组件状态。细则与 README 模板见 `custom/README.md`：

1. 涉及的应用/组件/驱动/库目录（`custom/app/`、`custom/component/<组件>/`、`custom/driver/<器件>/`、`custom/lib/<库>/`）维护 `README.md`，不存在则创建：用途、入口、依赖、当前状态、已知问题、变更记录。
2. 在 `custom/doc/PROJECT_STATUS.md` 表格**顶部**追加本次工作记录（日期/改动摘要/影响范围/下一步）。
3. 新增或删除组件、组件状态变化时，同步 `custom/README.md` 的组件索引表。
4. 只记录事实与结论，不复制 SDK 文档内容；文档更新与代码改动放进同一次提交。
