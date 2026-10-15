# 开发环境工具清单（Linux 为主）

> 目的：换机器（尤其 Linux）开发时，照着这份清单装环境，避免漏装卡住。
> 关联：`README.md`（编译方法）、`CONCEPTS.md`（概念科普）、`../../docs/BRINGUP.md`（上板操作卡）。

## 1. 手动装一次（3 样基础工具）

| 工具 | 作用 | 安装 |
|---|---|---|
| git | 版本管理 | `sudo apt install git` |
| Python 3 | PlatformIO 运行环境 | 系统自带，或 `sudo apt install python3 python3-pip` |
| PlatformIO Core | 编译/烧录/管库 | **`pipx install platformio`**（或装 VSCode 扩展） |

> ⚠️ **别用 `pip3 install platformio`**：Linux Mint 22.3 / Ubuntu 24.04 是 **PEP 668** 环境，系统 pip 会直接拒绝（`error: externally-managed-environment`）。用 **pipx**（独立虚拟环境，不污染系统 Python），装完 `pio` 在 `~/.local/bin/`。
> VSCode 扩展最省心（图形界面点 Build/Upload）；命令行适合脚本化批量编译。

## 2. PlatformIO 首次编译自动下载（无需手动）

首次 `pio run` 自动下载，存在 `~/.platformio/`（所有项目共享，只下一次）：

| 包 | 版本（实测） | 作用 |
|---|---|---|
| espressif32 平台 | 7.1.3 | ESP32 平台定义 |
| framework-arduinoespressif32 | 2.0.17（IDF 4.4.7） | Arduino 核心 |
| toolchain-xtensa-esp32s3 | 8.4.0 | S3 主编译器（gcc） |
| toolchain-riscv32-esp | 8.4.0 | RISC-V 工具链 |
| tool-esptoolpy | 自动（实测 4.11.0） | 烧录工具（esptool） |

## 3. 项目依赖库（全部本地化到 `lib/`，**完全离线编译**）

| 库 | 位置 | 版本 | 作用 |
|---|---|---|---|
| RadioLib | `lib/radiolib/` | 6.6.0 | LoRa 射频驱动（**本板用 `SX1262` 类**，芯片是 LLCC68） |
| Adafruit NeoPixel | `lib/neopixel/` | 1.15.5 | WS2818 RGB LED |
| MAVLink 2 | `lib/mavlink2/` | c_library_v2 | MAVLink 2 协议头文件 |

> ✅ **不依赖 git clone 拉库**：三个库都已在仓库里，编译时**不需要联网**。
> （历史提示：早期版本用 git 依赖拉 mavlink，文档里推荐过 `ghproxy.com` 加速 —— **该加速站 2026 年已停服，不要再用**；git 依赖也早已移除。）
> 编译产物在 `.pio/build/<env>/firmware.bin`；**合并镜像（含 bootloader/分区表/otadata/固件）由 `../../tools/flash_batch.sh` 生成**，不要手工拼。

## 4. 编译方式

- 单方案：`pio run -e esp32s3-supermini-e220-900m22s`
- 全部方案：`bash scripts/build_all.sh`
- VSCode：打开 `firmware` 文件夹，点 ✓ Build
- **IDE 报"无法打开源文件 XXX.h"不是编译错误**：是 IntelliSense 索引旧了 → `pio run -t compiledb` 重生成 `compile_commands.json`，再 Reload Window。

## 5. Linux 串口权限（重要，否则烧录失败）

```
sudo usermod -aG dialout $USER
```
执行后**注销重新登录**才生效。可选：加 udev 规则让 ESP32 自动识别。

## 6. 完整从零到能编译

```bash
# 1. 基础工具
sudo apt install git python3 python3-pip

# 2. PlatformIO（PEP 668 环境必须用 pipx）
pipx install platformio            # 或装 VSCode 扩展

# 3. 串口权限
sudo usermod -aG dialout $USER     # 然后注销重登

# 4. 拉项目 + 编译（依赖库已在仓库内，无需联网下库）
git clone <你的仓库> EMlink
cd EMlink/firmware
pio run -e esp32s3-supermini-e220-900m22s   # 首次会下载平台+工具链，5~15 分钟
```

## 7. 推送代码（校园网必读）

校园网会把 **github.com 的 HTTPS（443）** 掐断（表现为 `TLS connection non-properly terminated` 或端口 443 超时），**改用 SSH 通道**：

```bash
git remote -v                      # 确认是 git@github.com:Uribov/EMlink.git
git push origin main               # 走 22 端口，实测稳定（26MB 约 14 秒）
```

若 22 端口也被封，可改用 SSH over 443（`ssh.github.com:443`）。**不要**再配 `ghproxy.com` 之类的 HTTPS 加速。
