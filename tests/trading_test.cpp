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

}

int main()
{
    test_support::TestRunner runner{"trading"};
    test_local_order_states(runner);
    test_terminal_and_invalid_transitions(runner);
    test_order_capacity_and_duplicates(runner);
    return runner.finish();
}
