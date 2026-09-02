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

struct TradeRecord {
    bool occupied{false};
    TradeReport report{};
};

struct PositionRecord {
    bool occupied{false};
    PositionSnapshot snapshot{};
};

template <std::size_t N>
bool has_text(const std::array<char, N>& field) noexcept
{
    return field.front() != '\0';
}

template <std::size_t N>
void hash_field(std::size_t& hash, const std::array<char, N>& field) noexcept
{
    for (const char value : field) {
        if (value == '\0') break;
        hash ^= static_cast<unsigned char>(value);
        hash *= static_cast<std::size_t>(1099511628211ULL);
    }
    hash ^= 0xffU;
    hash *= static_cast<std::size_t>(1099511628211ULL);
}

std::size_t trade_hash(const TradeReport& report) noexcept
{
    std::size_t hash = static_cast<std::size_t>(1469598103934665603ULL);
    hash_field(hash, report.trading_day);
    hash_field(hash, report.exchange_id);
    hash_field(hash, report.trade_id);
    return hash;
}

bool same_trade_key(const TradeReport& left, const TradeReport& right) noexcept
{
    return left.trading_day == right.trading_day
        && left.exchange_id == right.exchange_id
        && left.trade_id == right.trade_id;
}

bool same_trade(const TradeReport& left, const TradeReport& right) noexcept
{
    return same_trade_key(left, right)
        && left.client_order_id == right.client_order_id
        && left.instrument == right.instrument
        && left.direction == right.direction
        && left.offset == right.offset
        && left.quantity == right.quantity;
}

std::size_t text_hash(std::string_view text) noexcept
{
    std::size_t hash = static_cast<std::size_t>(1469598103934665603ULL);
    for (const char value : text) {
        hash ^= static_cast<unsigned char>(value);
        hash *= static_cast<std::size_t>(1099511628211ULL);
    }
    return hash;
}

template <std::size_t N>
bool field_equals(
    const std::array<char, N>& field,
    std::string_view text) noexcept
{
    std::size_t length = 0;
    while (length < N && field[length] != '\0') ++length;
    return length == text.size()
        && std::equal(text.begin(), text.end(), field.begin());
}

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

