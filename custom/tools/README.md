# custom/tools — 自研脚本/工具

## cklink_flash —— CKLink 烧录/读回

| 脚本 | 作用 | 输出 |
|------|------|------|
| `cklink_program_verify.bat`（`cklink_program_verify.json`） | 烧 `project/txw82xApp/APP.bin`，**写入后全量校验** | 控制台报告 |
| `cklink_read_flash.bat`（`cklink_read_flash.json`） | 全片读回 flash 偏移 0 起 0x130000 字节 | `archive/cklink_full_read.bin`，`fc /b` 对比 APP.bin（报尺寸不同属预期，看内部差异） |

- 烧录算法：`D:\Program Files\CSKY-FlashProgrammer-windows\FlashProgrammer\TXW82X_FLASH_ALGORITHM.elf`（同目录 `.init` 为附加初始化）；Console：`D:\C-SKY\CDK\CSKY\FlashProgrammer\Bins\CSKYFlashProgramerConsole.exe -f <json>`。
- 连接：CKLink → J1（TCK=PA10、TMS=PA9，数据手册线序），板上电。

## rsp_read_regs.py —— GDB-RSP 调试客户端

对 DebugServerConsole（端口 1025）做寄存器/内存读写（停核、PC/SP 读取、`M`/`m` 包）。要点：连接后**先发 `+`** 再对话；stop 包寄存器按**小端**解码；pc=regnum 72（ck804 tdesc）。配套服务启动：
`DebugServerConsole.exe -setclk 5000 -port 1025`（可加 `-targetinit <TXW82X_FLASH_ALGORITHM.init>` 关 WDT/解锁后附加）。

## burn.bat —— 自主烧录（2026-10-08 定案的可靠方法）

cd 到工程目录后由 CSKYFlashProgramerConsole.exe -f 消费 CDK 生成的 CSKYFlashProgramerCfg。已验证可靠（含烧后全量校验+复位运行）。
- 关键差异：CDK 配置 EnableTRST=false + PreScriptFile 挂 TXW init 脚本 + ICECLk 12MHz——自写 JSON 里 EnableTRST=true 会把调试逻辑一直按在复位，console 永远等不到目标（表现为卡死在 banner 之后）。
- 前提：板上电；不能有 GUI 烧录器开着（独占 CKLink）；烧完 WiFi 需重连（板复位后 AP 重启）。
- 烧录后设备 AP：82X_2313A8 / 密码 12345678 / 设备 IP 192.168.1.1。
## 复用：烧录成功但不启动的诊断法

完整方法与寄存器速查见 `custom/doc/NE102 系统IO与连接关系.md` §10（CKLink 全片读回 → 停核看 PC → 手动建 XIP 重映射窗口引导固件 → 定位 ROM 引导参数）。2026-09-30 用此法定案 NE102 冷启动问题（`SPI_CLK_MHZ 3c→1c`）。

## 变更记录

| 日期 | 摘要 |
|------|------|
| 2026-09-30 | 初始创建；当晚定案 NE102 冷启动根因；会话性脚本（一次性捕获/复位实验）已清理 |
