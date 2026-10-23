# custom/tools — 工具脚本

| 工具 | 作用 |
|------|------|
| `burn.bat` | CKLink 烧录 `project/txw82xApp/APP.bin`（CDK 生成的 CSKYFlashProgramerCfg，EnableTRST=false；设备停泊中 CKLink 失联：先 MQTT `wake` 再烧） |
| `cklink_program_verify.bat/.json` | 烧录 + 全量校验 |
| `cklink_read_flash.bat/.json`、`cklink_reset_only.json` | 全片读回 / 仅复位（烧录不启动的诊断法见 `custom/doc/NE102 系统IO与连接关系.md` §10） |
| `serial_cmd.py` | COM7@921600 发 AT + 录日志（`python serial_cmd.py "AT+KAQ?" -w 5`；`-b` 持续到 serial_stop 文件；多条命令不要背靠背连发） |
| `serial_log.py` | 纯串口录制 |
| `mqttka_console.py` | MQTT 控制台（订阅 evt / 交互下发 cmd） |
| `cap_seq_verify.py` | 远程抓拍事件序验证（capture→photo→sleep，0 泄漏判定；带串口监听） |
| `ka_bench.py` | 性能基准：心跳维持 + 多配置抓图时序（`python ka_bench.py <轮数> <skip列表>`，结果 `ka_bench_result.json`） |
| `mqtt_recv_test.py` | NE301 格式收图验证（SOI/EOI 校验落盘） |
| `wake_for_burn.py` | 发 MQTT `wake` 唤醒停泊设备（烧录前置） |
| `run_broker.py` | 本地 amqtt broker（消除公网抖动用） |

- 连接：CKLink → J1（TCK=PA10、TMS=PA9）；板上电；不能有 GUI 烧录器开着（独占 CKLink）。
- 烧录算法：`D:\Program Files\CSKY-FlashProgrammer-windows\FlashProgrammer\TXW82X_FLASH_ALGORITHM.elf`；Console：`D:\C-SKY\CDK\CSKY\FlashProgrammer\Bins\CSKYFlashProgramerConsole.exe -f <json>`。
