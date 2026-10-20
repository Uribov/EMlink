# EM Studio

EMlink 桌面调试台：串口参数配置、分层状态台、MAVLink 强行编解码、USB 烧录、会话日志导出。

## 运行

方式一：VSCode（推荐）

VSCode 打开仓库根目录 → `Run and Debug` → 选 `EM Studio` → 按 <kbd>F5</kbd>。

配置在 `.vscode/launch.json`（已被 `.gitignore` 忽略），使用 `tools/em-studio/.venv`。

方式二：命令行

```bash
cd tools/em-studio
.venv/bin/python app.py
```

Windows：

```bat
tools\em-studio\.venv\Scripts\python.exe tools\em-studio\app.py
```

> 从 Hermes 等注入了 `LD_LIBRARY_PATH` 的环境启动时，前面要加 `env -u LD_LIBRARY_PATH`，否则 WebKitGTK 会加载错的 `libstdc++` 直接崩溃。

两种方式等价，F5 的好处是能断点调试。

## 功能

| 区域 | 能力 |
|---|---|
| 顶栏 | 串口枚举/刷新/连接；配置 ↔ 解码模式切换；「允许外部释放串口」开关 |
| 设备信息 | 端口、描述、序列号、VID:PID、连接状态 |
| 固件烧录 | 选 `.bin` → 进度条 → 自动断开串口再重连；整片烧录需合并镜像 |
| 参数配置 | 角色、节点号、频率（三位小数下拉）、SF/BW（带配对校验）、CR、功率、维护延时、加密密钥 |
| 状态台 | 每 2 秒静默读 `status`，按驱动/中间/应用分层显示为状态卡；每层点标题可折叠（只留标题行 + 状态点，给下方输出区腾位置） |
| 诊断归因 | 表驱动规则，命中才显示一条结论（射频未检测到 / 认证失败 / 协议不一致 / ADR 未对齐等） |
| 诊断台 | 常用命令快捷按钮、命令历史（↑↓）、命令语法提示、输出区 |
| 链路指标 | `ping`/`scan` 输出自动提取 RTT / RSSI / SNR 三格数字 + 采样时间；无数据时整条隐藏 |
| MAVLink | 强行组帧/拆帧（自定义 msgid 用 `crc_extra=0`）、帧表、payload hex 与文本还原；帧表右键可单独导出该帧（JSON/文本）或删除，「全部删除」一键清空 |
| 日志导出 | 会话日志导出为 `.txt` / `.json`（含状态快照与解码消息）；单帧导出只带那一帧、文件名带 msgid |

## 命令行

诊断台底部输入框，回车发送。常用命令也可直接点左侧快捷按钮。

- `↑` / `↓`：翻命令历史（最多 50 条）
- `Esc`：清空输入
- 输入时会显示该命令的用法和说明
- `ping` 按钮只填入名字，需自己补目标节点号
- `reset` 需要连点两次才执行

命令全集以固件为准：`firmware/src/app/cli.md`。

## 说明

- 串口默认 `115200`，连接时独占端口；被占用会明确提示。
- 每个设备一个标签页，可同时连接多块板；标签页右上角 `×` = 断开并移除。
- 外部释放串口：`kill -37 $(cat ~/em-studio.pid)`（`SIGRTMIN+3`）。不能用 `SIGUSR1` —— 那是 WebKit/JSC 的 GC 信号，抢占会让程序崩溃。
- 同一时刻只处理一种流：配置模式走 CLI，解码模式走 MAVLink 帧解析。

venv 需以 `--system-site-packages` 创建，已包含 `gi`、`pyserial`、`pymavlink`、`pywebview`。
