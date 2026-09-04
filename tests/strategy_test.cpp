#include "ctp/field.hpp"
#include "ctp/strategy.hpp"
#define CTP_TEST_DEFINE_ALLOCATION_OPERATORS
#include "test_support.hpp"

#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

    auto cooldown_config = strategy_config();
    cooldown_config.max_signals_per_run = 2;
    cooldown_config.retrigger_below_count = 1;
    cooldown_config.cooldown_market_events = 4;
    ctp::ThresholdStrategy cooldown{cooldown_config};
    decide(cooldown, market(1, 499));
    decide(cooldown, market(2, 500));
    decide(cooldown, market(3, 499));
    runner.expect(
        !decide(cooldown, market(4, 500)).has_intent,
        "a new crossing before cooldown completion must be suppressed");
    decide(cooldown, market(5, 499));
    runner.expect(
        decide(cooldown, market(6, 500)).has_intent,
        "a later crossing after cooldown completion may retrigger");
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

std::string replay_path(const char* filename)
{
    return std::string{CTP_SOURCE_DIR} + "/tests/data/replay/" + filename;
}

std::vector<ctp::ReplayEvent> read_replay(
    test_support::TestRunner& runner,
    const char* filename)
{
    std::ifstream input{replay_path(filename)};
    runner.expect(input.is_open(), "the replay input file must be readable");

    std::string line;
    runner.expect(
        static_cast<bool>(std::getline(input, line))
            && line == ctp::kReplayCsvHeader,
        "the replay file must start with the exact versioned header");

    std::vector<ctp::ReplayEvent> events;
    while (std::getline(input, line)) {
        const auto parsed = ctp::parse_replay_csv_line(line);
        runner.expect(
            parsed.code == ctp::ReplayParseCode::Parsed,
            "every committed replay row must pass strict parsing");
        if (parsed.code == ctp::ReplayParseCode::Parsed) {
            events.push_back(parsed.event);
        }
    }
    return events;
}

bool same_intent(const ctp::OrderIntent& left, const ctp::OrderIntent& right)
{
    return left.signal_id == right.signal_id
        && left.instrument == right.instrument
        && left.direction == right.direction
        && left.offset == right.offset
        && left.quantity == right.quantity
        && left.limit_price_ticks == right.limit_price_ticks;
}

bool same_decision(
    const ctp::StrategyDecision& left,
    const ctp::StrategyDecision& right)
{
    return left.code == right.code
        && left.faulted == right.faulted
        && left.has_intent == right.has_intent
        && same_intent(left.intent, right.intent);
}

std::vector<ctp::StrategyDecision> replay_with_yields(
    const std::vector<ctp::ReplayEvent>& events,
    int schedule_variant)
{
    auto config = strategy_config();
    config.max_signals_per_run = 2;
    config.retrigger_below_count = 2;
    config.cooldown_market_events = 3;
    ctp::ThresholdStrategy strategy{config};
    std::vector<ctp::StrategyDecision> decisions;
    decisions.reserve(events.size());
    for (std::size_t index = 0; index < events.size(); ++index) {
        const int yields = schedule_variant == 0
            ? 0 : static_cast<int>((index + schedule_variant) % 3);
        for (int count = 0; count < yields; ++count) {
            std::this_thread::yield();
        }
        decisions.push_back(strategy.on_market(
            events[index].market,
            events[index].decision_mono_ns));
    }
    return decisions;
}

void test_strict_replay_parser(test_support::TestRunner& runner)
{
    const auto parsed = ctp::parse_replay_csv_line(
        "1,IF2609,7,34200007,1007,1017,500,499,501,10,12,100,Valid");
    runner.expect(
        parsed.code == ctp::ReplayParseCode::Parsed,
        "a complete version-one row must parse");
    runner.expect(
        parsed.event.format_version == 1
            && parsed.event.market.market_seq == 7
            && parsed.event.market.exchange_time_ms == 34'200'007
            && parsed.event.market.recv_mono_ns == 1'007
            && parsed.event.decision_mono_ns == 1'017
            && parsed.event.market.last_price_ticks == 500
            && parsed.event.market.bid_price_ticks == 499
            && parsed.event.market.ask_price_ticks == 501
            && parsed.event.market.bid_volume == 10
            && parsed.event.market.ask_volume == 12
            && parsed.event.market.volume == 100
            && parsed.event.market.status == ctp::MarketDataStatus::Valid,
        "the parser must preserve every replay field and unit");

    const char* invalid_rows[]{
        "2,IF2609,7,34200007,1007,1017,500,499,501,10,12,100,Valid",
        "1,IF2609,7,34200007,1007,1017,500,499,501,10,12,100",
        "1,IF2609,7,34200007,1007,1017,500,499,501,10,12,100,Valid,extra",
        "1,IF2609,x,34200007,1007,1017,500,499,501,10,12,100,Valid",
        "1,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA,7,34200007,1007,1017,500,499,501,10,12,100,Valid",
        "1,IF2609,7,34200007,1007,1017,500,499,501,10,12,100,Unknown",
        "1,IF2609,7,34200007,1007,1017,500,499,501,10,12,100,Valid ",
    };
    for (const char* row : invalid_rows) {
        runner.expect(
            ctp::parse_replay_csv_line(row).code
                != ctp::ReplayParseCode::Parsed,
            "wrong version, shape, value, instrument, status, or whitespace must fail");
    }
}

