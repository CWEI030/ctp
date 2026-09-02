#pragma once

#include "ctp/config.hpp"

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

}
