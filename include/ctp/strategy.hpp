#pragma once

#include "ctp/engine.hpp"
#include "ctp/trading.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace ctp {

struct StrategyConfig {
    std::array<char, kInstrumentIdCapacity> instrument{};
    std::int64_t threshold_ticks{0};
    std::int64_t protection_ticks{0};
    std::int64_t max_market_age_ns{0};
    std::uint32_t max_signals_per_run{1};
    std::uint32_t retrigger_below_count{1};
    std::uint32_t cooldown_market_events{0};
};

enum class StrategyDecisionCode : std::uint8_t {
    BaselineEstablished,
    NoSignal,
    SignalGenerated,
    IgnoredInstrument,
    InvalidMarket,
    StaleMarket,
    SequenceFault,
    PriceOverflow,
    ConfigurationFault,
};

struct StrategyDecision {
    StrategyDecisionCode code{StrategyDecisionCode::NoSignal};
    bool faulted{false};
    bool has_intent{false};
    OrderIntent intent{};
};

static_assert(std::is_trivially_copyable<StrategyDecision>::value);

// 一个实例只消费一个账户的有序行情；所有状态由该账户线程串行修改。
class ThresholdStrategy {
public:
    explicit ThresholdStrategy(StrategyConfig config) noexcept;

    StrategyDecision on_market(
        const MarketEvent& event,
        std::int64_t decision_mono_ns) noexcept;
    bool faulted() const noexcept;

private:
    void clear_crossing_baseline() noexcept;

    StrategyConfig config_{};
    std::uint64_t last_market_seq_{0};
    std::int64_t previous_last_price_ticks_{0};
    std::uint32_t signals_emitted_{0};
    std::uint32_t below_threshold_count_{0};
    std::uint32_t events_since_signal_{0};
    bool has_sequence_{false};
    bool has_crossing_baseline_{false};
    bool faulted_{false};
};

}
