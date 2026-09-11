#include "ctp/field.hpp"
#include "ctp/engine.hpp"
#include "ctp/telemetry.hpp"
#include "ctp/trading.hpp"
#define CTP_TEST_DEFINE_ALLOCATION_OPERATORS
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

class CapturingTraceSink final : public ctp::TraceSink {
public:
    bool try_record(const ctp::TraceEvent& event) noexcept override
    {
        if (size == events.size()) return false;
        events[size++] = event;
        return true;
    }

    std::array<ctp::TraceEvent, 64> events{};
    std::size_t size{0};
};

class QueueTraceSink final : public ctp::TraceSink {
public:
    bool try_record(const ctp::TraceEvent& event) noexcept override
    {
        return queue.try_push(event);
    }

    ctp::TraceQueue queue;
};

ctp::OrderSeed order_seed(std::uint64_t id = 1, int quantity = 5)
{
    ctp::OrderSeed seed{};
    seed.client_order_id = id;
    ctp::copy_to_field(seed.instrument, "IF2609");
    seed.quantity = quantity;
    return seed;
}

ctp::OrderSnapshot snapshot(ctp::AccountTradingState& state, std::uint64_t id)
{
    ctp::OrderSnapshot value{};
    state.order_snapshot(id, value);
    return value;
}

ctp::TradeReport trade_report(
    std::uint64_t order_id,
    std::string_view trade_id,
    int quantity,
    ctp::Direction direction = ctp::Direction::Buy,
    ctp::Offset offset = ctp::Offset::Open,
    std::string_view trading_day = "20260902",
    std::string_view exchange = "CFFEX")
{
    ctp::TradeReport report{};
    report.client_order_id = order_id;
    ctp::copy_to_field(report.trading_day, trading_day);
    ctp::copy_to_field(report.exchange_id, exchange);
    ctp::copy_to_field(report.trade_id, trade_id);
    ctp::copy_to_field(report.instrument, "IF2609");
    report.direction = direction;
    report.offset = offset;
    report.quantity = quantity;
    return report;
}

ctp::PositionSnapshot position(ctp::AccountTradingState& state)
{
    ctp::PositionSnapshot value{};
    state.position_snapshot("IF2609", value);
    return value;
}

ctp::RiskLimits risk_limits()
{
    ctp::RiskLimits limits{};
    ctp::copy_to_field(limits.allowed_instrument, "IF2609");
    limits.max_market_age_ns = 1'000'000;
    limits.max_slippage_ticks = 2;
    limits.margin_per_lot = 100;
    limits.minimum_available_after_order = 100;
    limits.max_daily_signals = 10;
    limits.max_daily_orders = 10;
    limits.max_daily_cancels = 10;
    limits.max_order_volume = 5;
    limits.max_active_open_orders = 1;
    limits.max_net_open_position = 1;
    return limits;
}

ctp::RiskSnapshot healthy_risk_snapshot()
{
    ctp::RiskSnapshot snapshot{};
    snapshot.enabled = true;
    snapshot.authenticated = true;
    snapshot.logged_in = true;
    snapshot.reconciled = true;
    snapshot.trading_window_open = true;
    snapshot.market_valid = true;
    snapshot.funds_known = true;
    snapshot.positions_known = true;
    snapshot.now_ns = 2'000'000;
    snapshot.market_receive_ns = 1'500'000;
    snapshot.bid_price_ticks = 3'999;
    snapshot.ask_price_ticks = 4'000;
    snapshot.available_funds = 200;
    return snapshot;
}

ctp::OrderIntent opening_intent(std::uint64_t signal_id = 41)
{
    ctp::OrderIntent intent{};
    intent.signal_id = signal_id;
    ctp::copy_to_field(intent.instrument, "IF2609");
    intent.direction = ctp::Direction::Buy;
    intent.offset = ctp::Offset::Open;
    intent.quantity = 1;
    intent.limit_price_ticks = 4'002;
    return intent;
}

ctp::AutoClosePolicy auto_close_policy()
{
    ctp::AutoClosePolicy policy{};
    policy.enabled = true;
    policy.entry_timeout_market_events = 3;
    policy.close_protection_ticks = 1;
    policy.close_reprice_interval_market_events = 2;
    policy.max_close_reprices = 1;
    return policy;
}

ctp::MarketEvent valid_market(std::uint64_t sequence = 1)
{
    ctp::MarketEvent market{};
    market.market_seq = sequence;
    market.recv_mono_ns = 1'500'000;
    market.last_price_ticks = 4'000;
    market.bid_price_ticks = 3'999;
    market.ask_price_ticks = 4'000;
    market.bid_volume = 1;
    market.ask_volume = 1;
    market.status = ctp::MarketDataStatus::Valid;
    ctp::copy_to_field(market.instrument, "IF2609");
    return market;
}

void emit_trade(
    CThostFtdcTraderSpi* spi,
    std::string_view order_ref,
    std::string_view trade_id,
    ctp::Direction direction,
    ctp::Offset offset)
{
    CThostFtdcTradeField trade{};
    ctp::copy_to_field(trade.OrderRef, order_ref);
    ctp::copy_to_field(trade.TradingDay, "20260907");
    ctp::copy_to_field(trade.ExchangeID, "CFFEX");
    ctp::copy_to_field(trade.TradeID, trade_id);
    ctp::copy_to_field(trade.InstrumentID, "IF2609");
    trade.Direction = direction == ctp::Direction::Buy
        ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    trade.OffsetFlag = offset == ctp::Offset::Open
        ? THOST_FTDC_OF_Open : THOST_FTDC_OF_Close;
    trade.Volume = 1;
    spi->OnRtnTrade(&trade);
}

void emit_canceled(CThostFtdcTraderSpi* spi, std::string_view order_ref)
{
    CThostFtdcOrderField order{};
    ctp::copy_to_field(order.OrderRef, order_ref);
    order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    order.OrderStatus = THOST_FTDC_OST_Canceled;
    spi->OnRtnOrder(&order);
}

void test_local_order_states(test_support::TestRunner& runner)
{
    ctp::AccountTradingState state{"account1", 8, 16};
    runner.expect(
        state.create_order(order_seed()).code == ctp::ApplyCode::Applied,
        "a valid order must enter the account order table");
    runner.expect(
        state.apply_local_event(1, ctp::LocalOrderEvent::RiskAccepted).code
            == ctp::ApplyCode::Applied,
        "risk acceptance must advance a created order");
    runner.expect(
        state.apply_local_event(1, ctp::LocalOrderEvent::Submitted).code
            == ctp::ApplyCode::Applied,
        "a risk-accepted order must become submitted");
    runner.expect(
        snapshot(state, 1).state == ctp::OrderState::Submitted,
        "the submitted state must be observable");

    runner.expect(
        state.apply_order_report({1, ctp::OrderReportType::Accepted, 0}).code
            == ctp::ApplyCode::Applied,
        "an exchange acceptance must advance a submitted order");
    runner.expect(
        state.apply_local_event(1, ctp::LocalOrderEvent::CancelRequested).code
            == ctp::ApplyCode::Applied,
        "an accepted order may request cancellation");
    runner.expect(
        state.apply_order_report({1, ctp::OrderReportType::CancelRejected, 0}).code
            == ctp::ApplyCode::Applied,
        "cancel rejection must return to the known exchange facts");
    runner.expect(
        snapshot(state, 1).state == ctp::OrderState::Accepted,
        "cancel rejection is not a terminal order state");
}

void test_terminal_and_invalid_transitions(test_support::TestRunner& runner)
{
    ctp::AccountTradingState rejected{"account1", 4, 4};
    rejected.create_order(order_seed());
    rejected.apply_local_event(1, ctp::LocalOrderEvent::RiskRejected);
    runner.expect(
        rejected.apply_local_event(1, ctp::LocalOrderEvent::RiskAccepted).code
            == ctp::ApplyCode::Stale,
        "a terminal local rejection must not regress");
    runner.expect(
        snapshot(rejected, 1).state == ctp::OrderState::RiskRejected,
        "stale input must preserve the terminal state");

    ctp::AccountTradingState invalid{"account2", 4, 4};
    invalid.create_order(order_seed());
    runner.expect(
        invalid.apply_local_event(1, ctp::LocalOrderEvent::Submitted).code
            == ctp::ApplyCode::Conflict,
        "skipping risk acceptance must be diagnosed");
    runner.expect(
        invalid.reconciliation_required(),
        "an illegal transition must require reconciliation");
    runner.expect(
        invalid.apply_order_report({99, ctp::OrderReportType::Accepted, 0}).code
            == ctp::ApplyCode::UnknownOrder,
        "an unknown order report must not be guessed into an order");
}

void test_order_capacity_and_duplicates(test_support::TestRunner& runner)
{
    ctp::AccountTradingState state{"account1", 2, 2};
    runner.expect(
        state.create_order(order_seed(1)).code == ctp::ApplyCode::Applied,
        "first order must fit the preallocated table");
    runner.expect(
        state.create_order(order_seed(1)).code == ctp::ApplyCode::Duplicate,
        "the same client order id must not create a second order");
    auto conflicting_order = order_seed(1, 9);
    runner.expect(
        state.create_order(conflicting_order).code == ctp::ApplyCode::Conflict,
        "the same client order id with different contents must be diagnosed");
    runner.expect(
        state.create_order(order_seed(2)).code == ctp::ApplyCode::Applied,
        "second order must fit the declared capacity");
    runner.expect(
        state.create_order(order_seed(3)).code
            == ctp::ApplyCode::CapacityExceeded,
        "a full order table must fail explicitly instead of allocating");
}

void test_multiple_fills_are_deduplicated(test_support::TestRunner& runner)
{
    ctp::AccountTradingState state{"account1", 8, 8};
    state.create_order(order_seed(1, 5));
    state.apply_local_event(1, ctp::LocalOrderEvent::RiskAccepted);
    state.apply_local_event(1, ctp::LocalOrderEvent::Submitted);

    const auto first = trade_report(1, "trade-1", 2);
    const auto second = trade_report(1, "trade-2", 1);
    const auto third = trade_report(1, "trade-3", 2);
    runner.expect(
        state.apply_trade(first).code == ctp::ApplyCode::Applied,
        "the first two-lot fill must be accounted once");
    runner.expect(
        state.apply_trade(first).code == ctp::ApplyCode::Duplicate,
        "an identical trade callback must not be accounted twice");
    runner.expect(
        state.apply_trade(second).code == ctp::ApplyCode::Applied,
        "a distinct one-lot fill must be accumulated");
    runner.expect(
        state.apply_trade(third).code == ctp::ApplyCode::Applied,
        "a final two-lot fill must complete the five-lot order");

    const auto order = snapshot(state, 1);
    runner.expect(
        order.accounted_filled == 5 && order.state == ctp::OrderState::Filled,
        "two plus one plus two unique fills must produce a filled order");
    runner.expect(
        position(state).long_quantity == 5,
        "only unique opening buys may increase the long position");
}

void test_trade_identity_scope_and_account_isolation(
    test_support::TestRunner& runner)
{
    ctp::AccountTradingState first{"account1", 4, 8};
    ctp::AccountTradingState second{"account2", 4, 8};
    first.create_order(order_seed(1, 3));
    second.create_order(order_seed(1, 3));

    const auto common = trade_report(1, "same-id", 1);
    runner.expect(
        first.apply_trade(common).code == ctp::ApplyCode::Applied
            && second.apply_trade(common).code == ctp::ApplyCode::Applied,
        "the same exchange trade id in two accounts is two independent facts");
    runner.expect(
        position(first).long_quantity == 1
            && position(second).long_quantity == 1,
        "one account trade must not change another account position");

    runner.expect(
        first.apply_trade(
            trade_report(1, "same-id", 1, ctp::Direction::Buy,
                         ctp::Offset::Open, "20260903", "CFFEX"))
                .code == ctp::ApplyCode::Applied,
        "a trade id may be reused on another trading day");
    runner.expect(
        first.apply_trade(
            trade_report(1, "same-id", 1, ctp::Direction::Buy,
                         ctp::Offset::Open, "20260902", "SHFE"))
                .code == ctp::ApplyCode::Applied,
        "a trade id may be reused by another exchange");
    runner.expect(
        position(first).long_quantity == 3,
        "all three distinct scoped trade keys must be accounted");
}

