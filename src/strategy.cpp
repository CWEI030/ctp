#include "ctp/strategy.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <string_view>
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

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value) noexcept
{
    if (text.empty()) return false;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

bool split_replay_columns(
    std::string_view line,
    std::array<std::string_view, 13>& columns) noexcept
{
    std::size_t begin = 0;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const std::size_t comma = line.find(',', begin);
        if (index + 1 == columns.size()) {
            if (comma != std::string_view::npos) return false;
            columns[index] = line.substr(begin);
            return !columns[index].empty();
        }
        if (comma == std::string_view::npos || comma == begin) return false;
        columns[index] = line.substr(begin, comma - begin);
        begin = comma + 1;
    }
    return false;
}

bool copy_replay_instrument(
    std::string_view source,
    std::array<char, kInstrumentIdCapacity>& destination) noexcept
{
    if (source.empty() || source.size() >= destination.size()) return false;
    for (std::size_t index = 0; index < source.size(); ++index) {
        destination[index] = source[index];
    }
    destination[source.size()] = '\0';
    return true;
}

bool parse_market_status(
    std::string_view text,
    MarketDataStatus& status) noexcept
{
    if (text == "Valid") status = MarketDataStatus::Valid;
    else if (text == "NullData") status = MarketDataStatus::NullData;
    else if (text == "InvalidInstrument") {
        status = MarketDataStatus::InvalidInstrument;
    } else if (text == "InvalidTime") status = MarketDataStatus::InvalidTime;
    else if (text == "InvalidPrice") status = MarketDataStatus::InvalidPrice;
    else if (text == "InvalidBook") status = MarketDataStatus::InvalidBook;
    else return false;
    return true;
}

}

ReplayParseResult parse_replay_csv_line(std::string_view line) noexcept
{
    std::array<std::string_view, 13> columns{};
    if (!split_replay_columns(line, columns)) {
        return {ReplayParseCode::WrongColumnCount};
    }

    ReplayEvent event{};
    if (!parse_integer(columns[0], event.format_version)) {
        return {ReplayParseCode::InvalidField};
    }
    if (event.format_version != 1) {
        return {ReplayParseCode::UnsupportedVersion};
    }
    if (!copy_replay_instrument(columns[1], event.market.instrument)) {
        return {ReplayParseCode::InvalidInstrument};
    }
    if (!parse_integer(columns[2], event.market.market_seq)
        || event.market.market_seq == 0
        || !parse_integer(columns[3], event.market.exchange_time_ms)
        || !parse_integer(columns[4], event.market.recv_mono_ns)
        || !parse_integer(columns[5], event.decision_mono_ns)
        || !parse_integer(columns[6], event.market.last_price_ticks)
        || !parse_integer(columns[7], event.market.bid_price_ticks)
        || !parse_integer(columns[8], event.market.ask_price_ticks)
        || !parse_integer(columns[9], event.market.bid_volume)
        || !parse_integer(columns[10], event.market.ask_volume)
        || !parse_integer(columns[11], event.market.volume)) {
        return {ReplayParseCode::InvalidField};
    }
    if (!parse_market_status(columns[12], event.market.status)) {
        return {ReplayParseCode::InvalidStatus};
    }
    return {ReplayParseCode::Parsed, event};
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
