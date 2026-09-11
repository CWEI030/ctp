#include "ctp/trading.hpp"
#include "ctp/engine.hpp"
#include "ctp/telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

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

void AccountTradingState::begin_position_reconciliation() noexcept
{
    for (std::size_t index = 0; index < impl_->position_capacity; ++index) {
        impl_->positions[index] = {};
    }
    impl_->reconciliation = true;
}

ApplyResult AccountTradingState::set_reconciled_position(
    std::string_view instrument,
    std::int32_t long_quantity,
    std::int32_t short_quantity) noexcept
{
    if (instrument.empty() || long_quantity < 0 || short_quantity < 0) {
        return impl_->conflict();
    }
    PositionRecord* record = impl_->find_position(instrument);
    if (record == nullptr) record = impl_->empty_position(instrument);
    if (record == nullptr) {
        return {ApplyCode::CapacityExceeded, false, false, false, true};
    }
    if (!record->occupied) {
        record->occupied = true;
        copy_to_field(record->snapshot.instrument, instrument);
    }
    record->snapshot.long_quantity = long_quantity;
    record->snapshot.short_quantity = short_quantity;
    record->snapshot.known = true;
    return {ApplyCode::Applied, true, false, true, true};
}

void AccountTradingState::complete_reconciliation() noexcept
{
    impl_->reconciliation = false;
}

RiskRejectReason evaluate_risk(
    const OrderIntent& intent,
    const RiskSnapshot& snapshot,
    const RiskLimits& limits) noexcept
{
    if (!snapshot.enabled) return RiskRejectReason::AccountDisabled;
    if (!snapshot.authenticated) return RiskRejectReason::NotAuthenticated;
    if (!snapshot.logged_in) return RiskRejectReason::NotLoggedIn;
    if (snapshot.frozen) return RiskRejectReason::AccountFrozen;
    if (!snapshot.reconciled) return RiskRejectReason::NotReconciled;
    if (snapshot.exiting) return RiskRejectReason::Exiting;
    if (!snapshot.trading_window_open) {
        return RiskRejectReason::OutsideTradingWindow;
    }
    if (snapshot.global_kill_switch || snapshot.account_kill_switch) {
        return RiskRejectReason::KillSwitch;
    }
    if (snapshot.result_unknown) return RiskRejectReason::ResultUnknown;
    if (intent.instrument != limits.allowed_instrument) {
        return RiskRejectReason::InstrumentNotAllowed;
    }
    if ((intent.direction == Direction::Buy && !limits.allow_buy)
        || (intent.direction == Direction::Sell && !limits.allow_sell)) {
        return RiskRejectReason::DirectionNotAllowed;
    }
    if ((intent.offset == Offset::Open && !limits.allow_open)
        || (intent.offset == Offset::Close && !limits.allow_close)) {
        return RiskRejectReason::OffsetNotAllowed;
    }
    if (intent.quantity <= 0 || intent.quantity > limits.max_order_volume) {
        return RiskRejectReason::InvalidQuantity;
    }
    if (intent.offset == Offset::Open
        && snapshot.daily_signals >= limits.max_daily_signals) {
        return RiskRejectReason::DailySignalLimit;
    }
    if (snapshot.daily_orders >= limits.max_daily_orders) {
        return RiskRejectReason::DailyOrderLimit;
    }
    if (snapshot.orders_in_rate_window
        >= limits.max_order_rate_per_second) {
        return RiskRejectReason::OrderRateLimit;
    }
    if (!snapshot.market_valid
        || snapshot.bid_price_ticks <= 0
        || snapshot.ask_price_ticks <= 0
        || snapshot.bid_price_ticks > snapshot.ask_price_ticks
        || intent.limit_price_ticks <= 0) {
        return RiskRejectReason::InvalidMarket;
    }
    if (snapshot.now_ns < snapshot.market_receive_ns
        || snapshot.now_ns - snapshot.market_receive_ns
            > limits.max_market_age_ns) {
        return RiskRejectReason::StaleMarket;
    }
    if (limits.max_slippage_ticks < 0) {
        return RiskRejectReason::PriceProtection;
    }
    // 先确认报价位于盘口外侧，再计算正数价差；这样盘口接近 int64
    // 边界时无需直接加减保护 tick，避免有符号整数溢出。
    const bool outside_price_limit = intent.direction == Direction::Buy
        ? intent.limit_price_ticks > snapshot.ask_price_ticks
            && intent.limit_price_ticks - snapshot.ask_price_ticks
                > limits.max_slippage_ticks
        : intent.limit_price_ticks < snapshot.bid_price_ticks
            && snapshot.bid_price_ticks - intent.limit_price_ticks
                > limits.max_slippage_ticks;
    if (outside_price_limit) return RiskRejectReason::PriceProtection;

    if (intent.offset == Offset::Open) {
        if (!snapshot.funds_known) return RiskRejectReason::FundsUnknown;
        const auto quantity = static_cast<std::int64_t>(intent.quantity);
        if (limits.margin_per_lot < 0
            || limits.minimum_available_after_order < 0
            || limits.minimum_available_after_order
                > std::numeric_limits<std::int64_t>::max()
                    - quantity * std::min(
                        limits.margin_per_lot,
                        std::numeric_limits<std::int64_t>::max() / quantity)
            || (limits.margin_per_lot > 0
                && quantity
                    > (std::numeric_limits<std::int64_t>::max()
                       - limits.minimum_available_after_order)
                        / limits.margin_per_lot)
            || snapshot.available_funds
                < quantity * limits.margin_per_lot
                    + limits.minimum_available_after_order) {
            return RiskRejectReason::InsufficientFunds;
        }
        if (snapshot.active_open_orders >= limits.max_active_open_orders) {
            return RiskRejectReason::TooManyActiveOpenOrders;
        }
        const auto net = snapshot.long_position - snapshot.short_position;
        const auto next_net = intent.direction == Direction::Buy
            ? net + intent.quantity : net - intent.quantity;
        if (next_net > limits.max_net_open_position
            || next_net < -limits.max_net_open_position) {
            return RiskRejectReason::NetPositionLimit;
        }
    } else {
        if (!snapshot.positions_known) {
            return RiskRejectReason::PositionsUnknown;
        }
        const auto closable = intent.direction == Direction::Sell
            ? snapshot.closable_long : snapshot.closable_short;
        if (closable < intent.quantity) {
            return RiskRejectReason::InsufficientPosition;
        }
    }
    return RiskRejectReason::None;
}

namespace {

struct SignalRecord {
    bool occupied{false};
    bool active_open{false};
    bool cancel_requested{false};
    std::uint8_t traced_order_reports{0};
    std::uint32_t market_events_waited{0};
    OrderIntent intent{};
    SubmitResult result{};
    bool seen_during_recovery{false};
};

enum class CallbackType : std::uint8_t {
    Order,
    Trade,
    InsertRejected,
    CancelRejected,
    Unknown,
    Disconnected,
    Connected,
    Authenticated,
    LoggedIn,
    QueryOrder,
    QueryTrade,
    QueryPosition,
    QueryFunds,
};

struct CallbackEvent {
    CallbackType type{CallbackType::Order};
    std::array<char, 13> order_ref{};
    OrderReportType order_type{OrderReportType::Accepted};
    std::int32_t cumulative_filled{0};
    TradeReport trade{};
    CThostFtdcRspUserLoginField login{};
    CThostFtdcOrderField queried_order{};
    CThostFtdcTradeField queried_trade{};
    CThostFtdcInvestorPositionField queried_position{};
    CThostFtdcTradingAccountField queried_account{};
    int error_id{0};
    int request_id{0};
    bool has_payload{false};
    bool is_last{false};
};

bool same_intent(const OrderIntent& left, const OrderIntent& right) noexcept
{
    return left.signal_id == right.signal_id
        && left.instrument == right.instrument
        && left.direction == right.direction
        && left.offset == right.offset
        && left.quantity == right.quantity
        && left.limit_price_ticks == right.limit_price_ticks
        && left.purpose == right.purpose
        && left.attempt == right.attempt;
}

bool queried_order_type(
    const CThostFtdcOrderField& order,
    OrderReportType& type) noexcept
{
    if (order.OrderSubmitStatus == THOST_FTDC_OSS_InsertRejected) {
        type = OrderReportType::Rejected;
        return true;
    }
    if (order.OrderStatus == THOST_FTDC_OST_AllTraded) {
        type = OrderReportType::Filled;
        return true;
    }
    if (order.OrderStatus == THOST_FTDC_OST_PartTradedQueueing
        || order.OrderStatus == THOST_FTDC_OST_PartTradedNotQueueing) {
        type = OrderReportType::PartiallyFilled;
        return true;
    }
    if (order.OrderStatus == THOST_FTDC_OST_Canceled) {
        type = OrderReportType::Canceled;
        return true;
    }
    if (order.OrderStatus == THOST_FTDC_OST_NoTradeQueueing) {
        type = OrderReportType::Accepted;
        return true;
    }
    return false;
}

struct ExitState {
    bool pending{false};
    bool active{false};
    bool freeze_after_cancel{false};
    std::array<char, kInstrumentIdCapacity> instrument{};
    Direction direction{Direction::Sell};
    std::uint64_t signal_id{0};
    std::uint64_t active_client_order_id{0};
    std::int32_t pending_quantity{0};
    std::uint32_t attempt{0};
    std::uint32_t market_events_waited{0};
};

template <std::size_t N>
std::string_view field_view(const std::array<char, N>& field) noexcept
{
    const auto end = std::find(field.begin(), field.end(), '\0');
    return {field.data(), static_cast<std::size_t>(end - field.begin())};
}

}

