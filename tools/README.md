# tools/check_deps.py — 三层依赖体检

怎么跑：在仓库任意目录执行 `python3 tools/check_deps.py`（脚本从自身位置推导仓库根）；加 `--strict` 时若有违规退出码为 1，留给未来 CI，现阶段默认只报告、退出码恒为 0，不阻塞任何人。

怎么读：输出分三组。`[硬规则] 反向依赖` 是「中间层不得 include app / 驱动层不得 include 中间层（基础支撑白名单除外）」的越界，属一次能清零、修了就干净的硬约束；`[越层动作] … 非白名单文件` 是 `frame_encode/crypto_seal_frame/rtos_tx_enqueue/radio_transmit` 等中间件内部动作出现在了不该出现的文件里，用棘轮方式防止继续恶化；`[越层动作] 白名单内 · 待收敛` 是已登记在脚本 `WHITELIST` 中、但标记为「待收敛」的文件命中行（当前是 `service/mav_router.cpp`），未来把封装动作收进中间层出口后即可删掉该白名单项。末行给出统计；三层全部干净时打印 `✅ 分层干净`。白名单刻意写在脚本里并各带一行理由，与代码同仓库、同评审、同 diff，避免外部配置文件漂移。

# tools/flash_batch.sh — 批量烧录脚本

板子到货后要一次刷多块板用的。它把「合并镜像 → 逐块整片擦除 + 写入（带校验）→ 结果表」串起来，避免手敲 esptool 漏掉**整片擦除**这一步（分区表已改成 OTA 双分区 `partitions_ota.csv`，不擦会起不来）。

怎么跑：先 `unset LD_LIBRARY_PATH`（本机 Hermes 会把旧 libstdc++ 塞进该变量，导致平台工具报 GLIBCXX 错误；脚本内已自动 `unset`）。

```bash
bash tools/flash_batch.sh --port /dev/ttyUSB0          # 单块板
bash tools/flash_batch.sh --all                        # 自动找 /dev/ttyUSB* /dev/ttyACM*
bash tools/flash_batch.sh --all -e <env> --no-build    # 指定 PlatformIO env / 跳过构建
bash tools/flash_batch.sh --merge-only                 # 只生成合并镜像并打印 sha256（无板也能验证）
```

默认先 `cd firmware && pio run -e <env>` 构建（默认 env `esp32s3-supermini-e220-900m22s`，`--no-build` 跳过），再用 esptool 的 `merge_bin` 把 `firmware/.pio/build/<env>/flash_args` 描述的镜像合成 `firmware/.pio/build/<env>/emlink_factory_<env>.bin`；本平台不生成 `flash_args` 时按标准 ESP32-S3 布局（bootloader@0x0 / partitions@0x8000 / boot_app0@0xe000 / firmware@0x10000）合成。其中 `boot_app0.bin` 优先用仓库内 vendor 的标准文件 `tools/boot_app0.bin`（缺失时才回退 build 目录内的同名产物），显式写入 otadata 区，从而与 PlatformIO 原生 `pio run -t upload` 写入的内容一致，不依赖「otadata 空白 → 默认启动 ota_0」。其余产物只从 build 目录内取。

`--merge-only` 到这一步结束，打印镜像路径 / 大小 / sha256，不碰任何板子。烧写路径上，每块板会先 `erase_flash` 整片擦除，成功后等 1 秒（`erase_flash` 会硬复位，USB 桥 CP2102/CH34x 可能短暂重枚举）再 `write_flash 0x0`（带校验）；写入失败会自动 `sleep 1` 重试一次，两次都失败才记为「写入失败（已重试）」。`--all` 扫描不到串口会明确报错；既不 `--port` 也不 `--all` 也不 `--merge-only` 时打印用法并退出码 2；某一块板失败不中断其它板，最终退出码反映「有没有失败」。

环境变量：`ESPTOOL=<命令>` 指定 esptool（默认 PATH 里的 `esptool`，找不到时回退 `python3 -m esptool`）；`BAUD=<波特率>` 默认 `460800`；`CMD_TIMEOUT=<秒>` 是**单步**（合并 / 擦除 / 写入）的超时，默认 `60`，大 flash 或低波特率可调大。
