# cli —— USB CDC1 命令行

## 职责
插 USB 弹出的命令行，配置 / 状态 / 调试入口。

## 依赖
- `usb_cdc.h`（CDC1 收发）、`nvs_cfg.h`、`stats.h`

## 命令表（对齐架构 §9）
```
role gcs|air|relay   设置角色
set <键> <值>         改配置      get <键>      读配置
save                  保存        reset         重启
status                看状态      stats         看统计
scan                  扫描信道/RSSI
ping <node_id>        测通断
mavlink monitor on    旁路观察 MAVLink（调试）
help                  帮助
```

## 关键实现点
1. 行缓冲 + 回车解析；参数用空白分词
2. `set` 校验取值范围（sf 7~11、power 0~22、ttl 1~15…），非法报错
3. `save` 后发 `EV_CFG_CHANGED`，各模块重载

## 对外接口
见 `cli.h`。

## 验收（P0）
- 敲 `get` 能打印全部配置；`set power 20` + `save` 重启后仍生效