void test_open_and_close_positions(test_support::TestRunner& runner)
{
    ctp::AccountTradingState state{"account1", 8, 16};
    state.create_order(order_seed(1, 5));
    state.apply_trade(trade_report(1, "open-long", 5));

    auto close_long = order_seed(2, 5);
    close_long.direction = ctp::Direction::Sell;
    close_long.offset = ctp::Offset::Close;
    state.create_order(close_long);
    state.apply_trade(trade_report(
        2, "close-long-1", 2, ctp::Direction::Sell, ctp::Offset::Close));
    state.apply_trade(trade_report(
        2, "close-long-2", 3, ctp::Direction::Sell, ctp::Offset::Close));
    runner.expect(
        position(state).long_quantity == 0,
        "multiple closing sells must reduce the long position to zero");

    auto open_short = order_seed(3, 4);
    open_short.direction = ctp::Direction::Sell;
    state.create_order(open_short);
    state.apply_trade(trade_report(
        3, "open-short", 4, ctp::Direction::Sell, ctp::Offset::Open));

    auto close_short = order_seed(4, 4);
    close_short.offset = ctp::Offset::Close;
    state.create_order(close_short);
    state.apply_trade(trade_report(
        4, "close-short", 4, ctp::Direction::Buy, ctp::Offset::Close));
    const auto final_position = position(state);
    runner.expect(
        final_position.long_quantity == 0
            && final_position.short_quantity == 0
            && final_position.known,
        "opening and closing both position directions must be deterministic");
}

void test_all_report_permutations_converge(test_support::TestRunner& runner)
{
    std::array<int, 6> facts{0, 1, 2, 3, 4, 5};
    std::size_t permutation_count = 0;
    bool all_converged = true;
    do {
        ctp::AccountTradingState state{"account1", 4, 8};
        state.create_order(order_seed(1, 5));
        state.apply_local_event(1, ctp::LocalOrderEvent::RiskAccepted);
        state.apply_local_event(1, ctp::LocalOrderEvent::Submitted);

        const auto apply_fact = [&state](int fact) {
            switch (fact) {
            case 0:
                state.apply_order_report(
                    {1, ctp::OrderReportType::Accepted, 0});
                break;
            case 1:
                state.apply_order_report(
                    {1, ctp::OrderReportType::PartiallyFilled, 2});
                break;
            case 2:
                state.apply_order_report(
                    {1, ctp::OrderReportType::Filled, 5});
                break;
            case 3:
                state.apply_trade(trade_report(1, "trade-1", 2));
                break;
            case 4:
                state.apply_trade(trade_report(1, "trade-2", 1));
                break;
            case 5:
                state.apply_trade(trade_report(1, "trade-3", 2));
                break;
            }
        };

        for (const int fact : facts) {
            apply_fact(fact);
            apply_fact(fact);
        }
        const auto order = snapshot(state, 1);
        const auto final_position = position(state);
        all_converged = all_converged
            && order.state == ctp::OrderState::Filled
            && order.reported_filled == 5
            && order.accounted_filled == 5
            && final_position.long_quantity == 5
            && !state.reconciliation_required();
        ++permutation_count;
    } while (std::next_permutation(facts.begin(), facts.end()));

    runner.expect(
        permutation_count == 720,
        "all six-factor arrival permutations must be exercised");
    runner.expect(
        all_converged,
        "every duplicate and out-of-order report sequence must converge");
}

void test_cancel_fill_race_converges(test_support::TestRunner& runner)
{
    ctp::AccountTradingState state{"account1", 4, 8};
    state.create_order(order_seed(1, 5));
    state.apply_local_event(1, ctp::LocalOrderEvent::RiskAccepted);
    state.apply_local_event(1, ctp::LocalOrderEvent::Submitted);
    state.apply_order_report({1, ctp::OrderReportType::Accepted, 0});
    state.apply_trade(trade_report(1, "before-cancel", 2));
    state.apply_local_event(1, ctp::LocalOrderEvent::CancelRequested);
    state.apply_order_report({1, ctp::OrderReportType::Canceled, 2});

    runner.expect(
        snapshot(state, 1).state == ctp::OrderState::Canceled,
        "cancel acknowledgement must cancel only the unfilled remainder");
    runner.expect(
        state.apply_trade(trade_report(1, "late-fill", 3)).code
            == ctp::ApplyCode::Applied,
        "a valid late fill must still be recorded after cancellation");
    const auto order = snapshot(state, 1);
    runner.expect(
        order.state == ctp::OrderState::Filled
            && order.accounted_filled == 5
            && order.cancel_acknowledged,
        "late fills must refine canceled to filled without erasing cancel history");
    runner.expect(
        position(state).long_quantity == 5
            && !state.reconciliation_required(),
        "the cancel-fill race must converge without duplicate position effects");
}

void test_conflicts_and_fixed_capacity(test_support::TestRunner& runner)
{
    ctp::AccountTradingState conflict{"account1", 4, 4};
    conflict.create_order(order_seed(1, 5));
    const auto original = trade_report(1, "same-key", 2);
    conflict.apply_trade(original);
    auto changed = original;
    changed.quantity = 3;
    runner.expect(
        conflict.apply_trade(changed).code == ctp::ApplyCode::Conflict,
        "one trade key with different contents must be diagnosed");
    runner.expect(
        snapshot(conflict, 1).accounted_filled == 2
            && position(conflict).long_quantity == 2,
        "a conflicting duplicate must have no second-order effects");

    ctp::AccountTradingState full{"account2", 4, 1};
    full.create_order(order_seed(1, 2));
    full.apply_trade(trade_report(1, "only-slot", 1));
    runner.expect(
        full.apply_trade(trade_report(1, "no-slot", 1)).code
            == ctp::ApplyCode::CapacityExceeded,
        "a full trade table must fail explicitly without allocating");
    runner.expect(
        snapshot(full, 1).accounted_filled == 1
            && position(full).long_quantity == 1,
        "capacity failure must not partly update order or position");

    ctp::AccountTradingState missing_position{"account3", 4, 4};
    auto close = order_seed(1, 1);
    close.direction = ctp::Direction::Sell;
    close.offset = ctp::Offset::Close;
    missing_position.create_order(close);
    runner.expect(
        missing_position.apply_trade(trade_report(
            1, "bad-close", 1,
            ctp::Direction::Sell, ctp::Offset::Close)).code
            == ctp::ApplyCode::Conflict,
        "closing a position absent from local state must require reconciliation");
    runner.expect(
        !position(missing_position).known
            && missing_position.reconciliation_required(),
        "an inconsistent position must be marked unknown instead of going negative");
}

void test_trading_hot_path_does_not_allocate(
    test_support::TestRunner& runner)
{
    ctp::AccountTradingState state{"account1", 4, 8};
    const auto seed = order_seed(1, 5);
    const auto first = trade_report(1, "trade-1", 2);
    const auto second = trade_report(1, "trade-2", 3);
    ctp::OrderSnapshot order{};
    ctp::PositionSnapshot held{};

    test_support::AllocationProbe allocation_probe;
    state.create_order(seed);
    state.apply_local_event(1, ctp::LocalOrderEvent::RiskAccepted);
    state.apply_local_event(1, ctp::LocalOrderEvent::Submitted);
    state.apply_order_report({1, ctp::OrderReportType::Accepted, 0});
    state.apply_trade(first);
    state.apply_trade(second);
    state.order_snapshot(1, order);
    state.position_snapshot("IF2609", held);
    allocation_probe.stop();

    runner.expect(
        allocation_probe.count() == 0,
        "preallocated order, report, trade, and snapshot hot paths must not allocate");
    runner.expect(
        order.accounted_filled == 5 && held.long_quantity == 5,
        "the allocation probe must still execute the complete trading path");
}

void test_risk_boundaries_have_stable_reasons(
    test_support::TestRunner& runner)
{
    const auto limits = risk_limits();
    const auto intent = opening_intent();
    const auto healthy = healthy_risk_snapshot();
    runner.expect(
        ctp::evaluate_risk(intent, healthy, limits)
                == ctp::RiskRejectReason::None,
        "exact price, funds, active-order, and position boundaries must pass");

    auto disabled = healthy;
    disabled.enabled = false;
    runner.expect(
        ctp::evaluate_risk(intent, disabled, limits)
                == ctp::RiskRejectReason::AccountDisabled,
        "a disabled account must have a stable rejection reason");

    auto stale = healthy;
    stale.market_receive_ns = stale.now_ns - limits.max_market_age_ns - 1;
    runner.expect(
        ctp::evaluate_risk(intent, stale, limits)
                == ctp::RiskRejectReason::StaleMarket,
        "market data one nanosecond beyond the boundary must be stale");

    auto bad_price = intent;
    bad_price.limit_price_ticks = healthy.ask_price_ticks
        + limits.max_slippage_ticks + 1;
    runner.expect(
        ctp::evaluate_risk(bad_price, healthy, limits)
                == ctp::RiskRejectReason::PriceProtection,
        "a buy one tick beyond the protection boundary must be rejected");

    auto insufficient = healthy;
    --insufficient.available_funds;
    runner.expect(
        ctp::evaluate_risk(intent, insufficient, limits)
                == ctp::RiskRejectReason::InsufficientFunds,
        "funds one unit below margin plus reserve must be rejected");

    auto active = healthy;
    active.active_open_orders = limits.max_active_open_orders;
    runner.expect(
        ctp::evaluate_risk(intent, active, limits)
                == ctp::RiskRejectReason::TooManyActiveOpenOrders,
        "the next opening order beyond the active-order limit must be rejected");

    auto unauthenticated = healthy;
    unauthenticated.authenticated = false;
    runner.expect(
        ctp::evaluate_risk(intent, unauthenticated, limits)
                == ctp::RiskRejectReason::NotAuthenticated,
        "an unauthenticated account must be rejected");
    auto logged_out = healthy;
    logged_out.logged_in = false;
    runner.expect(
        ctp::evaluate_risk(intent, logged_out, limits)
                == ctp::RiskRejectReason::NotLoggedIn,
        "a logged-out account must be rejected");
    auto unreconciled = healthy;
    unreconciled.reconciled = false;
    runner.expect(
        ctp::evaluate_risk(intent, unreconciled, limits)
                == ctp::RiskRejectReason::NotReconciled,
        "an unreconciled account must be rejected");
    auto frozen = healthy;
    frozen.frozen = true;
    runner.expect(
        ctp::evaluate_risk(intent, frozen, limits)
                == ctp::RiskRejectReason::AccountFrozen,
        "a frozen account must be rejected");
    auto stopped = healthy;
    stopped.exiting = true;
    runner.expect(
        ctp::evaluate_risk(intent, stopped, limits)
                == ctp::RiskRejectReason::Exiting,
        "an exiting account must reject new orders");
    auto outside_window = healthy;
    outside_window.trading_window_open = false;
    runner.expect(
        ctp::evaluate_risk(intent, outside_window, limits)
                == ctp::RiskRejectReason::OutsideTradingWindow,
        "orders outside the trading window must be rejected");
    auto killed = healthy;
    killed.account_kill_switch = true;
    runner.expect(
        ctp::evaluate_risk(intent, killed, limits)
                == ctp::RiskRejectReason::KillSwitch,
        "the account kill switch must reject new orders");
    auto unknown_result = healthy;
    unknown_result.result_unknown = true;
    runner.expect(
        ctp::evaluate_risk(intent, unknown_result, limits)
                == ctp::RiskRejectReason::ResultUnknown,
        "an unknown prior submit result must stop another order");
    auto wrong_instrument = intent;
    ctp::copy_to_field(wrong_instrument.instrument, "IC2609");
    runner.expect(
        ctp::evaluate_risk(wrong_instrument, healthy, limits)
                == ctp::RiskRejectReason::InstrumentNotAllowed,
        "an instrument outside the whitelist must be rejected");
    auto forbidden_limits = limits;
    forbidden_limits.allow_buy = false;
    runner.expect(
        ctp::evaluate_risk(intent, healthy, forbidden_limits)
                == ctp::RiskRejectReason::DirectionNotAllowed,
        "a disabled direction must be rejected");
    forbidden_limits = limits;
    forbidden_limits.allow_open = false;
    runner.expect(
        ctp::evaluate_risk(intent, healthy, forbidden_limits)
                == ctp::RiskRejectReason::OffsetNotAllowed,
        "a disabled open offset must be rejected");
    auto bad_quantity = intent;
    bad_quantity.quantity = 0;
    runner.expect(
        ctp::evaluate_risk(bad_quantity, healthy, limits)
                == ctp::RiskRejectReason::InvalidQuantity,
        "a zero quantity must be rejected");
    bad_quantity.quantity = limits.max_order_volume + 1;
    runner.expect(
        ctp::evaluate_risk(bad_quantity, healthy, limits)
                == ctp::RiskRejectReason::InvalidQuantity,
        "an order beyond the configured lot limit must be rejected");
    auto signal_limited = healthy;
    signal_limited.daily_signals = limits.max_daily_signals;
    runner.expect(
        ctp::evaluate_risk(intent, signal_limited, limits)
                == ctp::RiskRejectReason::DailySignalLimit,
        "the next signal beyond the daily limit must be rejected");
    auto order_limited = healthy;
    order_limited.daily_orders = limits.max_daily_orders;
    runner.expect(
        ctp::evaluate_risk(intent, order_limited, limits)
                == ctp::RiskRejectReason::DailyOrderLimit,
        "the next order beyond the daily limit must be rejected");
    auto rate_limited = healthy;
    rate_limited.orders_in_rate_window =
        limits.max_order_rate_per_second;
    runner.expect(
        ctp::evaluate_risk(intent, rate_limited, limits)
                == ctp::RiskRejectReason::OrderRateLimit,
        "the next order at the per-second boundary must be rejected");
    auto invalid_book = healthy;
    invalid_book.bid_price_ticks = invalid_book.ask_price_ticks + 1;
    runner.expect(
        ctp::evaluate_risk(intent, invalid_book, limits)
                == ctp::RiskRejectReason::InvalidMarket,
        "a crossed market must be rejected");
    auto unknown_funds = healthy;
    unknown_funds.funds_known = false;
    runner.expect(
        ctp::evaluate_risk(intent, unknown_funds, limits)
                == ctp::RiskRejectReason::FundsUnknown,
        "an opening order with unknown funds must be rejected");
    auto position_limited = healthy;
    position_limited.long_position = limits.max_net_open_position;
    runner.expect(
        ctp::evaluate_risk(intent, position_limited, limits)
                == ctp::RiskRejectReason::NetPositionLimit,
        "the next lot beyond the net position limit must be rejected");

    auto close = intent;
    close.direction = ctp::Direction::Sell;
    close.offset = ctp::Offset::Close;
    close.limit_price_ticks = healthy.bid_price_ticks
        - limits.max_slippage_ticks;
    auto close_snapshot = healthy;
    close_snapshot.closable_long = 1;
    runner.expect(
        ctp::evaluate_risk(close, close_snapshot, limits)
                == ctp::RiskRejectReason::None,
        "an exactly covered closing order must pass");
    close_snapshot.positions_known = false;
    runner.expect(
        ctp::evaluate_risk(close, close_snapshot, limits)
                == ctp::RiskRejectReason::PositionsUnknown,
        "a closing order with unknown positions must be rejected");
    close_snapshot.positions_known = true;
    close_snapshot.closable_long = 0;
    runner.expect(
        ctp::evaluate_risk(close, close_snapshot, limits)
                == ctp::RiskRejectReason::InsufficientPosition,
        "a closing order beyond the available position must be rejected");

    auto upper_book = healthy;
    upper_book.bid_price_ticks =
        std::numeric_limits<std::int64_t>::max() - 2;
    upper_book.ask_price_ticks =
        std::numeric_limits<std::int64_t>::max() - 1;
    auto upper_price = intent;
    upper_price.limit_price_ticks = std::numeric_limits<std::int64_t>::max();
    runner.expect(
        ctp::evaluate_risk(upper_price, upper_book, limits)
                == ctp::RiskRejectReason::None,
        "price protection must not overflow near the int64 upper boundary");
}

