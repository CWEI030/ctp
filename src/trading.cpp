#include "ctp/trading.hpp"

#include <algorithm>
#include <utility>

namespace ctp {
namespace {

struct OrderRecord {
    bool occupied{false};
    OrderSeed seed{};
    OrderSnapshot snapshot{};
};

bool is_terminal(OrderState state) noexcept
{
    return state == OrderState::RiskRejected
        || state == OrderState::SubmitRejectedLocally
        || state == OrderState::SubmitResultUnknown
        || state == OrderState::Canceled
        || state == OrderState::Filled
        || state == OrderState::Rejected;
}

bool valid_seed(const OrderSeed& seed) noexcept
{
    return seed.client_order_id != 0 && seed.quantity > 0
        && seed.instrument.front() != '\0';
}

}

struct AccountTradingState::Impl {
    Impl(
        std::string owned_account_id,
        std::size_t owned_order_capacity,
        std::size_t owned_trade_capacity)
        : account_id(std::move(owned_account_id)),
          order_capacity(owned_order_capacity),
          trade_capacity(owned_trade_capacity),
          orders(order_capacity == 0
                     ? nullptr
                     : std::make_unique<OrderRecord[]>(order_capacity))
    {
    }

    OrderRecord* find_order(std::uint64_t client_order_id) noexcept
    {
        if (order_capacity == 0) {
            return nullptr;
        }
        const std::size_t first = client_order_id % order_capacity;
        for (std::size_t probe = 0; probe < order_capacity; ++probe) {
            OrderRecord& record = orders[(first + probe) % order_capacity];
            if (!record.occupied) {
                return nullptr;
            }
            if (record.seed.client_order_id == client_order_id) {
                return &record;
            }
        }
        return nullptr;
    }

    const OrderRecord* find_order(std::uint64_t client_order_id) const noexcept
    {
        return const_cast<Impl*>(this)->find_order(client_order_id);
    }

    OrderRecord* empty_order(std::uint64_t client_order_id) noexcept
    {
        if (order_capacity == 0) {
            return nullptr;
        }
        const std::size_t first = client_order_id % order_capacity;
        for (std::size_t probe = 0; probe < order_capacity; ++probe) {
            OrderRecord& record = orders[(first + probe) % order_capacity];
            if (!record.occupied) {
                return &record;
            }
        }
        return nullptr;
    }

    ApplyResult conflict(OrderRecord* record = nullptr) noexcept
    {
        reconciliation = true;
        if (record != nullptr) {
            record->snapshot.reconciliation_required = true;
        }
        return {ApplyCode::Conflict, false, false, false, true};
    }

