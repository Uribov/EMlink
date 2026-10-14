# 开发环境工具清单（Linux 为主）

> 目的：换机器（尤其 Linux）开发时，照着这份清单装环境，避免漏装卡住。
> 关联：`README.md`（编译方法）、`CONCEPTS.md`（概念科普）。

## 1. 手动装一次（3 样基础工具）

| 工具 | 作用 | 安装 |
|---|---|---|
| git | 拉 mavlink 等 git 依赖 | `sudo apt install git` |
| Python 3 | PlatformIO 运行环境 | 系统自带，或 `sudo apt install python3 python3-pip` |
| PlatformIO Core | 编译/烧录/管库 | `pip3 install platformio`，或 VSCode 扩展 |

> VSCode 扩展最省心（图形界面点 Build/Upload）；命令行适合脚本化批量编译。

## 2. PlatformIO 首次编译自动下载（无需手动）

首次 `pio run` 自动下载，存在 `~/.platformio/`（所有项目共享，只下一次）：

| 包 | 版本（实测） | 作用 |
|---|---|---|
| espressif32 平台 | 7.1.3 | ESP32 平台定义 |
| toolchain-xtensa-esp32s3 | 8.4.0 | S3 主编译器（gcc） |
| toolchain-riscv32-esp | 8.4.0 | RISC-V 工具链 |
| tool-esptoolpy | 自动 | 烧录工具（esptool） |

## 3. 项目依赖库（全部本地化到 `lib/`，完全离线编译）

| 库 | 位置 | 版本 | 作用 |
|---|---|---|---|
| RadioLib | lib/radiolib/ | 6.6.0 | LoRa 射频驱动（SX126x/LLCC68） |
| Adafruit NeoPixel | lib/neopixel/ | 1.15.5 | WS2818 RGB LED |
| MAVLink 2 | lib/mavlink2/ | — | MAVLink 2 协议头文件 |

> ⚠️ **mavlink 用 git 拉 GitHub**：校园网/受限网络连不上 GitHub 时，编译会卡在这一步（`UserSideException: Please install Git client`，或 clone 超时）。解决：
> - **方法 A（推荐）**：给 git 配国内加速，之后所有 GitHub clone 自动走代理：
>   ```
>   git config --global url."https://ghproxy.com/https://github.com/".insteadOf "https://github.com/"
>   ```
> - **方法 B**：手动下载 `c_library_v2` 解压到 `firmware/lib/mavlink2/`，并从 `platformio.ini` 删掉 mavlink 的 git 依赖行。

> 依赖下载到项目内 `.pio/libdeps/<env>/`，编译产物在 `.pio/build/<env>/firmware.bin`。

## 4. 编译方式

- 单方案：`pio run -e esp32s3-supermini-e220-900m22s`
- 全部方案：`bash scripts/build_all.sh`
- VSCode：打开 `firmware` 文件夹，点 ✓ Build

## 5. Linux 串口权限（重要，否则烧录失败）

```
sudo usermod -aG dialout $USER
```
执行后**注销重新登录**才生效。可选：加 udev 规则让 ESP32 自动识别。

## 6. 完整从零到能编译

```bash
# 1. 基础工具
sudo apt install git python3 python3-pip

# 2. PlatformIO
pip3 install platformio          # 或装 VSCode 扩展

# 3. 串口权限
sudo usermod -aG dialout $USER   # 然后注销重登

# 4. 校园网连不上 GitHub 时，先配 git 加速（见 §3 方法 A）
git config --global url."https://ghproxy.com/https://github.com/".insteadOf "https://github.com/"

# 5. 拉项目 + 编译
git clone <你的仓库>
cd EMlink/firmware
pio run -e esp32s3-supermini-e220-900m22s   # 首次下载平台+工具链+依赖，5~15 分钟
```
