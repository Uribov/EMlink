# frame_codec —— Mesh 帧编解码

## 职责

内存帧结构 ↔ 线上字节流互转，附 CRC16 与序号。全系统所有空口数据都经它打包/解包。

它是空口数据的唯一出入口：上层只管往 `frame_t` 里塞字段，怎么落到字节、怎么算 CRC、怎么解析回来，都在这里定死。


## 依赖

- 无硬件依赖（纯逻辑，可在 PC 单测）
- 被 `mac`（发包）、`mesh`（转发）、`frag`（分片头）使用

没有硬件依赖是关键：编解码规则改动可以在 PC 上跑完再上板。


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
| 10 | `enc_epoch` | 1 | 开机世代号（AES-CCM nonce 用，防跨重启复用；未加密帧=0）（P1-4） |
| 11–14 | `frag_hdr` | 4 | 分片头（§frag），非分片填 0 |
| 15–193 | `payload` | ≤179 | 数据（加密帧含 4B tag，明文上限 175） |
| 194–195 | `crc16` | 2 | CRC16-CCITT（header+payload） |

整帧上限 200 字节：15 字节头 + 最多 179 字节载荷 + 2 字节 CRC。加密只吃 payload 区，头部长度不变。

> 协议版本提醒：帧头 14→15 字节是不兼容变更（P1-4），两端固件必须同版本；上板前全网一起刷。


## 协议类型（proto，flags[3:2]）

- 0 = MAVLink（默认）
- 1 = CRSF
- 2 = MSP
- 3 = 自定义

proto 只占 2 bit，最多 4 种。当前实际只用到 MAVLink 一种，其余是给别的载荷协议留的口子。


## 关键实现点

1. 多字节字段小端；`ver`/`type` 打包为 `(ver<<4)|type`
2. CRC16-CCITT（poly 0x1021，init 0xFFFF），覆盖 header(15)+payload
3. 拆帧先查长度 ≥ `FRAME_HDR_LEN+FRAME_CRC_LEN`（17），再验 CRC，不过丢弃计 `dropped_crc`
4. 序号 `seq` 发送方自增（每源独立），供去重键 `(src_id, seq)`
5. `net_id` 默认 0；当前仅透传不校验（多网隔离尚未实现，属预留）
6. `enc_epoch` 由 crypto 在加密时写入（未加密帧为 0），逐跳转发原样保留

前三条是编解码本身的规则，后三条是字段语义。`net_id` 和 `enc_epoch` 这两条尤其容易改错：前者现在只是透传，后者必须在转发全程保持不变。


## 对外接口

见 `frame_codec.h`。

三个函数：`frame_encode`、`frame_decode`、`frame_crc16`。没有状态、没有初始化，随时可调。


## 验收（可在 PC 单测）

- 编解码往返：任意 payload 长度 0~179 无损还原
- CRC 篡改 1 字节 → `frame_decode` 返回 false
- `seq=0x0102` 编出字节 `02 01`；`proto`/`net_id`/`enc`/`enc_epoch` 正确读写
- 改帧头任一被 AAD 覆盖的字段（src_id/dst_id/type/seq/orig_sysid/enc_epoch）→ 对端解密认证失败

四条都能在 PC 上跑完，不用等硬件。


---

## 当前实现架构（供人和子 agent 阅读）

### 线上布局

字段顺序与上节「帧格式」完全一致（MTU 200），此处不重复列表。实现侧对应 `frame_encode()` 的写入顺序：

`ver:type` → `flags` → `src_id` → `dst_id` → `seq`(小端) → `ttl` → `relay_id` → `orig_sysid` → `net_id` → `enc_epoch` → `frag_hdr[4]` → `payload` → `crc16`


### 常量

`FRAME_HDR_LEN=15`、`FRAME_MAX_PAYLOAD=179`、`FRAME_MTU=200`、`FRAME_CRC_LEN=2`、`FRAME_DEF_TTL=5`、
`FRAME_TAG_LEN=4`（crypto tag）、`FRAME_MAX_PLAIN=175`（加密帧明文上限 = 179-4）

这组常量是所有分层共用的上限来源，改任何一个都要回到架构文档确认影响面。


### 加密标志（FLAG_ENC，flags bit0）

- `frame_t.enc` 字段 ↔ 线上 `flags` bit0；frame_codec 只做编解码，不做加解密（加解密在 crypto 模块）
- 加密后 payload = 明文 + 4B tag，故明文上限降到 `FRAME_MAX_PLAIN=175`

本模块只负责把标志位搬进搬出，加解密本身不在这里。


### 接口与关键点

- `frame_encode(frame_t*→buf)` / `frame_decode(buf→frame_t*)` / `frame_crc16()`
- `frame_t.payload` 是 `const uint8_t*`，decode 时指向 buf 内部，不能脱离 buf 单独存活
- CRC16-CCITT（poly 0x1021，init 0xFFFF），覆盖 header(15)+payload，不含 CRC 本身
- `frame_decode` 校验版本位、长度、CRC 三项，任一失败返回 false（上层计 drop_crc）
- `enc_epoch` 是逐跳不变的字段，故进入 crypto 的 AAD；`ttl`/`relay_id` 每跳都改，必须排除在 AAD 之外

最后两条是跨模块约束：哪些字段能进 AAD、哪些不能，取决于它们在转发途中会不会被改。写 crypto 或 mesh 的转发逻辑前必须先对这两条。
