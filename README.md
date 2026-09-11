# CTP 多账户低延迟交易客户端

一个面向 Linux x86-64 的 C++17 CTP 客户端实验项目。主运行模式使用一条共享行情连接，向按配置创建的多个独立账户工作线程分发行情；每个账户分别登录交易前置、恢复柜台状态、执行策略与风控、报撤单、处理回报并记录轨迹。

> 当前定位：核心功能已完成离线自动化验证，并提交了短时高样本性能证据；至少四个真实 SimNow 账户的联合在线验收和约 4.7 小时正式容量矩阵仍未完成。本项目不能据此直接用于实盘。

## 核心链路

```text
main()
  ↓
读取命令行、INI 配置和环境变量覆盖
  ↓
run_live_engine()
  ├─ 一条共享行情连接（候选账户凭据失败时按配置顺序切换）
  └─ N 个账户工作线程（每个账户拥有独立交易连接、队列和状态）
         ↓
CTP 行情回调
  ↓
校验行情并转换为整数价位 MarketEvent
  ↓
分别写入每个账户的固定容量行情队列
  ↓
账户工作线程取出行情
  ├─ 处理已有订单的超时撤单、平仓和有限次数重报
  └─ ThresholdStrategy 判断价格是否上穿阈值
         ↓
      OrderIntent
         ↓
账户风控检查 → AccountTradingSession::submit()
         ↓
      CTP 报单接口
         ↓
订单回报 / 成交回报
         ↓
更新订单、成交、持仓和恢复状态，并异步写入追踪文件
```

## 已实现能力

- 账户数量由配置决定，可运行 1、2、3、4 个或更多账户；验收要求是至少 4 个，不是程序只能创建 4 个。
- 每个账户独立持有交易连接、行情队列、订单表、成交去重状态、持仓、风控、恢复状态和追踪文件。
- 单个账户交易 API 创建、轨迹日志启动、恢复、认证、登录、查询、回报或队列故障只会冻结该账户；离线柜台替身测试验证其他账户仍可继续工作。
- 共享行情登录、订阅或重连失败时，会释放失败实例并按配置顺序使用下一账户凭据创建新行情 API；一轮候选全部失败才停止引擎。
- 共享行情回调会校验盘口和价格，并按合约最小变动价位转换为整数，避免策略使用浮点数直接比较价格。
- 确定性阈值策略在价格从阈值下方上穿时生成稳定信号；相同回放、配置和初始状态会产生相同决策。
- 风控覆盖行情有效性与时效、配置的交易时间窗口、账户就绪状态、熔断开关、单笔数量、净持仓、活动开仓单、每日报撤单次数、报单速率、价格偏离、每手保证金估算和下单后最低可用资金。
- 支持报单、撤单、订单回报、成交回报、重复回报去重、乱序回报合并和真实持仓更新。
- 开仓成交后可生成相反方向的平仓意图；入场单超时只撤一次，平仓单超时可在限定次数内撤单重报，次数耗尽后冻结账户。
- 账户重连后重新查询订单、成交、持仓和资金；回报队列溢出时也会由账户线程主动执行同样的四项核对，全部收敛前保持冻结。
- 安全重启映像保存未完成订单、CTP 交易日及当日信号、报单和撤单计数；同一交易日继续使用已消耗额度，确认跨日后清零。
- 应用自有实时热路径使用预分配、固定容量结构，不使用互斥锁、阻塞等待、动态内存分配或同步日志。CTP SDK 内部以及启动、退出、配置和文件写入属于控制面，不在此承诺内。
- 离线性能工具保存延迟原始样本、CPU、队列积压、丢弃统计、清单、结论和校验值，不只输出平均耗时。

## 尚未完成或不能保证

- 尚无至少四个真实 SimNow 账户同时登录、报撤单、接收回报、恢复状态和最终核对空仓的在线证据。
- 共享行情连接仍是公共依赖；所有候选账户凭据都无法登录或订阅，或行情服务整体不可用时，引擎仍会停止。
- Ctrl+C 退出只停止线程并汇总最终已知状态，不会主动撤销全部挂单或强制平仓。
- 没有跨账户汇总风险限制；现有风险按账户独立计算。
- 每手保证金是人工配置的保守估算值，不会从柜台合约保证金率自动计算或更新。
- 交易时间窗口由配置给出并使用行情交易所时间判断，没有自动接入交易所节假日、临时休市或品种夜盘日历。
- 每日额度的跨进程恢复依赖完整轨迹和安全检查点；轨迹损坏、关键事件丢弃或非安全退出不会被当作可信恢复状态。
- 没有独立的行情静默心跳监控；只有收到行情后才能执行时效检查。
- 运行中账户故障主要写入轨迹并在退出摘要中汇总，尚无外部实时告警系统。
- 不处理结算单确认、银期转账，也不提供任何收益保证。
- 正式性能矩阵尚未执行；短时离线数据不能代表真实 CTP 网络、柜台或生产机器性能。

