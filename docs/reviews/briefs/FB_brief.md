# EMlink 任务书 FB：`tools/flash_batch.sh` 批量烧录脚本

## 背景（为什么做）
板子到货后要一次刷多块板；手敲 esptool 容易漏步骤，尤其**整片擦除**这一步 —— 分区表改成了 OTA 双分区（`partitions_ota.csv`），不擦会起不来或行为怪异。
需要一个脚本：**合并镜像 → 逐块板整片擦除 + 烧写 + 校验 → 打一张结果表**。

**注意：现在没有板子。** 所以本任务的验证只允许走"生成合并镜像 + 校验镜像"这条路径；**不许真的去烧任何板子**（没给 `--port/--all` 时应友好报错退出）。

## 允许改动的文件（其它一律不碰）
1. **新建** `tools/flash_batch.sh`
2. `tools/README.md`（**只**追加一段用法说明；其它 `.md` 一律不碰）
允许运行：`pio run -e esp32s3-supermini-e220-900m22s`（只构建，**不许改任何源码**）、`bash tools/flash_batch.sh --merge-only`、esptool 的 `merge_bin`（只生成镜像，不写板）。不要 git commit/add。

## 脚本要求

### 基本
- 路径 `tools/flash_batch.sh`，`set -euo pipefail`
- **开头 `unset LD_LIBRARY_PATH`**（本机环境坑：Hermes 会把自己的旧 libstdc++ 塞进 `LD_LIBRARY_PATH`，导致平台工具报 GLIBCXX 错误）
- 中文输出；不引入新依赖（只用 bash + python3 + esptool + pio）

### 用法
```
bash tools/flash_batch.sh --port /dev/ttyUSB0          # 单块板
bash tools/flash_batch.sh --all                        # 自动找 /dev/ttyUSB* /dev/ttyACM*
bash tools/flash_batch.sh --all -e <env> --no-build     # 指定 PlatformIO env / 跳过构建
bash tools/flash_batch.sh --merge-only                  # 只生成合并镜像并打印 sha256（无板也能验证）
```

### 流程
1. 默认先构建：`cd firmware && pio run -e <env>`（`-e` 默认 `esp32s3-supermini-e220-900m22s`；`--no-build` 跳过）
2. **生成合并镜像**：读 `firmware/.pio/build/<env>/flash_args`，用 esptool 的 `merge_bin` 合成完整镜像
   - esptool **一律用 PATH 里的 `esptool`**（本机已确认 `/usr/bin/esptool` 可用）：`ESPTOOL="${ESPTOOL:-esptool}"` + `command -v` 检查；找不到时回退 `python3 -m esptool`
   - ⚠ **绝对不要去读/探测仓库之外的路径**（例如 `~/.platformio/packages/...`、`/usr/lib/...`）——OpenCode 对工作目录以外的读取会被**自动拒绝**，任务会当场失败（本任务第一次派发就是这么挂的）
   - 输出：`firmware/.pio/build/<env>/emlink_factory_<env>.bin`
   - 注意 `flash_args` 里的文件路径是相对 build 目录的（在 build 目录里执行）
3. `--merge-only` 到这一步就结束：打印 **镜像路径 / 大小 / sha256**
4. 逐块板：
   - 打印明确警告：`即将擦除并烧写 <port>，5 秒后开始（Ctrl-C 取消）`（`--yes` 跳过等待）
   - 检测芯片（`chip_id` 或 `flash_id`）
   - **整片擦除**（`erase_flash`）
   - 写入（`write_flash 0x0 <merged.bin>`，带校验）
   - 每块板一行结果：端口 / 芯片 / 结果 / 耗时
5. 结束打**汇总表**；某一块失败**不中断其它板**（记录失败继续），最终退出码反映"有没有失败"

### 健壮性
- `--all` 找不到任何串口时给明确提示（而不是静默成功）
- 参数非法（既不 `--port` 也不 `--all` 也不 `--merge-only`）时打印用法并退出码 2
- 端口不存在时明确报错并继续下一块

## 自检（报告里回答）
1. 为什么必须整片擦除？（结合分区表变更回答）
2. 用 `merge_bin` 相比"分三次写 bootloader / partitions / app"好在哪？（提示：偏移写错、版本不匹配的风险）
3. **真跑** `bash tools/flash_batch.sh --merge-only` 并贴回输出（镜像路径 / 大小 / sha256）；再说清：这个验证覆盖了什么、**没覆盖**什么

## 完成后报告（三件套）
1. 改动摘要（脚本结构、行数、用法）
2. 自检结论（含真实运行输出）
3. 本次改动不能保证什么（诚实列出，尤其"没在真板上烧过"）
