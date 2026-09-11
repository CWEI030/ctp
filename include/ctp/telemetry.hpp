#pragma once

#include "ctp/engine.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace ctp {

inline constexpr std::size_t kTraceQueueCapacity = 8192;
inline constexpr std::size_t kCriticalTraceReserve = 4096;
inline constexpr std::size_t kPerformanceQueueCapacity = 4096;

struct TraceId {
    std::uint64_t run_id{0};
    std::uint64_t signal_id{0};
};

enum class TraceStage : std::uint8_t {
    Market,
    Signal,
    RiskAccepted,
    RiskRejected,
    OrderSubmitted,
    OrderRejected,
    CancelRequested,
    OrderReport,
    Trade,
    ExitIntent,
    Disconnected,
    RecoveryStarted,
    RecoveryReady,
    RecoveryFrozen,
    RestartCheckpoint,
    CleanStop,
};

enum class TraceOrderReportCode : std::int32_t {
    Accepted,
    Rejected,
    PartiallyFilled,
    Filled,
    Canceled,
    CancelRejected,
};

// 交易线程只复制定长事实；文本格式化和磁盘写入由后台线程完成。
struct TraceEvent {
    TraceId trace_id{};
    std::uint64_t sequence{0};
    std::int64_t mono_ns{0};
    std::uint64_t client_order_id{0};
    std::uint64_t order_ref{0};
    std::int64_t limit_price_ticks{0};
    TraceStage stage{TraceStage::Market};
    std::int32_t quantity{0};
    std::int32_t code{0};
    std::uint32_t attempt{0};
    std::uint8_t direction{0};
    std::uint8_t offset{0};
    std::uint8_t purpose{0};
    std::array<char, kInstrumentIdCapacity> instrument{};
    std::array<char, kTradingDayCapacity> trading_day{};
    std::uint32_t daily_signals{0};
    std::uint32_t daily_orders{0};
    std::uint32_t daily_cancels{0};
};

static_assert(std::is_trivially_copyable<TraceEvent>::value);

// 每个订单槽最多产生一条风控、一条报单结果、一条撤单和六类规范订单回报；
// 信号、平仓意图及唯一成交分别受各自固定表容量约束，最后保留检查点和退出标记。
inline constexpr std::size_t kCriticalTraceEventsPerOrder = 9;
inline constexpr std::size_t kCriticalTraceCapacityRequired =
    kLiveOrderCapacity * kCriticalTraceEventsPerOrder
    + kLiveSignalCapacity * 2
    + kLiveOrderCapacity + 1
    + kLiveTradeCapacity
    + 2;
static_assert(kCriticalTraceCapacityRequired <= kCriticalTraceReserve);
static_assert(kCriticalTraceReserve < kTraceQueueCapacity);

inline bool is_critical_trace_event(const TraceEvent& event) noexcept
{
    switch (event.stage) {
    case TraceStage::Market:
        return event.trace_id.signal_id != 0;
    case TraceStage::Disconnected:
    case TraceStage::RecoveryStarted:
    case TraceStage::RecoveryReady:
    case TraceStage::RecoveryFrozen:
        return false;
    default:
        return true;
    }
}

