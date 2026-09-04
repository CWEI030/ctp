# 确定性行情回放数据

两个 CSV 文件均使用格式版本 `1`、UTF-8、LF 换行和固定列顺序：

```text
format_version,instrument,market_seq,exchange_time_ms,recv_mono_ns,decision_mono_ns,last_price_ticks,bid_price_ticks,ask_price_ticks,bid_volume,ask_volume,volume,status
```

- `instrument` 是合约代码；长度必须小于 `kInstrumentIdCapacity`。
- `market_seq` 从行情入口取得，用于检测重复、倒退和缺口。
- `exchange_time_ms` 的单位是交易日内毫秒；本批策略不读取墙上时间。
- `recv_mono_ns` 和 `decision_mono_ns` 的单位是纳秒。后者是回放虚拟时钟，用于确定性判断行情是否过期。
- 三个价格字段均为最小变动价位的整数倍数，不是浮点价格。
- 三个数量字段依次为买一量、卖一量和总成交量。
- `status` 只能是 `Valid`、`NullData`、`InvalidInstrument`、`InvalidTime`、`InvalidPrice` 或 `InvalidBook`。
- 行按文件顺序消费，不允许排序、跳过或使用实际墙钟替换 `decision_mono_ns`。

`minimal_signal_v1.csv` 覆盖首次上穿、持续高位不重复触发、回落计数、冷却和最大信号数。`faults_v1.csv` 覆盖无效盘口、过期行情、错误合约、错误价格状态、序号缺口及故障锁定。

当前文件 SHA-256：

```text
7843b7698379db7b5d9352e4db1ae97db77c868d7be9646f7a2f98c30c2ce116  minimal_signal_v1.csv
b480057ac9ce18e7b0ff896c1c2aa41f0e466f89713e12f12e6c1b7bb59fe5ca  faults_v1.csv
```

在本目录执行 `sha256sum minimal_signal_v1.csv faults_v1.csv` 可复核；修改数据时必须同时更新这里的校验值和对应测试断言。