void test_rate_limit_rejection_has_order_state_and_trace(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    CapturingTraceSink trace;
    auto limits = risk_limits();
    limits.max_order_rate_per_second = 1;
    ctp::AccountTradingSession session{
        account, limits, std::move(fake), 8, 16, 8, 16, 1, 1, {}, &trace};
    auto risk = healthy_risk_snapshot();
    risk.orders_in_rate_window = 1;

    const auto rejected = session.submit(opening_intent(78), risk);
    ctp::OrderSnapshot order{};
    const bool has_order = session.order_snapshot(
        rejected.client_order_id, order);
    const auto rejected_trace = std::find_if(
        trace.events.begin(), trace.events.begin() + trace.size,
        [](const ctp::TraceEvent& event) {
            return event.stage == ctp::TraceStage::RiskRejected;
        });

    runner.expect(
        rejected.code == ctp::SubmitCode::RiskRejected
            && rejected.risk_reason == ctp::RiskRejectReason::OrderRateLimit
            && has_order && order.state == ctp::OrderState::RiskRejected,
        "a rate-limited signal must create a terminal rejected order");
    runner.expect(
        rejected_trace != trace.events.begin() + trace.size
            && rejected_trace->trace_id.signal_id == 78
            && rejected_trace->client_order_id == rejected.client_order_id
            && rejected_trace->code == static_cast<std::int32_t>(
                ctp::RiskRejectReason::OrderRateLimit)
            && metrics->order_insert_calls == 0,
        "a rate-limited signal must emit its reason without calling CTP");
}

void test_one_signal_submits_at_most_once(test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth",
        "tcp://127.0.0.1:41001"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto api = std::make_unique<test_support::FakeTraderApi>(metrics);
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(api), 8, 16, 8, 8, 100, 7};
    session.activate(3, 9, "12", "20260909");

    const auto first = session.submit(
        opening_intent(77), healthy_risk_snapshot());
    const auto duplicate = session.submit(
        opening_intent(77), healthy_risk_snapshot());
    runner.expect(
        first.code == ctp::SubmitCode::Submitted
            && duplicate.code == ctp::SubmitCode::Duplicate,
        "the repeated signal must return the original order without resubmitting");
    runner.expect(
        first.client_order_id == 100
            && duplicate.client_order_id == first.client_order_id,
        "one signal must keep one stable client order id");
    runner.expect(
        metrics->order_insert_calls == 1
            && std::string_view{metrics->last_order.OrderRef} == "13",
        "login MaxOrderRef plus one must be used exactly once");
    runner.expect(
        std::string_view{metrics->last_order.InstrumentID} == "IF2609"
            && metrics->last_order.VolumeTotalOriginal == 1
            && metrics->last_order.LimitPrice == 4'002,
        "the accepted intent must be translated into the CTP order request");
}

void test_order_price_converts_ticks_to_ctp_price(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto api = std::make_unique<test_support::FakeTraderApi>(metrics);
    ctp::AccountTradingSession session{
        account,
        risk_limits(),
        std::move(api),
        8,
        16,
        8,
        8,
        1,
        1,
        {},
        nullptr,
        1,
        0.2};

    const auto submitted = session.submit(
        opening_intent(), healthy_risk_snapshot());

    runner.expect(
        submitted.code == ctp::SubmitCode::Submitted
            && std::abs(metrics->last_order.LimitPrice - 800.4) < 1e-9,
        "integer ticks must be converted to the CTP decimal price");
}

void test_ctp_callbacks_are_drained_on_account_thread(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto limits = risk_limits();
    limits.max_net_open_position = 5;
    auto risk = healthy_risk_snapshot();
    risk.available_funds = 600;
    auto intent = opening_intent(88);
    intent.quantity = 5;
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* fake_view = fake.get();
    ctp::AccountTradingSession session{
        account, limits, std::move(fake), 8, 16, 8, 16, 200, 20};
    session.activate(3, 9, "20", "20260909");
    const auto submitted = session.submit(intent, risk);

    CThostFtdcOrderField accepted{};
    ctp::copy_to_field(accepted.OrderRef, "21");
    ctp::copy_to_field(accepted.ExchangeID, "CFFEX");
    ctp::copy_to_field(accepted.OrderSysID, "sys-21");
    accepted.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    accepted.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    fake_view->spi()->OnRtnOrder(&accepted);

    ctp::OrderSnapshot before{};
    session.order_snapshot(submitted.client_order_id, before);
    runner.expect(
        before.state == ctp::OrderState::Submitted,
        "the CTP callback thread must enqueue instead of mutating account state");
    session.drain_callbacks();
    ctp::OrderSnapshot after{};
    session.order_snapshot(submitted.client_order_id, after);
    runner.expect(
        after.state == ctp::OrderState::Accepted,
        "the account thread must apply the queued acceptance");

    const auto send_trade = [fake_view](std::string_view trade_id, int volume) {
        CThostFtdcTradeField trade{};
        ctp::copy_to_field(trade.OrderRef, "21");
        ctp::copy_to_field(trade.TradingDay, "20260902");
        ctp::copy_to_field(trade.ExchangeID, "CFFEX");
        ctp::copy_to_field(trade.TradeID, trade_id);
        ctp::copy_to_field(trade.InstrumentID, "IF2609");
        trade.Direction = THOST_FTDC_D_Buy;
        trade.OffsetFlag = THOST_FTDC_OF_Open;
        trade.Volume = volume;
        fake_view->spi()->OnRtnTrade(&trade);
    };
    send_trade("trade-1", 2);
    send_trade("trade-1", 2);
    send_trade("trade-2", 1);
    send_trade("trade-3", 2);
    session.drain_callbacks();
    ctp::PositionSnapshot held{};
    session.position_snapshot("IF2609", held);
    session.order_snapshot(submitted.client_order_id, after);
    runner.expect(
        after.state == ctp::OrderState::Filled
            && after.accounted_filled == 5 && held.long_quantity == 5,
        "three distinct fills and one duplicate must produce exactly five lots");
}

void test_cancel_fill_race_calls_ctp_once(test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto limits = risk_limits();
    limits.max_net_open_position = 5;
    limits.max_daily_cancels = 3;
    auto risk = healthy_risk_snapshot();
    risk.available_funds = 600;
    auto intent = opening_intent(99);
    intent.quantity = 5;
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* fake_view = fake.get();
    ctp::AccountTradingSession session{
        account, limits, std::move(fake), 8, 16, 8, 16, 300, 30};
    session.activate(4, 10, "30", "20260909");
    const auto submitted = session.submit(intent, risk);

    const auto first_cancel = session.cancel(submitted.client_order_id);
    const auto duplicate_cancel = session.cancel(submitted.client_order_id);
    runner.expect(
        first_cancel.code == ctp::CancelCode::Requested
            && duplicate_cancel.code == ctp::CancelCode::Duplicate
            && metrics->order_action_calls == 1,
        "repeated cancellation must call the CTP interface exactly once");
    runner.expect(
        std::string_view{metrics->last_action.OrderRef} == "31"
            && metrics->last_action.FrontID == 4
            && metrics->last_action.SessionID == 10,
        "cancel must use the original order reference and login identity");

    CThostFtdcOrderField canceled{};
    ctp::copy_to_field(canceled.OrderRef, "31");
    canceled.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    canceled.OrderStatus = THOST_FTDC_OST_Canceled;
    canceled.VolumeTraded = 2;
    fake_view->spi()->OnRtnOrder(&canceled);
    CThostFtdcTradeField late{};
    ctp::copy_to_field(late.OrderRef, "31");
    ctp::copy_to_field(late.TradingDay, "20260902");
    ctp::copy_to_field(late.ExchangeID, "CFFEX");
    ctp::copy_to_field(late.TradeID, "late-fill");
    ctp::copy_to_field(late.InstrumentID, "IF2609");
    late.Direction = THOST_FTDC_D_Buy;
    late.OffsetFlag = THOST_FTDC_OF_Open;
    late.Volume = 5;
    fake_view->spi()->OnRtnTrade(&late);
    session.drain_callbacks();

    ctp::OrderSnapshot final_order{};
    ctp::PositionSnapshot final_position{};
    session.order_snapshot(submitted.client_order_id, final_order);
    session.position_snapshot("IF2609", final_position);
    runner.expect(
        final_order.state == ctp::OrderState::Filled
            && final_order.cancel_acknowledged
            && final_position.long_quantity == 5,
        "a late fill must refine canceled to filled without losing cancel history");
}

void test_synchronous_cancel_failure_can_be_retried(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    fake->order_action_return_code = -1;
    auto limits = risk_limits();
    limits.max_daily_cancels = 1;
    ctp::AccountTradingSession session{
        account, limits, std::move(fake), 4, 4, 4, 4};
    const auto submitted = session.submit(
        opening_intent(100), healthy_risk_snapshot());

    const auto first_cancel = session.cancel(submitted.client_order_id);
    const auto second_cancel = session.cancel(submitted.client_order_id);
    const auto daily_limits = session.daily_limit_snapshot();

    runner.expect(
        first_cancel.code == ctp::CancelCode::RejectedLocally
            && first_cancel.api_return_code == -1
            && second_cancel.code == ctp::CancelCode::RejectedLocally
            && second_cancel.api_return_code == -1
            && metrics->order_action_calls == 2
            && daily_limits.cancels == 0,
        "a synchronous cancel failure must release quota for a later CTP retry");
}

