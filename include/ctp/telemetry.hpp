#pragma once

#include "ctp/engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace ctp {

inline constexpr std::size_t kTraceQueueCapacity = 1024;
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
};

static_assert(std::is_trivially_copyable<TraceEvent>::value);

using TraceQueue = SpscQueue<TraceEvent, kTraceQueueCapacity>;

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
    std::size_t high_watermark{0};
    std::uint64_t dropped{0};
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
    TraceQueueSnapshot snapshot() const noexcept;

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
    std::vector<RestartOrder> uncertain_orders;
};

RestartImage build_restart_image(const TraceJournalReadResult& journal);

}
