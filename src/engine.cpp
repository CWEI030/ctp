#include "ctp/engine.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <string>

namespace ctp {
namespace {

bool copy_instrument(
    const char (&source)[kInstrumentIdCapacity],
    std::array<char, kInstrumentIdCapacity>& destination) noexcept
{
    for (std::size_t index = 0; index < destination.size(); ++index) {
        destination[index] = source[index];
        if (source[index] == '\0') {
            return index != 0;
        }
    }
    destination.back() = '\0';
    return false;
}

bool parse_exchange_time_ms(
    const char* update_time,
    int update_millisec,
    std::int64_t& result) noexcept
{
    const auto digit = [update_time](std::size_t index) {
        return update_time[index] >= '0' && update_time[index] <= '9';
    };
    if (!digit(0) || !digit(1) || update_time[2] != ':'
        || !digit(3) || !digit(4) || update_time[5] != ':'
        || !digit(6) || !digit(7) || update_time[8] != '\0'
        || update_millisec < 0 || update_millisec > 999) {
        return false;
    }

    const int hour = (update_time[0] - '0') * 10 + update_time[1] - '0';
    const int minute = (update_time[3] - '0') * 10 + update_time[4] - '0';
    const int second = (update_time[6] - '0') * 10 + update_time[7] - '0';
    if (hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    result = ((hour * 60LL + minute) * 60LL + second) * 1000LL
             + update_millisec;
    return true;
}

bool price_to_ticks(
    double price,
    double minimum_price_increment,
    std::int64_t& result) noexcept
{
    if (!std::isfinite(price) || price <= 0
        || !std::isfinite(minimum_price_increment)
        || minimum_price_increment <= 0) {
        return false;
    }

    const long double ratio =
        static_cast<long double>(price)
        / static_cast<long double>(minimum_price_increment);
    if (ratio > static_cast<long double>(std::numeric_limits<std::int64_t>::max())
        || ratio < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return false;
    }

    const auto rounded = static_cast<std::int64_t>(std::round(ratio));
    const long double rebuilt =
        static_cast<long double>(rounded)
        * static_cast<long double>(minimum_price_increment);
    const long double tolerance =
        std::fmax(1.0e-9L, std::fabs(static_cast<long double>(price)) * 1.0e-12L);
    if (std::fabs(rebuilt - static_cast<long double>(price)) > tolerance) {
        return false;
    }

    result = rounded;
    return true;
}

}

struct MarketIngress::AccountChannel {
    std::string account_id;
    SpscQueue<MarketEvent, kMarketQueueCapacity> queue;
    std::atomic<bool> overflowed{false};
};

MarketIngress::MarketIngress(
    const std::vector<AccountConfig>& accounts,
    double minimum_price_increment)
    : channels_(std::make_unique<AccountChannel[]>(accounts.size())),
      account_count_(accounts.size()),
      minimum_price_increment_(minimum_price_increment)
{
    for (std::size_t index = 0; index < account_count_; ++index) {
        channels_[index].account_id = accounts[index].alias();
    }
}

MarketIngress::~MarketIngress() = default;

void MarketIngress::start() noexcept
{
    accepting_.store(true, std::memory_order_release);
}

void MarketIngress::stop() noexcept
{
    accepting_.store(false, std::memory_order_release);
}

std::size_t MarketIngress::account_count() const noexcept
{
    return account_count_;
}

std::string_view MarketIngress::account_id(std::size_t account_index) const noexcept
{
    if (account_index >= account_count_) {
        return {};
    }
    return channels_[account_index].account_id;
}

bool MarketIngress::try_pop(
    std::size_t account_index,
    MarketEvent& event) noexcept
{
    return account_index < account_count_
           && channels_[account_index].queue.try_pop(event);
}

MarketQueueSnapshot MarketIngress::snapshot(
    std::size_t account_index) const noexcept
{
    if (account_index >= account_count_) {
        return {};
    }

    const auto& channel = channels_[account_index];
    return {
        channel.queue.depth(),
        channel.queue.high_watermark(),
        channel.queue.dropped_count(),
        channel.overflowed.load(std::memory_order_acquire)};
}

MarketPublishResult MarketIngress::ingest(
    const CThostFtdcDepthMarketDataField* tick,
    std::int64_t recv_mono_ns) noexcept
{
    if (!accepting_.load(std::memory_order_acquire)) {
        return {};
    }

    const MarketEvent event = normalize(tick, recv_mono_ns);
    MarketPublishResult result;
    for (std::size_t index = 0; index < account_count_; ++index) {
        auto& channel = channels_[index];
        if (channel.overflowed.load(std::memory_order_relaxed)) {
            channel.queue.record_drop();
            ++result.overflowed;
            continue;
        }

        if (channel.queue.try_push(event)) {
            ++result.published;
            continue;
        }

        channel.overflowed.store(true, std::memory_order_release);
        ++result.overflowed;
    }
    return result;
}

void MarketIngress::OnRtnDepthMarketData(
    CThostFtdcDepthMarketDataField* tick)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto recv_mono_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    static_cast<void>(ingest(tick, recv_mono_ns));
}

MarketEvent MarketIngress::normalize(
    const CThostFtdcDepthMarketDataField* tick,
    std::int64_t recv_mono_ns) noexcept
{
    MarketEvent event;
    event.market_seq = next_market_seq_++;
    event.recv_mono_ns = recv_mono_ns;
    if (tick == nullptr) {
        event.status = MarketDataStatus::NullData;
        return event;
    }

    event.bid_volume = tick->BidVolume1;
    event.ask_volume = tick->AskVolume1;
    event.volume = tick->Volume;
    if (!copy_instrument(tick->InstrumentID, event.instrument)) {
        event.status = MarketDataStatus::InvalidInstrument;
        return event;
    }
    if (!parse_exchange_time_ms(
            tick->UpdateTime,
            tick->UpdateMillisec,
            event.exchange_time_ms)) {
        event.status = MarketDataStatus::InvalidTime;
        return event;
    }
    if (!price_to_ticks(
            tick->LastPrice,
            minimum_price_increment_,
            event.last_price_ticks)
        || !price_to_ticks(
            tick->BidPrice1,
            minimum_price_increment_,
            event.bid_price_ticks)
        || !price_to_ticks(
            tick->AskPrice1,
            minimum_price_increment_,
            event.ask_price_ticks)) {
        event.status = MarketDataStatus::InvalidPrice;
        return event;
    }
    if (event.bid_volume < 0 || event.ask_volume < 0 || event.volume < 0
        || event.bid_price_ticks > event.ask_price_ticks) {
        event.status = MarketDataStatus::InvalidBook;
        return event;
    }

    event.status = MarketDataStatus::Valid;
    return event;
}

}