void test_unknown_callback_freezes_only_its_account(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig first_account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    const ctp::AccountConfig second_account{
        "account2", "9999", "user2", "password", "app", "auth", "front"};
    auto first_metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto second_metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto first_api = std::make_unique<test_support::FakeTraderApi>(first_metrics);
    auto second_api = std::make_unique<test_support::FakeTraderApi>(second_metrics);
    auto* first_view = first_api.get();
    ctp::AccountTradingSession first{
        first_account, risk_limits(), std::move(first_api), 4, 4, 4, 4};
    ctp::AccountTradingSession second{
        second_account, risk_limits(), std::move(second_api), 4, 4, 4, 4};

    CThostFtdcOrderField unknown{};
    ctp::copy_to_field(unknown.OrderRef, "999");
    unknown.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    first_view->spi()->OnRtnOrder(&unknown);
    first.drain_callbacks();
    const auto blocked = first.submit(
        opening_intent(401), healthy_risk_snapshot());
    runner.expect(
        first.reconciliation_required()
            && !second.reconciliation_required()
            && blocked.code == ctp::SubmitCode::RiskRejected
            && blocked.risk_reason == ctp::RiskRejectReason::AccountFrozen
            && first.execution_snapshot().fault
                == ctp::AccountFault::UnknownCallback
            && first_metrics->order_insert_calls == 0,
        "an unknown callback must freeze only the owning account session");
}

void test_local_and_exchange_rejections_are_distinct(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    fake->order_insert_return_code = -3;
    ctp::AccountTradingSession local_reject{
        account, risk_limits(), std::move(fake), 4, 4, 4, 4};
    const auto local = local_reject.submit(
        opening_intent(501), healthy_risk_snapshot());
    ctp::OrderSnapshot local_order{};
    local_reject.order_snapshot(local.client_order_id, local_order);
    runner.expect(
        local.code == ctp::SubmitCode::RejectedLocally
            && local.api_return_code == -3
            && local_order.state == ctp::OrderState::SubmitRejectedLocally,
        "a nonzero CTP return code must be a local rejection");

    metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto exchange_fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* exchange_view = exchange_fake.get();
    ctp::AccountTradingSession exchange_reject{
        account, risk_limits(), std::move(exchange_fake), 4, 4, 4, 4};
    const auto submitted = exchange_reject.submit(
        opening_intent(502), healthy_risk_snapshot());
    CThostFtdcInputOrderField rejected{};
    ctp::copy_to_field(rejected.OrderRef, "1");
    CThostFtdcRspInfoField error{};
    error.ErrorID = 31;
    exchange_view->spi()->OnRspOrderInsert(&rejected, &error, 1, true);
    exchange_reject.drain_callbacks();
    ctp::OrderSnapshot exchange_order{};
    exchange_reject.order_snapshot(submitted.client_order_id, exchange_order);
    runner.expect(
        submitted.code == ctp::SubmitCode::Submitted
            && exchange_order.state == ctp::OrderState::Rejected
            && exchange_reject.acceptance_snapshot().lifecycle_failed,
        "an exchange rejection must also fail the current acceptance lifecycle");
}

void test_callback_queue_overflow_is_explicit(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 4, 4, 4, 1};
    session.submit(opening_intent(601), healthy_risk_snapshot());
    CThostFtdcOrderField report{};
    ctp::copy_to_field(report.OrderRef, "1");
    report.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    view->spi()->OnRtnOrder(&report);
    view->spi()->OnRtnOrder(&report);
    runner.expect(
        session.reconciliation_required(),
        "a full callback queue must be observable before another order is allowed");
}

void test_cancel_rejection_and_daily_limit(test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 4, 4, 4, 4};
    const auto submitted = session.submit(
        opening_intent(651), healthy_risk_snapshot());
    session.cancel(submitted.client_order_id);
    CThostFtdcInputOrderActionField action{};
    ctp::copy_to_field(action.OrderRef, "1");
    CThostFtdcRspInfoField error{};
    error.ErrorID = 32;
    view->spi()->OnRspOrderAction(&action, &error, 2, true);
    session.drain_callbacks();
    ctp::OrderSnapshot order{};
    session.order_snapshot(submitted.client_order_id, order);
    runner.expect(
        order.state == ctp::OrderState::Accepted
            && session.daily_limit_snapshot().cancels == 1,
        "an asynchronously rejected cancellation must restore the live order "
        "and retain submitted cancel quota");

    auto no_cancel_limits = risk_limits();
    no_cancel_limits.max_daily_cancels = 0;
    auto limited_metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto limited_api =
        std::make_unique<test_support::FakeTraderApi>(limited_metrics);
    ctp::AccountTradingSession limited{
        account, no_cancel_limits, std::move(limited_api), 4, 4, 4, 4};
    const auto limited_order = limited.submit(
        opening_intent(652), healthy_risk_snapshot());
    runner.expect(
        limited.cancel(limited_order.client_order_id).code
                == ctp::CancelCode::DailyLimit
            && limited_metrics->order_action_calls == 0,
        "the cancel frequency boundary must reject before calling CTP");
}

void test_session_hot_path_does_not_allocate(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 4, 4, 4, 4};
    CThostFtdcOrderField accepted{};
    ctp::copy_to_field(accepted.OrderRef, "1");
    accepted.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    ctp::OrderSnapshot snapshot{};

    test_support::AllocationProbe allocation_probe;
    const auto submitted = session.submit(
        opening_intent(701), healthy_risk_snapshot());
    view->spi()->OnRtnOrder(&accepted);
    session.drain_callbacks();
    session.cancel(submitted.client_order_id);
    session.order_snapshot(submitted.client_order_id, snapshot);
    allocation_probe.stop();
    runner.expect(
        allocation_probe.count() == 0,
        "submit, callback enqueue/drain, cancel, and snapshot must not allocate");
}

void test_open_fill_submits_one_close_and_reaches_zero(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    CapturingTraceSink trace;
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 16, 1, 1,
        auto_close_policy(), &trace, 77};
    const auto entry = session.submit(opening_intent(801), healthy_risk_snapshot());
    emit_trade(view->spi(), "1", "open-801", ctp::Direction::Buy, ctp::Offset::Open);
    session.drain_callbacks();

    test_support::AllocationProbe allocation_probe;
    const auto step = session.on_market(valid_market(), healthy_risk_snapshot());
    allocation_probe.stop();
    runner.expect(
        step.action == ctp::ExecutionAction::ExitSubmitted
            && metrics->order_insert_calls == 2
            && metrics->last_order.Direction == THOST_FTDC_D_Sell
            && metrics->last_order.CombOffsetFlag[0] == THOST_FTDC_OF_Close
            && metrics->last_order.VolumeTotalOriginal == 1
            && metrics->last_order.LimitPrice == 3'998
            && allocation_probe.count() == 0,
        "an opening fill must submit exactly one protected closing order");
    session.on_market(valid_market(2), healthy_risk_snapshot());
    runner.expect(
        metrics->order_insert_calls == 2,
        "later market events must not duplicate an active closing order");

    emit_trade(view->spi(), "2", "close-801", ctp::Direction::Sell, ctp::Offset::Close);
    session.drain_callbacks();
    ctp::PositionSnapshot position{};
    session.position_snapshot("IF2609", position);
    const auto execution = session.execution_snapshot();
    runner.expect(
        entry.code == ctp::SubmitCode::Submitted
            && position.long_quantity == 0
            && execution.active_open_orders == 0
            && execution.close_orders_submitted == 1
            && execution.active_exit_order_id == 0,
        "the closing fill must reach zero and release lifecycle state once");
    const std::array expected_stages{
        ctp::TraceStage::RiskAccepted,
        ctp::TraceStage::OrderSubmitted,
        ctp::TraceStage::Trade,
        ctp::TraceStage::ExitIntent,
        ctp::TraceStage::RiskAccepted,
        ctp::TraceStage::OrderSubmitted,
        ctp::TraceStage::Trade,
    };
    runner.expect(
        trace.size == expected_stages.size(),
        "one lifecycle must emit every decision and effect stage");
    for (std::size_t index = 0;
         index < trace.size && index < expected_stages.size(); ++index) {
        runner.expect(
            trace.events[index].stage == expected_stages[index]
                && trace.events[index].trace_id.run_id == 77
                && trace.events[index].trace_id.signal_id == 801
                && trace.events[index].sequence == index + 1,
            "entry and exit must retain one trace identity in causal order");
    }
}

void test_order_lifecycle_survives_best_effort_trace_saturation(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    QueueTraceSink trace;
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 16, 1, 1,
        auto_close_policy(), &trace, 78};
    const auto market = valid_market();
    const auto best_effort_capacity =
        ctp::kTraceQueueCapacity - ctp::kCriticalTraceReserve;
    for (std::size_t index = 0; index < best_effort_capacity; ++index) {
        session.trace_market(market);
    }
    session.trace_market(market);

    const auto intent = opening_intent(802);
    session.trace_signal(market, intent, healthy_risk_snapshot().now_ns);
    session.submit(intent, healthy_risk_snapshot());
    emit_trade(
        view->spi(), "1", "open-802",
        ctp::Direction::Buy, ctp::Offset::Open);
    session.drain_callbacks();
    session.on_market(market, healthy_risk_snapshot());
    emit_trade(
        view->spi(), "2", "close-802",
        ctp::Direction::Sell, ctp::Offset::Close);
    session.drain_callbacks();

    std::array<ctp::TraceStage, 9> actual{};
    std::size_t actual_size = 0;
    ctp::TraceEvent event{};
    while (trace.queue.try_pop(event)) {
        if (event.trace_id.signal_id == intent.signal_id
            && actual_size < actual.size()) {
            actual[actual_size++] = event.stage;
        }
    }
    const std::array expected{
        ctp::TraceStage::Market,
        ctp::TraceStage::Signal,
        ctp::TraceStage::RiskAccepted,
        ctp::TraceStage::OrderSubmitted,
        ctp::TraceStage::Trade,
        ctp::TraceStage::ExitIntent,
        ctp::TraceStage::RiskAccepted,
        ctp::TraceStage::OrderSubmitted,
        ctp::TraceStage::Trade,
    };
    runner.expect(
        actual_size == expected.size() && actual == expected
            && trace.queue.best_effort_dropped_count() == 1
            && trace.queue.critical_dropped_count() == 0,
        "market trace saturation must not truncate an order lifecycle");
}

void test_entry_timeout_cancels_once(test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 16, 1, 1,
        auto_close_policy()};
    const auto entry = session.submit(opening_intent(804), healthy_risk_snapshot());
    const auto first = session.on_market(valid_market(1), healthy_risk_snapshot());
    const auto second = session.on_market(valid_market(2), healthy_risk_snapshot());
    const auto third = session.on_market(valid_market(3), healthy_risk_snapshot());
    const auto fourth = session.on_market(valid_market(4), healthy_risk_snapshot());
    runner.expect(
        entry.code == ctp::SubmitCode::Submitted
            && first.action == ctp::ExecutionAction::None
            && second.action == ctp::ExecutionAction::None
            && third.action == ctp::ExecutionAction::EntryCancelRequested
            && fourth.action == ctp::ExecutionAction::None
            && metrics->order_action_calls == 1,
        "an entry timeout must request cancellation exactly once");
}

void test_close_fill_wins_cancel_race_without_reprice(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 16, 1, 1,
        auto_close_policy()};
    session.submit(opening_intent(805), healthy_risk_snapshot());
    emit_trade(view->spi(), "1", "open-805", ctp::Direction::Buy, ctp::Offset::Open);
    session.drain_callbacks();
    session.on_market(valid_market(1), healthy_risk_snapshot());
    session.on_market(valid_market(2), healthy_risk_snapshot());
    session.on_market(valid_market(3), healthy_risk_snapshot());

    emit_trade(view->spi(), "2", "close-805", ctp::Direction::Sell, ctp::Offset::Close);
    session.drain_callbacks();
    emit_canceled(view->spi(), "2");
    session.drain_callbacks();
    session.on_market(valid_market(4), healthy_risk_snapshot());

    ctp::PositionSnapshot position{};
    session.position_snapshot("IF2609", position);
    const auto execution = session.execution_snapshot();
    runner.expect(
        metrics->order_insert_calls == 2
            && metrics->order_action_calls == 1
            && position.long_quantity == 0
            && !execution.frozen
            && execution.close_reprices == 0
            && execution.active_exit_order_id == 0,
        "a closing fill that wins the cancel race must suppress reprice");
}

