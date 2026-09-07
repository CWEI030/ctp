#include "ctp/field.hpp"
#include "ctp/telemetry.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace {

ctp::TraceEvent trace_event(
    std::uint64_t sequence,
    ctp::TraceStage stage = ctp::TraceStage::Signal)
{
    ctp::TraceEvent event{};
    event.trace_id = {17, 9001};
    event.sequence = sequence;
    event.mono_ns = static_cast<std::int64_t>(sequence * 100);
    event.client_order_id = 71;
    event.order_ref = 41;
    event.stage = stage;
    event.quantity = 1;
    ctp::copy_to_field(event.instrument, "IF2609");
    return event;
}

void test_trace_queue_is_nonblocking_and_counts_drops(
    test_support::TestRunner& runner)
{
    ctp::TraceQueue queue;
    {
        test_support::AllocationProbe probe;
        for (std::size_t index = 0; index < ctp::kTraceQueueCapacity; ++index) {
            runner.expect(
                queue.try_push(trace_event(index + 1)),
                "available trace slots must accept fixed events");
        }
        runner.expect(
            !queue.try_push(trace_event(9999)),
            "a full trace queue must reject without blocking");
        probe.stop();
        runner.expect(
            probe.count() == 0,
            "trace queue push and overflow accounting must not allocate");
    }
    runner.expect(
        queue.dropped_count() == 1
            && queue.high_watermark() == ctp::kTraceQueueCapacity,
        "trace queue must expose exact drop and high-water counts");
}

void test_async_journal_round_trip_and_clean_marker(
    test_support::TestRunner& runner)
{
    const std::filesystem::path path{
        "/tmp/ctp_batch007_trace_round_trip.csv"};
    std::filesystem::remove(path);
    ctp::AsyncTraceJournal journal{path, "account1"};
    runner.expect(journal.start(), "trace journal must open on the control plane");
    runner.expect(
        journal.try_record(trace_event(1, ctp::TraceStage::Market))
            && journal.try_record(trace_event(2, ctp::TraceStage::Signal))
            && journal.try_record(trace_event(3, ctp::TraceStage::OrderSubmitted)),
        "trace events must enter the asynchronous journal");
    journal.stop();

    const auto loaded = ctp::read_trace_journal(path);
    runner.expect(
        loaded.valid && loaded.clean_shutdown && loaded.events.size() == 4
            && loaded.events[0].sequence == 1
            && loaded.events[2].stage == ctp::TraceStage::OrderSubmitted,
        "journal reader must preserve order and detect the clean stop marker");

    std::ifstream input{path};
    const std::string text{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    runner.expect(
        text.find("password") == std::string::npos
            && text.find("auth") == std::string::npos,
        "trace output must not contain credential fields");
    std::filesystem::remove(path);
}

void test_truncated_journal_is_not_a_clean_restart(
    test_support::TestRunner& runner)
{
    const std::filesystem::path path{"/tmp/ctp_batch007_trace_truncated.csv"};
    std::filesystem::remove(path);
    {
        std::ofstream output{path};
        output << "ctp_trace_v1,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,quantity,instrument,code\n";
        output << "ctp_trace_v1,account1,17,9001,1,100,signal,71";
    }
    const auto loaded = ctp::read_trace_journal(path);
    runner.expect(
        !loaded.valid && !loaded.clean_shutdown,
        "a partial tail must never be accepted as a clean restart record");
    std::filesystem::remove(path);
}

}

int main()
{
    test_support::TestRunner runner{"telemetry"};
    test_trace_queue_is_nonblocking_and_counts_drops(runner);
    test_async_journal_round_trip_and_clean_marker(runner);
    test_truncated_journal_is_not_a_clean_restart(runner);
    return runner.finish();
}
