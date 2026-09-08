# CTP SimNow 多账户交易客户端

这是一个用于学习 CTP API 的 Linux C++17 命令行项目。它可以：

- `market`：登录行情前置，订阅一个合约，收到指定条数后退出；
- `account`：认证并登录交易前置，查询资金和全部持仓后退出；
- `engine --mode live`：运行共享行情和彼此隔离的一个或多个账户交易会话。

第二批已经完成离线行情进入层：行情回调会被校验并转换为定长整数事件，再分发到按配置数量
预分配的账户独立队列。第三批完成每账户独立的订单状态、成交去重和多空持仓纯状态核心；
5 手订单的 2+1+2 多笔成交、720 种回报顺序和撤单/迟到成交竞态均有离线测试。第四批在此基础上
加入了每账户风控、唯一报单引用、报单/撤单请求转换，以及订单和成交回调的有界队列接入。第五批
增加确定性阈值策略和严格的版本化 CSV 回放：有效行情上穿阈值时生成稳定 `signal_id` 和买开一手
的 `OrderIntent`，相同输入、配置和初始状态的三次回放结果逐字段一致。第六阶段继续复用交易状态
核心：真实开仓成交会形成待平仓敞口，下一条有效行情按盘口保护价发出相反方向的平仓单；入场超时
只撤一次，平仓超时按有限次数撤单重报，耗尽后冻结所属账户并保留真实持仓等待核对。断线、行情
队列溢出、回报队列溢出和异常回报均具有账户级稳定故障原因，四账户故障矩阵证明其他账户仍可报单。

上述交易与策略能力已经装入 `engine --mode live`：一条共享行情连接给各账户独立队列分发行情，
每个账户独立认证、登录、查询恢复、运行策略和风控，并记录异步轨迹。默认不允许报单；只有同时
提供 `--allow-orders`、启用策略并关闭熔断开关，订单才可能发往交易前置。现有在线运行代码已由
柜台替身测试，尚未取得四个真实 SimNow 账户同时开平仓和最终零持仓证据。离线基准数字也不代表
真实 CTP 网络性能。
CTP 官方 SDK 和账号凭据都不应提交到 Git。

## 1. 准备环境

需要 Linux x86-64、CMake 3.16 以上、支持 C++17 的编译器，以及与系统架构匹配的
CTP Linux SDK。SDK 放在仓库外，目录必须满足：

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

## 2. 编译和离线测试

进入本仓库根目录，将第一行改成你自己的 SDK 绝对路径：

```bash
export CTP_SDK_ROOT=/absolute/path/to/sdk/6.7.13/linux64
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCTP_SDK_ROOT="$CTP_SDK_ROOT"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

成功时会生成 `build/ctp_client`，CTest 应报告 8 个测试程序全部通过。这里测试的是配置、
离线行情分发、行情浮点价格到整数 tick 的边界转换、确定性策略与回放、订单/成交/持仓状态、
风控、替身接口报撤单、自动平仓、有限撤单重报、账户级故障隔离和回调接入，以及旧行情与交易
客户端状态、超时及资源清理，不需要 SimNow 账号，也不会连接 SimNow。

## 3. 配置方式

命令行只接受以下参数：

```text
ctp_client market  [--profile PROFILE] --instrument INSTRUMENT --ticks COUNT
ctp_client account [--profile PROFILE]
ctp_client engine --mode live [--config CONFIG] [--check] [--allow-orders]
ctp_client benchmark --config CONFIG --input REPLAY.csv --output RESULT_DIR \
  [--accounts COUNT] [--rate EVENTS_PER_SECOND] \
  [--warmup-seconds SECONDS] [--duration-seconds SECONDS] \
  [--burst-rate EVENTS_PER_SECOND --burst-seconds SECONDS]