void test_close_retry_exhaustion_freezes_without_faking_zero(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 16, 1, 1,
        auto_close_policy()};
    session.submit(opening_intent(802), healthy_risk_snapshot());
    emit_trade(view->spi(), "1", "open-802", ctp::Direction::Buy, ctp::Offset::Open);
    session.drain_callbacks();
    session.on_market(valid_market(1), healthy_risk_snapshot());
    session.on_market(valid_market(2), healthy_risk_snapshot());
    session.on_market(valid_market(3), healthy_risk_snapshot());
    emit_canceled(view->spi(), "2");
    session.drain_callbacks();
    session.on_market(valid_market(4), healthy_risk_snapshot());
    session.on_market(valid_market(5), healthy_risk_snapshot());
    session.on_market(valid_market(6), healthy_risk_snapshot());
    emit_canceled(view->spi(), "3");
    session.drain_callbacks();

    ctp::PositionSnapshot position{};
    session.position_snapshot("IF2609", position);
    const auto execution = session.execution_snapshot();
    const auto blocked = session.submit(
        opening_intent(803), healthy_risk_snapshot());
    runner.expect(
        metrics->order_insert_calls == 3
            && metrics->order_action_calls == 2
            && position.long_quantity == 1
            && execution.frozen
            && execution.reconciliation_required
            && execution.fault == ctp::AccountFault::CloseRetryExhausted
            && execution.alert_count == 1
            && blocked.code == ctp::SubmitCode::RiskRejected
            && blocked.risk_reason == ctp::RiskRejectReason::AccountFrozen,
        "retry exhaustion must freeze once while preserving the real position");
}

void complete_empty_recovery(
    ctp::AccountTradingSession& session,
    test_support::FakeTraderApi& api)
{
    api.spi()->OnFrontConnected();
    session.drain_callbacks();
    CThostFtdcRspInfoField ok{};
    api.spi()->OnRspAuthenticate(nullptr, &ok, 1, true);
    session.drain_callbacks();
    CThostFtdcRspUserLoginField login{};
    ctp::copy_to_field(login.TradingDay, "20260909");
    login.FrontID = 7;
    login.SessionID = 9;
    ctp::copy_to_field(login.MaxOrderRef, "40");
    api.spi()->OnRspUserLogin(&login, &ok, 2, true);
    session.drain_callbacks();
    api.spi()->OnRspQryOrder(nullptr, &ok, 3, true);
    session.drain_callbacks();
    api.spi()->OnRspQryTrade(nullptr, &ok, 4, true);
    session.drain_callbacks();
    api.spi()->OnRspQryInvestorPosition(nullptr, &ok, 5, true);
    session.drain_callbacks();
    CThostFtdcTradingAccountField funds{};
    api.spi()->OnRspQryTradingAccount(&funds, &ok, 6, true);
    session.drain_callbacks();
}

void test_callback_overflow_recovers_a_lost_trade(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    CapturingTraceSink trace;
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 1, 1, 1,
        auto_close_policy(), &trace, 91};
    session.start();
    complete_empty_recovery(session, *view);

    const auto submitted = session.submit(
        opening_intent(9501), healthy_risk_snapshot());
    CThostFtdcOrderField accepted{};
    ctp::copy_to_field(accepted.OrderRef, "41");
    accepted.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    accepted.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    view->spi()->OnRtnOrder(&accepted);
    emit_trade(
        view->spi(), "41", "lost-open-9501",
        ctp::Direction::Buy, ctp::Offset::Open);
    session.drain_callbacks();

    runner.expect(
        submitted.code == ctp::SubmitCode::Submitted
            && session.execution_snapshot().frozen
            && session.execution_snapshot().fault
                == ctp::AccountFault::CallbackQueueOverflow
            && session.recovery_snapshot().phase
                == ctp::RecoveryPhase::QueryingOrders
            && metrics->authenticate_calls == 1
            && metrics->login_calls == 1
            && metrics->order_query_calls == 2,
        "a ready account must start query reconciliation after callback overflow");

    CThostFtdcRspInfoField ok{};
    CThostFtdcOrderField filled{};
    ctp::copy_to_field(filled.OrderRef, "41");
    filled.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    filled.OrderStatus = THOST_FTDC_OST_AllTraded;
    filled.VolumeTraded = 1;
    view->spi()->OnRspQryOrder(
        &filled, &ok, metrics->order_query_request_id, true);
    session.drain_callbacks();

    CThostFtdcTradeField trade{};
    ctp::copy_to_field(trade.OrderRef, "41");
    ctp::copy_to_field(trade.TradingDay, "20260907");
    ctp::copy_to_field(trade.ExchangeID, "CFFEX");
    ctp::copy_to_field(trade.TradeID, "lost-open-9501");
    ctp::copy_to_field(trade.InstrumentID, "IF2609");
    trade.Direction = THOST_FTDC_D_Buy;
    trade.OffsetFlag = THOST_FTDC_OF_Open;
    trade.Volume = 1;
    view->spi()->OnRspQryTrade(
        &trade, &ok, metrics->trade_query_request_id, true);
    session.drain_callbacks();

    CThostFtdcInvestorPositionField position{};
    ctp::copy_to_field(position.InstrumentID, "IF2609");
    position.PosiDirection = THOST_FTDC_PD_Long;
    position.HedgeFlag = THOST_FTDC_HF_Speculation;
    position.Position = 1;
    view->spi()->OnRspQryInvestorPosition(
        &position, &ok, metrics->position_request_id, true);
    session.drain_callbacks();
    CThostFtdcTradingAccountField funds{};
    funds.Available = 2.0;
    view->spi()->OnRspQryTradingAccount(
        &funds, &ok, metrics->account_request_id, true);
    session.drain_callbacks();

    ctp::OrderSnapshot recovered_order{};
    ctp::PositionSnapshot recovered_position{};
    session.order_snapshot(submitted.client_order_id, recovered_order);
    session.position_snapshot("IF2609", recovered_position);
    const auto close = session.on_market(
        valid_market(), healthy_risk_snapshot());
    emit_trade(
        view->spi(), "41", "lost-open-9501",
        ctp::Direction::Buy, ctp::Offset::Open);
    session.drain_callbacks();
    session.on_market(valid_market(2), healthy_risk_snapshot());

    const auto recovered_traces = std::count_if(
        trace.events.begin(), trace.events.begin() + trace.size,
        [](const ctp::TraceEvent& event) {
            return event.stage == ctp::TraceStage::Trade
                && event.trace_id.signal_id == 9501;
        });
    runner.expect(
        session.recovery_snapshot().phase == ctp::RecoveryPhase::Ready
            && !session.execution_snapshot().frozen
            && recovered_order.state == ctp::OrderState::Filled
            && recovered_position.long_quantity == 1
            && close.action == ctp::ExecutionAction::ExitSubmitted
            && metrics->order_insert_calls == 2
            && recovered_traces == 1,
        "queries must restore a lost fill and a late duplicate must not resubmit");

    // 首次溢出故障值会保留，但恢复成功后的新缺口仍必须重新冻结并核对。
    emit_trade(
        view->spi(), "41", "lost-open-9501",
        ctp::Direction::Buy, ctp::Offset::Open);
    emit_trade(
        view->spi(), "41", "lost-open-9501",
        ctp::Direction::Buy, ctp::Offset::Open);
    session.drain_callbacks();
    runner.expect(
        session.recovery_snapshot().phase == ctp::RecoveryPhase::QueryingOrders
            && session.execution_snapshot().frozen
            && metrics->order_query_calls == 3
            && session.execution_snapshot().alert_count == 1,
        "a recovered account must reconcile again after another callback overflow");
}

void test_callback_overflow_during_recovery_fails_closed(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 1};
    session.start();
    complete_empty_recovery(session, *view);
    session.submit(opening_intent(9502), healthy_risk_snapshot());

    CThostFtdcOrderField accepted{};
    ctp::copy_to_field(accepted.OrderRef, "41");
    accepted.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    accepted.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    view->spi()->OnRtnOrder(&accepted);
    view->spi()->OnRtnOrder(&accepted);
    session.drain_callbacks();

    CThostFtdcRspInfoField ok{};
    view->spi()->OnRspQryOrder(
        &accepted, &ok, metrics->order_query_request_id, false);
    view->spi()->OnRspQryOrder(
        &accepted, &ok, metrics->order_query_request_id, true);
    session.drain_callbacks();

    runner.expect(
        session.recovery_snapshot().phase == ctp::RecoveryPhase::Frozen
            && session.recovery_snapshot().failure
                == ctp::RecoveryFailure::CallbackQueueOverflow
            && session.execution_snapshot().frozen
            && metrics->order_query_calls == 2
            && metrics->trade_query_calls == 1,
        "callback overflow during reconciliation must fail closed without retry");
}

void test_callback_overflow_recovery_is_isolated_across_four_accounts(
    test_support::TestRunner& runner)
{
    std::vector<std::shared_ptr<test_support::FakeTraderMetrics>> metrics;
    std::vector<test_support::FakeTraderApi*> api_views;
    std::vector<std::unique_ptr<ctp::AccountTradingSession>> sessions;
    for (int index = 0; index < 4; ++index) {
        const ctp::AccountConfig account{
            "account" + std::to_string(index + 1),
            "9999", "user" + std::to_string(index + 1),
            "password", "app", "auth", "front"};
        auto account_metrics =
            std::make_shared<test_support::FakeTraderMetrics>();
        auto fake = std::make_unique<test_support::FakeTraderApi>(account_metrics);
        api_views.push_back(fake.get());
        metrics.push_back(account_metrics);
        sessions.push_back(std::make_unique<ctp::AccountTradingSession>(
            account, risk_limits(), std::move(fake), 8, 16, 8,
            index == 0 ? 1 : 4));
        sessions.back()->start();
        complete_empty_recovery(*sessions.back(), *api_views.back());
    }

    const auto target_order = sessions[0]->submit(
        opening_intent(9600), healthy_risk_snapshot());
    CThostFtdcOrderField accepted{};
    ctp::copy_to_field(accepted.OrderRef, "41");
    accepted.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    accepted.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    api_views[0]->spi()->OnRtnOrder(&accepted);
    api_views[0]->spi()->OnRtnOrder(&accepted);
    sessions[0]->drain_callbacks();

    runner.expect(
        target_order.code == ctp::SubmitCode::Submitted
            && sessions[0]->execution_snapshot().frozen
            && sessions[0]->recovery_snapshot().phase
                == ctp::RecoveryPhase::QueryingOrders
            && metrics[0]->order_query_calls == 2,
        "only the overflowing account must enter query reconciliation");
    for (int index = 1; index < 4; ++index) {
        const auto submitted = sessions[index]->submit(
            opening_intent(9600 + index), healthy_risk_snapshot());
        runner.expect(
            submitted.code == ctp::SubmitCode::Submitted
                && sessions[index]->recovery_snapshot().phase
                    == ctp::RecoveryPhase::Ready
                && !sessions[index]->execution_snapshot().frozen
                && metrics[index]->order_query_calls == 1
                && metrics[index]->order_insert_calls == 1,
            "callback recovery must not freeze or query another account");
    }
}

void test_reconnect_queries_before_unfreezing(test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth",
        "tcp://127.0.0.1:41001"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 32};

    session.start();
    view->spi()->OnFrontDisconnected(0x1001);
    session.drain_callbacks();
    runner.expect(
        session.recovery_snapshot().phase == ctp::RecoveryPhase::Disconnected
            && session.execution_snapshot().frozen,
        "disconnect must freeze only this account before recovery");

    complete_empty_recovery(session, *view);
    const auto recovered = session.recovery_snapshot();
    runner.expect(
        recovered.phase == ctp::RecoveryPhase::Ready
            && !session.execution_snapshot().frozen
            && metrics->authenticate_calls == 1
            && metrics->login_calls == 1
            && metrics->order_query_calls == 1
            && metrics->trade_query_calls == 1
            && metrics->position_calls == 1
            && metrics->account_calls == 1,
        "reconnect must complete authentication and four serial queries");

    const auto submitted = session.submit(
        opening_intent(9001), healthy_risk_snapshot());
    runner.expect(
        submitted.code == ctp::SubmitCode::Submitted
            && submitted.order_ref == 41,
        "successful recovery must resume with MaxOrderRef plus one");
}

