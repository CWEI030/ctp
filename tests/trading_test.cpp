#include "ctp/field.hpp"
#include "ctp/trading.hpp"
#include "test_support.hpp"

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
    return runner.finish();
}
