# EMlink 任务书 SF1：SF 与带宽必须配对（LLCC68 硬约束）

## 背景（为什么改）

本机射频模块 E220-900M22S 的芯片是 **LLCC68**，它对"扩频因子 SF 与带宽 BW 的组合"有硬限制：

| 带宽 | 允许的 SF |
|---|---|
| **125 kHz** | SF5/6/**7/8/9**（最高 9） |
| 250 kHz | SF5…**10** |
| 500 kHz | SF5…**11** |

出处（三处独立官方来源一致）：亿佰特《E220-900M22S 产品规格书》第 7 页原文
"LoRa®，SF = 5 - 6 - 7 - 8 - 9 for BW = 125 kHz"；Semtech 数据手册 Table 6-1 脚注
"Not all SF are available for any bandwidth with the LLCC68"；Zephyr binding 文档同表。
完整依据：`docs/references/LLCC68_E220-900M22S.md`。

两个后果：
1. **`adr` 模块的慢速窗用的是 SF10 @ BW125** → 非法组合，而所有非 GCS 节点**开机就停在这一档**等 BEACON，
   两台机器的"最小互通验证"会在这里哑掉。
2. **配置面没有联动校验**：网页 `/set`、CLI `set`、远程 FRAME_CFG 都能把 `bw=125 + sf=10` 存进 NVS，
   重启后射频拿着非法组合工作。必须堵住。

## 允许改动的文件（其它一律不动）

- `firmware/src/link/adr.h`
- `firmware/src/link/adr.cpp`
- `firmware/src/service/nvs_cfg.h`
- `firmware/src/service/nvs_cfg.cpp`
- `firmware/src/service/ota.cpp`
- `firmware/src/app/cli.cpp`

## 逐条改法

### 1. `adr.h`：慢速窗 SF 10 → 9

```cpp
#define ADR_SF_SLOW     10     // 慢速保链接窗的 SF（BEACON 也用这个）
```
改为：
```cpp
#define ADR_SF_SLOW     9      // 慢速保链接窗的 SF（BEACON 也用这个）。
                               // 不能设为 10：本板芯片 LLCC68 在 125kHz 带宽下最高只支持 SF9
                               // （见 docs/references/LLCC68_E220-900M22S.md §2）
```
同文件第 24 行注释里的"先停在 SF10"改为"先停在 SF9"。
**注意**：`ADR_SF_FAST`（7）、`ADR_PERIOD_MS`、`ADR_SLOW_MS`、`ADR_GUARD_MS`、`ADR_COMPAT_SF` 等**一律不动**（SF7 与 SF9 在 125/250/500kHz 下都合法）。

### 2. `adr.cpp`：同步注释里的 SF10 → SF9

- 第 4 行注释"0~1s SF10（慢速窗）"→"0~1s SF9（慢速窗）"
- 第 42 行注释"开机先停 SF10"→"开机先停 SF9"
- 第 92 行注释"提前回 SF10 等下一个 BEACON"→"提前回 SF9 等下一个 BEACON"
**代码逻辑一行都不要改**（只用常量 `ADR_SF_SLOW`，改常量即可）。

### 3. `nvs_cfg.h/.cpp`：新增"配对"判定（供各处复用）

在 `nvs_cfg.h` 声明、`nvs_cfg.cpp` 实现（放在 `nvs_cfg_value_ok` 附近）：

```cpp
// LLCC68 硬约束：SF 与带宽必须配对（BW125→SF≤9、BW250→≤10、BW500→≤11）
// 返回该带宽允许的最大 SF；带宽不认识时返回 0。
uint8_t nvs_cfg_sf_max_for_bw(uint8_t bw);

// sf/bw 组合是否合法（bw 不在 125/250/500 时返回 false）
bool nvs_cfg_pair_ok(uint8_t sf, uint8_t bw);
```

实现要点：`125→9`、`250→10`、`500→11`、其余 `→0`；`pair_ok` = `sf >= 7 && sf <= nvs_cfg_sf_max_for_bw(bw)`（`sf_max_for_bw` 返回 0 时自然为 false）。

### 4. `nvs_cfg.cpp` `nvs_cfg_set()`：单键写入前做跨键配对检查

现在 `sf`/`bw` 是**直接写 NVS 单个键**，所以要读"另一个键"的当前值来配：

- 当 `key == "sf"`：从 NVS 读当前 `bw`（`Preferences::getUChar("bw", 125)`），若 `!nvs_cfg_pair_ok(新sf, 当前bw)` → 返回 `false`
- 当 `key == "bw"`：从 NVS 读当前 `sf`（`getUChar("sf", 7)`），若 `!nvs_cfg_pair_ok(当前sf, 新bw)` → 返回 `false`

放在函数开头 `nvs_cfg_value_ok()` 校验之后、写 NVS 之前。**不要**改变其它键的行为。

### 5. `nvs_cfg.cpp` `nvs_cfg_save()`：整表保存前做配对检查

`nvs_cfg_save(&c)` 开头加：
```cpp
if (!nvs_cfg_pair_ok(in->sf, in->bw)) return false;
```
（网页 `/set` 和远程 FRAME_CFG 都经这里落盘。）

### 6. `nvs_cfg.cpp` `nvs_cfg_load()`：老配置的兜底

读到非法组合时（例如旧固件已把 `bw=125&sf=10` 写进 NVS），**只改内存里的值 + 打一条警告，不写回 NVS**：

```cpp
uint8_t sf_max = nvs_cfg_sf_max_for_bw(out->bw);
if (sf_max != 0 && out->sf > sf_max) {
    Serial.printf("[cfg] NVS 里的 SF%d 与 BW%dkHz 组合非法（LLCC68 上限 SF%d），已临时降到 SF%d\n",
                  out->sf, out->bw, sf_max, sf_max);
    out->sf = sf_max;
}
```
放在 `out->uart_baud = ...` 之后、函数返回之前。（`Serial` 在本文件已可用；不要新增 include。）

### 7. `ota.cpp` `handle_set()`：网页给出**具体**报错

在 `if (bad) {...}` 之后、`nvs_cfg_save(&c)` 之前插入：
```cpp
if (!nvs_cfg_pair_ok(c.sf, c.bw)) {
    send_done(s, true, "SF 与带宽不匹配：本机射频芯片 LLCC68 要求 BW125→SF≤9、BW250→≤10、BW500→≤11。");
    return;
}
```

### 8. `cli.cpp` `"set"` 分支：失败时给提示

现在失败只打印"设置失败：键名未知或值超出范围"。改成：当 `arg1` 是 `sf` 或 `bw` 时追加一行配对提示：

```cpp
} else if (strcmp(cmd, "set") == 0) {
    if (n >= 3 && nvs_cfg_set(arg1, arg2)) {
        cli_print("已设置（save 后生效）\r\n");
    } else {
        cli_print("设置失败：键名未知或值超出范围（用 get 查看当前值）\r\n");
        if (strcmp(arg1, "sf") == 0 || strcmp(arg1, "bw") == 0) {
            cli_print("提示：本机射频 LLCC68 要求 SF 与带宽配对 —— BW125→SF≤9、BW250→≤10、BW500→≤11\r\n");
        }
    }
```
（`atoi`/`strcmp` 都已可用，不要新增 include。）

## 自检（报告里给结论）

1. 为什么"配对检查"要放在 `nvs_cfg_set()` 和 `nvs_cfg_save()` **两处**，而不是只放在网页的处理函数里？
2. 老设备 NVS 里已经存着 `bw=125 & sf=10`，开机后会怎样？（为什么选择"只改内存并警告"而不是"写回 NVS"）
3. 新板子首次开机会不会因为这条规则被误拦？（默认 sf/bw 是多少）
4. ADR 运行时切的 SF（7 与 9）在 125/250/500kHz 三种带宽下是否都合法？
5. 这次改动**不能保证**什么？

## 完成后报告

改动摘要（逐条对应上面的编号）+ 自检结论 + 本次改动不能保证哪些性质。
**禁止**：改动白名单以外的文件、编辑任何 `.md`、运行 `pio`/编译、`git add/commit`。
只在仓库目录内活动（不要读 `/tmp`、`~/.platformio` 等仓库外路径）。