class TraceQueue {
public:
    bool try_push(const TraceEvent& event) noexcept
    {
        if (!is_critical_trace_event(event)
            && queue_.depth() >= kTraceQueueCapacity - kCriticalTraceReserve) {
            best_effort_dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (queue_.try_push(event)) return true;
        critical_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool try_pop(TraceEvent& event) noexcept { return queue_.try_pop(event); }
    std::size_t depth() const noexcept { return queue_.depth(); }
    std::size_t high_watermark() const noexcept
    {
        return queue_.high_watermark();
    }
    std::int64_t oldest_age_ns(std::int64_t sample_mono_ns) const noexcept
    {
        return queue_.oldest_age_ns(sample_mono_ns);
    }
    std::uint64_t dropped_count() const noexcept
    {
        return critical_dropped_count() + best_effort_dropped_count();
    }
    std::uint64_t critical_dropped_count() const noexcept
    {
        return critical_dropped_.load(std::memory_order_relaxed);
    }
    std::uint64_t best_effort_dropped_count() const noexcept
    {
        return best_effort_dropped_.load(std::memory_order_relaxed);
    }

private:
    SpscQueue<TraceEvent, kTraceQueueCapacity> queue_;
    std::atomic<std::uint64_t> critical_dropped_{0};
    std::atomic<std::uint64_t> best_effort_dropped_{0};
};

enum class PerformanceStage : std::uint8_t {
    MarketToSignal,
    SignalToOrderCall,
    CallbackToState,
    SimulatedEndToEnd,
};

// 采样点只复制整数时间和本地账户序号，禁止把凭据或可变字符串带入热路径。
struct PerformanceSample {
    std::uint64_t sequence{0};
    std::int64_t mono_ns{0};
    std::int64_t latency_ns{0};
    std::uint32_t account_index{0};
    PerformanceStage stage{PerformanceStage::MarketToSignal};
};

static_assert(std::is_trivially_copyable<PerformanceSample>::value);

using PerformanceQueue = SpscQueue<PerformanceSample, kPerformanceQueueCapacity>;

struct LatencyStatistics {
    std::uint64_t count{0};
    std::int64_t minimum_ns{0};
    std::int64_t p50_ns{0};
    std::int64_t p95_ns{0};
    std::int64_t p99_ns{0};
    std::int64_t p999_ns{0};
    std::int64_t maximum_ns{0};
    double mean_ns{0.0};
    double standard_deviation_ns{0.0};
    std::int64_t jitter_p99_p50_ns{0};
    std::int64_t jitter_p999_p50_ns{0};
    std::int64_t maximum_pause_ns{0};
};

// 排序和统计属于控制面；调用者保留原始样本以便独立重算。
LatencyStatistics compute_latency_statistics(
    const std::vector<std::int64_t>& samples);

struct TraceQueueSnapshot {
    std::size_t depth{0};
    std::size_t capacity{0};
    std::size_t high_watermark{0};
    std::int64_t oldest_age_ns{0};
    std::uint64_t dropped{0};
    std::uint64_t critical_dropped{0};
    std::uint64_t best_effort_dropped{0};
    std::int32_t writer_tid{0};
};

struct PerformanceRecorderSnapshot {
    std::size_t depth{0};
    std::size_t capacity{0};
    std::size_t high_watermark{0};
    std::int64_t oldest_age_ns{0};
    std::uint64_t dropped{0};
    std::int64_t writer_thread_cpu_ns{0};
    std::int32_t writer_tid{0};
};

class TraceSink {
public:
    virtual ~TraceSink() = default;
    virtual bool try_record(const TraceEvent& event) noexcept = 0;
};

class AsyncTraceJournal final : public TraceSink {
public:
    AsyncTraceJournal(std::filesystem::path path, std::string account_id);
    ~AsyncTraceJournal();

    AsyncTraceJournal(const AsyncTraceJournal&) = delete;
    AsyncTraceJournal& operator=(const AsyncTraceJournal&) = delete;

    bool start();
    bool try_record(const TraceEvent& event) noexcept override;
    // 控制面刷新屏障；等待此前已接收的轨迹由日志线程刷新，不得从实时热路径调用。
    bool flush() noexcept;
    void stop() noexcept;
    TraceQueueSnapshot snapshot() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class AsyncPerformanceRecorder final {
public:
    explicit AsyncPerformanceRecorder(std::filesystem::path path);
    ~AsyncPerformanceRecorder();

    AsyncPerformanceRecorder(const AsyncPerformanceRecorder&) = delete;
    AsyncPerformanceRecorder& operator=(const AsyncPerformanceRecorder&) = delete;

    bool start();
    bool try_record(const PerformanceSample& sample) noexcept;
    void stop() noexcept;
    PerformanceRecorderSnapshot snapshot() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct TraceJournalReadResult {
    bool valid{false};
    bool clean_shutdown{false};
    std::string account_id;
    std::vector<TraceEvent> events;
    std::uint64_t max_client_order_id{0};
    std::uint64_t max_order_ref{0};
};

TraceJournalReadResult read_trace_journal(const std::filesystem::path& path);

// 重启只恢复本地身份和单调编号；是否成交仍由重新登录后的 CTP 查询裁定。
struct RestartOrder {
    TraceId trace_id{};
    std::uint64_t client_order_id{0};
    std::uint64_t order_ref{0};
    std::int64_t limit_price_ticks{0};
    std::int32_t quantity{0};
    std::uint32_t attempt{0};
    std::uint8_t direction{0};
    std::uint8_t offset{0};
    std::uint8_t purpose{0};
    std::array<char, kInstrumentIdCapacity> instrument{};
};

struct RestartImage {
    bool valid{false};
    std::string account_id;
    std::uint64_t next_client_order_id{1};
    std::uint64_t next_order_ref{1};
    std::string trading_day;
    std::uint32_t daily_signals{0};
    std::uint32_t daily_orders{0};
    std::uint32_t daily_cancels{0};
    std::vector<RestartOrder> uncertain_orders;
};

RestartImage build_restart_image(const TraceJournalReadResult& journal);

// 离线基准复用策略、风控、下单会话和回报归并链路，不连接真实柜台。
int run_benchmark(
    const RuntimeConfig& config,
    std::ostream& output,
    std::ostream& error);

}
