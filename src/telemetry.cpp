#include "ctp/telemetry.hpp"

#include "ctp/field.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <ctime>
#include <fstream>
#include <limits>
#include <cmath>
#include <numeric>
#include <string_view>
#include <thread>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace ctp {
namespace {

constexpr std::string_view kHeaderV1 =
    "ctp_trace_v1,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,limit_price_ticks,quantity,attempt,direction,offset,purpose,instrument,code";
constexpr std::string_view kHeaderV2 =
    "ctp_trace_v2,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,limit_price_ticks,quantity,attempt,direction,offset,purpose,instrument,code,trading_day,daily_signals,daily_orders,daily_cancels";
constexpr std::string_view kHeaderV3 =
    "ctp_trace_v3,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,limit_price_ticks,quantity,attempt,direction,offset,purpose,instrument,code,trading_day,daily_signals,daily_orders,daily_cancels,recovery_phase,failed_phase,error_id,error_message";

std::string_view stage_name(TraceStage stage) noexcept
{
    switch (stage) {
    case TraceStage::Market: return "market";
    case TraceStage::Signal: return "signal";
    case TraceStage::RiskAccepted: return "risk_accepted";
    case TraceStage::RiskRejected: return "risk_rejected";
    case TraceStage::OrderSubmitted: return "order_submitted";
    case TraceStage::OrderRejected: return "order_rejected";
    case TraceStage::CancelRequested: return "cancel_requested";
    case TraceStage::OrderReport: return "order_report";
    case TraceStage::Trade: return "trade";
    case TraceStage::ExitIntent: return "exit_intent";
    case TraceStage::Disconnected: return "disconnected";
    case TraceStage::RecoveryStarted: return "recovery_started";
    case TraceStage::RecoveryReady: return "recovery_ready";
    case TraceStage::RecoveryFrozen: return "recovery_frozen";
    case TraceStage::RestartCheckpoint: return "restart_checkpoint";
    case TraceStage::CleanStop: return "clean_stop";
    case TraceStage::RecoveryPhaseChanged: return "recovery_phase_changed";
    case TraceStage::RecoveryResponse: return "recovery_response";
    }
    return "invalid";
}

bool parse_stage(std::string_view text, TraceStage& stage) noexcept
{
    for (const auto candidate : {
             TraceStage::Market, TraceStage::Signal, TraceStage::RiskAccepted,
             TraceStage::RiskRejected, TraceStage::OrderSubmitted,
             TraceStage::OrderRejected, TraceStage::CancelRequested,
             TraceStage::OrderReport, TraceStage::Trade, TraceStage::ExitIntent,
             TraceStage::Disconnected, TraceStage::RecoveryStarted,
             TraceStage::RecoveryReady, TraceStage::RecoveryFrozen,
             TraceStage::RestartCheckpoint,
             TraceStage::CleanStop, TraceStage::RecoveryPhaseChanged,
             TraceStage::RecoveryResponse}) {
        if (stage_name(candidate) == text) {
            stage = candidate;
            return true;
        }
    }
    return false;
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value) noexcept
{
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{}
        && result.ptr == text.data() + text.size();
}

std::vector<std::string_view> split_line(const std::string& line)
{
    std::vector<std::string_view> fields;
    fields.reserve(25);
    std::size_t begin = 0;
    while (true) {
        const auto comma = line.find(',', begin);
        if (comma == std::string::npos) {
            fields.push_back(std::string_view{line}.substr(begin));
            return fields;
        }
        fields.push_back(std::string_view{line}.substr(begin, comma - begin));
        begin = comma + 1;
    }
}

std::string_view instrument_view(const TraceEvent& event) noexcept
{
    const auto end = std::find(
        event.instrument.begin(), event.instrument.end(), '\0');
    return {event.instrument.data(),
            static_cast<std::size_t>(end - event.instrument.begin())};
}

std::string_view trading_day_view(const TraceEvent& event) noexcept
{
    const auto end = std::find(
        event.trading_day.begin(), event.trading_day.end(), '\0');
    return {event.trading_day.data(),
            static_cast<std::size_t>(end - event.trading_day.begin())};
}

bool valid_trading_day(std::string_view day) noexcept
{
    return day.size() == 8
        && std::all_of(day.begin(), day.end(), [](char value) {
               return value >= '0' && value <= '9';
           });
}

std::string_view performance_stage_name(PerformanceStage stage) noexcept
{
    switch (stage) {
    case PerformanceStage::MarketToSignal: return "market_to_signal";
    case PerformanceStage::SignalToOrderCall: return "signal_to_order_call";
    case PerformanceStage::CallbackToState: return "callback_to_state";
    case PerformanceStage::SimulatedEndToEnd: return "simulated_end_to_end";
    }
    return "invalid";
}

}

