#pragma once

#include "ctp/config.hpp"
#include "ctp/trader_client.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ctp {

inline constexpr std::size_t kTradingDayCapacity = 9;
inline constexpr std::size_t kExchangeIdCapacity = 9;
inline constexpr std::size_t kTradeIdCapacity = 21;

enum class Direction : std::uint8_t {
    Buy,
    Sell,
};

enum class Offset : std::uint8_t {
    Open,
    Close,
};

enum class OrderState : std::uint8_t {
    Created,
    RiskAccepted,
    RiskRejected,
    SubmitRejectedLocally,
    SubmitResultUnknown,
    Submitted,
    Accepted,
    PartiallyFilled,
    CancelRequested,
    Canceled,
    Filled,
    Rejected,
};

enum class LocalOrderEvent : std::uint8_t {
    RiskAccepted,
    RiskRejected,
    SubmitRejected,
    SubmitResultUnknown,
    Submitted,
    CancelRequested,
};

enum class OrderReportType : std::uint8_t {
    Accepted,
    Rejected,
    PartiallyFilled,
    Filled,
    Canceled,
    CancelRejected,
};

enum class ApplyCode : std::uint8_t {
    Applied,
    Duplicate,
    Stale,
    Conflict,
    UnknownOrder,
    CapacityExceeded,
};

struct ApplyResult {
    ApplyCode code{ApplyCode::Applied};
    bool state_changed{false};
    bool fill_changed{false};
    bool position_changed{false};
    bool reconciliation_required{false};
};

struct OrderSeed {
    std::uint64_t client_order_id{0};
    std::array<char, kInstrumentIdCapacity> instrument{};
    Direction direction{Direction::Buy};
    Offset offset{Offset::Open};
    std::int32_t quantity{0};
};

struct OrderReport {
    std::uint64_t client_order_id{0};
    OrderReportType type{OrderReportType::Accepted};
    std::int32_t cumulative_filled{0};
};

struct OrderSnapshot {
    std::uint64_t client_order_id{0};
    OrderState state{OrderState::Created};
    std::int32_t original_quantity{0};
    std::int32_t reported_filled{0};
    std::int32_t accounted_filled{0};
    bool cancel_acknowledged{false};
    bool reconciliation_required{false};
};

struct TradeReport {
    std::uint64_t client_order_id{0};
    std::array<char, kTradingDayCapacity> trading_day{};
    std::array<char, kExchangeIdCapacity> exchange_id{};
    std::array<char, kTradeIdCapacity> trade_id{};
    std::array<char, kInstrumentIdCapacity> instrument{};
    Direction direction{Direction::Buy};
    Offset offset{Offset::Open};
    std::int32_t quantity{0};
};

struct PositionSnapshot {
    std::array<char, kInstrumentIdCapacity> instrument{};
    std::int32_t long_quantity{0};
    std::int32_t short_quantity{0};
    bool known{true};
};

struct OrderIntent {
    std::uint64_t signal_id{0};
    std::array<char, kInstrumentIdCapacity> instrument{};
    Direction direction{Direction::Buy};
    Offset offset{Offset::Open};
    std::int32_t quantity{0};
    std::int64_t limit_price_ticks{0};
};

struct RiskLimits {
    std::array<char, kInstrumentIdCapacity> allowed_instrument{};
    std::int64_t max_market_age_ns{0};
    std::int64_t max_slippage_ticks{0};
    std::int64_t margin_per_lot{0};
    std::int64_t minimum_available_after_order{0};
    std::uint32_t max_daily_signals{0};
    std::uint32_t max_daily_orders{0};
    std::uint32_t max_daily_cancels{0};
    std::int32_t max_active_open_orders{0};
    std::int32_t max_net_open_position{0};
    bool allow_buy{true};
    bool allow_sell{true};
    bool allow_open{true};
    bool allow_close{true};
};