## 目录与核心文件

| 文件或目录 | 作用 |
|---|---|
| `src/main.cpp` | 命令行入口和运行模式分发 |
| `src/config.cpp` | 命令行、INI 和环境变量解析 |
| `src/engine.cpp` | 共享行情连接、多账户工作线程和在线引擎装配 |
| `src/strategy.cpp` | 确定性阈值策略和 CSV 回放解析 |
| `src/trading.cpp` | 订单、成交、持仓、风控、报撤单、平仓与恢复状态 |
| `src/telemetry.cpp` | 异步轨迹、恢复文件读取和性能记录 |
| `src/benchmark.cpp` | 离线性能测试运行器 |
| `src/market_client.cpp` | 兼容保留的单合约行情诊断命令 |
| `src/trader_client.cpp` | 兼容保留的单账户查询诊断命令 |
| `tests/` | 八组离线自动化测试及 CTP 替身 |
| `tests/data/replay/` | 版本化行情回放数据及校验值 |
| `scripts/acceptance.sh` | 离线、性能和 SimNow 验收入口 |
| `scripts/audit.sh` | 凭据泄漏和应用热路径静态审计 |

## 构建与离线测试

依赖：Linux x86-64、CMake 3.16 以上、支持 C++17 的编译器，以及与系统架构匹配的 CTP Linux SDK。SDK 必须保存在仓库外，并具有以下结构：

```text
<CTP_SDK_ROOT>/
├── md/
│   ├── ThostFtdcMdApi.h
│   ├── ThostFtdcUserApiDataType.h
│   ├── ThostFtdcUserApiStruct.h
│   └── thostmduserapi_se.so
└── trader/
    ├── ThostFtdcTraderApi.h
    ├── ThostFtdcUserApiDataType.h
    ├── ThostFtdcUserApiStruct.h
    └── thosttraderapi_se.so
```

```bash
export CTP_SDK_ROOT=/absolute/path/to/ctp/sdk/linux64
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCTP_SDK_ROOT="$CTP_SDK_ROOT"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

仓库包含八个 CTest 测试程序：

| 测试 | 主要覆盖范围 |
|---|---|
| `interrupt` | Ctrl+C 信号状态 |
| `config` | 可变账户配置、环境变量覆盖、权限和安全门禁 |
| `market` | 旧行情诊断客户端的登录、订阅、超时和退出 |
| `engine` | 行情分发、多账户隔离、在线引擎装配和恢复 |
| `strategy` | 整数价位、阈值策略、故障锁定、回放确定性和下单衔接 |
| `trading` | 订单、成交、持仓、风控、撤单、平仓、重报和回报竞态 |
| `telemetry` | 异步轨迹、重启映像和性能证据文件 |
| `trader` | 旧账户诊断客户端的认证、查询、超时和退出 |

这些测试不连接 SimNow。测试通过只能证明离线输入和 CTP 替身覆盖的行为，不等于真实账户验收通过。

## 多账户配置

创建只在本机保存的配置文件：

```bash
cp config/accounts.example.ini config/accounts.local.ini
chmod 600 config/accounts.local.ini
```

将 `<...>` 占位值替换为测试环境信息。每个 `[account.别名]` 是一个账户；可以增加或删除账户段，`enabled=false` 的账户不会进入本次运行。`config/accounts.local.ini` 已被 Git 忽略，不得提交真实账号、密码、AppID 或 AuthCode。

账户字段可以被环境变量临时覆盖，格式为：

```text
CTP_ACCOUNT_<账户别名>_BROKER_ID
CTP_ACCOUNT_<账户别名>_USER_ID
CTP_ACCOUNT_<账户别名>_PASSWORD
CTP_ACCOUNT_<账户别名>_APP_ID
CTP_ACCOUNT_<账户别名>_AUTH_CODE
CTP_ACCOUNT_<账户别名>_TRADER_FRONT
```

例如只覆盖 `account1` 的密码：

```bash
export CTP_ACCOUNT_account1_PASSWORD='<temporary-password>'
```

## 安全预检与在线运行

只解析最终生效配置，不连接网络、不报单：

```bash
./build/ctp_client engine --mode live \
  --config config/accounts.local.ini --check