LatencyStatistics compute_latency_statistics(
    const std::vector<std::int64_t>& samples)
{
    LatencyStatistics result{};
    if (samples.empty()) return result;

    std::vector<std::int64_t> ordered = samples;
    std::sort(ordered.begin(), ordered.end());
    const auto percentile = [&ordered](std::uint64_t numerator) {
        const std::uint64_t rank =
            (numerator * ordered.size() + 999) / 1000;
        return ordered[static_cast<std::size_t>(std::max<std::uint64_t>(1, rank) - 1)];
    };

    result.count = ordered.size();
    result.minimum_ns = ordered.front();
    result.p50_ns = percentile(500);
    result.p95_ns = percentile(950);
    result.p99_ns = percentile(990);
    result.p999_ns = percentile(999);
    result.maximum_ns = ordered.back();
    const long double sum = std::accumulate(
        ordered.begin(), ordered.end(), 0.0L);
    result.mean_ns = static_cast<double>(sum / ordered.size());
    long double squared_difference = 0.0L;
    for (const auto value : ordered) {
        const long double difference = value - result.mean_ns;
        squared_difference += difference * difference;
    }
    result.standard_deviation_ns = static_cast<double>(
        std::sqrt(squared_difference / ordered.size()));
    result.jitter_p99_p50_ns = result.p99_ns - result.p50_ns;
    result.jitter_p999_p50_ns = result.p999_ns - result.p50_ns;
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        result.maximum_pause_ns = std::max(
            result.maximum_pause_ns, ordered[index] - ordered[index - 1]);
    }
    return result;
}

struct AsyncTraceJournal::Impl {
    Impl(std::filesystem::path output_path, std::string owned_account_id)
        : path(std::move(output_path)), account_id(std::move(owned_account_id))
    {
    }

    void write(const TraceEvent& event)
    {
        output << "ctp_trace_v3," << account_id << ','
               << event.trace_id.run_id << ',' << event.trace_id.signal_id << ','
               << event.sequence << ',' << event.mono_ns << ','
               << stage_name(event.stage) << ',' << event.client_order_id << ','
               << event.order_ref << ',' << event.limit_price_ticks << ','
               << event.quantity << ',' << event.attempt << ','
               << static_cast<unsigned>(event.direction) << ','
               << static_cast<unsigned>(event.offset) << ','
               << static_cast<unsigned>(event.purpose) << ','
               << instrument_view(event) << ',' << event.code << ','
               << trading_day_view(event) << ',' << event.daily_signals << ','
               << event.daily_orders << ',' << event.daily_cancels << ','
               << static_cast<unsigned>(event.recovery_phase) << ','
               << static_cast<unsigned>(event.diagnostic.failed_phase) << ','
               << event.diagnostic.error_id << ','
               << field_text(event.diagnostic.error_message) << '\n';
    }

    void run()
    {
        writer_tid.store(
            static_cast<std::int32_t>(::syscall(SYS_gettid)),
            std::memory_order_release);
        TraceEvent event{};
        while (running.load(std::memory_order_acquire) || queue.depth() != 0) {
            if (queue.try_pop(event)) {
                write(event);
                written_sequence.store(event.sequence, std::memory_order_release);
            } else {
                std::this_thread::yield();
            }
            const auto requested = flush_request.load(std::memory_order_acquire);
            if (requested != 0
                && written_sequence.load(std::memory_order_acquire) >= requested
                && flushed_sequence.load(std::memory_order_relaxed) < requested) {
                output.flush();
                if (!output) {
                    flush_failed.store(true, std::memory_order_release);
                } else {
                    flushed_sequence.store(requested, std::memory_order_release);
                }
            }
        }
        output.flush();
    }

    std::filesystem::path path;
    std::string account_id;
    TraceQueue queue;
    std::ofstream output;
    std::thread writer;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> largest_sequence{0};
    std::atomic<std::uint64_t> written_sequence{0};
    std::atomic<std::uint64_t> flush_request{0};
    std::atomic<std::uint64_t> flushed_sequence{0};
    std::atomic<bool> flush_failed{false};
    std::atomic<std::int32_t> writer_tid{0};
};

AsyncTraceJournal::AsyncTraceJournal(
    std::filesystem::path path,
    std::string account_id)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(account_id)))
{
}

AsyncTraceJournal::~AsyncTraceJournal()
{
    stop();
}