struct AccountTradingSession::Impl {
    Impl(
        const AccountConfig& source,
        RiskLimits risk_limits,
        std::unique_ptr<TraderApi> trader_api,
        std::size_t order_capacity,
        std::size_t trade_capacity,
        std::size_t signal_capacity,
        std::size_t callback_capacity,
        std::uint64_t first_client_order_id,
        std::uint64_t first_order_ref,
        AutoClosePolicy close_policy,
        TraceSink* trace_output,
        std::uint64_t trace_run_id,
        double price_increment)
        : account_id(source.alias()),
          broker_id(source.broker_id()),
          user_id(source.user_id()),
          password(source.password()),
          app_id(source.app_id()),
          auth_code(source.auth_code()),
          trader_front(source.trader_front()),
          limits(std::move(risk_limits)),
          api(std::move(trader_api)),
          state(source.alias(), order_capacity, trade_capacity),
          signals(signal_capacity),
          callbacks(callback_capacity + 1),
          callback_enqueue_ns(callback_capacity + 1),
          next_client_order_id(first_client_order_id),
          next_order_ref(first_order_ref),
          policy(close_policy),
          trace_sink(trace_output),
          run_id(trace_run_id),
          minimum_price_increment(price_increment)
    {
    }

    std::string account_id;
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string app_id;
    std::string auth_code;
    std::string trader_front;
    RiskLimits limits;
    std::unique_ptr<TraderApi> api;
    AccountTradingState state;
    std::vector<SignalRecord> signals;
    std::vector<CallbackEvent> callbacks;
    std::vector<std::atomic<std::int64_t>> callback_enqueue_ns;
    std::atomic<std::size_t> callback_write{0};
    std::atomic<std::size_t> callback_read{0};
    std::atomic<bool> callback_overflow{false};
    std::atomic<std::size_t> callback_high_watermark{0};
    std::atomic<std::uint64_t> callback_dropped{0};
    std::atomic<std::uint64_t> order_requests{0};
    std::atomic<std::uint64_t> order_acceptances{0};
    std::atomic<std::uint64_t> order_rejections{0};
    std::atomic<std::uint64_t> cancel_requests{0};
    std::atomic<std::uint64_t> cancel_acceptances{0};
    std::atomic<std::uint64_t> trades{0};
    std::atomic<std::uint64_t> traded_volume{0};
    std::atomic<std::uint64_t> freeze_transitions{0};
    std::atomic<std::uint64_t> recovery_completions{0};
    std::uint64_t next_client_order_id{1};
    std::uint64_t next_order_ref{1};
    std::uint32_t daily_signals{0};
    std::uint32_t daily_orders{0};
    std::int32_t active_open_orders{0};
    int front_id{0};
    int session_id{0};
    int next_request_id{1};
    int expected_recovery_request_id{0};
    int next_action_ref{1};
    std::uint32_t daily_cancels{0};
    std::array<char, kTradingDayCapacity> trading_day{};
    std::array<char, kTradingDayCapacity> restored_trading_day{};
    std::uint32_t restored_daily_signals{0};
    std::uint32_t restored_daily_orders{0};
    std::uint32_t restored_daily_cancels{0};
    bool restored_daily_limits_pending{false};
    bool reconciliation{false};
    bool frozen{false};
    AccountFault fault{AccountFault::None};
    std::uint32_t alert_count{0};
    std::uint32_t close_orders_submitted{0};
    std::uint32_t close_reprices{0};
    AccountAcceptanceSnapshot acceptance{};
    AutoClosePolicy policy{};
    ExitState exit{};
    RecoverySnapshot recovery{};
    TraceSink* trace_sink{nullptr};
    std::uint64_t run_id{1};
    std::uint64_t trace_sequence{0};
    double minimum_price_increment{1.0};

    void trace(
        TraceStage stage,
        const SignalRecord* record = nullptr,
        std::int32_t code = 0,
        std::int32_t quantity = 0) noexcept
    {
        if (trace_sink == nullptr) return;
        TraceEvent event{};
        event.trace_id.run_id = run_id;
        event.sequence = ++trace_sequence;
        event.stage = stage;
        event.code = code;
        event.quantity = quantity;
        if (record != nullptr) {
            event.trace_id.signal_id = record->intent.signal_id;
            event.client_order_id = record->result.client_order_id;
            event.order_ref = record->result.order_ref;
            event.limit_price_ticks = record->intent.limit_price_ticks;
            event.quantity = quantity == 0 ? record->intent.quantity : quantity;
            event.attempt = record->intent.attempt;
            event.direction = static_cast<std::uint8_t>(record->intent.direction);
            event.offset = static_cast<std::uint8_t>(record->intent.offset);
            event.purpose = static_cast<std::uint8_t>(record->intent.purpose);
            event.instrument = record->intent.instrument;
        }
        trace_sink->try_record(event);
    }

    void trace_order_report(
        SignalRecord& record,
        OrderReportType type,
        const ApplyResult& result,
        std::int32_t cumulative_filled) noexcept
    {
        if (result.code != ApplyCode::Applied) return;
        const auto bit = static_cast<std::uint8_t>(
            1U << static_cast<std::uint8_t>(type));
        if ((record.traced_order_reports & bit) != 0) return;
        record.traced_order_reports = static_cast<std::uint8_t>(
            record.traced_order_reports | bit);
        trace(
            TraceStage::OrderReport,
            &record,
            static_cast<std::int32_t>(type),
            cumulative_filled);
    }

    void release_terminal_open(SignalRecord& record) noexcept
    {
        OrderSnapshot snapshot{};
        if (record.active_open
            && state.order_snapshot(record.result.client_order_id, snapshot)
            && is_terminal(snapshot.state)) {
            record.active_open = false;
            --active_open_orders;
        }
    }

    void apply_trade_effect(
        SignalRecord& record,
        const TradeReport& trade,
        const ApplyResult& result) noexcept
    {
        if (!policy.enabled || result.code != ApplyCode::Applied
            || !result.position_changed) {
            return;
        }
        if (record.intent.offset == Offset::Open) {
            const bool incompatible = (exit.pending || exit.active)
                && (exit.signal_id != record.intent.signal_id
                    || exit.instrument != trade.instrument);
            if (incompatible
                || trade.quantity > std::numeric_limits<std::int32_t>::max()
                        - exit.pending_quantity) {
                set_fault(AccountFault::StateConflict);
                return;
            }
            exit.pending = true;
            exit.instrument = trade.instrument;
            exit.direction = trade.direction == Direction::Buy
                ? Direction::Sell : Direction::Buy;
            exit.signal_id = record.intent.signal_id;
            exit.pending_quantity += trade.quantity;
            return;
        }
        PositionSnapshot held{};
        if (state.position_snapshot(field_view(trade.instrument), held)
            && held.long_quantity == 0 && held.short_quantity == 0) {
            exit = {};
        }
    }

    void set_fault(AccountFault reason) noexcept
    {
        // 故障原因保持首次值，但账户每次出现缺口都必须重新冻结。
        if (!frozen) freeze_transitions.fetch_add(1, std::memory_order_relaxed);
        frozen = true;
        reconciliation = true;
        if (fault != AccountFault::None) return;
        fault = reason;
        ++alert_count;
    }

    void recovery_failure(RecoveryFailure reason) noexcept
    {
        if (recovery.phase == RecoveryPhase::Frozen) return;
        recovery.phase = RecoveryPhase::Frozen;
        recovery.failure = reason;
        set_fault(AccountFault::RecoveryFailed);
        trace(TraceStage::RecoveryFrozen, nullptr,
              static_cast<std::int32_t>(reason));
    }

    int request_authentication() noexcept
    {
        CThostFtdcReqAuthenticateField request{};
        copy_to_field(request.BrokerID, broker_id);
        copy_to_field(request.UserID, user_id);
        copy_to_field(request.AppID, app_id);
        copy_to_field(request.AuthCode, auth_code);
        recovery.phase = RecoveryPhase::Authenticating;
        expected_recovery_request_id = next_request_id++;
        return api->request_authenticate(
            &request, expected_recovery_request_id);
    }

    int request_login() noexcept
    {
        CThostFtdcReqUserLoginField request{};
        copy_to_field(request.BrokerID, broker_id);
        copy_to_field(request.UserID, user_id);
        copy_to_field(request.Password, password);
        recovery.phase = RecoveryPhase::LoggingIn;
        expected_recovery_request_id = next_request_id++;
        return api->request_user_login(&request, expected_recovery_request_id);
    }