    std::string account_id;
    std::size_t order_capacity{0};
    std::size_t trade_capacity{0};
    std::unique_ptr<OrderRecord[]> orders;
    bool reconciliation{false};
};

AccountTradingState::AccountTradingState(
    std::string account_id,
    std::size_t order_capacity,
    std::size_t trade_capacity)
    : impl_(std::make_unique<Impl>(
          std::move(account_id), order_capacity, trade_capacity))
{
}

AccountTradingState::~AccountTradingState() = default;

std::string_view AccountTradingState::account_id() const noexcept
{
    return impl_->account_id;
}

ApplyResult AccountTradingState::create_order(const OrderSeed& seed) noexcept
{
    if (!valid_seed(seed)) {
        return impl_->conflict();
    }
    if (impl_->find_order(seed.client_order_id) != nullptr) {
        return {ApplyCode::Duplicate};
    }
    OrderRecord* record = impl_->empty_order(seed.client_order_id);
    if (record == nullptr) {
        impl_->reconciliation = true;
        return {ApplyCode::CapacityExceeded, false, false, false, true};
    }

    record->occupied = true;
    record->seed = seed;
    record->snapshot.client_order_id = seed.client_order_id;
    record->snapshot.original_quantity = seed.quantity;
    return {ApplyCode::Applied, true};
}

ApplyResult AccountTradingState::apply_local_event(
    std::uint64_t client_order_id,
    LocalOrderEvent event) noexcept
{
    OrderRecord* record = impl_->find_order(client_order_id);
    if (record == nullptr) {
        impl_->reconciliation = true;
        return {ApplyCode::UnknownOrder, false, false, false, true};
    }

    const OrderState before = record->snapshot.state;
    OrderState after = before;
    switch (event) {
    case LocalOrderEvent::RiskAccepted:
        if (before == OrderState::Created) after = OrderState::RiskAccepted;
        break;
    case LocalOrderEvent::RiskRejected:
        if (before == OrderState::Created) after = OrderState::RiskRejected;
        break;
    case LocalOrderEvent::SubmitRejected:
        if (before == OrderState::RiskAccepted) {
            after = OrderState::SubmitRejectedLocally;
        }
        break;
    case LocalOrderEvent::SubmitResultUnknown:
        if (before == OrderState::RiskAccepted) {
            after = OrderState::SubmitResultUnknown;
        }
        break;
    case LocalOrderEvent::Submitted:
        if (before == OrderState::RiskAccepted) after = OrderState::Submitted;
        break;
    case LocalOrderEvent::CancelRequested:
        if (before == OrderState::Submitted || before == OrderState::Accepted
            || before == OrderState::PartiallyFilled) {
            after = OrderState::CancelRequested;
        }
        break;
    }

    if (after == before) {
        return is_terminal(before)
            ? ApplyResult{ApplyCode::Stale}
            : impl_->conflict(record);
    }
    record->snapshot.state = after;
    if (after == OrderState::SubmitResultUnknown) {
        record->snapshot.reconciliation_required = true;
        impl_->reconciliation = true;
    }
    return {ApplyCode::Applied, true, false, false,
            record->snapshot.reconciliation_required};
}

ApplyResult AccountTradingState::apply_order_report(
    const OrderReport& report) noexcept
{
    OrderRecord* record = impl_->find_order(report.client_order_id);
    if (record == nullptr) {
        impl_->reconciliation = true;
        return {ApplyCode::UnknownOrder, false, false, false, true};
    }
    if (report.cumulative_filled < 0
        || report.cumulative_filled > record->seed.quantity) {
        return impl_->conflict(record);
    }

    const std::int32_t previous_fill = record->snapshot.reported_filled;
    if (report.cumulative_filled < previous_fill) {
        return {ApplyCode::Stale};
    }
    record->snapshot.reported_filled = report.cumulative_filled;

    const OrderState before = record->snapshot.state;
    OrderState after = before;
    switch (report.type) {
    case OrderReportType::Accepted:
        if (before == OrderState::Submitted) after = OrderState::Accepted;
        break;
    case OrderReportType::Rejected:
        if (before == OrderState::Submitted) after = OrderState::Rejected;
        break;
    case OrderReportType::PartiallyFilled:
        if (!is_terminal(before) || before == OrderState::Canceled) {
            after = OrderState::PartiallyFilled;
        }
        break;
    case OrderReportType::Filled:
        if (report.cumulative_filled == record->seed.quantity
            && before != OrderState::RiskRejected
            && before != OrderState::SubmitRejectedLocally) {
            after = OrderState::Filled;
        }
        break;
    case OrderReportType::Canceled:
        if (before != OrderState::Filled && before != OrderState::Rejected
            && before != OrderState::RiskRejected
            && before != OrderState::SubmitRejectedLocally) {
            after = OrderState::Canceled;
            record->snapshot.cancel_acknowledged = true;
        }
        break;
    case OrderReportType::CancelRejected:
        if (before == OrderState::CancelRequested) {
            after = report.cumulative_filled == 0
                ? OrderState::Accepted
                : OrderState::PartiallyFilled;
        }
        break;
    }

    const bool fill_changed = previous_fill != report.cumulative_filled;
    const bool state_changed = before != after;
    if (!state_changed && !fill_changed) {
        return {ApplyCode::Duplicate};
    }
    if (!state_changed && report.type != OrderReportType::Accepted) {
        return impl_->conflict(record);
    }
    record->snapshot.state = after;
    return {ApplyCode::Applied, state_changed, fill_changed};
}

ApplyResult AccountTradingState::apply_trade(const TradeReport&) noexcept
{
    return impl_->conflict();
}

bool AccountTradingState::order_snapshot(
    std::uint64_t client_order_id,
    OrderSnapshot& snapshot) const noexcept
{
    const OrderRecord* record = impl_->find_order(client_order_id);
    if (record == nullptr) return false;
    snapshot = record->snapshot;
    return true;
}

bool AccountTradingState::position_snapshot(
    std::string_view,
    PositionSnapshot&) const noexcept
{
    return false;
}

bool AccountTradingState::reconciliation_required() const noexcept
{
    return impl_->reconciliation;
}

}