bool AsyncTraceJournal::start()
{
    if (impl_->running.load(std::memory_order_acquire)) return true;
    impl_->output.open(impl_->path, std::ios::out | std::ios::trunc);
    if (!impl_->output) return false;
    impl_->output << kHeaderV3 << '\n';
    impl_->output.flush();
    if (!impl_->output) {
        impl_->output.close();
        return false;
    }
    impl_->largest_sequence.store(0, std::memory_order_relaxed);
    impl_->written_sequence.store(0, std::memory_order_relaxed);
    impl_->flush_request.store(0, std::memory_order_relaxed);
    impl_->flushed_sequence.store(0, std::memory_order_relaxed);
    impl_->flush_failed.store(false, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);
    impl_->writer = std::thread([this] { impl_->run(); });
    return true;
}

bool AsyncTraceJournal::try_record(const TraceEvent& event) noexcept
{
    if (!impl_->running.load(std::memory_order_acquire)) return false;
    if (!impl_->queue.try_push(event)) return false;
    auto previous = impl_->largest_sequence.load(std::memory_order_relaxed);
    while (previous < event.sequence
           && !impl_->largest_sequence.compare_exchange_weak(
               previous,
               event.sequence,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    return true;
}

bool AsyncTraceJournal::flush() noexcept
{
    if (!impl_->running.load(std::memory_order_acquire)) return false;
    const auto target = impl_->largest_sequence.load(std::memory_order_acquire);
    if (target == 0) return true;
    impl_->flush_request.store(target, std::memory_order_release);
    while (impl_->flushed_sequence.load(std::memory_order_acquire) < target) {
        if (impl_->flush_failed.load(std::memory_order_acquire)
            || !impl_->running.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

void AsyncTraceJournal::stop() noexcept
{
    if (!impl_->running.load(std::memory_order_acquire)) {
        if (impl_->writer.joinable()) impl_->writer.join();
        return;
    }
    TraceEvent stop_event{};
    stop_event.sequence =
        impl_->largest_sequence.load(std::memory_order_relaxed) + 1;
    stop_event.stage = TraceStage::CleanStop;
    const auto critical_dropped = impl_->queue.critical_dropped_count();
    stop_event.code = critical_dropped > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        ? std::numeric_limits<std::int32_t>::max()
        : static_cast<std::int32_t>(critical_dropped);
    const auto best_effort_dropped = impl_->queue.best_effort_dropped_count();
    stop_event.quantity = best_effort_dropped > static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max())
        ? std::numeric_limits<std::int32_t>::max()
        : static_cast<std::int32_t>(best_effort_dropped);
    // CleanStop 属于控制面屏障。先等消费者腾出位置，避免等待本身被计入业务丢弃。
    while (impl_->queue.depth() == kTraceQueueCapacity) {
        std::this_thread::yield();
    }
    if (!impl_->queue.try_push(stop_event)) {
        impl_->running.store(false, std::memory_order_release);
        if (impl_->writer.joinable()) impl_->writer.join();
        impl_->output.close();
        return;
    }
    impl_->running.store(false, std::memory_order_release);
    if (impl_->writer.joinable()) impl_->writer.join();
    impl_->output.close();
}

TraceQueueSnapshot AsyncTraceJournal::snapshot() const noexcept
{
    return {
        impl_->queue.depth(),
        kTraceQueueCapacity,
        impl_->queue.high_watermark(),
        impl_->queue.oldest_age_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()),
        impl_->queue.dropped_count(),
        impl_->queue.critical_dropped_count(),
        impl_->queue.best_effort_dropped_count(),
        impl_->writer_tid.load(std::memory_order_acquire),
    };
}

struct AsyncPerformanceRecorder::Impl {
    explicit Impl(std::filesystem::path output_path)
        : path(std::move(output_path))
    {
    }

    void run()
    {
        writer_tid.store(
            static_cast<std::int32_t>(::syscall(SYS_gettid)),
            std::memory_order_release);
        timespec cpu_start{};
        timespec cpu_finish{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start);
        PerformanceSample sample{};
        while (running.load(std::memory_order_acquire) || queue.depth() != 0) {
            if (queue.try_pop(sample)) {
                output << sample.sequence << ',' << sample.mono_ns << ','
                       << sample.account_index << ','
                       << performance_stage_name(sample.stage) << ','
                       << sample.latency_ns << '\n';
            } else {
                std::this_thread::yield();
            }
        }
        output.flush();
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_finish);
        writer_thread_cpu_ns.store(
            (cpu_finish.tv_sec - cpu_start.tv_sec) * 1'000'000'000LL
                + cpu_finish.tv_nsec - cpu_start.tv_nsec,
            std::memory_order_release);
    }

    std::filesystem::path path;
    PerformanceQueue queue;
    std::ofstream output;
    std::thread writer;
    std::atomic<bool> running{false};
    std::atomic<std::int64_t> writer_thread_cpu_ns{0};
    std::atomic<std::int32_t> writer_tid{0};
};

AsyncPerformanceRecorder::AsyncPerformanceRecorder(std::filesystem::path path)
    : impl_(std::make_unique<Impl>(std::move(path)))
{
}

AsyncPerformanceRecorder::~AsyncPerformanceRecorder()
{
    stop();
}

bool AsyncPerformanceRecorder::start()
{
    if (impl_->running.load(std::memory_order_acquire)) return true;
    impl_->output.open(impl_->path, std::ios::out | std::ios::trunc);
    if (!impl_->output) return false;
    impl_->output << "sequence,mono_ns,account_index,stage,latency_ns\n";
    impl_->running.store(true, std::memory_order_release);
    impl_->writer = std::thread([this] { impl_->run(); });
    return true;
}

bool AsyncPerformanceRecorder::try_record(
    const PerformanceSample& sample) noexcept
{
    if (!impl_->running.load(std::memory_order_acquire)) return false;
    return impl_->queue.try_push(sample);
}

void AsyncPerformanceRecorder::stop() noexcept
{
    impl_->running.store(false, std::memory_order_release);
    if (impl_->writer.joinable()) impl_->writer.join();
    impl_->output.close();
}

PerformanceRecorderSnapshot AsyncPerformanceRecorder::snapshot() const noexcept
{
    return {
        impl_->queue.depth(),
        kPerformanceQueueCapacity,
        impl_->queue.high_watermark(),
        impl_->queue.oldest_age_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()),
        impl_->queue.dropped_count(),
        impl_->writer_thread_cpu_ns.load(std::memory_order_acquire),
        impl_->writer_tid.load(std::memory_order_acquire),
    };
}

TraceJournalReadResult read_trace_journal(const std::filesystem::path& path)
{
    TraceJournalReadResult result{};
    std::ifstream input{path};
    std::string line;
    if (!std::getline(input, line)) return result;
    const bool version_three = line == kHeaderV3;
    const bool has_checkpoint = version_three || line == kHeaderV2;
    if (!has_checkpoint && line != kHeaderV1) return result;

    std::uint64_t previous_sequence = 0;
    while (std::getline(input, line)) {
        const auto fields = split_line(line);
        TraceEvent event{};
        std::int32_t stage_code = 0;
        unsigned direction = 0;
        unsigned offset = 0;
        unsigned purpose = 0;
        unsigned recovery_phase = 0;
        unsigned failed_phase = 0;
        if (fields.size() != (version_three ? 25U : has_checkpoint ? 21U : 17U)
            || fields[0] != (version_three ? "ctp_trace_v3"
                : has_checkpoint ? "ctp_trace_v2" : "ctp_trace_v1")
            || fields[1].empty()
            || !parse_integer(fields[2], event.trace_id.run_id)
            || !parse_integer(fields[3], event.trace_id.signal_id)
            || !parse_integer(fields[4], event.sequence)
            || !parse_integer(fields[5], event.mono_ns)
            || !parse_stage(fields[6], event.stage)
            || !parse_integer(fields[7], event.client_order_id)
            || !parse_integer(fields[8], event.order_ref)
            || !parse_integer(fields[9], event.limit_price_ticks)
            || !parse_integer(fields[10], event.quantity)
            || !parse_integer(fields[11], event.attempt)
            || !parse_integer(fields[12], direction) || direction > 1
            || !parse_integer(fields[13], offset) || offset > 1
            || !parse_integer(fields[14], purpose) || purpose > 1
            || fields[15].size() >= event.instrument.size()
            || !parse_integer(fields[16], stage_code)
            || (has_checkpoint
                && (fields[17].size() >= event.trading_day.size()
                    || !parse_integer(fields[18], event.daily_signals)
                    || !parse_integer(fields[19], event.daily_orders)
                    || !parse_integer(fields[20], event.daily_cancels)))
            || (version_three
                && (!parse_integer(fields[21], recovery_phase)
                    || recovery_phase > static_cast<unsigned>(RecoveryPhase::Frozen)
                    || !parse_integer(fields[22], failed_phase)
                    || failed_phase > static_cast<unsigned>(RecoveryPhase::Frozen)
                    || !parse_integer(fields[23], event.diagnostic.error_id)
                    || fields[24].size() >= event.diagnostic.error_message.size()
                    || std::any_of(fields[24].begin(), fields[24].end(), [](unsigned char value) {
                           return value < 32 || value == 127 || value == '"';
                       })))
            || (previous_sequence != 0 && event.sequence <= previous_sequence)) {
            return {};
        }
        event.code = stage_code;
        event.direction = static_cast<std::uint8_t>(direction);
        event.offset = static_cast<std::uint8_t>(offset);
        event.purpose = static_cast<std::uint8_t>(purpose);
        copy_to_field(event.instrument, fields[15]);
        if (has_checkpoint) copy_to_field(event.trading_day, fields[17]);
        if (version_three) {
            event.recovery_phase = static_cast<RecoveryPhase>(recovery_phase);
            event.diagnostic.failed_phase = static_cast<RecoveryPhase>(failed_phase);
            copy_to_field(event.diagnostic.error_message, fields[24]);
        }
        if (result.account_id.empty()) result.account_id = std::string{fields[1]};
        if (result.account_id != fields[1]) return {};
        previous_sequence = event.sequence;
        result.max_client_order_id = std::max(
            result.max_client_order_id, event.client_order_id);
        result.max_order_ref = std::max(result.max_order_ref, event.order_ref);
        result.events.push_back(event);
    }
    if (!input.eof()) return {};
    result.clean_shutdown = !result.events.empty()
        && result.events.back().stage == TraceStage::CleanStop;
    // 结构完整与可直接构造重启镜像是两个不同契约。异常退出的日志
    // 不能驱动本地镜像，但可允许会话连接柜台并以查询事实重新建账。
    result.valid = !result.clean_shutdown
        || result.events.back().code == 0;
    return result;
}

RestartImage build_restart_image(const TraceJournalReadResult& journal)
{
    RestartImage image{};
    if (!journal.valid || !journal.clean_shutdown || journal.account_id.empty()
        || journal.events.size() < 2) {
        return image;
    }
    const auto& checkpoint = journal.events[journal.events.size() - 2];
    const auto checkpoint_day = trading_day_view(checkpoint);
    if (checkpoint.stage != TraceStage::RestartCheckpoint
        || journal.events.back().stage != TraceStage::CleanStop
        || !valid_trading_day(checkpoint_day)) {
        return image;
    }
    image.account_id = journal.account_id;
    image.next_client_order_id = journal.max_client_order_id + 1;
    image.next_order_ref = journal.max_order_ref + 1;
    image.trading_day = std::string{checkpoint_day};
    image.daily_signals = checkpoint.daily_signals;
    image.daily_orders = checkpoint.daily_orders;
    image.daily_cancels = checkpoint.daily_cancels;
    for (const auto& event : journal.events) {
        if (event.stage == TraceStage::OrderSubmitted) {
            const auto duplicate = std::find_if(
                image.uncertain_orders.begin(),
                image.uncertain_orders.end(),
                [&event](const RestartOrder& order) {
                    return order.client_order_id == event.client_order_id
                        || order.order_ref == event.order_ref
                        || order.trace_id.signal_id == event.trace_id.signal_id;
                });
            if (event.trace_id.run_id == 0 || event.trace_id.signal_id == 0
                || event.client_order_id == 0 || event.order_ref == 0
                || event.quantity <= 0
                || instrument_view(event).empty()
                || event.direction > 1 || event.offset > 1 || event.purpose > 1
                || duplicate != image.uncertain_orders.end()) {
                return {};
            }
            RestartOrder order{};
            order.trace_id = event.trace_id;
            order.client_order_id = event.client_order_id;
            order.order_ref = event.order_ref;
            order.limit_price_ticks = event.limit_price_ticks;
            order.quantity = event.quantity;
            order.attempt = event.attempt;
            order.direction = event.direction;
            order.offset = event.offset;
            order.purpose = event.purpose;
            order.instrument = event.instrument;
            image.uncertain_orders.push_back(order);
            continue;
        }
        const bool terminal_report = event.stage == TraceStage::OrderRejected
            || (event.stage == TraceStage::OrderReport
                && (event.code == static_cast<std::int32_t>(
                        TraceOrderReportCode::Rejected)
                    || event.code == static_cast<std::int32_t>(
                        TraceOrderReportCode::Filled)
                    || event.code == static_cast<std::int32_t>(
                        TraceOrderReportCode::Canceled)));
        if (!terminal_report) continue;
        image.uncertain_orders.erase(
            std::remove_if(
                image.uncertain_orders.begin(),
                image.uncertain_orders.end(),
                [&event](const RestartOrder& order) {
                    return order.client_order_id == event.client_order_id;
                }),
            image.uncertain_orders.end());
    }
    image.valid = image.next_client_order_id != 0 && image.next_order_ref != 0;
    return image;
}

}
