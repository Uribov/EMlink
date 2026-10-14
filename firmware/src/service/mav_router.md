# mav_router —— MAVLink 2 解析与路由

## 职责
系统的「翻译/搬运工」：把 MAVLink 字节流切成一条条消息，按目标系统标识（`target_sysid`）决定送去哪。
**只支持 MAVLink 2**（STX=0xFD），不支持 v1。

## 依赖
- MAVLink 2 官方 C 库（`mavlink/c_library_v2`，头文件库，git 引入，`#include <mavlink/common/mavlink.h>`）
- `frag.h`（超长消息拆片）、`addr_map.h`（sysid↔node 映射）

## MAVLink 2 帧（供解析）
```
0xFD | LEN | incompat_flags | compat_flags | SEQ | SYSID | COMPID | MSGID(3B,24位小端) | payload(LEN) | CKA | CKB
```
- 帧头 10 字节，MSGID 3 字节，最小帧 12 字节
- 状态机找 `0xFD` → 读 LEN → 收齐 `10 + LEN + 2` → 校验 CKA/CKB（CRC-16/MCRF4XX，含 CRC extra）
- `incompat_flags` 若置签名位（bit0），帧尾多 13 字节签名——本项目关闭签名，遇签名帧可丢弃
- `MSGID==0`（HEARTBEAT）最高优先级：**优先、及时**中转，绝不滞留（架构 §8.2）

## 路由规则（关键：看 target_sysid，不是帧头 sysid）
- **帧头 SYSID 是「发送者」**，不是目标，不能拿它做路由
- **目标在 payload 里的 `target_sysid` 字段**（命令/请求类消息才有，位置随 MSGID 变，按 mavlink 库消息结构读）
- GCS→多机：GCS 广播；AIR 节点看 `target_sysid` 是否匹配自己飞控，匹配才送 UART，否则丢
- AIR→GCS：反向路径单播回 GCS；GCS 合并各机数据上 USB，QGC 靠 sysid 自行区分
- 收到 HEARTBEAT 时调 `addr_map_learn(帧头SYSID, node_id)` 维护对照表

## 心跳中转（保命，架构 §8.2 第 1 条）
GCS HEARTBEAT 投 `q_mac_tx` 时设 **QOS2 最高优先级**，飞控自己数心跳超时→返航，固件只保证不误时。

## 对外接口
见 `mav_router.h`。

## 验收（P3）
- 地面站同时看到 2 架飞机（APM 或 PX4，均配 MAVLink 2）；能读参数、传航点、收遥测
- 掐断上行后，GCS HEARTBEAT 仍在 1 帧内被中转（无滞留）