```

- `PROFILE` 默认是 `simnow-7x24`；
- `INSTRUMENT` 是有效合约代码，例如运行当天仍有效的期货合约；
- `COUNT` 必须是正整数；
- 禁止使用 `--password`，密码只能通过环境变量传入。

### 多账户本地配置

先从可提交的占位模板创建本机配置，再将权限限制为只有文件所有者可以读写：

```bash
cp config/accounts.example.ini config/accounts.local.ini
chmod 600 config/accounts.local.ini
```

把 `config/accounts.local.ini` 中的 `<...>` 替换为本机测试账户信息。该文件已被 Git
忽略，不得提交。每个 `[account.别名]` 表示一个账户；可以配置 1 个、2 个、3 个、4 个
或更多账户。四账户只是本阶段必须通过的验收门槛，不是程序上限。`enabled=false` 的账户
不会进入启动时生成的只读账户集合；如果没有任何启用账户，程序拒绝启动。

省略 `--config` 时默认读取 `config/accounts.local.ini`。临时覆盖某个账户字段时，环境变量
名称为 `CTP_ACCOUNT_账户别名_字段名`，例如：

```bash
export CTP_ACCOUNT_account1_PASSWORD='<temporary-password>'
export CTP_ACCOUNT_account2_TRADER_FRONT='tcp://127.0.0.1:40001'
```

可覆盖字段为 `BROKER_ID`、`USER_ID`、`PASSWORD`、`APP_ID`、`AUTH_CODE` 和
`TRADER_FRONT`。覆盖只影响别名对应的账户。运行秘密审计：

```bash
scripts/audit.sh secrets
```

应用自有热路径禁用项审计和短性能工具验收分别执行：

```bash
scripts/audit.sh hot-path
scripts/acceptance.sh benchmark smoke
```

短测会运行 1、2、4 账户结构矩阵并把结果写入忽略提交的 `runtime/performance/`。正式矩阵使用
`scripts/acceptance.sh benchmark full`，包含预热、每档至少 15 分钟、突发和三次重复，运行时间较长。
两种模式都只使用离线回放与柜台替身，不连接 SimNow。

### 在线引擎安全门禁

默认预检只解析最终生效配置，不创建网络连接，也不报单：

```bash
scripts/acceptance.sh simnow preflight
```

预检要求至少四个启用账户，并且具有四个不同的用户代码；未替换的 `<...>` 占位值、缺失字段、
危险文件权限、未启用策略或仍开启熔断都会在联网前拒绝。当前只有一个真实账号时，此门禁按设计
返回阻断，不能用重复账号冒充四账户验收。

真实在线命令具有报单风险，只能在四账户预检通过后显式确认：

```bash
export CTP_SIMNOW_CONFIRM=I_UNDERSTAND_SIMNOW_ORDERS
scripts/acceptance.sh simnow online
unset CTP_SIMNOW_CONFIRM
```

可用 `CTP_SIMNOW_CONFIG` 指定本机配置路径。在线引擎持续运行到按下 Ctrl+C；结束摘要给出就绪、
失败、策略报单、最终已知空仓和持仓未知账户数。摘要和本地轨迹用于验收，但不能替代柜台查询核对。

程序内置的 profile 如下。它们是公共接入参数，不是账号信息：

| profile | 行情前置 | 交易前置 |
|---|---|---|
| `simnow-1` | `tcp://180.168.146.187:10211` | `tcp://180.168.146.187:10201` |
| `simnow-2` | `tcp://180.168.146.187:10212` | `tcp://180.168.146.187:10202` |
| `simnow-7x24` | `tcp://182.254.243.31:40011` | `tcp://182.254.243.31:40001` |