void test_recovery_persists_available_funds(test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 32};

    session.start();
    view->spi()->OnFrontConnected();
    session.drain_callbacks();
    CThostFtdcRspInfoField ok{};
    view->spi()->OnRspAuthenticate(
        nullptr, &ok, metrics->authenticate_request_id, true);
    session.drain_callbacks();
    CThostFtdcRspUserLoginField login{};
    ctp::copy_to_field(login.TradingDay, "20260909");
    view->spi()->OnRspUserLogin(
        &login, &ok, metrics->login_request_id, true);
    session.drain_callbacks();
    view->spi()->OnRspQryOrder(
        nullptr, &ok, metrics->order_query_request_id, true);
    session.drain_callbacks();
    view->spi()->OnRspQryTrade(
        nullptr, &ok, metrics->trade_query_request_id, true);
    session.drain_callbacks();
    view->spi()->OnRspQryInvestorPosition(
        nullptr, &ok, metrics->position_request_id, true);
    session.drain_callbacks();
    CThostFtdcTradingAccountField funds{};
    funds.Available = 12'345.67;
    view->spi()->OnRspQryTradingAccount(
        &funds, &ok, metrics->account_request_id, true);
    session.drain_callbacks();

    const auto recovered = session.recovery_snapshot();
    runner.expect(
        recovered.phase == ctp::RecoveryPhase::Ready
            && recovered.funds_known
            && recovered.available_funds == 1'234'567,
        "recovery must retain available funds in fixed-point cents");
}

void test_unknown_recovery_order_never_resubmits(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth",
        "tcp://127.0.0.1:41001"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 32};
    session.start();
    view->spi()->OnFrontConnected();
    session.drain_callbacks();
    CThostFtdcRspInfoField ok{};
    view->spi()->OnRspAuthenticate(
        nullptr, &ok, metrics->authenticate_request_id, true);
    session.drain_callbacks();
    CThostFtdcRspUserLoginField login{};
    ctp::copy_to_field(login.TradingDay, "20260909");
    view->spi()->OnRspUserLogin(
        &login, &ok, metrics->login_request_id, true);
    session.drain_callbacks();
    CThostFtdcOrderField unknown{};
    ctp::copy_to_field(unknown.OrderRef, "777");
    unknown.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    view->spi()->OnRspQryOrder(&unknown, &ok, 3, true);
    session.drain_callbacks();

    runner.expect(
        session.recovery_snapshot().phase == ctp::RecoveryPhase::Frozen
            && session.recovery_snapshot().failure
                == ctp::RecoveryFailure::UnknownOrder
            && metrics->order_insert_calls == 0,
        "an unknown queried order must freeze without submitting a replacement");
}

void test_counter_recovery_rejects_invalid_ownership_and_capacity(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    const auto start_recovery = [](
        ctp::AccountTradingSession& session,
        test_support::FakeTraderApi& view,
        const std::shared_ptr<test_support::FakeTraderMetrics>& metrics) {
        session.start();
        view.spi()->OnFrontConnected();
        session.drain_callbacks();
        CThostFtdcRspInfoField ok{};
        view.spi()->OnRspAuthenticate(
            nullptr, &ok, metrics->authenticate_request_id, true);
        session.drain_callbacks();
        CThostFtdcRspUserLoginField login{};
        ctp::copy_to_field(login.TradingDay, "20260911");
        view.spi()->OnRspUserLogin(
            &login, &ok, metrics->login_request_id, true);
        session.drain_callbacks();
    };
    const auto make_order = [](const char* ref, const char* marker) {
        CThostFtdcOrderField order{};
        ctp::copy_to_field(order.OrderRef, std::string_view{ref});
        ctp::copy_to_field(order.BusinessUnit, std::string_view{marker});
        ctp::copy_to_field(order.InstrumentID, "IF2609");
        order.Direction = THOST_FTDC_D_Buy;
        order.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
        order.LimitPrice = 4'000.0;
        order.VolumeTotalOriginal = 1;
        order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
        order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
        return order;
    };

    {
        auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
        auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
        auto* view = fake.get();
        ctp::AccountTradingSession session{
            account, risk_limits(), std::move(fake), 8, 16, 8, 32};
        start_recovery(session, *view, metrics);
        CThostFtdcRspInfoField ok{};
        auto mismatched = make_order("777", "JCTP1:778");
        view->spi()->OnRspQryOrder(
            &mismatched, &ok, metrics->order_query_request_id, true);
        session.drain_callbacks();
        runner.expect(
            session.recovery_snapshot().phase == ctp::RecoveryPhase::Frozen
                && session.recovery_snapshot().failure
                    == ctp::RecoveryFailure::UnknownOrder
                && metrics->order_insert_calls == 0,
            "a mismatched ownership marker must not import a counter order");
    }

    {
        auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
        auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
        auto* view = fake.get();
        ctp::AccountTradingSession session{
            account, risk_limits(), std::move(fake), 8, 16, 8, 32};
        start_recovery(session, *view, metrics);
        CThostFtdcRspInfoField ok{};
        view->spi()->OnRspQryOrder(
            nullptr, &ok, metrics->order_query_request_id, true);
        session.drain_callbacks();
        CThostFtdcTradeField orphan{};
        ctp::copy_to_field(orphan.OrderRef, "777");
        ctp::copy_to_field(orphan.BusinessUnit, "JCTP1:777");
        ctp::copy_to_field(orphan.TradingDay, "20260911");
        ctp::copy_to_field(orphan.ExchangeID, "CFFEX");
        ctp::copy_to_field(orphan.TradeID, "orphan-fill");
        ctp::copy_to_field(orphan.InstrumentID, "IF2609");
        orphan.Direction = THOST_FTDC_D_Buy;
        orphan.OffsetFlag = THOST_FTDC_OF_Open;
        orphan.Volume = 1;
        view->spi()->OnRspQryTrade(
            &orphan, &ok, metrics->trade_query_request_id, true);
        session.drain_callbacks();
        runner.expect(
            session.recovery_snapshot().phase == ctp::RecoveryPhase::Frozen
                && session.recovery_snapshot().failure
                    == ctp::RecoveryFailure::UnknownTrade
                && metrics->order_insert_calls == 0,
            "an owned-looking trade without an imported order must fail closed");
    }

    {
        auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
        auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
        auto* view = fake.get();
        ctp::AccountTradingSession session{
            account, risk_limits(), std::move(fake), 1, 16, 1, 32};
        start_recovery(session, *view, metrics);
        CThostFtdcRspInfoField ok{};
        auto first = make_order("777", "JCTP1:777");
        view->spi()->OnRspQryOrder(
            &first, &ok, metrics->order_query_request_id, false);
        session.drain_callbacks();
        auto second = make_order("778", "JCTP1:778");
        view->spi()->OnRspQryOrder(
            &second, &ok, metrics->order_query_request_id, true);
        session.drain_callbacks();
        runner.expect(
            session.recovery_snapshot().phase == ctp::RecoveryPhase::Frozen
                && session.recovery_snapshot().failure
                    == ctp::RecoveryFailure::CapacityExceeded
                && metrics->order_insert_calls == 0,
            "owned counter orders beyond fixed capacity must fail closed explicitly");
    }
}

bool persist_counter_order(
    const std::filesystem::path& path,
    const CThostFtdcInputOrderField& order)
{
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(
        reinterpret_cast<const char*>(&order),
        static_cast<std::streamsize>(sizeof(order)));
    output.flush();
    return static_cast<bool>(output);
}

bool load_counter_order(
    const std::filesystem::path& path,
    CThostFtdcInputOrderField& order)
{
    std::ifstream input{path, std::ios::binary};
    input.read(
        reinterpret_cast<char*>(&order),
        static_cast<std::streamsize>(sizeof(order)));
    return input.gcount() == static_cast<std::streamsize>(sizeof(order))
        && input.peek() == std::ifstream::traits_type::eof();
}

int run_crashing_order_source(
    int crash_point,
    const std::filesystem::path& journal_path,
    const std::filesystem::path& counter_path)
{
    const auto child = ::fork();
    if (child != 0) return child;

    ctp::AsyncTraceJournal journal{journal_path, "account1"};
    if (!journal.start()) ::_exit(10);
    // 切点 0：日志已建立，但进程在调用 CTP 报单 API 前异常退出。
    if (crash_point == 0) ::_exit(0);

    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    fake->on_order_insert = [&, crash_point](test_support::FakeTraderApi&) {
        // 假柜台在返回成功前同步保存请求，模拟 API 已接受后的外部事实。
        if (!persist_counter_order(counter_path, metrics->last_order)) {
            ::_exit(11);
        }
        // 切点 1：柜台已接受，但 request_order_insert() 尚未返回。
        if (crash_point == 1) ::_exit(0);
        // 先刷新 RiskAccepted，使切点 2 只缺 OrderSubmitted。
        if (!journal.flush()) ::_exit(12);
    };
    ctp::AccountTradingSession source{
        account, risk_limits(), std::move(fake), 8, 16, 8, 32,
        1, 1, {}, &journal, 97};
    const auto submitted = source.submit(
        opening_intent(9701), healthy_risk_snapshot());
    if (submitted.code != ctp::SubmitCode::Submitted
        || journal.snapshot().critical_dropped != 0) {
        ::_exit(13);
    }
    // 切点 2：OrderSubmitted 已进入异步队列，但没有执行刷新屏障。
    if (crash_point == 2) ::_exit(0);
    // 切点 3：OrderSubmitted 已刷新，进程未调用 stop()，因此没有 CleanStop。
    ::_exit(journal.flush() ? 0 : 14);
}

bool recover_crashed_counter_facts(
    const ctp::AccountConfig& account,
    const CThostFtdcInputOrderField* accepted_request)
{
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession successor{
        account, risk_limits(), std::move(fake), 8, 16, 8, 32};
    successor.start();
    view->spi()->OnFrontConnected();
    successor.drain_callbacks();
    CThostFtdcRspInfoField ok{};
    view->spi()->OnRspAuthenticate(
        nullptr, &ok, metrics->authenticate_request_id, true);
    successor.drain_callbacks();
    CThostFtdcRspUserLoginField login{};
    ctp::copy_to_field(login.TradingDay, "20260911");
    view->spi()->OnRspUserLogin(
        &login, &ok, metrics->login_request_id, true);
    successor.drain_callbacks();

    if (accepted_request == nullptr) {
        view->spi()->OnRspQryOrder(
            nullptr, &ok, metrics->order_query_request_id, true);
    } else {
        CThostFtdcOrderField order{};
        ctp::copy_to_field(order.OrderRef, accepted_request->OrderRef);
        ctp::copy_to_field(order.BusinessUnit, accepted_request->BusinessUnit);
        ctp::copy_to_field(order.InstrumentID, accepted_request->InstrumentID);
        order.Direction = accepted_request->Direction;
        order.CombOffsetFlag[0] = accepted_request->CombOffsetFlag[0];
        order.LimitPrice = accepted_request->LimitPrice;
        order.VolumeTotalOriginal = accepted_request->VolumeTotalOriginal;
        order.VolumeTraded = 1;
        order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
        order.OrderStatus = THOST_FTDC_OST_AllTraded;
        view->spi()->OnRspQryOrder(
            &order, &ok, metrics->order_query_request_id, true);
    }
    successor.drain_callbacks();

    if (accepted_request == nullptr) {
        view->spi()->OnRspQryTrade(
            nullptr, &ok, metrics->trade_query_request_id, true);
    } else {
        CThostFtdcTradeField trade{};
        ctp::copy_to_field(trade.OrderRef, accepted_request->OrderRef);
        ctp::copy_to_field(trade.BusinessUnit, accepted_request->BusinessUnit);
        ctp::copy_to_field(trade.TradingDay, "20260911");
        ctp::copy_to_field(trade.ExchangeID, "CFFEX");
        ctp::copy_to_field(trade.TradeID, "crash-fill-1");
        ctp::copy_to_field(trade.InstrumentID, accepted_request->InstrumentID);
        trade.Direction = accepted_request->Direction;
        trade.OffsetFlag = accepted_request->CombOffsetFlag[0];
        trade.Volume = 1;
        view->spi()->OnRspQryTrade(
            &trade, &ok, metrics->trade_query_request_id, true);
    }
    successor.drain_callbacks();

    if (accepted_request == nullptr) {
        view->spi()->OnRspQryInvestorPosition(
            nullptr, &ok, metrics->position_request_id, true);
    } else {
        CThostFtdcInvestorPositionField position{};
        ctp::copy_to_field(position.InstrumentID, accepted_request->InstrumentID);
        position.PosiDirection = THOST_FTDC_PD_Long;
        position.HedgeFlag = THOST_FTDC_HF_Speculation;
        position.Position = 1;
        view->spi()->OnRspQryInvestorPosition(
            &position, &ok, metrics->position_request_id, true);
    }
    successor.drain_callbacks();
    CThostFtdcTradingAccountField funds{};
    funds.Available = 100.0;
    view->spi()->OnRspQryTradingAccount(
        &funds, &ok, metrics->account_request_id, true);
    successor.drain_callbacks();

    ctp::PositionSnapshot held{};
    const bool has_position = successor.position_snapshot("IF2609", held);
    return successor.recovery_snapshot().phase == ctp::RecoveryPhase::Ready
        && metrics->order_insert_calls == 0
        && (accepted_request == nullptr
            ? !has_position
            : has_position && held.long_quantity == 1);
}