void test_replay_is_deterministic(test_support::TestRunner& runner)
{
    const auto events = read_replay(runner, "minimal_signal_v1.csv");
    const auto first = replay_with_yields(events, 0);
    const auto second = replay_with_yields(events, 1);
    const auto third = replay_with_yields(events, 2);
    runner.expect(first.size() == 9, "minimal replay must retain all nine rows");
    for (std::size_t index = 0; index < first.size(); ++index) {
        runner.expect(
            same_decision(first[index], second[index])
                && same_decision(first[index], third[index]),
            "three fresh strategies must produce identical field-level decisions");
    }
    runner.expect(
        first.size() > 5 && first[1].has_intent && first[1].intent.signal_id == 2
            && first[5].has_intent && first[5].intent.signal_id == 6,
        "minimal replay must cover first signal, debounce, cooldown, and retrigger");
}

void test_fault_replay_fails_closed(test_support::TestRunner& runner)
{
    const auto events = read_replay(runner, "faults_v1.csv");
    const auto decisions = replay_with_yields(events, 2);
    bool saw_fault = false;
    for (const auto& decision : decisions) {
        runner.expect(!decision.has_intent, "fault replay must never create an order intent");
        saw_fault = saw_fault || decision.faulted;
    }
    runner.expect(saw_fault, "sequence gap in fault replay must latch a strategy fault");
    runner.expect(
        !decisions.empty() && decisions.back().faulted,
        "normal-looking data after a sequence fault must remain blocked");
    runner.expect(
        decisions.size() == 11
            && decisions[1].code == ctp::StrategyDecisionCode::InvalidMarket
            && decisions[4].code == ctp::StrategyDecisionCode::StaleMarket
            && decisions[6].code == ctp::StrategyDecisionCode::IgnoredInstrument
            && decisions[7].code == ctp::StrategyDecisionCode::InvalidMarket
            && decisions[9].code == ctp::StrategyDecisionCode::SequenceFault,
        "fault rows must reach their exact invalid, stale, instrument, and gap decisions");
}

ctp::AccountConfig account_config()
{
    return {
        "account1", "9999", "user1", "password1", "app1", "auth1",
        "tcp://127.0.0.1:41001"};
}

ctp::RiskLimits replay_risk_limits()
{
    ctp::RiskLimits limits{};
    ctp::copy_to_field(limits.allowed_instrument, "IF2609");
    limits.max_market_age_ns = 100;
    limits.max_slippage_ticks = 2;
    limits.margin_per_lot = 100;
    limits.minimum_available_after_order = 0;
    limits.max_daily_signals = 10;
    limits.max_daily_orders = 10;
    limits.max_daily_cancels = 10;
    limits.max_active_open_orders = 1;
    limits.max_net_open_position = 1;
    return limits;
}

ctp::RiskSnapshot replay_risk_snapshot(const ctp::ReplayEvent& event)
{
    ctp::RiskSnapshot snapshot{};
    snapshot.enabled = true;
    snapshot.authenticated = true;
    snapshot.logged_in = true;
    snapshot.reconciled = true;
    snapshot.trading_window_open = true;
    snapshot.market_valid = event.market.status == ctp::MarketDataStatus::Valid;
    snapshot.funds_known = true;
    snapshot.positions_known = true;
    snapshot.now_ns = event.decision_mono_ns;
    snapshot.market_receive_ns = event.market.recv_mono_ns;
    snapshot.bid_price_ticks = event.market.bid_price_ticks;
    snapshot.ask_price_ticks = event.market.ask_price_ticks;
    snapshot.available_funds = 1'000;
    return snapshot;
}

void test_replay_intent_reaches_submit(test_support::TestRunner& runner)
{
    const auto events = read_replay(runner, "minimal_signal_v1.csv");
    ctp::ThresholdStrategy strategy{strategy_config()};
    decide(strategy, events[0].market);
    const auto decision = strategy.on_market(
        events[1].market, events[1].decision_mono_ns);
    runner.expect(decision.has_intent, "replay crossing must create an order intent");

    auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
    auto api = std::make_unique<test_support::FakeTraderApi>(metrics);
    ctp::AccountTradingSession session{
        account_config(), replay_risk_limits(), std::move(api), 4, 4, 4, 4};
    session.activate(1, 2, "0");
    const auto submitted = session.submit(
        decision.intent, replay_risk_snapshot(events[1]));
    runner.expect(
        submitted.code == ctp::SubmitCode::Submitted
            && metrics->order_insert_calls == 1,
        "the replay-generated intent must enter the existing submit path once");
    runner.expect(
        metrics->last_order.Direction == THOST_FTDC_D_Buy
            && metrics->last_order.CombOffsetFlag[0] == THOST_FTDC_OF_Open
            && metrics->last_order.VolumeTotalOriginal == 1
            && metrics->last_order.LimitPrice == 503.0,
        "submit must receive the signal direction, offset, quantity, and protected price");
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
    test_strict_replay_parser(runner);
    test_replay_is_deterministic(runner);
    test_fault_replay_fails_closed(runner);
    test_replay_intent_reaches_submit(runner);
    return runner.finish();
}
