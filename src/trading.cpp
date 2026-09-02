#include "ctp/trading.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
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

RiskRejectReason evaluate_risk(
    const OrderIntent& intent,
    const RiskSnapshot& snapshot,
    const RiskLimits& limits) noexcept
{
    if (!snapshot.enabled) return RiskRejectReason::AccountDisabled;
    if (!snapshot.authenticated) return RiskRejectReason::NotAuthenticated;
    if (!snapshot.logged_in) return RiskRejectReason::NotLoggedIn;
    if (!snapshot.reconciled) return RiskRejectReason::NotReconciled;
    if (snapshot.frozen) return RiskRejectReason::AccountFrozen;
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
    if (intent.quantity <= 0) return RiskRejectReason::InvalidQuantity;
    if (snapshot.daily_signals >= limits.max_daily_signals) {
        return RiskRejectReason::DailySignalLimit;
    }
    if (snapshot.daily_orders >= limits.max_daily_orders) {
        return RiskRejectReason::DailyOrderLimit;
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
    const bool outside_price_limit = intent.direction == Direction::Buy
        ? intent.limit_price_ticks
            > snapshot.ask_price_ticks + limits.max_slippage_ticks
        : intent.limit_price_ticks
            < snapshot.bid_price_ticks - limits.max_slippage_ticks;
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
    OrderIntent intent{};
    SubmitResult result{};
};

enum class CallbackType : std::uint8_t {
    Order,
    Trade,
    InsertRejected,
    CancelRejected,
    Unknown,
};

struct CallbackEvent {
    CallbackType type{CallbackType::Order};
    std::array<char, 13> order_ref{};
    OrderReportType order_type{OrderReportType::Accepted};
    std::int32_t cumulative_filled{0};
    TradeReport trade{};
};

bool same_intent(const OrderIntent& left, const OrderIntent& right) noexcept
{
    return left.signal_id == right.signal_id
        && left.instrument == right.instrument
        && left.direction == right.direction
        && left.offset == right.offset
        && left.quantity == right.quantity
        && left.limit_price_ticks == right.limit_price_ticks;
}

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
        std::uint64_t first_order_ref)
        : account_id(source.alias()),
          broker_id(source.broker_id()),
          user_id(source.user_id()),
          limits(std::move(risk_limits)),
          api(std::move(trader_api)),
          state(source.alias(), order_capacity, trade_capacity),
          signals(signal_capacity),
          callbacks(callback_capacity + 1),
          next_client_order_id(first_client_order_id),
          next_order_ref(first_order_ref)
    {
    }

    std::string account_id;
    std::string broker_id;
    std::string user_id;
    RiskLimits limits;
    std::unique_ptr<TraderApi> api;
    AccountTradingState state;
    std::vector<SignalRecord> signals;
    std::vector<CallbackEvent> callbacks;
    std::atomic<std::size_t> callback_write{0};
    std::atomic<std::size_t> callback_read{0};
    std::atomic<bool> callback_overflow{false};
    std::uint64_t next_client_order_id{1};
    std::uint64_t next_order_ref{1};
    std::uint32_t daily_signals{0};
    std::uint32_t daily_orders{0};
    std::int32_t active_open_orders{0};
    int front_id{0};
    int session_id{0};
    int next_request_id{1};
    int next_action_ref{1};
    std::uint32_t daily_cancels{0};
    bool reconciliation{false};

    bool push_callback(const CallbackEvent& event) noexcept
    {
        const auto write = callback_write.load(std::memory_order_relaxed);
        const auto next = (write + 1) % callbacks.size();
        if (next == callback_read.load(std::memory_order_acquire)) {
            callback_overflow.store(true, std::memory_order_release);
            return false;
        }
        callbacks[write] = event;
        callback_write.store(next, std::memory_order_release);
        return true;
    }

    bool pop_callback(CallbackEvent& event) noexcept
    {
        const auto read = callback_read.load(std::memory_order_relaxed);
        if (read == callback_write.load(std::memory_order_acquire)) return false;
        event = callbacks[read];
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
    std::uint64_t persisted_next_order_ref)
    : impl_(std::make_unique<Impl>(
          account,
          std::move(limits),
          std::move(api),
          order_capacity,
          trade_capacity,
          signal_capacity,
          callback_capacity,
          next_client_order_id,
          persisted_next_order_ref))
{
    if (impl_->api) impl_->api->register_spi(this);
}

AccountTradingSession::~AccountTradingSession()
{
    if (impl_->api) {
        impl_->api->register_spi(nullptr);
        impl_->api->release();
    }
}

void AccountTradingSession::activate(
    int front_id,
    int session_id,
    std::string_view max_order_ref) noexcept
{
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
}