```

四账户验收预检会额外要求至少四个启用账户、每个启用账户使用不同的用户代码、显式报单开关以及单次只产生一个信号：

```bash
scripts/acceptance.sh simnow preflight
```

默认不会报单。要让订单可能发往交易前置，必须同时满足：

1. 配置中的 `strategy.enabled=true`；
2. 配置中的 `risk.kill_switch=false`；
3. 命令显式包含 `--allow-orders`；
4. 其他策略和风险参数均通过检查。

真实在线验收存在报单风险，脚本还要求显式确认：

```bash
export CTP_SIMNOW_CONFIRM=I_UNDERSTAND_SIMNOW_ORDERS
scripts/acceptance.sh simnow online
unset CTP_SIMNOW_CONFIRM
```

可通过 `CTP_SIMNOW_CONFIG` 指定其他本地配置路径。在线脚本启用专用 `--acceptance` 模式：每个配置中的启用账户都必须分别完成一次开仓报单、开仓成交、自动平仓报单、平仓成交，以及订单、成交、持仓和资金的最终柜台核对；最终持仓必须已知且多空均为零。任一账户失败、阶段缺失、重复开仓或在完成前按 Ctrl+C，进程都返回非零；故障账户不会阻止其他账户继续完成自己的链路。每个账户及总结果均输出可机器解析的 `[acceptance]` 行，不包含凭据。

轨迹写入 `runtime/traces/<run_id>/<账户别名>.csv`，CTP 流文件写入 `runtime/flow/<账户别名>/`。`runtime/` 整体被 Git 忽略。普通 `engine --mode live` 不启用严格验收，仍持续运行到 Ctrl+C。

前置地址和服务时间可能变化。联网前应以 [SimNow 官方产品与服务页面](https://www.simnow.com.cn/product.action)公布的信息为准。

## 兼容诊断命令

以下命令用于单独检查行情或账户连接，不经过多账户策略交易主链路：

```bash
./build/ctp_client market --profile simnow-7x24 \
  --instrument <有效合约> --ticks 5

./build/ctp_client account --profile simnow-7x24
```

这两个命令使用 `CTP_USER_ID` 和 `CTP_PASSWORD`；`account` 还需要 `CTP_APP_ID` 和 `CTP_AUTH_CODE`。可用 `CTP_BROKER_ID`、`CTP_MD_FRONT` 和 `CTP_TD_FRONT` 覆盖默认值。`account` 只查询资金和持仓，不报单。

## 回放、审计与性能证据

“回放”是把同一组已保存行情按固定顺序重新送入策略，用于验证相同条件是否产生相同决策；它不会让历史市场对新订单作出真实反馈，也不是撮合模拟器。

```bash
# 确定性策略回放重复三次
scripts/acceptance.sh replay

# 凭据与应用热路径审计
scripts/audit.sh all

# 1、2、4 账户短时离线性能结构检查
scripts/acceptance.sh benchmark smoke

# 1、2、4 账户短时高样本尾分位证据（约 2 分钟，不是容量测试）
scripts/acceptance.sh benchmark evidence

# 15 分钟稳态、突发、饱和档位和三次重复；耗时较长
scripts/acceptance.sh benchmark full

# 全部离线检查
scripts/acceptance.sh all-offline
```

性能结果保存在 `runtime/performance/`，每个结果目录包含：

- `latency_raw.csv`：延迟原始样本；
- `queue_raw.csv`：队列积压与丢弃；
- `cpu_raw.csv`：进程和线程 CPU 样本；
- `events_summary.json`：事件汇总；
- `manifest.json`、`report.md`：运行参数与结论；
- `reproduce.sh`、`SHA256SUMS`：复现命令和文件校验值。

报告会分别汇总进程和各线程角色的用户态/内核态 CPU、平均与峰值单核等效利用率、上下文切换，以及逐账户五类运行队列的最大深度、最高水位、最大最旧事件年龄、丢弃和首次丢弃时刻。P95、P99、P99.9 分别少于 200、1000、10000 个样本时会明确标为样本不足。

`benchmark evidence` 和完整执行后的 `benchmark full` 都会在 `evidence/performance/` 生成可提交的逐纳秒无损直方图、原始 CSV 摘要、CPU/队列时间序列、环境快照和矩阵索引。前者要求每账户四段链路各至少 10000 个样本，只证明短时离线分布可重算，不替代后者的持续负载、突发、饱和点和重复性结论。完整原始 CSV 仍保留在被 Git 忽略的 `runtime/` 目录；未完成的运行不会发布。

## 常见问题

- **CMake 提示缺少 SDK 文件**：确认 `CTP_SDK_ROOT` 指向同时包含 `md/` 和 `trader/` 的目录。
- **程序找不到 `.so`**：使用 `file` 和 `ldd` 检查动态库架构与缺失依赖。
- **连接被拒绝或超时**：检查 SimNow 当天的前置地址、服务时段、防火墙和 TCP 端口连通性。
- **返回 `USER_NOT_ACTIVE`**：请求已到达前置，但账户在目标环境中不可用；检查环境、激活状态和服务时段。
- **行情无推送**：确认合约在目标环境中有效，并处于实际有行情的时段。

## 安全提醒

不要把真实凭据写入 README、源码、可提交配置、命令行参数或聊天记录。在线运行前先使用测试账户、小仓位和可人工核对的合约，并以柜台查询结果作为最终事实来源。