    int request_orders() noexcept
    {
        CThostFtdcQryOrderField request{};
        copy_to_field(request.BrokerID, broker_id);
        copy_to_field(request.InvestorID, user_id);
        recovery.phase = RecoveryPhase::QueryingOrders;
        expected_recovery_request_id = next_request_id++;
        return api->request_order_query(&request, expected_recovery_request_id);
    }

    int request_trades() noexcept
    {
        CThostFtdcQryTradeField request{};
        copy_to_field(request.BrokerID, broker_id);
        copy_to_field(request.InvestorID, user_id);
        recovery.phase = RecoveryPhase::QueryingTrades;
        expected_recovery_request_id = next_request_id++;
        return api->request_trade_query(&request, expected_recovery_request_id);
    }

    int request_positions() noexcept
    {
        CThostFtdcQryInvestorPositionField request{};
        copy_to_field(request.BrokerID, broker_id);
        copy_to_field(request.InvestorID, user_id);
        state.begin_position_reconciliation();
        recovery.phase = RecoveryPhase::QueryingPositions;
        expected_recovery_request_id = next_request_id++;
        return api->request_investor_position(
            &request, expected_recovery_request_id);
    }

    int request_funds() noexcept
    {
        CThostFtdcQryTradingAccountField request{};
        copy_to_field(request.BrokerID, broker_id);
        copy_to_field(request.InvestorID, user_id);
        recovery.phase = RecoveryPhase::QueryingFunds;
        expected_recovery_request_id = next_request_id++;
        return api->request_trading_account(
            &request, expected_recovery_request_id);
    }

    int begin_query_reconciliation() noexcept
    {
        frozen = true;
        reconciliation = true;
        recovery.failure = RecoveryFailure::None;
        recovery.queried_orders = 0;
        recovery.queried_trades = 0;
        recovery.queried_positions = 0;
        for (auto& record : signals) {
            record.seen_during_recovery = false;
        }
        return request_orders();
    }

    void push_callback(const CallbackEvent& event) noexcept
    {
        const auto write = callback_write.load(std::memory_order_relaxed);
        const auto next = (write + 1) % callbacks.size();
        if (next == callback_read.load(std::memory_order_acquire)) {
            callback_overflow.store(true, std::memory_order_release);
            callback_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        callbacks[write] = event;
        callback_enqueue_ns[write].store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);
        callback_write.store(next, std::memory_order_release);
        const auto read = callback_read.load(std::memory_order_acquire);
        const auto depth = next >= read
            ? next - read : callbacks.size() - read + next;
        auto previous = callback_high_watermark.load(std::memory_order_relaxed);
        while (previous < depth
               && !callback_high_watermark.compare_exchange_weak(
                   previous, depth, std::memory_order_relaxed)) {
        }
    }

    bool pop_callback(CallbackEvent& event) noexcept
    {
        const auto read = callback_read.load(std::memory_order_relaxed);
        if (read == callback_write.load(std::memory_order_acquire)) return false;
        event = callbacks[read];
        callback_enqueue_ns[read].store(0, std::memory_order_relaxed);
        callback_read.store(
            (read + 1) % callbacks.size(), std::memory_order_release);
        return true;
    }

    SignalRecord* find_order(std::uint64_t client_order_id) noexcept
    {
        for (auto& record : signals) {
            if (record.occupied
                && record.result.client_order_id == client_order_id) {
                return &record;
            }
        }
        return nullptr;
    }

    SignalRecord* find_order_ref(std::uint64_t order_ref) noexcept
    {
        for (auto& record : signals) {
            if (record.occupied && record.result.order_ref == order_ref) {
                return &record;
            }
        }
        return nullptr;
    }
};

AccountTradingSession::AccountTradingSession(
    const AccountConfig& account,
    RiskLimits limits,
    std::unique_ptr<TraderApi> api,
    std::size_t order_capacity,
    std::size_t trade_capacity,
    std::size_t signal_capacity,
    std::size_t callback_capacity,
    std::uint64_t next_client_order_id,
    std::uint64_t persisted_next_order_ref,
    AutoClosePolicy auto_close_policy,
    TraceSink* trace_sink,
    std::uint64_t run_id,
    double minimum_price_increment)
    : impl_(std::make_unique<Impl>(
          account,
          std::move(limits),
          std::move(api),
          order_capacity,
          trade_capacity,
          signal_capacity,
          callback_capacity,
          next_client_order_id,
          persisted_next_order_ref,
          auto_close_policy,
          trace_sink,
          run_id,
          minimum_price_increment))
{
    if (impl_->api) impl_->api->register_spi(this);
    if (!std::isfinite(impl_->minimum_price_increment)
        || impl_->minimum_price_increment <= 0.0) {
        impl_->set_fault(AccountFault::Configuration);
    }
    if (impl_->policy.enabled
        && (impl_->policy.close_reprice_interval_market_events == 0
            || impl_->policy.close_protection_ticks < 0)) {
        impl_->set_fault(AccountFault::Configuration);
    }
}

AccountTradingSession::~AccountTradingSession()
{
    if (impl_->api) {
        impl_->api->register_spi(nullptr);
        impl_->api->release();
    }
}

bool AccountTradingSession::activate(
    int front_id,
    int session_id,
    std::string_view max_order_ref,
    std::string_view trading_day) noexcept
{
    const bool valid_day = trading_day.size() == 8
        && std::all_of(trading_day.begin(), trading_day.end(), [](char value) {
               return value >= '0' && value <= '9';
           });
    if (!valid_day) return false;

    impl_->front_id = front_id;
    impl_->session_id = session_id;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        max_order_ref.data(), max_order_ref.data() + max_order_ref.size(), parsed);
    if (result.ec == std::errc{} && result.ptr == max_order_ref.data()
            + max_order_ref.size()
        && parsed < std::numeric_limits<std::uint64_t>::max()) {
        impl_->next_order_ref = std::max(impl_->next_order_ref, parsed + 1);
    }
    const auto previous_day = field_view(impl_->trading_day);
    if (impl_->restored_daily_limits_pending) {
        if (field_view(impl_->restored_trading_day) == trading_day) {
            impl_->daily_signals = impl_->restored_daily_signals;
            impl_->daily_orders = impl_->restored_daily_orders;
            impl_->daily_cancels = impl_->restored_daily_cancels;
        } else {
            impl_->daily_signals = 0;
            impl_->daily_orders = 0;
            impl_->daily_cancels = 0;
        }
        impl_->restored_daily_limits_pending = false;
    } else if (!previous_day.empty() && previous_day != trading_day) {
        impl_->daily_signals = 0;
        impl_->daily_orders = 0;
        impl_->daily_cancels = 0;
    }
    copy_to_field(impl_->trading_day, trading_day);
    return true;
}

void AccountTradingSession::start()
{
    if (!impl_->api || impl_->recovery.phase != RecoveryPhase::Idle) return;
    impl_->recovery.phase = RecoveryPhase::Connecting;
    impl_->frozen = true;
    impl_->reconciliation = true;
    impl_->api->subscribe_private_topic(THOST_TERT_QUICK, 0);
    impl_->api->subscribe_public_topic(THOST_TERT_QUICK);
    impl_->api->register_front(impl_->trader_front);
    impl_->api->init();
}

bool AccountTradingSession::restore_restart_image(
    const RestartImage& image) noexcept
{
    const bool valid_day = image.trading_day.size() == 8
        && std::all_of(
            image.trading_day.begin(),
            image.trading_day.end(),
            [](char value) { return value >= '0' && value <= '9'; });
    if (!image.valid || image.account_id != impl_->account_id || !valid_day
        || impl_->recovery.phase != RecoveryPhase::Idle
        || image.uncertain_orders.size() > impl_->signals.size()
        || image.daily_signals > impl_->limits.max_daily_signals
        || image.daily_orders > impl_->limits.max_daily_orders
        || image.daily_cancels > impl_->limits.max_daily_cancels) {
        return false;
    }
    for (const auto& restored : image.uncertain_orders) {
        if (restored.trace_id.run_id == 0 || restored.trace_id.signal_id == 0
            || restored.client_order_id == 0 || restored.order_ref == 0
            || restored.quantity <= 0
            || field_view(restored.instrument).empty()
            || image.next_client_order_id <= restored.client_order_id
            || image.next_order_ref <= restored.order_ref
            || restored.direction > static_cast<std::uint8_t>(Direction::Sell)
            || restored.offset > static_cast<std::uint8_t>(Offset::Close)
            || restored.purpose > static_cast<std::uint8_t>(OrderPurpose::Exit)) {
            return false;
        }
    }
    std::size_t index = 0;
    for (const auto& restored : image.uncertain_orders) {
        auto& record = impl_->signals[index++];
        record.occupied = true;
        record.intent.signal_id = restored.trace_id.signal_id;
        record.intent.instrument = restored.instrument;
        record.intent.direction = static_cast<Direction>(restored.direction);
        record.intent.offset = static_cast<Offset>(restored.offset);
        record.intent.quantity = restored.quantity;
        record.intent.limit_price_ticks = restored.limit_price_ticks;
        record.intent.purpose = static_cast<OrderPurpose>(restored.purpose);
        record.intent.attempt = restored.attempt;
        record.result.code = SubmitCode::Submitted;
        record.result.client_order_id = restored.client_order_id;
        record.result.order_ref = restored.order_ref;
        OrderSeed seed{};
        seed.client_order_id = restored.client_order_id;
        seed.instrument = restored.instrument;
        seed.direction = record.intent.direction;
        seed.offset = record.intent.offset;
        seed.quantity = restored.quantity;
        if (impl_->state.create_order(seed).code != ApplyCode::Applied
            || impl_->state.apply_local_event(
                   restored.client_order_id,
                   LocalOrderEvent::RiskAccepted).code != ApplyCode::Applied
            || impl_->state.apply_local_event(
                   restored.client_order_id,
                   LocalOrderEvent::Submitted).code != ApplyCode::Applied) {
            impl_->recovery_failure(RecoveryFailure::CapacityExceeded);
            return false;
        }
        if (record.intent.offset == Offset::Open) {
            record.active_open = true;
            ++impl_->active_open_orders;
        }
    }
    impl_->next_client_order_id = std::max(
        impl_->next_client_order_id, image.next_client_order_id);
    impl_->next_order_ref = std::max(
        impl_->next_order_ref, image.next_order_ref);
    copy_to_field(impl_->restored_trading_day, image.trading_day);
    impl_->restored_daily_signals = image.daily_signals;
    impl_->restored_daily_orders = image.daily_orders;
    impl_->restored_daily_cancels = image.daily_cancels;
    impl_->restored_daily_limits_pending = true;
    impl_->frozen = true;
    impl_->reconciliation = true;
    return true;
}

