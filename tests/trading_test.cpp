#include "ctp/field.hpp"
#include "ctp/trading.hpp"
#define CTP_TEST_DEFINE_ALLOCATION_OPERATORS
#include "test_support.hpp"

#include <algorithm>
#include <array>

namespace {

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
    return runner.finish();
}
