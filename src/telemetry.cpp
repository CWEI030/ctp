#include "ctp/telemetry.hpp"

#include "ctp/field.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <string_view>
#include <thread>
#include <utility>

namespace ctp {
namespace {

constexpr std::string_view kHeader =
    "ctp_trace_v1,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,quantity,instrument,code";

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
    case TraceStage::CleanStop: return "clean_stop";
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
             TraceStage::CleanStop}) {
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

std::array<std::string_view, 12> split_line(
    const std::string& line,
    bool& valid) noexcept
{
    std::array<std::string_view, 12> fields{};
    std::size_t begin = 0;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto comma = line.find(',', begin);
        if (index + 1 == fields.size()) {
            if (comma != std::string::npos) return fields;
            fields[index] = std::string_view{line}.substr(begin);
            valid = true;
            return fields;
        }
        if (comma == std::string::npos) return fields;
        fields[index] = std::string_view{line}.substr(begin, comma - begin);
        begin = comma + 1;
    }
    return fields;
}

std::string_view instrument_view(const TraceEvent& event) noexcept
{
    const auto end = std::find(
        event.instrument.begin(), event.instrument.end(), '\0');
    return {event.instrument.data(),
            static_cast<std::size_t>(end - event.instrument.begin())};
}

}

struct AsyncTraceJournal::Impl {
    Impl(std::filesystem::path output_path, std::string owned_account_id)
        : path(std::move(output_path)), account_id(std::move(owned_account_id))
    {
    }

    void write(const TraceEvent& event)
    {
        output << "ctp_trace_v1," << account_id << ','
               << event.trace_id.run_id << ',' << event.trace_id.signal_id << ','
               << event.sequence << ',' << event.mono_ns << ','
               << stage_name(event.stage) << ',' << event.client_order_id << ','
               << event.order_ref << ',' << event.quantity << ','
               << instrument_view(event) << ',' << event.code << '\n';
    }

    void run()
    {
        TraceEvent event{};
        while (running.load(std::memory_order_acquire) || queue.depth() != 0) {
            if (queue.try_pop(event)) {
                write(event);
            } else {
                std::this_thread::yield();
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
    impl_->output << kHeader << '\n';
    impl_->running.store(true, std::memory_order_release);
    impl_->writer = std::thread([this] { impl_->run(); });
    return true;
}

bool AsyncTraceJournal::try_record(const TraceEvent& event) noexcept
{
    if (!impl_->running.load(std::memory_order_acquire)) return false;
    auto previous = impl_->largest_sequence.load(std::memory_order_relaxed);
    while (previous < event.sequence
           && !impl_->largest_sequence.compare_exchange_weak(
               previous,
               event.sequence,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    return impl_->queue.try_push(event);
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
    while (!impl_->queue.try_push(stop_event)) std::this_thread::yield();
    impl_->running.store(false, std::memory_order_release);
    if (impl_->writer.joinable()) impl_->writer.join();
    impl_->output.close();
}

TraceQueueSnapshot AsyncTraceJournal::snapshot() const noexcept
{
    return {
        impl_->queue.depth(),
        impl_->queue.high_watermark(),
        impl_->queue.dropped_count(),
    };
}

TraceJournalReadResult read_trace_journal(const std::filesystem::path& path)
{
    TraceJournalReadResult result{};
    std::ifstream input{path};
    std::string line;
    if (!std::getline(input, line) || line != kHeader) return result;

    std::uint64_t previous_sequence = 0;
    while (std::getline(input, line)) {
        bool split_valid = false;
        const auto fields = split_line(line, split_valid);
        TraceEvent event{};
        std::int32_t stage_code = 0;
        if (!split_valid || fields[0] != "ctp_trace_v1"
            || fields[1].empty()
            || !parse_integer(fields[2], event.trace_id.run_id)
            || !parse_integer(fields[3], event.trace_id.signal_id)
            || !parse_integer(fields[4], event.sequence)
            || !parse_integer(fields[5], event.mono_ns)
            || !parse_stage(fields[6], event.stage)
            || !parse_integer(fields[7], event.client_order_id)
            || !parse_integer(fields[8], event.order_ref)
            || !parse_integer(fields[9], event.quantity)
            || fields[10].size() >= event.instrument.size()
            || !parse_integer(fields[11], stage_code)
            || (previous_sequence != 0 && event.sequence <= previous_sequence)) {
            return {};
        }
        event.code = stage_code;
        copy_to_field(event.instrument, fields[10]);
        if (result.account_id.empty()) result.account_id = std::string{fields[1]};
        if (result.account_id != fields[1]) return {};
        previous_sequence = event.sequence;
        result.max_client_order_id = std::max(
            result.max_client_order_id, event.client_order_id);
        result.max_order_ref = std::max(result.max_order_ref, event.order_ref);
        result.events.push_back(event);
    }
    if (!input.eof() || result.events.empty()) return {};
    result.clean_shutdown = result.events.back().stage == TraceStage::CleanStop;
    result.valid = result.clean_shutdown;
    return result;
}

}