void test_owned_counter_order_and_trade_survive_crash_boundaries(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    constexpr std::array<std::size_t, 4> expected_events{0, 0, 1, 2};

    for (int crash_point = 0; crash_point < 4; ++crash_point) {
        const auto suffix = std::to_string(crash_point);
        const std::filesystem::path journal_path{
            "/tmp/ctp_crash_boundary_" + suffix + ".csv"};
        const std::filesystem::path counter_path{
            "/tmp/ctp_crash_boundary_" + suffix + ".order"};
        std::filesystem::remove(journal_path);
        std::filesystem::remove(counter_path);

        const auto child = run_crashing_order_source(
            crash_point, journal_path, counter_path);
        int status = 0;
        const bool child_ok = child > 0
            && ::waitpid(child, &status, 0) == child
            && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        const auto journal = ctp::read_trace_journal(journal_path);
        const auto image = ctp::build_restart_image(journal);
        CThostFtdcInputOrderField accepted_request{};
        const bool counter_has_order = load_counter_order(
            counter_path, accepted_request);
        const bool expected_prefix = journal.events.size()
                == expected_events[static_cast<std::size_t>(crash_point)]
            && (crash_point < 2
                || journal.events[0].stage == ctp::TraceStage::RiskAccepted)
            && (crash_point < 3
                || journal.events[1].stage == ctp::TraceStage::OrderSubmitted);

        runner.expect(
            child_ok && journal.valid && !journal.clean_shutdown && !image.valid
                && counter_has_order == (crash_point != 0)
                && (crash_point == 0
                    || std::string_view{accepted_request.BusinessUnit}
                        == "JCTP1:1")
                && expected_prefix
                && recover_crashed_counter_facts(
                    account, counter_has_order ? &accepted_request : nullptr),
            "each real API/log crash boundary must preserve its distinct facts and recover without resubmitting");

        std::filesystem::remove(journal_path);
        std::filesystem::remove(counter_path);
    }
}

void test_session_emits_one_order_trace_without_allocating(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    CapturingTraceSink trace;
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 16,
        1, 1, {}, &trace, 77};

    test_support::AllocationProbe probe;
    const auto submitted = session.submit(
        opening_intent(9101), healthy_risk_snapshot());
    CThostFtdcOrderField accepted{};
    ctp::copy_to_field(accepted.OrderRef, "1");
    accepted.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    view->spi()->OnRtnOrder(&accepted);
    session.drain_callbacks();
    session.cancel(submitted.client_order_id);
    probe.stop();

    runner.expect(
        probe.count() == 0 && trace.size == 4
            && trace.events[0].stage == ctp::TraceStage::RiskAccepted
            && trace.events[1].stage == ctp::TraceStage::OrderSubmitted
            && trace.events[2].stage == ctp::TraceStage::OrderReport
            && trace.events[3].stage == ctp::TraceStage::CancelRequested,
        "session decisions and CTP effects must enter the trace without allocation");
    for (std::size_t index = 0; index < trace.size; ++index) {
        runner.expect(
            trace.events[index].trace_id.run_id == 77
                && trace.events[index].trace_id.signal_id == 9101
                && trace.events[index].sequence == index + 1,
            "one order trace must keep stable identity and monotonic sequence");
    }
}

void test_duplicate_exchange_facts_do_not_consume_trace_capacity(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    CapturingTraceSink trace;
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 16,
        1, 1, {}, &trace, 77};

    session.submit(opening_intent(9151), healthy_risk_snapshot());
    CThostFtdcOrderField accepted{};
    ctp::copy_to_field(accepted.OrderRef, "1");
    accepted.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    view->spi()->OnRtnOrder(&accepted);
    session.drain_callbacks();
    view->spi()->OnRtnOrder(&accepted);
    session.drain_callbacks();
    emit_trade(
        view->spi(), "1", "trade-9151",
        ctp::Direction::Buy, ctp::Offset::Open);
    session.drain_callbacks();
    emit_trade(
        view->spi(), "1", "trade-9151",
        ctp::Direction::Buy, ctp::Offset::Open);
    session.drain_callbacks();

    runner.expect(
        trace.size == 4
            && trace.events[0].stage == ctp::TraceStage::RiskAccepted
            && trace.events[1].stage == ctp::TraceStage::OrderSubmitted
            && trace.events[2].stage == ctp::TraceStage::OrderReport
            && trace.events[3].stage == ctp::TraceStage::Trade,
        "duplicate order and trade callbacks must not spend critical trace slots");
}

void test_restart_restores_identity_without_resubmitting(
    test_support::TestRunner& runner)
{
    ctp::TraceJournalReadResult journal{};
    journal.valid = true;
    journal.clean_shutdown = true;
    journal.account_id = "account1";
    ctp::TraceEvent submitted{};
    submitted.trace_id = {81, 9201};
    submitted.sequence = 1;
    submitted.stage = ctp::TraceStage::OrderSubmitted;
    submitted.client_order_id = 9;
    submitted.order_ref = 17;
    submitted.quantity = 1;
    submitted.limit_price_ticks = 4'002;
    submitted.direction = static_cast<std::uint8_t>(ctp::Direction::Buy);
    submitted.offset = static_cast<std::uint8_t>(ctp::Offset::Open);
    submitted.purpose = static_cast<std::uint8_t>(ctp::OrderPurpose::Entry);
    ctp::copy_to_field(submitted.instrument, "IF2609");
    journal.events.push_back(submitted);
    ctp::TraceEvent checkpoint{};
    checkpoint.sequence = 2;
    checkpoint.stage = ctp::TraceStage::RestartCheckpoint;
    ctp::copy_to_field(checkpoint.trading_day, "20260907");
    checkpoint.daily_signals = 1;
    checkpoint.daily_orders = 1;
    journal.events.push_back(checkpoint);
    ctp::TraceEvent clean_stop{};
    clean_stop.sequence = 3;
    clean_stop.stage = ctp::TraceStage::CleanStop;
    journal.events.push_back(clean_stop);
    journal.max_client_order_id = 9;
    journal.max_order_ref = 17;
    const auto image = ctp::build_restart_image(journal);

    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 32, 1, 1,
        auto_close_policy()};
    runner.expect(
        session.restore_restart_image(image),
        "a valid journal must restore the uncertain local order identity");
    const auto duplicate = session.submit(
        opening_intent(9201), healthy_risk_snapshot());
    runner.expect(
        duplicate.code == ctp::SubmitCode::Duplicate
            && duplicate.client_order_id == 9
            && duplicate.order_ref == 17
            && metrics->order_insert_calls == 0,
        "restart must return the restored identity without repeating CTP insert");

    session.start();
    view->spi()->OnFrontConnected();
    session.drain_callbacks();
    CThostFtdcRspInfoField ok{};
    view->spi()->OnRspAuthenticate(
        nullptr, &ok, metrics->authenticate_request_id, true);
    session.drain_callbacks();
    CThostFtdcRspUserLoginField login{};
    ctp::copy_to_field(login.TradingDay, "20260907");
    login.FrontID = 7;
    login.SessionID = 9;
    ctp::copy_to_field(login.MaxOrderRef, "40");
    view->spi()->OnRspUserLogin(
        &login, &ok, metrics->login_request_id, true);
    session.drain_callbacks();

    CThostFtdcOrderField filled{};
    ctp::copy_to_field(filled.OrderRef, "17");
    filled.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    filled.OrderStatus = THOST_FTDC_OST_AllTraded;
    filled.VolumeTraded = 1;
    view->spi()->OnRspQryOrder(
        &filled, &ok, metrics->order_query_request_id, true);
    session.drain_callbacks();

    CThostFtdcTradeField trade{};
    ctp::copy_to_field(trade.OrderRef, "17");
    ctp::copy_to_field(trade.TradingDay, "20260907");
    ctp::copy_to_field(trade.ExchangeID, "CFFEX");
    ctp::copy_to_field(trade.TradeID, "restart-open-9201");
    ctp::copy_to_field(trade.InstrumentID, "IF2609");
    trade.Direction = THOST_FTDC_D_Buy;
    trade.OffsetFlag = THOST_FTDC_OF_Open;
    trade.Volume = 1;
    view->spi()->OnRspQryTrade(
        &trade, &ok, metrics->trade_query_request_id, true);
    session.drain_callbacks();

    CThostFtdcInvestorPositionField position{};
    ctp::copy_to_field(position.InstrumentID, "IF2609");
    position.PosiDirection = THOST_FTDC_PD_Long;
    position.HedgeFlag = THOST_FTDC_HF_Speculation;
    position.Position = 1;
    view->spi()->OnRspQryInvestorPosition(
        &position, &ok, metrics->position_request_id, true);
    session.drain_callbacks();
    CThostFtdcTradingAccountField funds{};
    view->spi()->OnRspQryTradingAccount(
        &funds, &ok, metrics->account_request_id, true);
    session.drain_callbacks();

    const auto exit = session.on_market(
        valid_market(2), healthy_risk_snapshot());
    runner.expect(
        session.recovery_snapshot().phase == ctp::RecoveryPhase::Ready
            && exit.action == ctp::ExecutionAction::ExitSubmitted
            && metrics->order_insert_calls == 1
            && std::string_view{metrics->last_order.OrderRef} == "41"
            && metrics->last_order.CombOffsetFlag[0] == THOST_FTDC_OF_Close,
        "queried fill and position must resume at close without replaying the open");
}

void test_risk_rejections_preserve_restart_signal_quota(
    test_support::TestRunner& runner)
{
    char directory[] = "/tmp/ctp-signal-quota-XXXXXX";
    const auto* root = ::mkdtemp(directory);
    runner.expect(root != nullptr, "signal quota journal directory must exist");
    if (root == nullptr) return;
    const auto path = std::filesystem::path{root} / "account1.csv";
    const ctp::AccountConfig account{
        "account1", "9999", "user", "password", "app", "auth", "front"};
    auto limits = risk_limits();
    limits.max_daily_signals = 2;
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    // 同步拒单不会留下柜台未决单，但已经接受的信号仍占额度。
    fake->order_insert_return_code = -1;
    auto* source_api = fake.get();
    ctp::AsyncTraceJournal journal{path, "account1"};
    runner.expect(journal.start(), "signal quota journal must start");
    {
        ctp::AccountTradingSession source{
            account, limits, std::move(fake), 16, 32, 8, 32,
            1, 1, {}, &journal, 101};
        ctp::RestartImage initial{};
        initial.valid = true;
        initial.account_id = "account1";
        initial.trading_day = "20260909";
        initial.daily_signals = 1;
        runner.expect(source.restore_restart_image(initial),
            "source must restore one of two signal slots");
        source.start();
        complete_empty_recovery(source, *source_api);
        runner.expect(source.recovery_snapshot().phase == ctp::RecoveryPhase::Ready
                && source.daily_limit_snapshot().signals == 1,
            "source must finish counter reconciliation before accepting signals");
        const auto accepted = source.submit(opening_intent(9801), healthy_risk_snapshot());
        runner.expect(accepted.code == ctp::SubmitCode::RejectedLocally
                && metrics->order_insert_calls == 1
                && source.daily_limit_snapshot().signals == 2,
            "risk-accepted signal must consume its slot even if submission fails");
        for (std::uint64_t id = 9802; id < 9805; ++id) {
            const auto rejected = source.submit(opening_intent(id), healthy_risk_snapshot());
            runner.expect(rejected.risk_reason == ctp::RiskRejectReason::DailySignalLimit
                    && source.daily_limit_snapshot().signals == 2
                    && metrics->order_insert_calls == 1,
                "repeated daily limit rejections must preserve the full quota");
        }
        auto disabled = healthy_risk_snapshot();
        disabled.enabled = false;
        runner.expect(source.submit(opening_intent(9800), disabled).risk_reason
                == ctp::RiskRejectReason::AccountDisabled
                && source.daily_limit_snapshot().signals == 2,
            "non-quota risk rejection must not consume a signal slot");
        runner.expect(source.checkpoint_restart_state(), "full quota checkpoint must persist");
    }
    journal.stop();
    const auto image = ctp::build_restart_image(ctp::read_trace_journal(path));
    auto next_metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto next_api = std::make_unique<test_support::FakeTraderApi>(next_metrics);
    auto* next_view = next_api.get();
    ctp::AccountTradingSession successor{
        account, limits, std::move(next_api),
        16, 32, 8, 32};
    runner.expect(image.valid && image.daily_signals == 2
            && successor.restore_restart_image(image),
        "successor must restore the real full-quota journal without initialization failure");
    successor.start();
    complete_empty_recovery(successor, *next_view);
    runner.expect(successor.recovery_snapshot().phase == ctp::RecoveryPhase::Ready,
        "successor must become ready after restoring the full quota");
    runner.expect(successor.submit(opening_intent(9810), healthy_risk_snapshot()).risk_reason
            == ctp::RiskRejectReason::DailySignalLimit
            && next_metrics->order_insert_calls == 0,
        "same-day restart must not refresh the signal quota");
    runner.expect(successor.activate(1, 1, "0", "20260910")
            && successor.daily_limit_snapshot().signals == 0
            && successor.submit(opening_intent(9811), healthy_risk_snapshot()).code
                == ctp::SubmitCode::Submitted
            && next_metrics->order_insert_calls == 1,
        "confirmed next trading day must allow a new signal");
    std::filesystem::remove_all(root);
}

