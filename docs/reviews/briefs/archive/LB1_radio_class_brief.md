# EMlink 任务书 LB1：射频驱动类 `SX1268` → `SX1262`

## 背景（为什么改）

本机射频模块 E220-900M22S 的芯片是 **LLCC68**（亿佰特规格书 + Semtech 手册确认）。
RadioLib 在 `begin()` 时会读芯片的版本寄存器，把"芯片自报的名字"与驱动类声明的名字**逐字符比对**
（`firmware/lib/radiolib/src/modules/SX126x/SX126x.cpp:2169`：`strncmp(verStr, version, 6) == 0`），
对不上就返回 `RADIOLIB_ERR_CHIP_NOT_FOUND`（= **-2**，`TypeDef.h:118`），射频根本起不来。

各驱动类要求的名字（同一目录头文件）：
- `SX1268` 类要求 `"SX1268"`（`SX1268.h:15`）
- `SX1262` 类要求 `"SX1261"`（`SX1262.h:15-16`，注释原文：*"it seems that all SX1262 devices report as SX1261"*）
- `LLCC68` 类要求 `"LLCC68"`（`LLCC68.h:12`）

而 **E220-900M22S 实测自报 `"SX1261 V2D 2D02"`**（RadioLib issue #1329，用的就是这块模块）；
该 issue 里 `LLCC68` 类初始化失败、**`SX1262` 类可正常收发**。所以现在代码里的 `SX1268` 类**大概率直接 -2**，
表现出来就是：`status` 显示射频未就绪、LED 红、每 10 秒重试。

## 允许改动的文件（其它一律不动）

- `firmware/src/drivers/radio.cpp`（**仅此一个文件**）

## 逐条改法

1. 文件头注释（约第 3~5 行，现在是）：
   ```
   // 上层只认 radio_*，不直接碰 RadioLib。芯片按宏分叉：
   //   RADIO_SX126X：SX1261/2/8 + LLCC68
   ```
   改成（保留原意，补上"为什么用 SX1262 类"）：
   ```
   // 上层只认 radio_*，不直接碰 RadioLib。芯片按宏分叉：
   //   RADIO_SX126X：SX1261/2/8 + LLCC68
   //
   // 本板模块 = E220-900M22S = LLCC68 die，用 RadioLib 的 `SX1262` 类驱动。原因：
   //   RadioLib 的 begin() 会拿"芯片自报型号"和驱动类声明的名字逐字符比对
   //   （lib/radiolib/src/modules/SX126x/SX126x.cpp:2169），对不上直接返回 -2。
   //   SX1268 类要求 "SX1268"、SX1262 类要求 "SX1261"（SX1262.h:15 注释：
   //   "all SX1262 devices report as SX1261"），而 E220 实测自报 "SX1261"（RadioLib issue #1329）。
   // 若上板仍报 -2：依次把下面的 SX1262 换成 LLCC68、再换回 SX1268 试（三者同一套 SPI 命令，
   //   切换成本 = 改这一处 + 重编译）。依据见 docs/references/LLCC68_E220-900M22S.md §3。
   ```

2. 第 12 行：
   ```cpp
   static SX1268* g_radio = nullptr;
   ```
   改为：
   ```cpp
   static SX1262* g_radio = nullptr;
   ```

3. 第 74 行：
   ```cpp
   g_radio = new SX1268(g_mod);
   ```
   改为：
   ```cpp
   g_radio = new SX1262(g_mod);
   ```

**除以上四处（一段注释 + 两行代码）外，不要改任何其它内容**：
- 不要改 `begin(...)` 的参数（`SX1262::begin` 与 `SX1268::begin` 签名完全一致，参数序不用动）
- 不要改 `setRfSwitchPins` / `setCurrentLimit` / `transmit` / `startReceive` 等任何调用
- 不要动 `radio.h`、`pinmap.h`、`platformio.ini`

## 自检（报告里给结论）

1. 为什么用 `SX1262` 类而不是 `LLCC68` 类？（提示：两者都要求芯片自报特定名字，而这块模块自报什么）
2. 如果驱动类选错，上板会看到什么现象、`status` 里体现为哪个数字？
3. `SX1262::begin()` 与 `SX1268::begin()` 的参数表有何不同？（需要改调用处吗）
4. 这次改动**不能保证**什么？（诚实列出，例如：不能保证这颗芯片一定自报 SX1261、不能保证 TX 功率档位读数准确）

## 完成后报告

改动摘要（逐条对应上面的编号）+ 自检结论 + 本次改动不能保证哪些性质。
**禁止**：改动白名单以外的文件、编辑任何 `.md`、运行 `pio`/编译、`git add/commit`。
只在仓库目录内活动（不要读 `/tmp`、`~/.platformio` 等仓库外路径）。