bool AccountTradingSession::checkpoint_restart_state() noexcept
{
    if (impl_->trace_sink == nullptr || field_view(impl_->trading_day).empty()) {
        return false;
    }
    TraceEvent event{};
    event.trace_id.run_id = impl_->run_id;
    event.sequence = ++impl_->trace_sequence;
    event.stage = TraceStage::RestartCheckpoint;
    event.trading_day = impl_->trading_day;
    event.daily_signals = impl_->daily_signals;
    event.daily_orders = impl_->daily_orders;
    event.daily_cancels = impl_->daily_cancels;
    return impl_->trace_sink->try_record(event);
}

DailyLimitSnapshot AccountTradingSession::daily_limit_snapshot() const noexcept
{
    return {impl_->daily_signals, impl_->daily_orders, impl_->daily_cancels};
}

RecoverySnapshot AccountTradingSession::recovery_snapshot() const noexcept
{
    return impl_->recovery;
}

CallbackQueueSnapshot AccountTradingSession::callback_queue_snapshot() const noexcept
{
    const auto read = impl_->callback_read.load(std::memory_order_acquire);
    const auto write = impl_->callback_write.load(std::memory_order_acquire);
    const auto depth = write >= read
        ? write - read : impl_->callbacks.size() - read + write;
    std::int64_t oldest_age_ns = 0;
    if (depth != 0) {
        const auto enqueued = impl_->callback_enqueue_ns[read].load(
            std::memory_order_acquire);
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (enqueued > 0 && now > enqueued) oldest_age_ns = now - enqueued;
    }
    return {
        depth,
        impl_->callbacks.size() - 1,
        impl_->callback_high_watermark.load(std::memory_order_relaxed),
        oldest_age_ns,
        impl_->callback_dropped.load(std::memory_order_relaxed)};
}

TradingEventSnapshot AccountTradingSession::event_snapshot() const noexcept
{
    return {
        impl_->order_requests.load(std::memory_order_relaxed),
        impl_->order_acceptances.load(std::memory_order_relaxed),
        impl_->order_rejections.load(std::memory_order_relaxed),
        impl_->cancel_requests.load(std::memory_order_relaxed),
        impl_->cancel_acceptances.load(std::memory_order_relaxed),
        impl_->trades.load(std::memory_order_relaxed),
        impl_->traded_volume.load(std::memory_order_relaxed),
        impl_->freeze_transitions.load(std::memory_order_relaxed),
        impl_->recovery_completions.load(std::memory_order_relaxed)};
}

void AccountTradingSession::trace_market(const MarketEvent& market) noexcept
{
    if (impl_->trace_sink == nullptr) return;
    TraceEvent event{};
    event.trace_id.run_id = impl_->run_id;
    event.sequence = ++impl_->trace_sequence;
    event.mono_ns = market.recv_mono_ns;
    event.limit_price_ticks = market.last_price_ticks;
    event.stage = TraceStage::Market;
    event.code = static_cast<std::int32_t>(market.status);
    event.instrument = market.instrument;
    impl_->trace_sink->try_record(event);
}

void AccountTradingSession::trace_signal(
    const MarketEvent& market,
    const OrderIntent& intent,
    std::int64_t decision_mono_ns) noexcept
{
    if (impl_->trace_sink == nullptr) return;
    TraceEvent trigger{};
    trigger.trace_id = {impl_->run_id, intent.signal_id};
    trigger.sequence = ++impl_->trace_sequence;
    trigger.mono_ns = market.recv_mono_ns;
    trigger.limit_price_ticks = market.last_price_ticks;
    trigger.stage = TraceStage::Market;
    trigger.code = static_cast<std::int32_t>(market.status);
    trigger.instrument = market.instrument;
    impl_->trace_sink->try_record(trigger);

    TraceEvent event{};
    event.trace_id = {impl_->run_id, intent.signal_id};
    event.sequence = ++impl_->trace_sequence;
    event.mono_ns = decision_mono_ns;
    event.limit_price_ticks = intent.limit_price_ticks;
    event.stage = TraceStage::Signal;
    event.quantity = intent.quantity;
    event.attempt = intent.attempt;
    event.direction = static_cast<std::uint8_t>(intent.direction);
    event.offset = static_cast<std::uint8_t>(intent.offset);
    event.purpose = static_cast<std::uint8_t>(intent.purpose);
    event.instrument = intent.instrument;
    impl_->trace_sink->try_record(event);
}

SubmitResult AccountTradingSession::submit(
    const OrderIntent& intent,
    const RiskSnapshot& snapshot) noexcept
{
    impl_->order_requests.fetch_add(1, std::memory_order_relaxed);
    SignalRecord* free_record = nullptr;
    for (auto& record : impl_->signals) {
        if (record.occupied
            && record.intent.signal_id == intent.signal_id
            && record.intent.purpose == intent.purpose
            && record.intent.attempt == intent.attempt) {
            auto duplicate = record.result;
            duplicate.code = same_intent(record.intent, intent)
                ? SubmitCode::Duplicate : SubmitCode::RejectedLocally;
            return duplicate;
        }
        if (!record.occupied && free_record == nullptr) free_record = &record;
    }
    if (free_record == nullptr) {
        return {SubmitCode::CapacityExceeded};
    }

    free_record->occupied = true;
    free_record->intent = intent;
    free_record->result.client_order_id = impl_->next_client_order_id++;
    if (intent.offset == Offset::Open) ++impl_->daily_signals;

    OrderSeed seed{};
    seed.client_order_id = free_record->result.client_order_id;
    seed.instrument = intent.instrument;
    seed.direction = intent.direction;
    seed.offset = intent.offset;
    seed.quantity = intent.quantity;
    if (impl_->state.create_order(seed).code != ApplyCode::Applied) {
        free_record->result.code = SubmitCode::CapacityExceeded;
        return free_record->result;
    }

    auto effective = snapshot;
    effective.daily_signals = std::max(
        snapshot.daily_signals,
        impl_->daily_signals - (intent.offset == Offset::Open ? 1U : 0U));
    effective.daily_orders = std::max(snapshot.daily_orders, impl_->daily_orders);
    effective.active_open_orders = std::max(
        snapshot.active_open_orders, impl_->active_open_orders);
    effective.reconciled = snapshot.reconciled
        && !impl_->reconciliation
        && !impl_->state.reconciliation_required()
        && !impl_->callback_overflow.load(std::memory_order_acquire);
    effective.frozen = snapshot.frozen || impl_->frozen;
    const auto reason = evaluate_risk(intent, effective, impl_->limits);
    if (reason != RiskRejectReason::None) {
        impl_->state.apply_local_event(
            free_record->result.client_order_id,
            LocalOrderEvent::RiskRejected);
        free_record->result.code = SubmitCode::RiskRejected;
        free_record->result.risk_reason = reason;
        impl_->trace(
            TraceStage::RiskRejected,
            free_record,
            static_cast<std::int32_t>(reason));
        return free_record->result;
    }
    impl_->state.apply_local_event(
        free_record->result.client_order_id, LocalOrderEvent::RiskAccepted);

    CThostFtdcInputOrderField request{};
    copy_to_field(request.BrokerID, impl_->broker_id);
    copy_to_field(request.InvestorID, impl_->user_id);
    copy_to_field(request.UserID, impl_->user_id);
    copy_to_field(request.InstrumentID, field_view(intent.instrument));
    constexpr std::uint64_t kLargestCtpOrderRef = 999'999'999'999ULL;
    if (impl_->next_order_ref > kLargestCtpOrderRef) {
        impl_->state.apply_local_event(
            free_record->result.client_order_id,
            LocalOrderEvent::SubmitRejected);
        free_record->result.code = SubmitCode::RejectedLocally;
        impl_->trace(TraceStage::OrderRejected, free_record, -1);
        return free_record->result;
    }
    const auto order_ref = impl_->next_order_ref++;
    const auto converted = std::to_chars(
        std::begin(request.OrderRef), std::end(request.OrderRef) - 1, order_ref);
    if (converted.ec != std::errc{}) {
        impl_->state.apply_local_event(
            free_record->result.client_order_id,
            LocalOrderEvent::SubmitRejected);
        free_record->result.code = SubmitCode::RejectedLocally;
        impl_->trace(TraceStage::OrderRejected, free_record, -1);
        return free_record->result;
    }
    *converted.ptr = '\0';
    request.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    request.Direction = intent.direction == Direction::Buy
        ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    request.CombOffsetFlag[0] = intent.offset == Offset::Open
        ? THOST_FTDC_OF_Open : THOST_FTDC_OF_Close;
    request.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    request.LimitPrice = static_cast<double>(intent.limit_price_ticks)
        * impl_->minimum_price_increment;
    if (!std::isfinite(request.LimitPrice)) {
        impl_->state.apply_local_event(
            free_record->result.client_order_id,
            LocalOrderEvent::SubmitRejected);
        free_record->result.code = SubmitCode::RejectedLocally;
        impl_->trace(TraceStage::OrderRejected, free_record, -1);
        return free_record->result;
    }
    request.VolumeTotalOriginal = intent.quantity;
    request.TimeCondition = THOST_FTDC_TC_GFD;
    request.VolumeCondition = THOST_FTDC_VC_AV;
    request.MinVolume = 1;
    request.ContingentCondition = THOST_FTDC_CC_Immediately;
    request.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    request.IsAutoSuspend = 0;
    request.UserForceClose = 0;

    free_record->result.order_ref = order_ref;
    impl_->trace(TraceStage::RiskAccepted, free_record);
    ++impl_->daily_orders;
    const int api_code = impl_->api == nullptr
        ? -1
        : impl_->api->request_order_insert(
              &request, impl_->next_request_id++);
    free_record->result.api_return_code = api_code;
    if (api_code != 0) {
        impl_->order_rejections.fetch_add(1, std::memory_order_relaxed);
        impl_->state.apply_local_event(
            free_record->result.client_order_id,
            LocalOrderEvent::SubmitRejected);
        free_record->result.code = SubmitCode::RejectedLocally;
        impl_->trace(TraceStage::OrderRejected, free_record, api_code);
        return free_record->result;
    }
    impl_->state.apply_local_event(
        free_record->result.client_order_id, LocalOrderEvent::Submitted);
    if (intent.offset == Offset::Open) {
        ++impl_->active_open_orders;
        free_record->active_open = true;
    }
    free_record->result.code = SubmitCode::Submitted;
    if (intent.offset == Offset::Open) {
        ++impl_->acceptance.entry_orders_submitted;
    } else {
        ++impl_->acceptance.exit_orders_submitted;
    }
    impl_->trace(TraceStage::OrderSubmitted, free_record);
    return free_record->result;
}