前置地址和服务时间可能变化。真实联网前，以
[SimNow 产品与服务](https://www.simnow.com.cn/product.action)页面当天公布的信息为准。
如官网地址与上表不一致，可临时覆盖：

```bash
export CTP_MD_FRONT=tcp://MARKET_IP:PORT
export CTP_TD_FRONT=tcp://TRADER_IP:PORT
```

环境变量：

| 名称 | market | account | 说明 |
|---|---:|---:|---|
| `CTP_USER_ID` | 必需 | 必需 | SimNow 用户代码 |
| `CTP_PASSWORD` | 必需 | 必需 | SimNow 密码 |
| `CTP_BROKER_ID` | 可选 | 可选 | 默认 `9999` |
| `CTP_APP_ID` | 不使用 | 必需 | SimNow 提供的 AppID |
| `CTP_AUTH_CODE` | 不使用 | 必需 | SimNow 提供的 AuthCode |
| `CTP_MD_FRONT` | 可选 | 可选 | 覆盖 profile 的行情前置，必须以 `tcp://` 开头 |
| `CTP_TD_FRONT` | 可选 | 可选 | 覆盖 profile 的交易前置，必须以 `tcp://` 开头 |

## 4. 安全输入凭据

在同一个 Bash 终端执行下面的命令。密码和 AuthCode 使用隐藏输入，不会显示在屏幕上，
也不会作为值写进 shell 命令历史：

```bash
read -rp "SimNow 账号: " CTP_USER_ID
read -srp "SimNow 密码: " CTP_PASSWORD
echo
export CTP_USER_ID CTP_PASSWORD
```

运行 `account` 前还需要：

```bash
read -srp "SimNow AppID: " CTP_APP_ID
echo
read -srp "SimNow AuthCode: " CTP_AUTH_CODE
echo
export CTP_APP_ID CTP_AUTH_CODE
```

不要把真实值写入 README、源码、`.env`、命令参数或聊天记录。完成测试后清除：

```bash
unset CTP_USER_ID CTP_PASSWORD CTP_APP_ID CTP_AUTH_CODE
unset CTP_BROKER_ID CTP_MD_FRONT CTP_TD_FRONT
```

`unset` 只在运行结束后执行；程序运行时仍然需要这些环境变量。

## 5. 运行行情模式

先确认 SimNow 正在服务，并从交易软件或 SimNow 环境确认一个当天有效的合约代码，然后运行：

```bash
read -rp "有效合约: " CTP_INSTRUMENT
./build/ctp_client market --profile simnow-7x24 --instrument "$CTP_INSTRUMENT" --ticks 5
echo "exit_code=$?"
unset CTP_INSTRUMENT
```

成功时输出恰好 5 条行情，格式类似：

```text
time=21:00:00.123 instrument=<合约> last=0 bid1=0 ask1=0 volume=0
[ok] market data completed: ticks=5
```

示例中的价格只是格式占位，不代表真实行情。无效或没有行情推送的合约可能收到订阅错误，
也可能在固定 15 秒等待后超时。

## 6. 运行账户模式

账户模式需要账号、密码、AppID 和 AuthCode：

```bash
./build/ctp_client account --profile simnow-7x24
echo "exit_code=$?"
```

成功时会输出一行脱敏资金摘要、零条或多条持仓，以及：

```text
[ok] account queries completed: positions=<数量>
```

这个兼容命令不会发送结算单确认、报单或撤单请求；报撤单只存在于带显式安全门禁的在线引擎。

## 7. SimNow 联网前提

根据项目收到的 SimNow 7×24 公告：交易日服务时间为 16:00 至次日 09:00，非交易日为
16:00 至次日 12:00；新注册用户可能需要等到第三个交易日才能使用第二套环境。

这些规则和地址都可能由 SimNow 调整，因此每次真实测试前都要重新查看官网。网页能够通过
HTTPS 打开，只证明网站可访问，不代表服务器能够连接 CTP 的 TCP 前置端口。

## 8. 退出码

| 退出码 | 含义 |
|---:|---|
| `0` | 操作成功 |
| `2` | 命令行或环境变量配置无效，尚未联网 |
| `3` | 本地初始化、API、流目录或信号处理器创建失败 |
| `4` | 断线或 15 秒超时 |
| `5` | 认证或登录失败 |
| `6` | 行情订阅、资金查询或持仓查询失败 |
| `130` | 用户按 Ctrl+C，资源清理后退出 |

`0 tests failed out of 8` 只表示八组离线自动化测试通过，不表示真实账号已经登录成功。

## 9. 常见问题

### CMake 提示缺少 SDK 文件

检查路径和文件名：

```bash
find "$CTP_SDK_ROOT" -maxdepth 2 -type f | sort
```

`CTP_SDK_ROOT` 应指向同时包含 `md` 和 `trader` 的目录，不能只指向其中一个子目录。

### 程序提示找不到 `.so`

确认动态库架构和依赖：

```bash
file "$CTP_SDK_ROOT"/md/thostmduserapi_se.so
ldd "$CTP_SDK_ROOT"/md/thostmduserapi_se.so
ldd "$CTP_SDK_ROOT"/trader/thosttraderapi_se.so
```

应为 x86-64，并且 `ldd` 不应出现 `not found`。

### `Connection refused` 或超时

依次检查：官网当天地址、SimNow 服务时间、服务器防火墙和到 TCP 端口的网络连通性。
浏览器能打开官网不能排除 CTP TCP 端口被拦截。

### 返回 `USER_NOT_ACTIVE`

请求已经到达前置，但该账号在目标环境中尚未激活。确认注册等待期、所选环境和服务时间，
必要时等待 SimNow 同步账号状态。这不等于本地编译失败。

### 行情收不到或等待超时

确认合约仍在该环境挂牌、代码大小写正确，并处于有行情推送的时段。先换一个已在 SimNow
客户端中确认有效的活跃合约测试。

## 10. 当前限制

- 仅支持 Linux x86-64 和仓库外的 CTP Linux SDK；
- 行情模式一次只订阅一个合约，只运行到指定 `ticks`；
- 旧 `market`、`account` 命令固定等待 15 秒；在线引擎持续运行到 Ctrl+C；
- 账户模式只查询资金和全部持仓；
- 不处理结算单确认或转账；在线引擎虽已装配报撤单、自动平仓、逐账户恢复与轨迹，但尚未经过
  四个真实 SimNow 账户同时验证；
- 本机当前只有一个账号，不能完成四账户登录、故障隔离、开平仓和最终零持仓验收；
- 没有独立的 `replay` 命令；`benchmark` 命令可离线生成性能证据，但尚未运行正式长时矩阵，
  其结果不能替代真实 CTP 网络链路验收。
