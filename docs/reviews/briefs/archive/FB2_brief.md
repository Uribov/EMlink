# EMlink 任务书 FB2：`tools/flash_batch.sh` 三处改进（FB 的补丁）

## 背景（审查 FB 后发现的三件事）
FB 交付的脚本已可用（`--merge-only` 实测产出 944544 字节镜像）。逐行审查后有三处要改：

1. **合并镜像缺 `boot_app0.bin`**（otadata 区留空）——该文件只在框架包里（仓库之外，OpenCode 读不到），所以 FB 的回退布局只能把它跳过：靠"otadata 空白 → ESP-IDF 默认启动 ota_0"来工作。虽然能开机，但**与 PlatformIO 原生 `pio run -t upload` 写入的内容不一致**（差这一处），排查问题时容易变成干扰项。
   → **已由我把标准文件复制到仓库内 `tools/boot_app0.bin`**（8KB 静态文件），本任务让脚本回退布局里优先用它，写到 `0xe000`。
2. **擦除后串口/桥可能重枚举**：`erase_flash` 结束后板子会硬复位，USB 桥（CP2102/CH34x）可能短暂消失 → 紧接着的 `write_flash` 偶发连接失败。要加"擦除后等 1 秒 + 写入失败重试一次"。
3. **`MERGE_TIMEOUT` 名字与用途不符**：它只影响 merge，却被用在擦除/写入上 → 改名 `CMD_TIMEOUT`，并在帮助里写明"大 flash / 低波特率时可调大"。

## 允许改动的文件（其它一律不碰）
1. `tools/flash_batch.sh`
2. `tools/README.md`（**只**追加/更新用法说明）
不要动 `tools/boot_app0.bin`、`tools/check_deps.py` 以及其它任何文件；不要改任何 .md 之外的其它文档；不要运行 pio/编译（**允许**运行 `bash tools/flash_batch.sh --merge-only --no-build` 自检）；**绝不能真的烧写任何板子**；不要 git commit/add。

## 改法

### 1. 用上仓库内的 `boot_app0.bin`
回退布局（没有 `flash_args` 时）里，`boot_app0.bin` 的查找顺序改为：
1. `$REPO_ROOT/tools/boot_app0.bin`（**优先**，仓库内 vendor 的标准文件）
2. `$BUILD_DIR/boot_app0.bin`（兼容，若将来平台把它产物放到 build 目录）

命令拼接：`0xe000 <路径>`。
⚠ **注意**：`merge_bin` 是在 `$BUILD_DIR` 里执行的（因为 `flash_args` 里的路径是相对 build 目录的），所以 vendor 文件的路径**必须用绝对路径**（`$REPO_ROOT/tools/boot_app0.bin`），否则会按 build 目录去找而失败。
（`flash_args` 存在的那条分支不用改。）

### 2. 擦除后的重试
- `erase_flash` 成功后 `sleep 1`（等 USB 桥回来）
- `write_flash` **失败时重试一次**：打印 `写入失败，1 秒后重试一次……`，`sleep 1` 后再试；两次都失败才记该板失败（失败原因写"写入失败（已重试）"）

### 3. 改名
- `MERGE_TIMEOUT` → `CMD_TIMEOUT`（默认仍 `60`，仍可由环境变量覆盖），帮助文本里加一句：`CMD_TIMEOUT=<秒>  单步（合并/擦除/写入）超时，默认 60；大 flash 或低波特率可调大`

## 自检（报告里回答）
1. 为什么 `boot_app0.bin` 要显式写进 `0xe000`，而不是靠"otadata 空白默认启动 ota_0"？
2. 为什么传给 `merge_bin` 的 vendor 文件路径必须用绝对路径？
3. **真跑** `bash tools/flash_batch.sh --merge-only --no-build` 并贴回输出（路径/大小/sha256）；说明**镜像比上次（944544 字节）变化了多少、多出来的是哪一段**

## 完成后报告（三件套）
1. 改动摘要
2. 自检结论（含真实运行输出）
3. 本次改动不能保证什么（尤其：仍然没在真板上烧过）