CancelResult AccountTradingSession::cancel(
    std::uint64_t client_order_id) noexcept
{
    impl_->cancel_requests.fetch_add(1, std::memory_order_relaxed);
    auto* record = impl_->find_order(client_order_id);
    if (record == nullptr) {
        return {CancelCode::UnknownOrder, client_order_id};
    }
    if (record->cancel_requested) {
        return {CancelCode::Duplicate, client_order_id};
    }
    OrderSnapshot snapshot{};
    if (!impl_->state.order_snapshot(client_order_id, snapshot)
        || is_terminal(snapshot.state)) {
        return {CancelCode::NotCancelable, client_order_id};
    }
    if (impl_->daily_cancels >= impl_->limits.max_daily_cancels) {
        return {CancelCode::DailyLimit, client_order_id};
    }
    const auto applied = impl_->state.apply_local_event(
        client_order_id, LocalOrderEvent::CancelRequested);
    if (applied.code != ApplyCode::Applied) {
        return {CancelCode::NotCancelable, client_order_id};
    }

    CThostFtdcInputOrderActionField action{};
    copy_to_field(action.BrokerID, impl_->broker_id);
    copy_to_field(action.InvestorID, impl_->user_id);
    copy_to_field(action.UserID, impl_->user_id);
    copy_to_field(action.InstrumentID, field_view(record->intent.instrument));
    const auto converted = std::to_chars(
        std::begin(action.OrderRef),
        std::end(action.OrderRef) - 1,
        record->result.order_ref);
    if (converted.ec != std::errc{}) {
        impl_->state.apply_order_report(
            {client_order_id, OrderReportType::CancelRejected, 0});
        return {CancelCode::RejectedLocally, client_order_id, -1};
    }
    *converted.ptr = '\0';
    action.FrontID = impl_->front_id;
    action.SessionID = impl_->session_id;
    action.OrderActionRef = impl_->next_action_ref++;
    action.ActionFlag = THOST_FTDC_AF_Delete;
    record->cancel_requested = true;
    ++impl_->daily_cancels;
    const int api_code = impl_->api == nullptr
        ? -1
        : impl_->api->request_order_action(
              &action, impl_->next_request_id++);
    if (api_code != 0) {
        impl_->state.apply_order_report(
            {client_order_id, OrderReportType::CancelRejected, 0});
        return {CancelCode::RejectedLocally, client_order_id, api_code};
    }
    impl_->trace(TraceStage::CancelRequested, record);
    return {CancelCode::Requested, client_order_id, 0};
}

ExecutionStepResult AccountTradingSession::on_market(
    const MarketEvent& market,
    const RiskSnapshot& snapshot) noexcept
{
    ExecutionStepResult step{};
    if (impl_->frozen || !impl_->policy.enabled) return step;
    if (market.status != MarketDataStatus::Valid
        || market.instrument != impl_->limits.allowed_instrument
        || market.bid_price_ticks <= 0
        || market.ask_price_ticks <= 0
        || market.bid_price_ticks > market.ask_price_ticks) {
        return step;
    }

    if (impl_->exit.pending && !impl_->exit.active) {
        PositionSnapshot held{};
        if (!impl_->state.position_snapshot(
                field_view(impl_->exit.instrument), held)
            || !held.known) {
            impl_->set_fault(AccountFault::StateConflict);
            step.action = ExecutionAction::Frozen;
            return step;
        }
        const auto closable = impl_->exit.direction == Direction::Sell
            ? held.long_quantity : held.short_quantity;
        if (closable <= 0) {
            impl_->exit = {};
            return step;
        }

        std::int64_t price = 0;
        if (impl_->exit.direction == Direction::Sell) {
            if (market.bid_price_ticks <= impl_->policy.close_protection_ticks) {
                impl_->set_fault(AccountFault::PriceOverflow);
                step.action = ExecutionAction::Frozen;
                return step;
            }
            price = market.bid_price_ticks
                - impl_->policy.close_protection_ticks;
        } else {
            if (market.ask_price_ticks
                > std::numeric_limits<std::int64_t>::max()
                    - impl_->policy.close_protection_ticks) {
                impl_->set_fault(AccountFault::PriceOverflow);
                step.action = ExecutionAction::Frozen;
                return step;
            }
            price = market.ask_price_ticks
                + impl_->policy.close_protection_ticks;
        }

        OrderIntent intent{};
        intent.signal_id = impl_->exit.signal_id;
        intent.instrument = impl_->exit.instrument;
        intent.direction = impl_->exit.direction;
        intent.offset = Offset::Close;
        intent.quantity = std::min(closable, impl_->exit.pending_quantity);
        intent.limit_price_ticks = price;
        intent.purpose = OrderPurpose::Exit;
        intent.attempt = impl_->exit.attempt;

        SignalRecord exit_trace{};
        exit_trace.intent = intent;
        impl_->trace(TraceStage::ExitIntent, &exit_trace);

        auto effective = snapshot;
        effective.market_valid = true;
        effective.market_receive_ns = market.recv_mono_ns;
        effective.bid_price_ticks = market.bid_price_ticks;
        effective.ask_price_ticks = market.ask_price_ticks;
        effective.positions_known = held.known;
        effective.long_position = held.long_quantity;
        effective.short_position = held.short_quantity;
        effective.closable_long = held.long_quantity;
        effective.closable_short = held.short_quantity;
        step.submit = submit(intent, effective);
        if (step.submit.code != SubmitCode::Submitted) {
            impl_->set_fault(step.submit.code == SubmitCode::CapacityExceeded
                    ? AccountFault::CapacityExceeded
                    : AccountFault::StateConflict);
            step.action = ExecutionAction::Frozen;
            return step;
        }
        impl_->exit.pending = false;
        impl_->exit.active = true;
        impl_->exit.active_client_order_id = step.submit.client_order_id;
        impl_->exit.market_events_waited = 0;
        impl_->exit.freeze_after_cancel = false;
        ++impl_->close_orders_submitted;
        step.action = ExecutionAction::ExitSubmitted;
        return step;
    }

    if (impl_->exit.active) {
        ++impl_->exit.market_events_waited;
        if (impl_->exit.market_events_waited
            >= impl_->policy.close_reprice_interval_market_events) {
            step.cancel = cancel(impl_->exit.active_client_order_id);
            if (step.cancel.code == CancelCode::Requested) {
                impl_->exit.freeze_after_cancel =
                    impl_->exit.attempt >= impl_->policy.max_close_reprices;
                step.action = ExecutionAction::ExitCancelRequested;
            } else if (step.cancel.code != CancelCode::Duplicate) {
                impl_->set_fault(AccountFault::CloseCancelRejected);
                step.action = ExecutionAction::Frozen;
            }
        }
        return step;
    }

    if (impl_->policy.entry_timeout_market_events == 0) return step;
    for (auto& record : impl_->signals) {
        if (!record.occupied || !record.active_open
            || record.cancel_requested) continue;
        ++record.market_events_waited;
        if (record.market_events_waited
            < impl_->policy.entry_timeout_market_events) continue;
        step.cancel = cancel(record.result.client_order_id);
        if (step.cancel.code == CancelCode::Requested) {
            step.action = ExecutionAction::EntryCancelRequested;
        } else if (step.cancel.code != CancelCode::Duplicate) {
            impl_->set_fault(AccountFault::CloseCancelRejected);
            step.action = ExecutionAction::Frozen;
        }
        return step;
    }
    return step;
}

