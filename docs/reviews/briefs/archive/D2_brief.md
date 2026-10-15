# EMlink 任务书 D2：中间层不再反向依赖 `app/`（角色状态下移到基础支撑）

## 背景（为什么改）
`tools/check_deps.py` 报出两处硬规则违规 —— **中间层 include 了应用层**：
- `firmware/src/link/adr.cpp:10` → `#include "../app/role.h"`
- `firmware/src/service/ota.cpp:14` → `#include "../app/role.h"`

它们要的其实只有两样：
1. **角色常量**（`ROLE_UNSET/AIR/GCS/RELAY`）—— **已经在基础支撑 `firmware/src/service/nvs_cfg.h:10`** 里（`enum : uint8_t { ... }`），这一项不用动；
2. **`role_current()`（读"本机当前角色"）**—— 它的**状态**目前放在应用层 `app/role.cpp`，这才是把中间层拖向应用层的唯一原因。

按用户拍板的三层理念（**应用 / 中间 / 驱动** + 横切"基础支撑"），修法：**把"角色状态"下移到基础支撑**；应用层保留"决定角色"的策略（读 NVS、选 sysid），中间层只"读角色"。

## 允许改动的文件（其它一律不碰）
1. **新建** `firmware/src/service/role_state.h`
2. **新建** `firmware/src/service/role_state.cpp`
3. `firmware/src/app/role.cpp`
4. `firmware/src/link/adr.cpp`
5. `firmware/src/service/ota.cpp`
6. `tools/check_deps.py`（**只**在"基础支撑"清单里加上 `service/role_state.*`）
不要改任何 `.md`；不要运行 pio/编译；**允许且要求**运行 `python3 tools/check_deps.py` 做自检；不要 git commit/add。

## 1. 新建 `service/role_state.h`
```c
#pragma once
#include <Arduino.h>
// ============================================================
// role_state —— 本机角色的唯一真源（基础支撑，跨层共享）
// ============================================================
// 为什么单独放这里：中间层（link/adr、service/ota）需要知道"本机是不是 GCS"，
// 但按分层理念，中间层不允许依赖应用层（app/role）。角色值是各层的共同词汇，
// 所以把"当前角色"这个状态放在基础支撑里：
//   应用层（app/role）= 决定角色（读 NVS、按角色选 MAVLink sysid）
//   中间层            = 只读角色
// 角色常量 ROLE_* 定义在 service/nvs_cfg.h。
uint8_t role_state_get(void);          // 读当前角色（未设置时 = ROLE_UNSET）
void    role_state_set(uint8_t role);  // 由 app/role 的 role_apply() 调用
```

## 2. 新建 `service/role_state.cpp`
`static uint8_t g_role = ROLE_UNSET;` + 上面两个函数（include `role_state.h` 与 `nvs_cfg.h` 取 `ROLE_UNSET`）。
加注释说明：只在启动/配置变更时被写、其余多任务只读（无需加锁，与本工程其它状态一致）。

## 3. `app/role.cpp`
- `role_apply()`：把 `g_role = role;` 改成 `role_state_set(role);`，并**删掉本文件的 `static uint8_t g_role`**
- `role_current()` **保留符号**，改为 `return role_state_get();`（这样 `stats`/`main` 等调用点不用动）
- 补 `#include "../service/role_state.h"`

## 4. `link/adr.cpp` 与 `service/ota.cpp`
- 删掉 `#include "../app/role.h"`，改为 `#include "../service/role_state.h"`
- **把 `role_current()` 调用改为 `role_state_get()`**（行为完全一致）
- 如果这两个文件当前是靠 `role.h` 间接拿到 `ROLE_*` 常量的，**显式**加 `#include "../service/nvs_cfg.h"`
- **不要动任何判断逻辑**（`== ROLE_GCS` 之类原样保留）

## 5. `tools/check_deps.py`
在 `is_base_support()` 的正则/清单里加上 `firmware/src/service/role_state.(h|cpp)`。

## 自检（报告里回答）
1. 为什么"角色状态"可以放基础支撑，而"决定角色"（读 NVS、按角色选 sysid）必须留在 `app/role`？
2. `role_current()` 为什么不直接删掉、而保留为 `role_state_get()` 的包装？
3. **真跑** `python3 tools/check_deps.py` 并贴回输出：硬规则违规应从 2 处变成几处？（期望 **0**）

## 完成后报告（三件套）
1. 改动摘要（逐文件：原样 → 新样子）
2. 自检结论（含真实运行输出）
3. 本次改动不能保证什么
