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
        output << "ctp_trace_v1,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,limit_price_ticks,quantity,attempt,direction,offset,purpose,instrument,code\n";
        output << "ctp_trace_v1,account1,17,9001,1,100,signal,71";
    }
    const auto loaded = ctp::read_trace_journal(path);
    runner.expect(
        !loaded.valid && !loaded.clean_shutdown,
        "a partial tail must never be accepted as a clean restart record");
    std::filesystem::remove(path);
}

void test_dropped_trace_journal_is_not_a_restart_source(
    test_support::TestRunner& runner)
{
    const std::filesystem::path path{"/tmp/ctp_batch007_trace_dropped.csv"};
    std::filesystem::remove(path);
    {
        std::ofstream output{path};
        output << "ctp_trace_v1,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,limit_price_ticks,quantity,attempt,direction,offset,purpose,instrument,code\n";
        output << "ctp_trace_v1,account1,17,9001,1,100,order_submitted,71,41,4000,1,0,0,0,0,IF2609,0\n";
        output << "ctp_trace_v1,account1,0,0,2,0,clean_stop,0,0,0,0,0,0,0,0,,1\n";
    }
    const auto loaded = ctp::read_trace_journal(path);
    const auto image = ctp::build_restart_image(loaded);
    runner.expect(
        loaded.clean_shutdown && !loaded.valid && !image.valid,
        "a clean file with dropped business facts must not drive restart");
    std::filesystem::remove(path);
}

void test_restart_image_keeps_only_unresolved_orders(
    test_support::TestRunner& runner)
{
    ctp::TraceJournalReadResult journal{};
    journal.valid = true;
    journal.clean_shutdown = true;
    journal.account_id = "account1";
    auto submitted = trace_event(1, ctp::TraceStage::OrderSubmitted);
    auto filled = trace_event(2, ctp::TraceStage::OrderReport);
    filled.code = static_cast<std::int32_t>(
        ctp::TraceOrderReportCode::Filled);
    journal.events = {submitted, filled};
    journal.max_client_order_id = submitted.client_order_id;
    journal.max_order_ref = submitted.order_ref;
    const auto image = ctp::build_restart_image(journal);
    runner.expect(
        image.valid && image.uncertain_orders.empty()
            && image.next_client_order_id == 72
            && image.next_order_ref == 42,
        "restart image must advance identities but omit terminal orders");
}

}

int main()
{
    test_support::TestRunner runner{"telemetry"};
    test_trace_queue_is_nonblocking_and_counts_drops(runner);
    test_async_journal_round_trip_and_clean_marker(runner);
    test_truncated_journal_is_not_a_clean_restart(runner);
    test_dropped_trace_journal_is_not_a_restart_source(runner);
    test_restart_image_keeps_only_unresolved_orders(runner);
    return runner.finish();
}