namespace {

bool parse_order_ref(
    const std::array<char, 13>& field,
    std::uint64_t& value) noexcept
{
    const auto text = field_view(field);
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

template <std::size_t N>
void copy_from_ctp(std::array<char, N>& destination, const char* source) noexcept
{
    destination.fill('\0');
    std::size_t index = 0;
    while (index + 1 < N && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
}

}

std::size_t AccountTradingSession::drain_callbacks(std::size_t maximum) noexcept
{
    const auto overflow_phase = impl_->recovery.phase;
    const bool callback_overflow =
        impl_->callback_overflow.exchange(false, std::memory_order_acq_rel);
    if (callback_overflow) {
        impl_->set_fault(AccountFault::CallbackQueueOverflow);
    }
    std::size_t applied_count = 0;
    CallbackEvent event{};
    while (applied_count < maximum && impl_->pop_callback(event)) {
        ++applied_count;
        if (event.type == CallbackType::Disconnected) {
            ++impl_->recovery.generation;
            impl_->recovery.phase = RecoveryPhase::Disconnected;
            impl_->recovery.failure = RecoveryFailure::None;
            impl_->frozen = true;
            impl_->reconciliation = true;
            impl_->set_fault(AccountFault::Disconnected);
            impl_->trace(TraceStage::Disconnected);
            continue;
        }
        if (event.type == CallbackType::Connected) {
            impl_->frozen = true;
            impl_->reconciliation = true;
            impl_->trace(TraceStage::RecoveryStarted);
            if (impl_->request_authentication() != 0) {
                impl_->recovery_failure(RecoveryFailure::RequestRejected);
            }
            continue;
        }
        if (event.type == CallbackType::Authenticated) {
            if (impl_->recovery.phase != RecoveryPhase::Authenticating
                || event.request_id != impl_->expected_recovery_request_id) {
                impl_->recovery_failure(RecoveryFailure::UnexpectedResponse);
            } else if (event.error_id != 0 || !event.is_last) {
                impl_->recovery_failure(RecoveryFailure::ResponseError);
            } else if (impl_->request_login() != 0) {
                impl_->recovery_failure(RecoveryFailure::RequestRejected);
            }
            continue;
        }
        if (event.type == CallbackType::LoggedIn) {
            if (impl_->recovery.phase != RecoveryPhase::LoggingIn
                || event.request_id != impl_->expected_recovery_request_id) {
                impl_->recovery_failure(RecoveryFailure::UnexpectedResponse);
            } else if (event.error_id != 0 || !event.is_last
                       || !event.has_payload) {
                impl_->recovery_failure(RecoveryFailure::ResponseError);
            } else {
                if (!activate(
                    event.login.FrontID,
                    event.login.SessionID,
                    event.login.MaxOrderRef,
                    event.login.TradingDay)) {
                    impl_->recovery_failure(RecoveryFailure::ResponseError);
                } else if (impl_->begin_query_reconciliation() != 0) {
                    impl_->recovery_failure(RecoveryFailure::RequestRejected);
                }
            }
            continue;
        }
        if (event.type == CallbackType::QueryOrder) {
            if (impl_->recovery.phase != RecoveryPhase::QueryingOrders
                || event.request_id != impl_->expected_recovery_request_id) {
                impl_->recovery_failure(RecoveryFailure::UnexpectedResponse);
                continue;
            }
            if (event.error_id != 0) {
                impl_->recovery_failure(RecoveryFailure::ResponseError);
                continue;
            }
            if (event.has_payload) {
                std::array<char, 13> queried_ref{};
                copy_from_ctp(queried_ref, event.queried_order.OrderRef);
                std::uint64_t order_ref = 0;
                auto* record = parse_order_ref(queried_ref, order_ref)
                    ? impl_->find_order_ref(order_ref) : nullptr;
                if (record == nullptr) {
                    impl_->recovery_failure(RecoveryFailure::UnknownOrder);
                    continue;
                }
                record->seen_during_recovery = true;
                ++impl_->recovery.queried_orders;
                OrderReportType type{};
                if (!queried_order_type(event.queried_order, type)) {
                    impl_->recovery_failure(RecoveryFailure::UnknownOrder);
                    continue;
                }
                const auto applied = impl_->state.apply_order_report({
                    record->result.client_order_id,
                    type,
                    event.queried_order.VolumeTraded});
                if (applied.reconciliation_required) {
                    impl_->recovery_failure(RecoveryFailure::UnknownOrder);
                    continue;
                }
                impl_->trace_order_report(
                    *record, type, applied, event.queried_order.VolumeTraded);
                impl_->release_terminal_open(*record);
            }
            if (event.is_last) {
                bool missing = false;
                for (auto& candidate : impl_->signals) {
                    OrderSnapshot snapshot{};
                    if (candidate.occupied && !candidate.seen_during_recovery
                        && impl_->state.order_snapshot(
                            candidate.result.client_order_id, snapshot)
                        && !is_terminal(snapshot.state)) {
                        missing = true;
                        break;
                    }
                }
                if (missing) {
                    impl_->recovery_failure(RecoveryFailure::MissingOrder);
                } else if (impl_->request_trades() != 0) {
                    impl_->recovery_failure(RecoveryFailure::RequestRejected);
                }
            }
            continue;
        }
        if (event.type == CallbackType::QueryTrade) {
            if (impl_->recovery.phase != RecoveryPhase::QueryingTrades
                || event.request_id != impl_->expected_recovery_request_id) {
                impl_->recovery_failure(RecoveryFailure::UnexpectedResponse);
                continue;
            }
            if (event.error_id != 0) {
                impl_->recovery_failure(RecoveryFailure::ResponseError);
                continue;
            }
            if (event.has_payload) {
                const bool valid_direction =
                    event.queried_trade.Direction == THOST_FTDC_D_Buy
                    || event.queried_trade.Direction == THOST_FTDC_D_Sell;
                const bool valid_offset =
                    event.queried_trade.OffsetFlag == THOST_FTDC_OF_Open
                    || event.queried_trade.OffsetFlag == THOST_FTDC_OF_Close
                    || event.queried_trade.OffsetFlag
                        == THOST_FTDC_OF_CloseToday
                    || event.queried_trade.OffsetFlag
                        == THOST_FTDC_OF_CloseYesterday;
                if (!valid_direction || !valid_offset
                    || event.queried_trade.Volume <= 0) {
                    impl_->recovery_failure(RecoveryFailure::UnknownTrade);
                    continue;
                }
                std::array<char, 13> queried_ref{};
                copy_from_ctp(queried_ref, event.queried_trade.OrderRef);
                std::uint64_t order_ref = 0;
                auto* record = parse_order_ref(queried_ref, order_ref)
                    ? impl_->find_order_ref(order_ref) : nullptr;
                if (record == nullptr) {
                    impl_->recovery_failure(RecoveryFailure::UnknownTrade);
                    continue;
                }
                TradeReport report{};
                report.client_order_id = record->result.client_order_id;
                copy_from_ctp(report.trading_day, event.queried_trade.TradingDay);
                copy_from_ctp(report.exchange_id, event.queried_trade.ExchangeID);
                copy_from_ctp(report.trade_id, event.queried_trade.TradeID);
                copy_from_ctp(report.instrument, event.queried_trade.InstrumentID);
                report.direction = event.queried_trade.Direction
                        == THOST_FTDC_D_Buy
                    ? Direction::Buy : Direction::Sell;
                report.offset = event.queried_trade.OffsetFlag
                        == THOST_FTDC_OF_Open
                    ? Offset::Open : Offset::Close;
                report.quantity = event.queried_trade.Volume;
                const auto applied = impl_->state.apply_trade(report);
                if (applied.code == ApplyCode::CapacityExceeded) {
                    impl_->recovery_failure(RecoveryFailure::CapacityExceeded);
                    continue;
                }
                if (applied.reconciliation_required) {
                    impl_->recovery_failure(RecoveryFailure::UnknownTrade);
                    continue;
                }
                ++impl_->recovery.queried_trades;
                if (applied.code == ApplyCode::Applied) {
                    impl_->trace(
                        TraceStage::Trade,
                        record,
                        0,
                        event.queried_trade.Volume);
                }
                impl_->apply_trade_effect(*record, report, applied);
            }
            if (event.is_last && impl_->request_positions() != 0) {
                impl_->recovery_failure(RecoveryFailure::RequestRejected);
            }
            continue;
        }
        if (event.type == CallbackType::QueryPosition) {
            if (impl_->recovery.phase != RecoveryPhase::QueryingPositions
                || event.request_id != impl_->expected_recovery_request_id) {
                impl_->recovery_failure(RecoveryFailure::UnexpectedResponse);
                continue;
            }
            if (event.error_id != 0) {
                impl_->recovery_failure(RecoveryFailure::ResponseError);
                continue;
            }
            if (event.has_payload) {
                const auto& position = event.queried_position;
                std::array<char, kInstrumentIdCapacity> instrument_field{};
                copy_from_ctp(instrument_field, position.InstrumentID);
                const auto instrument = field_view(instrument_field);
                if (position.HedgeFlag != THOST_FTDC_HF_Speculation
                    || position.Position < 0
                    || instrument.empty()
                    || (position.PosiDirection != THOST_FTDC_PD_Long
                        && position.PosiDirection != THOST_FTDC_PD_Short)) {
                    impl_->recovery_failure(RecoveryFailure::InvalidPosition);
                    continue;
                }
                PositionSnapshot existing{};
                impl_->state.position_snapshot(instrument, existing);
                const auto existing_quantity = position.PosiDirection
                        == THOST_FTDC_PD_Long
                    ? existing.long_quantity : existing.short_quantity;
                if (position.Position
                    > std::numeric_limits<std::int32_t>::max()
                        - existing_quantity) {
                    impl_->recovery_failure(RecoveryFailure::InvalidPosition);
                    continue;
                }
                const auto long_quantity = position.PosiDirection
                        == THOST_FTDC_PD_Long
                    ? existing.long_quantity + position.Position
                    : existing.long_quantity;
                const auto short_quantity = position.PosiDirection
                        == THOST_FTDC_PD_Short
                    ? existing.short_quantity + position.Position
                    : existing.short_quantity;
                const auto applied = impl_->state.set_reconciled_position(
                    instrument, long_quantity, short_quantity);
                if (applied.code == ApplyCode::CapacityExceeded) {
                    impl_->recovery_failure(RecoveryFailure::CapacityExceeded);
                    continue;
                }
                ++impl_->recovery.queried_positions;
            }
            if (event.is_last && impl_->request_funds() != 0) {
                impl_->recovery_failure(RecoveryFailure::RequestRejected);
            }
            continue;
        }
        if (event.type == CallbackType::QueryFunds) {
            if (impl_->recovery.phase != RecoveryPhase::QueryingFunds
                || event.request_id != impl_->expected_recovery_request_id) {
                impl_->recovery_failure(RecoveryFailure::UnexpectedResponse);
            } else if (event.error_id != 0 || !event.is_last
                       || !event.has_payload) {
                impl_->recovery_failure(RecoveryFailure::ResponseError);
            } else {
                const double available_cents =
                    event.queried_account.Available * 100.0;
                if (!std::isfinite(available_cents)
                    || available_cents < 0.0
                    || available_cents
                        > static_cast<double>(
                            std::numeric_limits<std::int64_t>::max())) {
                    impl_->recovery_failure(RecoveryFailure::InvalidFunds);
                    continue;
                }
                impl_->recovery.available_funds =
                    static_cast<std::int64_t>(std::llround(available_cents));
                impl_->recovery.funds_known = true;
                impl_->recovery.phase = RecoveryPhase::Reconciling;
                impl_->state.complete_reconciliation();
                impl_->reconciliation = false;
                impl_->frozen = false;
                impl_->recovery.failure = RecoveryFailure::None;
                impl_->recovery.phase = RecoveryPhase::Ready;
                impl_->recovery_completions.fetch_add(
                    1, std::memory_order_relaxed);
                impl_->trace(TraceStage::RecoveryReady);
            }
            continue;
        }
        std::uint64_t order_ref = 0;
        if (!parse_order_ref(event.order_ref, order_ref)) {
            impl_->set_fault(AccountFault::UnknownCallback);
            continue;
        }
        auto* record = impl_->find_order_ref(order_ref);
        if (record == nullptr) {
            impl_->set_fault(AccountFault::UnknownCallback);
            continue;
        }
        if (event.type == CallbackType::Unknown) {
            impl_->set_fault(AccountFault::UnknownCallback);
            continue;
        }
        ApplyResult result{};
        if (event.type == CallbackType::Trade) {
            event.trade.client_order_id = record->result.client_order_id;
            result = impl_->state.apply_trade(event.trade);
            if (result.code == ApplyCode::Applied) {
                impl_->trades.fetch_add(1, std::memory_order_relaxed);
                impl_->traded_volume.fetch_add(
                    static_cast<std::uint64_t>(event.trade.quantity),
                    std::memory_order_relaxed);
                if (record->intent.offset == Offset::Open) {
                    impl_->acceptance.entry_filled_quantity +=
                        static_cast<std::uint32_t>(event.trade.quantity);
                } else {
                    impl_->acceptance.exit_filled_quantity +=
                        static_cast<std::uint32_t>(event.trade.quantity);
                }
                impl_->trace(
                    TraceStage::Trade, record, 0, event.trade.quantity);
            }
        } else {
            const auto type = event.type == CallbackType::InsertRejected
                ? OrderReportType::Rejected
                : event.type == CallbackType::CancelRejected
                    ? OrderReportType::CancelRejected : event.order_type;
            result = impl_->state.apply_order_report(
                {record->result.client_order_id,
                 type,
                 event.cumulative_filled});
            if (result.code == ApplyCode::Applied) {
                if (type == OrderReportType::Accepted) {
                    impl_->order_acceptances.fetch_add(
                        1, std::memory_order_relaxed);
                } else if (type == OrderReportType::Rejected) {
                    impl_->order_rejections.fetch_add(
                        1, std::memory_order_relaxed);
                } else if (type == OrderReportType::Canceled) {
                    impl_->cancel_acceptances.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            if (result.code == ApplyCode::Applied
                && (type == OrderReportType::Rejected
                    || (record->intent.offset == Offset::Open
                        && type == OrderReportType::Canceled))) {
                impl_->acceptance.lifecycle_failed = true;
            }
            impl_->trace_order_report(
                *record, type, result, event.cumulative_filled);
        }
        if (result.reconciliation_required
            || result.code == ApplyCode::Conflict
            || result.code == ApplyCode::UnknownOrder) {
            impl_->set_fault(AccountFault::StateConflict);
        } else if (result.code == ApplyCode::CapacityExceeded) {
            impl_->set_fault(AccountFault::CapacityExceeded);
        }

        if (event.type == CallbackType::CancelRejected) {
            record->cancel_requested = false;
            if (record->result.client_order_id
                == impl_->exit.active_client_order_id) {
                impl_->set_fault(AccountFault::CloseCancelRejected);
            }
        }

        if (event.type == CallbackType::Trade) {
            impl_->apply_trade_effect(*record, event.trade, result);
        }

        if (event.type == CallbackType::Order
            && event.order_type == OrderReportType::Canceled
            && result.code == ApplyCode::Applied
            && record->result.client_order_id
                == impl_->exit.active_client_order_id) {
            impl_->exit.active = false;
            impl_->exit.active_client_order_id = 0;
            if (impl_->exit.freeze_after_cancel) {
                impl_->set_fault(AccountFault::CloseRetryExhausted);
            } else {
                PositionSnapshot held{};
                if (!impl_->state.position_snapshot(
                        field_view(impl_->exit.instrument), held)) {
                    impl_->set_fault(AccountFault::StateConflict);
                } else {
                    impl_->exit.pending_quantity =
                        impl_->exit.direction == Direction::Sell
                        ? held.long_quantity : held.short_quantity;
                    impl_->exit.pending = impl_->exit.pending_quantity > 0;
                    ++impl_->exit.attempt;
                    ++impl_->close_reprices;
                }
            }
        }
        impl_->release_terminal_open(*record);
    }
    if (callback_overflow) {
        const bool overflowed_during_recovery =
            overflow_phase == RecoveryPhase::Connecting
            || overflow_phase == RecoveryPhase::Authenticating
            || overflow_phase == RecoveryPhase::LoggingIn
            || overflow_phase == RecoveryPhase::QueryingOrders
            || overflow_phase == RecoveryPhase::QueryingTrades
            || overflow_phase == RecoveryPhase::QueryingPositions
            || overflow_phase == RecoveryPhase::QueryingFunds
            || overflow_phase == RecoveryPhase::Reconciling;
        if (overflowed_during_recovery) {
            impl_->recovery_failure(RecoveryFailure::CallbackQueueOverflow);
        } else if (overflow_phase == RecoveryPhase::Ready
                   && impl_->recovery.phase == RecoveryPhase::Ready) {
            // 先排空已入队回报，再由账户线程发起查询，
            // 避免队列尚在满载时立即丢失查询响应。
            impl_->trace(TraceStage::RecoveryStarted);
            if (impl_->begin_query_reconciliation() != 0) {
                impl_->recovery_failure(RecoveryFailure::RequestRejected);
            }
        }
    }
    return applied_count;
}

bool AccountTradingSession::order_snapshot(
    std::uint64_t client_order_id,
    OrderSnapshot& snapshot) const noexcept
{
    return impl_->state.order_snapshot(client_order_id, snapshot);
}

bool AccountTradingSession::position_snapshot(
    std::string_view instrument,
    PositionSnapshot& snapshot) const noexcept
{
    return impl_->state.position_snapshot(instrument, snapshot);
}

bool AccountTradingSession::reconciliation_required() const noexcept
{
    return impl_->reconciliation || impl_->state.reconciliation_required()
        || impl_->callback_overflow.load(std::memory_order_acquire);
}

void AccountTradingSession::mark_fault(AccountFault fault) noexcept
{
    if (fault != AccountFault::None) impl_->set_fault(fault);
}

AccountExecutionSnapshot AccountTradingSession::execution_snapshot() const noexcept
{
    return {
        impl_->frozen,
        reconciliation_required(),
        impl_->fault,
        impl_->alert_count,
        impl_->close_orders_submitted,
        impl_->close_reprices,
        impl_->active_open_orders,
        impl_->exit.pending_quantity,
        impl_->exit.active_client_order_id,
    };
}

AccountAcceptanceSnapshot
AccountTradingSession::acceptance_snapshot() const noexcept
{
    return impl_->acceptance;
}

bool AccountTradingSession::request_reconciliation() noexcept
{
    if (impl_->recovery.phase != RecoveryPhase::Ready
        || reconciliation_required()) {
        return false;
    }
    if (impl_->begin_query_reconciliation() != 0) {
        impl_->recovery_failure(RecoveryFailure::RequestRejected);
        return false;
    }
    return true;
}

void AccountTradingSession::OnFrontDisconnected(int)
{
    CallbackEvent event{};
    event.type = CallbackType::Disconnected;
    impl_->push_callback(event);
}

void AccountTradingSession::OnFrontConnected()
{
    CallbackEvent event{};
    event.type = CallbackType::Connected;
    impl_->push_callback(event);
}

void AccountTradingSession::OnRspAuthenticate(
    CThostFtdcRspAuthenticateField*,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackEvent event{};
    event.type = CallbackType::Authenticated;
    event.error_id = info == nullptr ? 0 : info->ErrorID;
    event.request_id = request_id;
    event.is_last = is_last;
    impl_->push_callback(event);
}

void AccountTradingSession::OnRspUserLogin(
    CThostFtdcRspUserLoginField* response,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackEvent event{};
    event.type = CallbackType::LoggedIn;
    event.error_id = info == nullptr ? 0 : info->ErrorID;
    event.request_id = request_id;
    event.is_last = is_last;
    event.has_payload = response != nullptr;
    if (response != nullptr) event.login = *response;
    impl_->push_callback(event);
}

void AccountTradingSession::OnRspQryOrder(
    CThostFtdcOrderField* order,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackEvent event{};
    event.type = CallbackType::QueryOrder;
    event.error_id = info == nullptr ? 0 : info->ErrorID;
    event.request_id = request_id;
    event.is_last = is_last;
    event.has_payload = order != nullptr;
    if (order != nullptr) event.queried_order = *order;
    impl_->push_callback(event);
}

void AccountTradingSession::OnRspQryTrade(
    CThostFtdcTradeField* trade,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackEvent event{};
    event.type = CallbackType::QueryTrade;
    event.error_id = info == nullptr ? 0 : info->ErrorID;
    event.request_id = request_id;
    event.is_last = is_last;
    event.has_payload = trade != nullptr;
    if (trade != nullptr) event.queried_trade = *trade;
    impl_->push_callback(event);
}

void AccountTradingSession::OnRspQryInvestorPosition(
    CThostFtdcInvestorPositionField* position,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackEvent event{};
    event.type = CallbackType::QueryPosition;
    event.error_id = info == nullptr ? 0 : info->ErrorID;
    event.request_id = request_id;
    event.is_last = is_last;
    event.has_payload = position != nullptr;
    if (position != nullptr) event.queried_position = *position;
    impl_->push_callback(event);
}

void AccountTradingSession::OnRspQryTradingAccount(
    CThostFtdcTradingAccountField* account,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackEvent event{};
    event.type = CallbackType::QueryFunds;
    event.error_id = info == nullptr ? 0 : info->ErrorID;
    event.request_id = request_id;
    event.is_last = is_last;
    event.has_payload = account != nullptr;
    if (account != nullptr) event.queried_account = *account;
    impl_->push_callback(event);
}

void AccountTradingSession::OnRtnOrder(CThostFtdcOrderField* order)
{
    if (order == nullptr) return;
    CallbackEvent event{};
    event.type = CallbackType::Order;
    copy_from_ctp(event.order_ref, order->OrderRef);
    event.cumulative_filled = order->VolumeTraded;
    if (order->OrderSubmitStatus == THOST_FTDC_OSS_InsertRejected) {
        event.order_type = OrderReportType::Rejected;
    } else if (order->OrderSubmitStatus == THOST_FTDC_OSS_CancelRejected) {
        event.order_type = OrderReportType::CancelRejected;
    } else if (order->OrderStatus == THOST_FTDC_OST_AllTraded) {
        event.order_type = OrderReportType::Filled;
    } else if (order->OrderStatus == THOST_FTDC_OST_PartTradedQueueing
               || order->OrderStatus == THOST_FTDC_OST_PartTradedNotQueueing) {
        event.order_type = OrderReportType::PartiallyFilled;
    } else if (order->OrderStatus == THOST_FTDC_OST_Canceled) {
        event.order_type = OrderReportType::Canceled;
    } else if (order->OrderStatus == THOST_FTDC_OST_NoTradeQueueing) {
        event.order_type = OrderReportType::Accepted;
    } else {
        event.type = CallbackType::Unknown;
    }
    impl_->push_callback(event);
}

void AccountTradingSession::OnRtnTrade(CThostFtdcTradeField* trade)
{
    if (trade == nullptr) return;
    CallbackEvent event{};
    event.type = CallbackType::Trade;
    copy_from_ctp(event.order_ref, trade->OrderRef);
    copy_from_ctp(event.trade.trading_day, trade->TradingDay);
    copy_from_ctp(event.trade.exchange_id, trade->ExchangeID);
    copy_from_ctp(event.trade.trade_id, trade->TradeID);
    copy_from_ctp(event.trade.instrument, trade->InstrumentID);
    if (trade->Direction == THOST_FTDC_D_Buy) {
        event.trade.direction = Direction::Buy;
    } else if (trade->Direction == THOST_FTDC_D_Sell) {
        event.trade.direction = Direction::Sell;
    } else {
        event.type = CallbackType::Unknown;
    }
    if (trade->OffsetFlag == THOST_FTDC_OF_Open) {
        event.trade.offset = Offset::Open;
    } else if (trade->OffsetFlag == THOST_FTDC_OF_Close
               || trade->OffsetFlag == THOST_FTDC_OF_CloseToday
               || trade->OffsetFlag == THOST_FTDC_OF_CloseYesterday) {
        event.trade.offset = Offset::Close;
    } else {
        event.type = CallbackType::Unknown;
    }
    event.trade.quantity = trade->Volume;
    impl_->push_callback(event);
}

void AccountTradingSession::OnRspOrderInsert(
    CThostFtdcInputOrderField* order,
    CThostFtdcRspInfoField* info,
    int,
    bool)
{
    if (order == nullptr || info == nullptr || info->ErrorID == 0) return;
    CallbackEvent event{};
    event.type = CallbackType::InsertRejected;
    copy_from_ctp(event.order_ref, order->OrderRef);
    impl_->push_callback(event);
}

void AccountTradingSession::OnErrRtnOrderInsert(
    CThostFtdcInputOrderField* order,
    CThostFtdcRspInfoField* info)
{
    OnRspOrderInsert(order, info, 0, true);
}

void AccountTradingSession::OnRspOrderAction(
    CThostFtdcInputOrderActionField* action,
    CThostFtdcRspInfoField* info,
    int,
    bool)
{
    if (action == nullptr || info == nullptr || info->ErrorID == 0) return;
    CallbackEvent event{};
    event.type = CallbackType::CancelRejected;
    copy_from_ctp(event.order_ref, action->OrderRef);
    impl_->push_callback(event);
}

void AccountTradingSession::OnErrRtnOrderAction(
    CThostFtdcOrderActionField* action,
    CThostFtdcRspInfoField* info)
{
    if (action == nullptr || info == nullptr || info->ErrorID == 0) return;
    CallbackEvent event{};
    event.type = CallbackType::CancelRejected;
    copy_from_ctp(event.order_ref, action->OrderRef);
    impl_->push_callback(event);
}

}
