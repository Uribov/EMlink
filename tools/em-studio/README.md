# EM Studio（D-1）

EMlink 桌面调试台第一版：串口枚举、多设备标签页、连接/断开、诊断台雏形。

## 运行

Linux / macOS：

```bash
tools/em-studio/.venv/bin/python tools/em-studio/app.py
```

Windows：

```bat
tools\em-studio\.venv\Scripts\python.exe tools\em-studio\app.py
```

## 说明

- 串口默认 `115200`，连接时独占端口；被占用会明确提示。
- 每个设备一个标签页，可同时连接多块板；标签页右上角 `×` = 断开并移除。
- 诊断台用于看 `status` / `stats` 和敲 `help` / `get` / `ver`；命令串行发送。
- 本版只有「配置」模式；「解码」按钮为 D-3 预留，当前禁用。

venv 需以 `--system-site-packages` 创建，已包含 `gi`、`pyserial`、`pymavlink`、`pywebview`。