SubmitResult AccountTradingSession::submit(
    const OrderIntent& intent,
    const RiskSnapshot& snapshot) noexcept
{
    SignalRecord* free_record = nullptr;
    for (auto& record : impl_->signals) {
        if (record.occupied && record.intent.signal_id == intent.signal_id) {
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
    ++impl_->daily_signals;

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
        snapshot.daily_signals, impl_->daily_signals - 1);
    effective.daily_orders = std::max(snapshot.daily_orders, impl_->daily_orders);
    effective.active_open_orders = std::max(
        snapshot.active_open_orders, impl_->active_open_orders);
    effective.reconciled = snapshot.reconciled
        && !impl_->reconciliation
        && !impl_->state.reconciliation_required()
        && !impl_->callback_overflow.load(std::memory_order_acquire);
    const auto reason = evaluate_risk(intent, effective, impl_->limits);
    if (reason != RiskRejectReason::None) {
        impl_->state.apply_local_event(
            free_record->result.client_order_id,
            LocalOrderEvent::RiskRejected);
        free_record->result.code = SubmitCode::RiskRejected;
        free_record->result.risk_reason = reason;
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
        return free_record->result;
    }
    *converted.ptr = '\0';
    request.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    request.Direction = intent.direction == Direction::Buy
        ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    request.CombOffsetFlag[0] = intent.offset == Offset::Open
        ? THOST_FTDC_OF_Open : THOST_FTDC_OF_Close;
    request.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    request.LimitPrice = static_cast<double>(intent.limit_price_ticks);
    request.VolumeTotalOriginal = intent.quantity;
    request.TimeCondition = THOST_FTDC_TC_GFD;
    request.VolumeCondition = THOST_FTDC_VC_AV;
    request.MinVolume = 1;
    request.ContingentCondition = THOST_FTDC_CC_Immediately;
    request.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    request.IsAutoSuspend = 0;
    request.UserForceClose = 0;

    free_record->result.order_ref = order_ref;
    ++impl_->daily_orders;
    const int api_code = impl_->api == nullptr
        ? -1
        : impl_->api->request_order_insert(
              &request, impl_->next_request_id++);
    free_record->result.api_return_code = api_code;
    if (api_code != 0) {
        impl_->state.apply_local_event(
            free_record->result.client_order_id,
            LocalOrderEvent::SubmitRejected);
        free_record->result.code = SubmitCode::RejectedLocally;
        return free_record->result;
    }
    impl_->state.apply_local_event(
        free_record->result.client_order_id, LocalOrderEvent::Submitted);
    if (intent.offset == Offset::Open) {
        ++impl_->active_open_orders;
        free_record->active_open = true;
    }
    free_record->result.code = SubmitCode::Submitted;
    return free_record->result;
}

CancelResult AccountTradingSession::cancel(
    std::uint64_t client_order_id) noexcept
{
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
    return {CancelCode::Requested, client_order_id, 0};
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

std::size_t AccountTradingSession::drain_callbacks() noexcept
{
    if (impl_->callback_overflow.exchange(false, std::memory_order_acq_rel)) {
        impl_->reconciliation = true;
    }
    std::size_t applied_count = 0;
    CallbackEvent event{};
    while (impl_->pop_callback(event)) {
        ++applied_count;
        std::uint64_t order_ref = 0;
        if (!parse_order_ref(event.order_ref, order_ref)) {
            impl_->reconciliation = true;
            continue;
        }
        auto* record = impl_->find_order_ref(order_ref);
        if (record == nullptr) {
            impl_->reconciliation = true;
            continue;
        }
        if (event.type == CallbackType::Unknown) {
            impl_->reconciliation = true;
            continue;
        }
        ApplyResult result{};
        if (event.type == CallbackType::Trade) {
            event.trade.client_order_id = record->result.client_order_id;
            result = impl_->state.apply_trade(event.trade);
        } else {
            const auto type = event.type == CallbackType::InsertRejected
                ? OrderReportType::Rejected
                : event.type == CallbackType::CancelRejected
                    ? OrderReportType::CancelRejected : event.order_type;
            result = impl_->state.apply_order_report(
                {record->result.client_order_id,
                 type,
                 event.cumulative_filled});
        }
        if (result.reconciliation_required
            || result.code == ApplyCode::Conflict
            || result.code == ApplyCode::UnknownOrder
            || result.code == ApplyCode::CapacityExceeded) {
            impl_->reconciliation = true;
        }
        OrderSnapshot snapshot{};
        if (record->active_open
            && impl_->state.order_snapshot(
                record->result.client_order_id, snapshot)
            && is_terminal(snapshot.state)) {
            record->active_open = false;
            --impl_->active_open_orders;
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
