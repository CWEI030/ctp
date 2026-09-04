#include "ctp/field.hpp"
#include "ctp/strategy.hpp"
#define CTP_TEST_DEFINE_ALLOCATION_OPERATORS
#include "test_support.hpp"

#include <limits>

namespace {

ctp::StrategyConfig strategy_config()
{
    ctp::StrategyConfig config{};
    ctp::copy_to_field(config.instrument, "IF2609");
    config.threshold_ticks = 500;
    config.protection_ticks = 2;
    config.max_market_age_ns = 100;
    config.max_signals_per_run = 1;
    config.retrigger_below_count = 1;
    return config;
}

ctp::MarketEvent market(
    std::uint64_t sequence,
    std::int64_t last_price,
    std::int64_t bid_price = 499,
    std::int64_t ask_price = 501)
{
    ctp::MarketEvent event{};
    event.market_seq = sequence;
    event.exchange_time_ms = 34'200'000 + static_cast<std::int64_t>(sequence);
    event.recv_mono_ns = 1'000 + static_cast<std::int64_t>(sequence);
    event.last_price_ticks = last_price;
    event.bid_price_ticks = bid_price;
    event.ask_price_ticks = ask_price;
    event.bid_volume = 10;
    event.ask_volume = 12;
    event.volume = 100;
    event.status = ctp::MarketDataStatus::Valid;
    ctp::copy_to_field(event.instrument, "IF2609");
    return event;
}

ctp::StrategyDecision decide(
    ctp::ThresholdStrategy& strategy,
    const ctp::MarketEvent& event)
{
    return strategy.on_market(event, event.recv_mono_ns + 10);
}

void test_exact_crossing_and_intent(test_support::TestRunner& runner)
{
    ctp::ThresholdStrategy strategy{strategy_config()};
    const auto first = decide(strategy, market(1, 499));
    runner.expect(
        first.code == ctp::StrategyDecisionCode::BaselineEstablished
            && !first.has_intent,
        "the first valid quote must only establish the comparison baseline");

    const auto crossing = decide(strategy, market(2, 500, 499, 502));
    runner.expect(
        crossing.code == ctp::StrategyDecisionCode::SignalGenerated
            && crossing.has_intent,
        "strictly below to exactly equal threshold must generate a signal");
    runner.expect(crossing.intent.signal_id == 2, "signal id must equal market sequence");
    runner.expect(
        crossing.intent.instrument == strategy_config().instrument,
        "the configured fixed instrument must reach the order intent");
    runner.expect(
        crossing.intent.direction == ctp::Direction::Buy
            && crossing.intent.offset == ctp::Offset::Open
            && crossing.intent.quantity == 1,
        "the threshold signal must create one-lot buy-open intent");
    runner.expect(
        crossing.intent.limit_price_ticks == 504,
        "limit price must equal integer ask ticks plus protection ticks");

    const auto repeated_high = decide(strategy, market(3, 510));
    runner.expect(
        repeated_high.code == ctp::StrategyDecisionCode::NoSignal
            && !repeated_high.has_intent,
        "remaining above threshold must not emit another signal");
}

void test_first_quote_and_crossing_edges(test_support::TestRunner& runner)
{
    ctp::ThresholdStrategy starts_equal{strategy_config()};
    runner.expect(
        !decide(starts_equal, market(1, 500)).has_intent,
        "a first quote equal to threshold must not trigger");

    ctp::ThresholdStrategy starts_high{strategy_config()};
    runner.expect(
        !decide(starts_high, market(1, 501)).has_intent,
        "a first quote above threshold must not trigger");

    ctp::ThresholdStrategy no_crossing{strategy_config()};
    decide(no_crossing, market(1, 499));
    runner.expect(
        !decide(no_crossing, market(2, 499)).has_intent,
        "two quotes below threshold must not trigger");
}

void test_invalid_input_clears_baseline(test_support::TestRunner& runner)
{
    const auto expect_cleared = [&runner](ctp::MarketEvent invalid, const char* message) {
        ctp::ThresholdStrategy strategy{strategy_config()};
        decide(strategy, market(1, 499));
        runner.expect(!decide(strategy, invalid).has_intent, message);
        runner.expect(
            !decide(strategy, market(3, 500)).has_intent,
            "a quote after invalid input must rebuild rather than cross the old baseline");
    };

    auto invalid_book = market(2, 499);
    invalid_book.bid_price_ticks = 502;
    invalid_book.ask_price_ticks = 501;
    expect_cleared(invalid_book, "a crossed book must not generate an intent");

    auto invalid_price = market(2, 0);
    expect_cleared(invalid_price, "a non-positive integer price must be rejected");

    auto wrong_instrument = market(2, 499);
    ctp::copy_to_field(wrong_instrument.instrument, "rb2701");
    expect_cleared(wrong_instrument, "an unconfigured instrument must be ignored");

    auto invalid_status = market(2, 499);
    invalid_status.status = ctp::MarketDataStatus::InvalidPrice;
    expect_cleared(invalid_status, "an upstream invalid status must be rejected");

    ctp::ThresholdStrategy stale{strategy_config()};
    decide(stale, market(1, 499));
    auto stale_event = market(2, 499);
    runner.expect(
        !stale.on_market(stale_event, stale_event.recv_mono_ns + 101).has_intent,
        "an event older than the configured age must be rejected");
    runner.expect(
        !decide(stale, market(3, 500)).has_intent,
        "a stale event must clear the previous crossing baseline");
}

void test_sequence_fault_latches(test_support::TestRunner& runner)
{
    const auto expect_fault = [&runner](std::uint64_t faulty_sequence) {
        ctp::ThresholdStrategy strategy{strategy_config()};
        decide(strategy, market(10, 499));
        const auto fault = decide(strategy, market(faulty_sequence, 500));
        runner.expect(
            fault.code == ctp::StrategyDecisionCode::SequenceFault
                && fault.faulted && !fault.has_intent,
            "duplicate, backward, or gapped sequence must latch a fault");
        const auto later = decide(strategy, market(faulty_sequence + 1, 499));
        runner.expect(
            later.faulted && !later.has_intent,
            "a latched sequence fault must reject all later events");
    };

    expect_fault(10);
    expect_fault(9);
    expect_fault(12);
}

void test_retrigger_cooldown_and_limit(test_support::TestRunner& runner)
{
    auto config = strategy_config();
    config.max_signals_per_run = 2;
    config.retrigger_below_count = 2;
    config.cooldown_market_events = 3;
    ctp::ThresholdStrategy strategy{config};

    decide(strategy, market(1, 499));
    runner.expect(decide(strategy, market(2, 500)).has_intent, "first crossing must trigger");
    decide(strategy, market(3, 499));
    decide(strategy, market(4, 498));
    const auto second = decide(strategy, market(5, 500));
    runner.expect(
        second.has_intent && second.intent.signal_id == 5,
        "retrigger requires the configured below count and cooldown");
    decide(strategy, market(6, 499));
    decide(strategy, market(7, 499));
    runner.expect(
        !decide(strategy, market(8, 500)).has_intent,
        "maximum signals per run must remain a hard limit");
}

void test_integer_protection_and_overflow(test_support::TestRunner& runner)
{
    auto zero = strategy_config();
    zero.protection_ticks = 0;
    ctp::ThresholdStrategy zero_strategy{zero};
    decide(zero_strategy, market(1, 499));
    runner.expect(
        decide(zero_strategy, market(2, 500, 499, 503)).intent.limit_price_ticks
            == 503,
        "zero protection must preserve exact integer ask ticks");

    auto multiple = strategy_config();
    multiple.protection_ticks = 7;
    ctp::ThresholdStrategy multiple_strategy{multiple};
    decide(multiple_strategy, market(1, 499));
    runner.expect(
        decide(multiple_strategy, market(2, 501, 499, 503)).intent.limit_price_ticks
            == 510,
        "multiple protection ticks must use exact integer addition");

    ctp::ThresholdStrategy overflow{strategy_config()};
    decide(overflow, market(1, 499));
    const auto rejected = decide(
        overflow,
        market(2, 500, 499, std::numeric_limits<std::int64_t>::max() - 1));
    runner.expect(
        rejected.code == ctp::StrategyDecisionCode::PriceOverflow
            && !rejected.has_intent,
        "ask plus protection overflow must reject the intent");

    auto negative = strategy_config();
    negative.protection_ticks = -1;
    ctp::ThresholdStrategy invalid{negative};
    runner.expect(
        decide(invalid, market(1, 499)).code
                == ctp::StrategyDecisionCode::ConfigurationFault
            && invalid.faulted(),
        "negative protection configuration must fail closed");
}

void test_hot_path_does_not_allocate(test_support::TestRunner& runner)
{
    ctp::ThresholdStrategy strategy{strategy_config()};
    const auto first = market(1, 499);
    const auto second = market(2, 500);
    test_support::AllocationProbe probe;
    const auto baseline = decide(strategy, first);
    const auto signal = decide(strategy, second);
    probe.stop();
    runner.expect(
        baseline.code == ctp::StrategyDecisionCode::BaselineEstablished
            && signal.has_intent,
        "the allocation probe must exercise baseline and signal paths");
    runner.expect(probe.count() == 0, "strategy hot path must not allocate memory");
}

}

int main()
{
    test_support::TestRunner runner{"strategy"};
    test_exact_crossing_and_intent(runner);
    test_first_quote_and_crossing_edges(runner);
    test_invalid_input_clears_baseline(runner);
    test_sequence_fault_latches(runner);
    test_retrigger_cooldown_and_limit(runner);
    test_integer_protection_and_overflow(runner);
    test_hot_path_does_not_allocate(runner);
    return runner.finish();
}
