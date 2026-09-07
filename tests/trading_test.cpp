#include "ctp/field.hpp"
#include "ctp/engine.hpp"
#include "ctp/telemetry.hpp"
#include "ctp/trading.hpp"
#define CTP_TEST_DEFINE_ALLOCATION_OPERATORS
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

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

void test_one_signal_submits_at_most_once(test_support::TestRunner& runner)
{
    const ctp::AccountConfig account{
        "account1", "9999", "user1", "password", "app", "auth",
        "tcp://127.0.0.1:41001"};
    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto api = std::make_unique<test_support::FakeTraderApi>(metrics);
    ctp::AccountTradingSession session{
        account, risk_limits(), std::move(api), 8, 16, 8, 8, 100, 7};
    session.activate(3, 9, "12");

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
    session.activate(3, 9, "20");
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
    session.activate(4, 10, "30");
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
            && exchange_order.state == ctp::OrderState::Rejected,
        "a zero API return followed by an error callback is an exchange rejection");
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
        order.state == ctp::OrderState::Accepted,
        "a rejected cancellation must restore the last known live order state");

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
    test_one_signal_submits_at_most_once(runner);
    test_order_price_converts_ticks_to_ctp_price(runner);
    test_ctp_callbacks_are_drained_on_account_thread(runner);
    test_cancel_fill_race_calls_ctp_once(runner);
    test_unknown_callback_freezes_only_its_account(runner);
    test_local_and_exchange_rejections_are_distinct(runner);
    test_callback_queue_overflow_is_explicit(runner);
    test_cancel_rejection_and_daily_limit(runner);
    test_session_hot_path_does_not_allocate(runner);
    test_open_fill_submits_one_close_and_reaches_zero(runner);
    test_close_retry_exhaustion_freezes_without_faking_zero(runner);
    test_entry_timeout_cancels_once(runner);
    test_close_fill_wins_cancel_race_without_reprice(runner);
    test_reconnect_queries_before_unfreezing(runner);
    test_recovery_persists_available_funds(runner);
    test_unknown_recovery_order_never_resubmits(runner);
    test_session_emits_one_order_trace_without_allocating(runner);
    test_restart_restores_identity_without_resubmitting(runner);
    test_missing_uncertain_order_stays_frozen(runner);
    test_one_of_four_recovery_failures_is_isolated(runner);
    test_stale_recovery_response_cannot_advance_phase(runner);
    test_each_recovery_query_error_stays_frozen(runner);
    return runner.finish();
}
