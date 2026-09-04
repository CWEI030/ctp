#include "ctp/strategy.hpp"

#include <limits>
#include <utility>

namespace ctp {
namespace {

bool has_valid_instrument(
    const std::array<char, kInstrumentIdCapacity>& instrument) noexcept
{
    if (instrument.front() == '\0') return false;
    for (const char character : instrument) {
        if (character == '\0') return true;
    }
    return false;
}

void increment_saturated(std::uint32_t& value) noexcept
{
    if (value != std::numeric_limits<std::uint32_t>::max()) ++value;
}

}

ThresholdStrategy::ThresholdStrategy(StrategyConfig config) noexcept
    : config_(std::move(config))
{
    faulted_ = !has_valid_instrument(config_.instrument)
        || config_.threshold_ticks <= 0
        || config_.protection_ticks < 0
        || config_.max_market_age_ns < 0
        || config_.max_signals_per_run == 0
        || config_.retrigger_below_count == 0;
}

StrategyDecision ThresholdStrategy::on_market(
    const MarketEvent& event,
    std::int64_t decision_mono_ns) noexcept
{
    if (faulted_) {
        return {
            has_sequence_ ? StrategyDecisionCode::SequenceFault
                          : StrategyDecisionCode::ConfigurationFault,
            true};
    }

    if (has_sequence_
        && (last_market_seq_ == std::numeric_limits<std::uint64_t>::max()
            || event.market_seq != last_market_seq_ + 1)) {
        faulted_ = true;
        clear_crossing_baseline();
        return {StrategyDecisionCode::SequenceFault, true};
    }
    last_market_seq_ = event.market_seq;
    has_sequence_ = true;

    if (event.instrument != config_.instrument) {
        clear_crossing_baseline();
        return {StrategyDecisionCode::IgnoredInstrument};
    }
    if (event.status != MarketDataStatus::Valid
        || event.last_price_ticks <= 0
        || event.bid_price_ticks <= 0
        || event.ask_price_ticks <= 0
        || event.bid_volume < 0
        || event.ask_volume < 0
        || event.volume < 0
        || event.bid_price_ticks > event.ask_price_ticks) {
        clear_crossing_baseline();
        return {StrategyDecisionCode::InvalidMarket};
    }
    if (event.recv_mono_ns < 0
        || decision_mono_ns < event.recv_mono_ns
        || decision_mono_ns - event.recv_mono_ns
            > config_.max_market_age_ns) {
        clear_crossing_baseline();
        return {StrategyDecisionCode::StaleMarket};
    }

    if (signals_emitted_ > 0) increment_saturated(events_since_signal_);

    const bool had_baseline = has_crossing_baseline_;
    const bool crossed = had_baseline
        && previous_last_price_ticks_ < config_.threshold_ticks
        && event.last_price_ticks >= config_.threshold_ticks;

    if (event.last_price_ticks < config_.threshold_ticks) {
        increment_saturated(below_threshold_count_);
    } else if (!crossed) {
        below_threshold_count_ = 0;
    }

    previous_last_price_ticks_ = event.last_price_ticks;
    has_crossing_baseline_ = true;
    if (!had_baseline) return {StrategyDecisionCode::BaselineEstablished};
    if (!crossed || signals_emitted_ >= config_.max_signals_per_run) {
        return {StrategyDecisionCode::NoSignal};
    }

    const std::uint32_t required_below = signals_emitted_ == 0
        ? 1 : config_.retrigger_below_count;
    const bool cooldown_complete = signals_emitted_ == 0
        || events_since_signal_ >= config_.cooldown_market_events;
    if (below_threshold_count_ < required_below || !cooldown_complete) {
        below_threshold_count_ = 0;
        return {StrategyDecisionCode::NoSignal};
    }
    below_threshold_count_ = 0;

    if (event.ask_price_ticks
        > std::numeric_limits<std::int64_t>::max()
            - config_.protection_ticks) {
        return {StrategyDecisionCode::PriceOverflow};
    }

    StrategyDecision decision{};
    decision.code = StrategyDecisionCode::SignalGenerated;
    decision.has_intent = true;
    decision.intent.signal_id = event.market_seq;
    decision.intent.instrument = config_.instrument;
    decision.intent.direction = Direction::Buy;
    decision.intent.offset = Offset::Open;
    decision.intent.quantity = 1;
    decision.intent.limit_price_ticks =
        event.ask_price_ticks + config_.protection_ticks;
    ++signals_emitted_;
    events_since_signal_ = 0;
    return decision;
}

bool ThresholdStrategy::faulted() const noexcept
{
    return faulted_;
}

void ThresholdStrategy::clear_crossing_baseline() noexcept
{
    has_crossing_baseline_ = false;
    below_threshold_count_ = 0;
}

}