bool same_seed(const OrderSeed& left, const OrderSeed& right) noexcept
{
    return left.client_order_id == right.client_order_id
        && left.instrument == right.instrument
        && left.direction == right.direction
        && left.offset == right.offset
        && left.quantity == right.quantity;
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
                     : std::make_unique<OrderRecord[]>(order_capacity)),
          trades(trade_capacity == 0
                     ? nullptr
                     : std::make_unique<TradeRecord[]>(trade_capacity)),
          position_capacity(order_capacity),
          positions(position_capacity == 0
                        ? nullptr
                        : std::make_unique<PositionRecord[]>(position_capacity))
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

    TradeRecord* find_trade(const TradeReport& report) noexcept
    {
        if (trade_capacity == 0) return nullptr;
        const std::size_t first = trade_hash(report) % trade_capacity;
        for (std::size_t probe = 0; probe < trade_capacity; ++probe) {
            TradeRecord& record = trades[(first + probe) % trade_capacity];
            if (!record.occupied) return nullptr;
            if (same_trade_key(record.report, report)) return &record;
        }
        return nullptr;
    }

    TradeRecord* empty_trade(const TradeReport& report) noexcept
    {
        if (trade_capacity == 0) return nullptr;
        const std::size_t first = trade_hash(report) % trade_capacity;
        for (std::size_t probe = 0; probe < trade_capacity; ++probe) {
            TradeRecord& record = trades[(first + probe) % trade_capacity];
            if (!record.occupied) return &record;
        }
        return nullptr;
    }

    PositionRecord* find_position(std::string_view instrument) noexcept
    {
        if (position_capacity == 0) return nullptr;
        const std::size_t first = text_hash(instrument) % position_capacity;
        for (std::size_t probe = 0; probe < position_capacity; ++probe) {
            PositionRecord& record =
                positions[(first + probe) % position_capacity];
            if (!record.occupied) return nullptr;
            if (field_equals(record.snapshot.instrument, instrument)) {
                return &record;
            }
        }
        return nullptr;
    }

    const PositionRecord* find_position(
        std::string_view instrument) const noexcept
    {
        return const_cast<Impl*>(this)->find_position(instrument);
    }

    PositionRecord* empty_position(std::string_view instrument) noexcept
    {
        if (position_capacity == 0) return nullptr;
        const std::size_t first = text_hash(instrument) % position_capacity;
        for (std::size_t probe = 0; probe < position_capacity; ++probe) {
            PositionRecord& record =
                positions[(first + probe) % position_capacity];
            if (!record.occupied) return &record;
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
    std::unique_ptr<TradeRecord[]> trades;
    std::size_t position_capacity{0};
    std::unique_ptr<PositionRecord[]> positions;
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
    if (OrderRecord* existing = impl_->find_order(seed.client_order_id)) {
        return same_seed(existing->seed, seed)
            ? ApplyResult{ApplyCode::Duplicate}
            : impl_->conflict(existing);
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
    const bool duplicate =
        (event == LocalOrderEvent::RiskAccepted
         && before == OrderState::RiskAccepted)
        || (event == LocalOrderEvent::RiskRejected
            && before == OrderState::RiskRejected)
        || (event == LocalOrderEvent::SubmitRejected
            && before == OrderState::SubmitRejectedLocally)
        || (event == LocalOrderEvent::SubmitResultUnknown
            && before == OrderState::SubmitResultUnknown)
        || (event == LocalOrderEvent::Submitted
            && before == OrderState::Submitted)
        || (event == LocalOrderEvent::CancelRequested
            && before == OrderState::CancelRequested);
    if (duplicate) return {ApplyCode::Duplicate};

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
    const OrderState before = record->snapshot.state;
    const bool locally_impossible = before == OrderState::RiskRejected
        || before == OrderState::SubmitRejectedLocally;

    const bool invalid_report =
        (report.type == OrderReportType::Rejected
         && report.cumulative_filled != 0)
        || (report.type == OrderReportType::PartiallyFilled
            && (report.cumulative_filled == 0
                || report.cumulative_filled == record->seed.quantity))
        || (report.type == OrderReportType::Filled
            && report.cumulative_filled != record->seed.quantity)
        || (report.type == OrderReportType::Canceled
            && report.cumulative_filled == record->seed.quantity);
    if (invalid_report || locally_impossible) {
        return impl_->conflict(record);
    }
    if (report.cumulative_filled < previous_fill) {
        return {ApplyCode::Stale};
    }

    OrderState after = before;
    switch (report.type) {
    case OrderReportType::Accepted:
        if (before == OrderState::Created
            || before == OrderState::RiskAccepted
            || before == OrderState::Submitted) {
            after = OrderState::Accepted;
        }
        break;
    case OrderReportType::Rejected:
        if (before == OrderState::Created
            || before == OrderState::RiskAccepted
            || before == OrderState::Submitted
            || before == OrderState::Accepted) {
            after = OrderState::Rejected;
        } else if (before != OrderState::Rejected) {
            return impl_->conflict(record);
        }
        break;
    case OrderReportType::PartiallyFilled:
        if (before == OrderState::Rejected) {
            return impl_->conflict(record);
        }
        if (before != OrderState::Filled
            && before != OrderState::Canceled
            && before != OrderState::CancelRequested) {
            after = OrderState::PartiallyFilled;
        }
        break;
    case OrderReportType::Filled:
        if (before == OrderState::Rejected) {
            record->snapshot.reported_filled = report.cumulative_filled;
            record->snapshot.state = OrderState::Filled;
            impl_->reconciliation = true;
            record->snapshot.reconciliation_required = true;
            return {ApplyCode::Conflict, true,
                    previous_fill != report.cumulative_filled,
                    false, true};
        }
        after = OrderState::Filled;
        break;
    case OrderReportType::Canceled:
        if (before == OrderState::Rejected) {
            return impl_->conflict(record);
        }
        if (before != OrderState::Filled) {
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

    record->snapshot.reported_filled = report.cumulative_filled;
    const bool fill_changed = previous_fill != report.cumulative_filled;
    const bool state_changed = before != after;
    if (!state_changed && !fill_changed) {
        return is_terminal(before) || before == OrderState::PartiallyFilled
            || before == OrderState::CancelRequested
            ? ApplyResult{ApplyCode::Stale}
            : ApplyResult{ApplyCode::Duplicate};
    }
    record->snapshot.state = after;
    return {ApplyCode::Applied, state_changed, fill_changed};
}

ApplyResult AccountTradingState::apply_trade(const TradeReport& report) noexcept
{
    if (!has_text(report.trading_day) || !has_text(report.exchange_id)
        || !has_text(report.trade_id) || !has_text(report.instrument)
        || report.quantity <= 0) {
        return impl_->conflict();
    }

    if (TradeRecord* duplicate = impl_->find_trade(report)) {
        return same_trade(duplicate->report, report)
            ? ApplyResult{ApplyCode::Duplicate}
            : impl_->conflict(
                  impl_->find_order(report.client_order_id));
    }

    OrderRecord* order = impl_->find_order(report.client_order_id);
    if (order == nullptr) {
        impl_->reconciliation = true;
        return {ApplyCode::UnknownOrder, false, false, false, true};
    }
    if (order->seed.instrument != report.instrument
        || order->seed.direction != report.direction
        || order->seed.offset != report.offset
        || order->snapshot.accounted_filled + report.quantity
               > order->seed.quantity) {
        return impl_->conflict(order);
    }

    TradeRecord* trade = impl_->empty_trade(report);
    if (trade == nullptr) {
        impl_->reconciliation = true;
        order->snapshot.reconciliation_required = true;
        return {ApplyCode::CapacityExceeded, false, false, false, true};
    }

    std::string_view instrument{
        report.instrument.data(),
        static_cast<std::size_t>(std::find(
            report.instrument.begin(), report.instrument.end(), '\0')
            - report.instrument.begin())};
    PositionRecord* position = impl_->find_position(instrument);
    if (position == nullptr) {
        position = impl_->empty_position(instrument);
        if (position == nullptr) {
            impl_->reconciliation = true;
            order->snapshot.reconciliation_required = true;
            return {ApplyCode::CapacityExceeded, false, false, false, true};
        }
        position->occupied = true;
        position->snapshot.instrument = report.instrument;
    }

    // 成交去重记录必须先于订单和持仓副作用落下；此后重复回报只能命中它。
    trade->occupied = true;
    trade->report = report;
    order->snapshot.accounted_filled += report.quantity;

    bool position_changed = true;
    bool inconsistent_position = false;
    if (report.offset == Offset::Open && report.direction == Direction::Buy) {
        position->snapshot.long_quantity += report.quantity;
    } else if (report.offset == Offset::Open) {
        position->snapshot.short_quantity += report.quantity;
    } else if (report.direction == Direction::Sell
               && position->snapshot.long_quantity >= report.quantity) {
        position->snapshot.long_quantity -= report.quantity;
    } else if (report.direction == Direction::Buy
               && position->snapshot.short_quantity >= report.quantity) {
        position->snapshot.short_quantity -= report.quantity;
    } else {
        position->snapshot.known = false;
        position_changed = false;
        inconsistent_position = true;
    }

    const OrderState before = order->snapshot.state;
    const bool incompatible_terminal =
        before == OrderState::RiskRejected
        || before == OrderState::SubmitRejectedLocally
        || before == OrderState::Rejected;
    // 撤单只取消未成交余量；迟到但有效的成交仍可把 Canceled 推进为 Filled。
    order->snapshot.state =
        order->snapshot.accounted_filled == order->seed.quantity
        ? OrderState::Filled
        : OrderState::PartiallyFilled;

    if (incompatible_terminal || inconsistent_position) {
        impl_->reconciliation = true;
        order->snapshot.reconciliation_required = true;
        return {ApplyCode::Conflict,
                before != order->snapshot.state,
                true,
                position_changed,
                true};
    }
    return {ApplyCode::Applied,
            before != order->snapshot.state,
            true,
            position_changed,
            order->snapshot.reconciliation_required};
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
    std::string_view instrument,
    PositionSnapshot& snapshot) const noexcept
{
    const PositionRecord* record = impl_->find_position(instrument);
    if (record == nullptr) return false;
    snapshot = record->snapshot;
    return true;
}

bool AccountTradingState::reconciliation_required() const noexcept
{
    return impl_->reconciliation;
}

}
