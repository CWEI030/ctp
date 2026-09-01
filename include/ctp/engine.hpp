#pragma once

#include "ThostFtdcMdApi.h"
#include "ctp/config.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ctp {

inline constexpr std::size_t kMarketQueueCapacity = 1024;

enum class MarketDataStatus : std::uint8_t {
    Valid,
    NullData,
    InvalidInstrument,
    InvalidTime,
    InvalidPrice,
    InvalidBook,
};

// 热路径事件只包含定长、可直接复制的数据。字符串转换和格式化留在控制面。
struct MarketEvent {
    std::uint64_t market_seq{0};
    std::int64_t exchange_time_ms{0};
    std::int64_t recv_mono_ns{0};
    std::int64_t last_price_ticks{0};
    std::int64_t bid_price_ticks{0};
    std::int64_t ask_price_ticks{0};
    std::int32_t bid_volume{0};
    std::int32_t ask_volume{0};
    std::int32_t volume{0};
    MarketDataStatus status{MarketDataStatus::NullData};
    std::array<char, kInstrumentIdCapacity> instrument{};
};

static_assert(std::is_trivially_copyable<MarketEvent>::value);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

template <typename Event, std::size_t Capacity>
class SpscQueue {
public:
    static_assert(Capacity > 0, "SPSC queue capacity must be positive");
    static_assert(
        std::is_trivially_copyable<Event>::value,
        "hot-path queue events must be trivially copyable");

    bool try_push(const Event& event) noexcept
    {
        const std::uint64_t write = write_index_.load(std::memory_order_relaxed);
        const std::uint64_t read = read_index_.load(std::memory_order_acquire);
        if (write - read == Capacity) {
            record_drop();
            return false;
        }

        events_[write % Capacity] = event;
        write_index_.store(write + 1, std::memory_order_release);
        update_high_watermark(write + 1 - read);
        return true;
    }

    bool try_pop(Event& event) noexcept
    {
        const std::uint64_t read = read_index_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_index_.load(std::memory_order_acquire);
        if (read == write) {
            return false;
        }

        event = events_[read % Capacity];
        read_index_.store(read + 1, std::memory_order_release);
        return true;
    }

    std::size_t depth() const noexcept
    {
        const auto write = write_index_.load(std::memory_order_acquire);
        const auto read = read_index_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(write - read);
    }

    std::size_t high_watermark() const noexcept
    {
        return static_cast<std::size_t>(
            high_watermark_.load(std::memory_order_relaxed));
    }

    std::uint64_t dropped_count() const noexcept
    {
        return dropped_count_.load(std::memory_order_relaxed);
    }

    void record_drop() noexcept
    {
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    void update_high_watermark(std::uint64_t depth) noexcept
    {
        std::uint64_t previous =
            high_watermark_.load(std::memory_order_relaxed);
        while (previous < depth
               && !high_watermark_.compare_exchange_weak(
                   previous,
                   depth,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    std::array<Event, Capacity> events_{};
    alignas(64) std::atomic<std::uint64_t> write_index_{0};
    alignas(64) std::atomic<std::uint64_t> read_index_{0};
    std::atomic<std::uint64_t> high_watermark_{0};
    std::atomic<std::uint64_t> dropped_count_{0};
};

struct MarketQueueSnapshot {
    std::size_t depth{0};
    std::size_t high_watermark{0};
    std::uint64_t dropped{0};
    bool overflowed{false};
};

struct MarketPublishResult {
    std::size_t published{0};
    std::size_t overflowed{0};
};

class MarketIngress final : public CThostFtdcMdSpi {
public:
    MarketIngress(
        const std::vector<AccountConfig>& accounts,
        double minimum_price_increment);
    ~MarketIngress();

    MarketIngress(const MarketIngress&) = delete;
    MarketIngress& operator=(const MarketIngress&) = delete;

    void start() noexcept;
    void stop() noexcept;

    std::size_t account_count() const noexcept;
    std::string_view account_id(std::size_t account_index) const noexcept;
    bool try_pop(std::size_t account_index, MarketEvent& event) noexcept;
    MarketQueueSnapshot snapshot(std::size_t account_index) const noexcept;

    MarketPublishResult ingest(
        const CThostFtdcDepthMarketDataField* tick,
        std::int64_t recv_mono_ns) noexcept;
    void OnRtnDepthMarketData(
        CThostFtdcDepthMarketDataField* tick) override;

private:
    struct AccountChannel;

    MarketEvent normalize(
        const CThostFtdcDepthMarketDataField* tick,
        std::int64_t recv_mono_ns) noexcept;

    std::unique_ptr<AccountChannel[]> channels_;
    std::size_t account_count_{0};
    double minimum_price_increment_{0};
    std::uint64_t next_market_seq_{1};
    std::atomic<bool> accepting_{false};
};

}
