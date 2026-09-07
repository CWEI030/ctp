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

// 交易线程只复制定长事实；文本格式化和磁盘写入由后台线程完成。
struct TraceEvent {
    TraceId trace_id{};
    std::uint64_t sequence{0};
    std::int64_t mono_ns{0};
    std::uint64_t client_order_id{0};
    std::uint64_t order_ref{0};
    TraceStage stage{TraceStage::Market};
    std::int32_t quantity{0};
    std::int32_t code{0};
    std::array<char, kInstrumentIdCapacity> instrument{};
};

static_assert(std::is_trivially_copyable<TraceEvent>::value);

using TraceQueue = SpscQueue<TraceEvent, kTraceQueueCapacity>;

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

struct TraceJournalReadResult {
    bool valid{false};
    bool clean_shutdown{false};
    std::string account_id;
    std::vector<TraceEvent> events;
    std::uint64_t max_client_order_id{0};
    std::uint64_t max_order_ref{0};
};

TraceJournalReadResult read_trace_journal(const std::filesystem::path& path);

}