void test_restart_daily_limits_are_same_day_and_account_scoped(
    test_support::TestRunner& runner)
{
    {
        const ctp::AccountConfig account{
            "invalid-quota", "9999", "user", "password", "app", "auth", "front"};
        auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
        auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
        ctp::AccountTradingSession session{
            account, risk_limits(), std::move(fake), 8, 16, 8, 32};
        ctp::RestartImage image{};
        image.valid = true;
        image.account_id = "invalid-quota";
        image.trading_day = "20260909";
        image.daily_orders = 11;
        runner.expect(
            !session.restore_restart_image(image),
            "a checkpoint beyond the configured quota must fail closed");
    }

    for (std::size_t index = 0; index < 4; ++index) {
        const std::string alias = "account" + std::to_string(index + 1);
        const ctp::AccountConfig account{
            alias, "9999", "user", "password", "app", "auth", "front"};
        auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
        auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
        CapturingTraceSink trace;
        ctp::AccountTradingSession session{
            account, risk_limits(), std::move(fake), 8, 16, 8, 32,
            1, 1, {}, &trace, 91 + index};
        ctp::RestartImage image{};
        image.valid = true;
        image.account_id = alias;
        image.next_client_order_id = 1;
        image.next_order_ref = 1;
        image.trading_day = "20260909";
        image.daily_orders = index == 0 ? 10 : 0;
        runner.expect(
            session.restore_restart_image(image)
                && session.activate(1, 1, "0", "20260909"),
            "each account must restore only its own same-day quota snapshot");
        const auto restored = session.daily_limit_snapshot();
        runner.expect(
            index == 0
                ? restored.orders == 10
                : restored.orders == 0,
            "one exhausted account must not refresh quota or affect peers");
        runner.expect(
            session.checkpoint_restart_state()
                && trace.events[trace.size - 1].stage
                    == ctp::TraceStage::RestartCheckpoint
                && std::string_view{
                       trace.events[trace.size - 1].trading_day.data()}
                    == "20260909",
            "shutdown checkpoint must retain the account trading day and counters");
    }

    const ctp::AccountConfig account{
        "next-day", "9999", "user", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 32};
    ctp::RestartImage image{};
    image.valid = true;
    image.account_id = "next-day";
    image.trading_day = "20260909";
    image.daily_signals = 10;
    image.daily_orders = 10;
    image.daily_cancels = 10;
    runner.expect(
        session.restore_restart_image(image)
            && session.activate(1, 1, "0", "20260910")
            && session.daily_limit_snapshot().signals == 0
            && session.daily_limit_snapshot().orders == 0
            && session.daily_limit_snapshot().cancels == 0,
        "a confirmed new CTP trading day must reset restored daily quotas");
}

void test_missing_uncertain_order_stays_frozen(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 32};
    session.submit(opening_intent(9301), healthy_risk_snapshot());
    session.start();
    view->spi()->OnFrontConnected();
    session.drain_callbacks();
    CThostFtdcRspInfoField ok{};
    view->spi()->OnRspAuthenticate(
        nullptr, &ok, metrics->authenticate_request_id, true);
    session.drain_callbacks();
    CThostFtdcRspUserLoginField login{};
    ctp::copy_to_field(login.TradingDay, "20260909");
    view->spi()->OnRspUserLogin(
        &login, &ok, metrics->login_request_id, true);
    session.drain_callbacks();
    view->spi()->OnRspQryOrder(
        nullptr, &ok, metrics->order_query_request_id, true);
    session.drain_callbacks();

    runner.expect(
        session.recovery_snapshot().phase == ctp::RecoveryPhase::Frozen
            && session.recovery_snapshot().failure
                == ctp::RecoveryFailure::MissingOrder
            && metrics->trade_query_calls == 0
            && metrics->order_insert_calls == 1,
        "an uncertain local order missing from CTP query must not be guessed away");
}

void test_one_of_four_recovery_failures_is_isolated(
    test_support::TestRunner& runner)
{
    std::vector<std::shared_ptr<test_support::FakeTraderMetrics>> metrics;
    std::vector<test_support::FakeTraderApi*> api_views;
    std::vector<std::unique_ptr<ctp::AccountTradingSession>> sessions;
    for (int index = 0; index < 4; ++index) {
        const ctp::AccountConfig account{
            "account" + std::to_string(index + 1),
            "9999", "user", "password", "app", "auth", "front"};
        auto account_metrics =
            std::make_shared<test_support::FakeTraderMetrics>();
        auto fake = std::make_unique<test_support::FakeTraderApi>(account_metrics);
        api_views.push_back(fake.get());
        metrics.push_back(account_metrics);
        sessions.push_back(std::make_unique<ctp::AccountTradingSession>(
            account, risk_limits(), std::move(fake), 8, 16, 8, 32));
        sessions.back()->start();
    }

    for (int index = 0; index < 4; ++index) {
        if (index == 1) {
            api_views[index]->spi()->OnFrontConnected();
            sessions[index]->drain_callbacks();
            CThostFtdcRspInfoField error{};
            error.ErrorID = 7;
            api_views[index]->spi()->OnRspAuthenticate(
                nullptr, &error, 1, true);
            sessions[index]->drain_callbacks();
        } else {
            complete_empty_recovery(*sessions[index], *api_views[index]);
        }
    }

    for (int index = 0; index < 4; ++index) {
        const auto result = sessions[index]->submit(
            opening_intent(9400 + index), healthy_risk_snapshot());
        if (index == 1) {
            runner.expect(
                result.code == ctp::SubmitCode::RiskRejected
                    && metrics[index]->order_insert_calls == 0,
                "the failed account must remain frozen");
        } else {
            runner.expect(
                result.code == ctp::SubmitCode::Submitted
                    && metrics[index]->order_insert_calls == 1,
                "each healthy account must recover and submit independently");
        }
    }
}

void test_stale_recovery_response_cannot_advance_phase(
    test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth", "front"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
    auto* view = fake.get();
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(fake), 8, 16, 8, 16};
    session.start();
    view->spi()->OnFrontConnected();
    session.drain_callbacks();
    CThostFtdcRspInfoField ok{};
    view->spi()->OnRspAuthenticate(nullptr, &ok, 99, true);
    session.drain_callbacks();
    runner.expect(
        session.recovery_snapshot().phase == ctp::RecoveryPhase::Frozen
            && session.recovery_snapshot().failure
                == ctp::RecoveryFailure::UnexpectedResponse
            && metrics->login_calls == 0,
        "a stale request id must freeze instead of advancing recovery");
}

void test_each_recovery_query_error_stays_frozen(
    test_support::TestRunner& runner)
{
    for (int failed_query = 0; failed_query < 4; ++failed_query) {
        const ctp::AccountConfig account{
            "account" + std::to_string(failed_query + 1),
            "9999", "user", "password", "app", "auth", "front"};
        auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
        auto fake = std::make_unique<test_support::FakeTraderApi>(metrics);
        auto* view = fake.get();
        ctp::AccountTradingSession session{
            account, risk_limits(), std::move(fake), 8, 16, 8, 32};

        session.start();
        view->spi()->OnFrontConnected();
        session.drain_callbacks();
        CThostFtdcRspInfoField ok{};
        view->spi()->OnRspAuthenticate(
            nullptr, &ok, metrics->authenticate_request_id, true);
        session.drain_callbacks();
        CThostFtdcRspUserLoginField login{};
        ctp::copy_to_field(login.TradingDay, "20260909");
        view->spi()->OnRspUserLogin(
            &login, &ok, metrics->login_request_id, true);
        session.drain_callbacks();

        CThostFtdcRspInfoField error{};
        error.ErrorID = 7;
        if (failed_query == 0) {
            view->spi()->OnRspQryOrder(
                nullptr, &error, metrics->order_query_request_id, true);
        } else {
            view->spi()->OnRspQryOrder(
                nullptr, &ok, metrics->order_query_request_id, true);
            session.drain_callbacks();
        }
        if (failed_query == 1) {
            view->spi()->OnRspQryTrade(
                nullptr, &error, metrics->trade_query_request_id, true);
        } else if (failed_query > 1) {
            view->spi()->OnRspQryTrade(
                nullptr, &ok, metrics->trade_query_request_id, true);
            session.drain_callbacks();
        }
        if (failed_query == 2) {
            view->spi()->OnRspQryInvestorPosition(
                nullptr, &error, metrics->position_request_id, true);
        } else if (failed_query > 2) {
            view->spi()->OnRspQryInvestorPosition(
                nullptr, &ok, metrics->position_request_id, true);
            session.drain_callbacks();
        }
        if (failed_query == 3) {
            view->spi()->OnRspQryTradingAccount(
                nullptr, &error, metrics->account_request_id, true);
        }
        session.drain_callbacks();

        const auto recovery = session.recovery_snapshot();
        runner.expect(
            recovery.phase == ctp::RecoveryPhase::Frozen
                && recovery.failure == ctp::RecoveryFailure::ResponseError
                && session.execution_snapshot().frozen
                && metrics->order_insert_calls == 0,
            "every failed recovery query must keep its account frozen");
    }
}

}

int main()
{
    test_support::TestRunner runner{"trading"};
    test_local_order_states(runner);
    test_terminal_and_invalid_transitions(runner);
    test_order_capacity_and_duplicates(runner);
    test_multiple_fills_are_deduplicated(runner);
    test_trade_identity_scope_and_account_isolation(runner);
    test_open_and_close_positions(runner);
    test_all_report_permutations_converge(runner);
    test_cancel_fill_race_converges(runner);
    test_conflicts_and_fixed_capacity(runner);
    test_trading_hot_path_does_not_allocate(runner);
    test_risk_boundaries_have_stable_reasons(runner);
    test_rate_limit_rejection_has_order_state_and_trace(runner);
    test_one_signal_submits_at_most_once(runner);
    test_order_price_converts_ticks_to_ctp_price(runner);
    test_ctp_callbacks_are_drained_on_account_thread(runner);
    test_cancel_fill_race_calls_ctp_once(runner);
    test_synchronous_cancel_failure_can_be_retried(runner);
    test_unknown_callback_freezes_only_its_account(runner);
    test_local_and_exchange_rejections_are_distinct(runner);
    test_callback_queue_overflow_is_explicit(runner);
    test_cancel_rejection_and_daily_limit(runner);
    test_session_hot_path_does_not_allocate(runner);
    test_open_fill_submits_one_close_and_reaches_zero(runner);
    test_order_lifecycle_survives_best_effort_trace_saturation(runner);
    test_close_retry_exhaustion_freezes_without_faking_zero(runner);
    test_entry_timeout_cancels_once(runner);
    test_close_fill_wins_cancel_race_without_reprice(runner);
    test_callback_overflow_recovers_a_lost_trade(runner);
    test_callback_overflow_during_recovery_fails_closed(runner);
    test_callback_overflow_recovery_is_isolated_across_four_accounts(runner);
    test_reconnect_queries_before_unfreezing(runner);
    test_recovery_persists_available_funds(runner);
    test_unknown_recovery_order_never_resubmits(runner);
    test_counter_recovery_rejects_invalid_ownership_and_capacity(runner);
    test_owned_counter_order_and_trade_survive_crash_boundaries(runner);
    test_session_emits_one_order_trace_without_allocating(runner);
    test_duplicate_exchange_facts_do_not_consume_trace_capacity(runner);
    test_restart_restores_identity_without_resubmitting(runner);
    test_restart_daily_limits_are_same_day_and_account_scoped(runner);
    test_risk_rejections_preserve_restart_signal_quota(runner);
    test_missing_uncertain_order_stays_frozen(runner);
    test_one_of_four_recovery_failures_is_isolated(runner);
    test_stale_recovery_response_cannot_advance_phase(runner);
    test_each_recovery_query_error_stays_frozen(runner);
    return runner.finish();
}
