# frame_codec —— Mesh 帧编解码

## 职责
内存帧结构 ↔ 线上字节流互转，附 CRC16 与序号。全系统所有空口数据都经它打包/解包。

## 依赖
- 无硬件依赖（纯逻辑，可在 PC 单测）
- 被 `mac`（发包）、`mesh`（转发）、`frag`（分片头）使用

## 帧格式（严格对齐架构 §4.1，MTU=200）

| 偏移 | 字段 | 字节 | 说明 |
|---|---|---|---|
| 0 | `ver:type` | 1 | 高4b版本=1；低4b类型 |
| 1 | `flags` | 1 | `qos[7:6]` `ack_req[5]` `frag[4]` `proto[3:2]` `enc[0]` |
| 2 | `src_id` | 1 | 源节点地址 |
| 3 | `dst_id` | 1 | 目的节点地址，0=广播 |
| 4–5 | `seq` | 2 | 源序号（小端，回绕） |
| 6 | `ttl` | 1 | 剩余跳数 |
| 7 | `relay_id` | 1 | 广播帧=上一跳转发者；单播帧=下一跳；源发=0 |
| 8 | `orig_sysid` | 1 | 源 MAVLink sysid |
| 9 | `net_id` | 1 | 网络 ID（多网共存，0=默认网） |
| 10–13 | `frag_hdr` | 4 | 分片头（§frag），非分片填 0 |
| 14–192 | `payload` | ≤179 | 数据（加密帧含 4B tag，明文上限 175） |
| 193–194 | `crc16` | 2 | CRC16-CCITT（header+payload） |

## 协议类型（proto，flags[3:2]）
- 0 = MAVLink（默认）
- 1 = CRSF
- 2 = MSP
- 3 = 自定义

## 关键实现点
1. 多字节字段**小端**；`ver`/`type` 打包为 `(ver<<4)|type`
2. CRC16-CCITT（poly 0x1021，init 0xFFFF），覆盖偏移 0~192
3. 拆帧先查长度 ≥ `FRAME_HDR_LEN+FRAME_CRC_LEN`（15），再验 CRC，不过丢弃计 `dropped_crc`
4. 序号 `seq` 发送方自增（每源独立），供去重键 `(src_id, seq)`
5. `net_id` 默认 0；非 0 时只收同 `net_id` 的帧（多网隔离）

## 对外接口
见 `frame_codec.h`。

## 验收（可在 PC 单测）
- 编解码往返：任意 payload 长度 0~179 无损还原
- CRC 篡改 1 字节 → `frame_decode` 返回 false
- `seq=0x0102` 编出字节 `02 01`；`proto`/`net_id`/`enc` 正确读写

## 当前实现架构（已实现，供子 agent 分析）

### 线上布局（MTU 200，含 relay_id 扩展）
| 偏移 | 字段 |
|---|---|
| 0 | ver:type（ver=1 高4b，type 低4b） |
| 1 | flags（qos[7:6] ack_req[5] frag[4] proto[3:2] enc[0]） |
| 2 | src_id |
| 3 | dst_id（0=广播） |
| 4-5 | seq（小端） |
| 6 | ttl |
| 7 | relay_id（广播=上一跳转发者；单播=下一跳；源发=0） |
| 8 | orig_sysid |
| 9 | net_id |
| 10-13 | frag_hdr[4] |
| 14.. | payload（≤179，加密帧含 4B tag） |
| 末2字节 | crc16 |

### 常量
`FRAME_HDR_LEN=14`、`FRAME_MAX_PAYLOAD=179`、`FRAME_MTU=200`、`FRAME_CRC_LEN=2`、`FRAME_DEF_TTL=5`、
`FRAME_TAG_LEN=4`（crypto tag）、`FRAME_MAX_PLAIN=175`（加密帧明文上限 = 179-4）

### 加密标志（FLAG_ENC，flags bit0）
- `frame_t.enc` 字段 ↔ 线上 `flags` bit0；frame_codec 只做编解码，**不做加解密**（加解密在 crypto 模块）
- 加密后 payload = 明文 + 4B tag，故明文上限降到 `FRAME_MAX_PLAIN=175`

### 接口与关键点
- `frame_encode(frame_t*→buf)` / `frame_decode(buf→frame_t*)` / `frame_crc16()`
- `frame_t.payload` 是 `const uint8_t*`，decode 时指向 buf 内部，不能脱离 buf 单独存活
- CRC16-CCITT（poly 0x1021，init 0xFFFF），覆盖 header(14)+payload，不含 CRC 本身
- `frame_decode` 校验版本位、长度、CRC 三项，任一失败返回 false（上层计 drop_crc）