struct RiskSnapshot {
    bool enabled{false};
    bool authenticated{false};
    bool logged_in{false};
    bool reconciled{false};
    bool frozen{false};
    bool exiting{false};
    bool trading_window_open{false};
    bool global_kill_switch{false};
    bool account_kill_switch{false};
    bool market_valid{false};
    bool funds_known{false};
    bool positions_known{false};
    bool result_unknown{false};
    std::int64_t now_ns{0};
    std::int64_t market_receive_ns{0};
    std::int64_t bid_price_ticks{0};
    std::int64_t ask_price_ticks{0};
    std::int64_t available_funds{0};
    std::int32_t long_position{0};
    std::int32_t short_position{0};
    std::int32_t closable_long{0};
    std::int32_t closable_short{0};
    std::uint32_t daily_signals{0};
    std::uint32_t daily_orders{0};
    std::int32_t active_open_orders{0};
};

enum class RiskRejectReason : std::uint8_t {
    None,
    AccountDisabled,
    NotAuthenticated,
    NotLoggedIn,
    NotReconciled,
    AccountFrozen,
    Exiting,
    OutsideTradingWindow,
    KillSwitch,
    InstrumentNotAllowed,
    DirectionNotAllowed,
    OffsetNotAllowed,
    InvalidQuantity,
    DailySignalLimit,
    DailyOrderLimit,
    InvalidMarket,
    StaleMarket,
    PriceProtection,
    FundsUnknown,
    InsufficientFunds,
    PositionsUnknown,
    InsufficientPosition,
    TooManyActiveOpenOrders,
    NetPositionLimit,
    ResultUnknown,
};

RiskRejectReason evaluate_risk(
    const OrderIntent& intent,
    const RiskSnapshot& snapshot,
    const RiskLimits& limits) noexcept;

enum class SubmitCode : std::uint8_t {
    Submitted,
    RiskRejected,
    RejectedLocally,
    Duplicate,
    CapacityExceeded,
};

struct SubmitResult {
    SubmitCode code{SubmitCode::RejectedLocally};
    std::uint64_t client_order_id{0};
    std::uint64_t order_ref{0};
    RiskRejectReason risk_reason{RiskRejectReason::None};
    int api_return_code{0};
};

// 一个实例只属于一个账户，并由该账户执行线程串行修改。
class AccountTradingState {
public:
    AccountTradingState(
        std::string account_id,
        std::size_t order_capacity,
        std::size_t trade_capacity);
    ~AccountTradingState();

    AccountTradingState(const AccountTradingState&) = delete;
    AccountTradingState& operator=(const AccountTradingState&) = delete;

    std::string_view account_id() const noexcept;
    ApplyResult create_order(const OrderSeed& seed) noexcept;
    ApplyResult apply_local_event(
        std::uint64_t client_order_id,
        LocalOrderEvent event) noexcept;
    ApplyResult apply_order_report(const OrderReport& report) noexcept;
    ApplyResult apply_trade(const TradeReport& report) noexcept;

    bool order_snapshot(
        std::uint64_t client_order_id,
        OrderSnapshot& snapshot) const noexcept;
    bool position_snapshot(
        std::string_view instrument,
        PositionSnapshot& snapshot) const noexcept;
    bool reconciliation_required() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 一个会话只绑定一个账户；提交热路径只扫描构造时预留的定长表。
class AccountTradingSession final : public CThostFtdcTraderSpi {
public:
    AccountTradingSession(
        const AccountConfig& account,
        RiskLimits limits,
        std::unique_ptr<TraderApi> api,
        std::size_t order_capacity,
        std::size_t trade_capacity,
        std::size_t signal_capacity,
        std::size_t callback_capacity,
        std::uint64_t next_client_order_id = 1,
        std::uint64_t persisted_next_order_ref = 1);
    ~AccountTradingSession();

    AccountTradingSession(const AccountTradingSession&) = delete;
    AccountTradingSession& operator=(const AccountTradingSession&) = delete;

    void activate(
        int front_id,
        int session_id,
        std::string_view max_order_ref) noexcept;
    SubmitResult submit(
        const OrderIntent& intent,
        const RiskSnapshot& snapshot) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
