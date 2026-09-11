#include "ctp/field.hpp"
#include "ctp/telemetry.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

std::size_t csv_sample_count(const std::string& text)
{
    std::istringstream input{text};
    std::set<std::string> samples;
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        const auto comma = line.find(',');
        if (comma != std::string::npos) samples.insert(line.substr(0, comma));
    }
    return samples.size();
}

std::size_t csv_sample_count_matching(
    const std::string& text,
    const std::string& fields)
{
    std::istringstream input{text};
    std::set<std::string> samples;
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        const auto comma = line.find(',');
        if (comma != std::string::npos
            && line.compare(comma, fields.size(), fields) == 0) {
            samples.insert(line.substr(0, comma));
        }
    }
    return samples.size();
}

std::size_t csv_row_count_containing(
    const std::string& text,
    const std::string& fields)
{
    std::istringstream input{text};
    std::size_t count = 0;
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        if (line.find(fields) != std::string::npos) ++count;
    }
    return count;
}

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

ctp::TraceEvent restart_checkpoint(std::uint64_t sequence)
{
    auto event = trace_event(sequence, ctp::TraceStage::RestartCheckpoint);
    ctp::copy_to_field(event.trading_day, "20260909");
    event.daily_signals = 7;
    event.daily_orders = 5;
    event.daily_cancels = 3;
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

void test_trace_queue_reserves_capacity_for_order_facts(
    test_support::TestRunner& runner)
{
    ctp::TraceQueue queue;
    auto market = trace_event(1, ctp::TraceStage::Market);
    market.trace_id.signal_id = 0;
    const auto best_effort_capacity =
        ctp::kTraceQueueCapacity - ctp::kCriticalTraceReserve;
    for (std::size_t index = 0; index < best_effort_capacity; ++index) {
        market.sequence = index + 1;
        runner.expect(
            queue.try_push(market),
            "unlinked markets must use the best-effort queue region");
    }
    runner.expect(
        !queue.try_push(market),
        "an unlinked market must not consume the reserved order region");
    runner.expect(
        !queue.try_push(trace_event(9000, ctp::TraceStage::RecoveryResponse))
            && !queue.try_push(trace_event(9001, ctp::TraceStage::RecoveryPhaseChanged)),
        "recovery diagnostics must not consume reserved order capacity");

    auto order_fact = trace_event(10'000, ctp::TraceStage::RiskAccepted);
    for (std::size_t index = 0; index < ctp::kCriticalTraceReserve; ++index) {
        order_fact.sequence = 10'000 + index;
        runner.expect(
            queue.try_push(order_fact),
            "order facts must remain writable after market saturation");
    }
    runner.expect(
        !queue.try_push(order_fact),
        "a physically full queue must reject without blocking");
    runner.expect(
        queue.dropped_count() == 4
            && queue.best_effort_dropped_count() == 3
            && queue.critical_dropped_count() == 1
            && queue.high_watermark() == ctp::kTraceQueueCapacity,
        "critical and best-effort drops must be separately exact");
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
            && journal.try_record(trace_event(3, ctp::TraceStage::OrderSubmitted))
            && journal.try_record(restart_checkpoint(4)),
        "trace events must enter the asynchronous journal");
    journal.stop();

    const auto loaded = ctp::read_trace_journal(path);
    runner.expect(
        loaded.valid && loaded.clean_shutdown && loaded.events.size() == 5
            && loaded.events[0].sequence == 1
            && loaded.events[2].stage == ctp::TraceStage::OrderSubmitted
            && loaded.events[3].stage == ctp::TraceStage::RestartCheckpoint
            && std::string_view{loaded.events[3].trading_day.data()} == "20260909"
            && loaded.events[3].daily_signals == 7
            && loaded.events[3].daily_orders == 5
            && loaded.events[3].daily_cancels == 3,
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

void test_v3_diagnostics_round_trip_and_reject_malformed_fields(
    test_support::TestRunner& runner)
{
    char directory[] = "/tmp/ctp-diagnostic-trace-XXXXXX";
    const auto* created = ::mkdtemp(directory);
    runner.expect(created != nullptr, "diagnostic fixture directory must be unique");
    if (created == nullptr) return;
    const auto path = std::filesystem::path{created} / "trace.csv";
    {
        ctp::AsyncTraceJournal journal{path, "account1"};
        runner.expect(journal.start(), "diagnostic journal must start");
        auto event = trace_event(1, ctp::TraceStage::RecoveryFrozen);
        event.recovery_phase = ctp::RecoveryPhase::Frozen;
        event.diagnostic.failed_phase = ctp::RecoveryPhase::QueryingFunds;
        event.diagnostic.error_id = -7;
        ctp::copy_to_field(event.diagnostic.error_message, "denied ********");
        runner.expect(journal.try_record(event)
                && journal.try_record(restart_checkpoint(2)),
            "diagnostic and checkpoint must be accepted together");
        journal.stop();
    }
    const auto loaded = ctp::read_trace_journal(path);
    runner.expect(loaded.valid && loaded.clean_shutdown && loaded.events.size() == 3
            && loaded.events[0].recovery_phase == ctp::RecoveryPhase::Frozen
            && loaded.events[0].diagnostic.failed_phase == ctp::RecoveryPhase::QueryingFunds
            && loaded.events[0].diagnostic.error_id == -7
            && ctp::field_text(loaded.events[0].diagnostic.error_message) == "denied ********"
            && ctp::build_restart_image(loaded).valid
            && ctp::build_restart_image(loaded).daily_signals == 7,
        "v3 must round-trip diagnostics without changing checkpoint semantics");
    std::ifstream input{path};
    std::string header;
    std::string row;
    std::getline(input, header);
    std::getline(input, row);
    input.close();
    runner.expect(header.find("ctp_trace_v3,") == 0,
        "new journal writer must identify its schema version");
    const auto suffix = row.rfind(",11,8,-7,denied ********");
    runner.expect(suffix != std::string::npos, "fixture must contain the diagnostic columns");
    if (suffix != std::string::npos) {
        const auto prefix = row.substr(0, suffix);
        for (const auto& invalid : std::vector<std::string>{
                 ",12,8,-7,denied", ",11,12,-7,denied", ",11,8,2147483648,denied",
                 ",11,8,-7," + std::string(81, 'x'), ",11,8,-7,tab\there",
                 ",11,8,-7,quote\"here", ",11,8,-7,comma,here", ",11,8,-7"}) {
            {
                std::ofstream output{path};
                output << header << '\n' << prefix << invalid << '\n';
            }
            runner.expect(!ctp::read_trace_journal(path).valid,
                "invalid diagnostic fields must not become a trusted journal");
        }
    }
    std::filesystem::remove_all(created);
}

void test_abnormal_exit_after_start_keeps_a_complete_empty_journal(
    test_support::TestRunner& runner)
{
    const std::filesystem::path path{
        "/tmp/ctp_crash_boundary_empty_journal.csv"};
    std::filesystem::remove(path);

    const auto child = ::fork();
    if (child == 0) {
        ctp::AsyncTraceJournal journal{path, "account1"};
        // _exit 不执行析构函数，用真实异常退出证明 start() 已持久化文件头。
        ::_exit(journal.start() ? 0 : 2);
    }

    int status = 0;
    const bool child_ok = child > 0
        && ::waitpid(child, &status, 0) == child
        && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    const auto loaded = ctp::read_trace_journal(path);
    runner.expect(
        child_ok && loaded.valid && !loaded.clean_shutdown
            && loaded.account_id.empty() && loaded.events.empty(),
        "a crash immediately after journal start must leave a complete abnormal prefix");
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

void test_structurally_complete_abnormal_journal_allows_counter_recovery(
    test_support::TestRunner& runner)
{
    const std::filesystem::path path{
        "/tmp/ctp_crash_recovery_abnormal_trace.csv"};
    for (std::size_t flushed_events = 0; flushed_events <= 2;
         ++flushed_events) {
        std::filesystem::remove(path);
        {
            std::ofstream output{path};
            output << "ctp_trace_v2,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,limit_price_ticks,quantity,attempt,direction,offset,purpose,instrument,code,trading_day,daily_signals,daily_orders,daily_cancels\n";
            if (flushed_events >= 1) {
                output << "ctp_trace_v2,account1,17,9001,1,100,risk_accepted,71,41,4000,1,0,0,0,0,IF2609,0,,0,0,0\n";
            }
            if (flushed_events >= 2) {
                output << "ctp_trace_v2,account1,17,9001,2,200,order_submitted,71,41,4000,1,0,0,0,0,IF2609,0,,0,0,0\n";
            }
        }
        const auto loaded = ctp::read_trace_journal(path);
        const auto image = ctp::build_restart_image(loaded);
        runner.expect(
            loaded.valid && !loaded.clean_shutdown && !image.valid
                && loaded.events.size() == flushed_events,
            "every complete prefix around async log flush must allow counter recovery without becoming a restart image");
        for (const auto& event : loaded.events) {
            runner.expect(event.recovery_phase == ctp::RecoveryPhase::Idle
                    && event.diagnostic.error_id == 0
                    && event.diagnostic.failed_phase == ctp::RecoveryPhase::Idle
                    && event.diagnostic.error_message[0] == '\0',
                "legacy v2 must default absent diagnostic fields");
        }
    }
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

void test_best_effort_drop_does_not_poison_restart_source(
    test_support::TestRunner& runner)
{
    const std::filesystem::path path{
        "/tmp/ctp_trace_best_effort_drop.csv"};
    std::filesystem::remove(path);
    {
        std::ofstream output{path};
        output << "ctp_trace_v2,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,limit_price_ticks,quantity,attempt,direction,offset,purpose,instrument,code,trading_day,daily_signals,daily_orders,daily_cancels\n";
        output << "ctp_trace_v2,account1,17,9001,1,100,order_submitted,71,41,4000,1,0,0,0,0,IF2609,0,,0,0,0\n";
        output << "ctp_trace_v2,account1,17,9001,2,200,restart_checkpoint,0,0,0,0,0,0,0,0,,0,20260909,1,1,0\n";
        output << "ctp_trace_v2,account1,0,0,3,0,clean_stop,0,0,0,7,0,0,0,0,,0,,0,0,0\n";
    }
    const auto loaded = ctp::read_trace_journal(path);
    const auto image = ctp::build_restart_image(loaded);
    runner.expect(
        loaded.valid && loaded.clean_shutdown && image.valid
            && image.uncertain_orders.size() == 1,
        "best-effort market loss must remain observable without invalidating order recovery");
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
    journal.events = {
        submitted,
        filled,
        restart_checkpoint(3),
        trace_event(4, ctp::TraceStage::CleanStop)};
    journal.max_client_order_id = submitted.client_order_id;
    journal.max_order_ref = submitted.order_ref;
    const auto image = ctp::build_restart_image(journal);
    runner.expect(
        image.valid && image.uncertain_orders.empty()
            && image.next_client_order_id == 72
            && image.next_order_ref == 42
            && image.trading_day == "20260909"
            && image.daily_signals == 7
            && image.daily_orders == 5
            && image.daily_cancels == 3,
        "restart image must advance identities but omit terminal orders");
}

void test_v1_journal_remains_auditable_but_cannot_restore_daily_limits(
    test_support::TestRunner& runner)
{
    const std::filesystem::path path{"/tmp/ctp_trace_v1_legacy.csv"};
    std::filesystem::remove(path);
    {
        std::ofstream output{path};
        output << "ctp_trace_v1,account_id,run_id,signal_id,sequence,mono_ns,stage,client_order_id,order_ref,limit_price_ticks,quantity,attempt,direction,offset,purpose,instrument,code\n";
        output << "ctp_trace_v1,account1,17,9001,1,100,signal,71,41,4000,1,0,0,0,0,IF2609,0\n";
        output << "ctp_trace_v1,account1,0,0,2,0,clean_stop,0,0,0,0,0,0,0,0,,0\n";
    }
    const auto loaded = ctp::read_trace_journal(path);
    const auto image = ctp::build_restart_image(loaded);
    runner.expect(
        loaded.valid && loaded.clean_shutdown && !image.valid
            && loaded.events[0].recovery_phase == ctp::RecoveryPhase::Idle
            && loaded.events[0].diagnostic.error_id == 0
            && loaded.events[0].diagnostic.failed_phase == ctp::RecoveryPhase::Idle
            && loaded.events[0].diagnostic.error_message[0] == '\0',
        "legacy v1 trace must remain readable but cannot refresh daily quotas");
    std::filesystem::remove(path);
}

void test_latency_statistics_include_tail_and_jitter(
    test_support::TestRunner& runner)
{
    const std::vector<std::int64_t> samples{1, 2, 3, 4, 5, 100};
    const auto statistics = ctp::compute_latency_statistics(samples);

    runner.expect(
        statistics.count == 6 && statistics.minimum_ns == 1
            && statistics.p50_ns == 3 && statistics.p95_ns == 100
            && statistics.p99_ns == 100 && statistics.p999_ns == 100
            && statistics.maximum_ns == 100,
        "latency statistics must retain the complete tail distribution");
    runner.expect(
        statistics.jitter_p99_p50_ns == 97
            && statistics.jitter_p999_p50_ns == 97
            && statistics.maximum_pause_ns == 95,
        "latency statistics must report tail jitter and maximum pause");
}

void test_performance_queue_is_fixed_and_counts_drops(
    test_support::TestRunner& runner)
{
    ctp::PerformanceQueue queue;
    ctp::PerformanceSample sample{};
    sample.stage = ctp::PerformanceStage::MarketToSignal;
    sample.latency_ns = 17;

    test_support::AllocationProbe probe;
    for (std::size_t index = 0; index < ctp::kPerformanceQueueCapacity; ++index) {
        sample.sequence = index + 1;
        runner.expect(queue.try_push(sample), "available performance slot must accept a sample");
    }
    runner.expect(
        !queue.try_push(sample),
        "a full performance queue must reject without waiting");
    probe.stop();

    runner.expect(
        probe.count() == 0 && queue.dropped_count() == 1
            && queue.high_watermark() == ctp::kPerformanceQueueCapacity,
        "performance collection must not allocate and must expose exact overflow");
}

void test_performance_recorder_writes_raw_samples(
    test_support::TestRunner& runner)
{
    const std::filesystem::path path{"/tmp/ctp_performance_raw.csv"};
    std::filesystem::remove(path);
    ctp::AsyncPerformanceRecorder recorder{path};
    runner.expect(recorder.start(), "performance recorder must open its raw file");
    runner.expect(
        recorder.try_record({1, 100, 17, 0, ctp::PerformanceStage::MarketToSignal})
            && recorder.try_record({2, 120, 19, 1, ctp::PerformanceStage::CallbackToState}),
        "fixed performance samples must enter the asynchronous recorder");
    recorder.stop();

    std::ifstream input{path};
    const std::string text{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    runner.expect(
        text == "sequence,mono_ns,account_index,stage,latency_ns\n"
                "1,100,0,market_to_signal,17\n"
                "2,120,1,callback_to_state,19\n",
        "raw performance output must preserve order, account and stage");
    std::filesystem::remove(path);
}

void test_offline_benchmark_writes_complete_evidence(
    test_support::TestRunner& runner)
{
    const std::filesystem::path output_path{"/tmp/ctp-benchmark-evidence-test"};
    std::filesystem::remove_all(output_path);
    ctp::BenchmarkConfig benchmark{};
    benchmark.config_path = "config/accounts.local.ini";
    benchmark.input_path =
        std::string{CTP_SOURCE_DIR} + "/tests/data/replay/minimal_signal_v1.csv";
    benchmark.output_path = output_path.string();
    benchmark.account_count = 4;
    const std::vector<ctp::AccountConfig> accounts{
        {"account1", "9999", "user1", "test-password", "app", "auth", "front"},
        {"account2", "9999", "user2", "test-password", "app", "auth", "front"},
        {"account3", "9999", "user3", "test-password", "app", "auth", "front"},
        {"account4", "9999", "user4", "test-password", "app", "auth", "front"}};
    const ctp::RuntimeConfig config{
        ctp::Mode::Benchmark, {}, {}, {}, {}, {}, {}, {}, {}, {}, 0,
        accounts, benchmark};
    std::ostringstream output;
    std::ostringstream error;

    runner.expect(
        ctp::run_benchmark(config, output, error) == 0,
        "offline benchmark must finish without a real CTP connection");
    constexpr std::string_view files[]{
        "manifest.json", "latency_raw.csv", "queue_raw.csv", "cpu_raw.csv",
        "events_summary.json", "report.md", "reproduce.sh", "SHA256SUMS"};
    bool complete = true;
    for (const auto file : files) {
        const auto path = output_path / file;
        complete = complete && std::filesystem::exists(path)
            && std::filesystem::file_size(path) > 0;
    }
    runner.expect(complete, "benchmark must preserve every required evidence file");

    std::ifstream manifest{output_path / "manifest.json"};
    const std::string manifest_text{
        std::istreambuf_iterator<char>{manifest},
        std::istreambuf_iterator<char>{}};
    std::ifstream events{output_path / "events_summary.json"};
    const std::string event_text{
        std::istreambuf_iterator<char>{events},
        std::istreambuf_iterator<char>{}};
    runner.expect(
        manifest_text.find(
            "7843b7698379db7b5d9352e4db1ae97db77c868d7be9646f7a2f98c30c2ce116")
                != std::string::npos
            && event_text.find("\"submitted\": 0") == std::string::npos
            && event_text.find("\"api_order_calls\": 0") == std::string::npos,
        "evidence must identify its replay input and prove entry into submit");
    std::ifstream queue{output_path / "queue_raw.csv"};
    const std::string queue_text{
        std::istreambuf_iterator<char>{queue},
        std::istreambuf_iterator<char>{}};
    std::ifstream cpu{output_path / "cpu_raw.csv"};
    const std::string cpu_text{
        std::istreambuf_iterator<char>{cpu},
        std::istreambuf_iterator<char>{}};
    bool all_queue_roles = true;
    bool all_thread_roles = true;
    constexpr std::string_view queue_roles[]{
        "market_ingress", "trader_request", "trading_callback", "trace", "latency"};
    constexpr std::string_view thread_roles[]{
        "account_worker", "trader_callback", "trace_writer", "latency_writer"};
    for (std::size_t account = 0; account < 4; ++account) {
        for (const auto role : queue_roles) {
            all_queue_roles = all_queue_roles
                && csv_sample_count_matching(
                    queue_text, "," + std::to_string(account) + ","
                        + std::string{role} + ",") >= 2;
        }
        for (const auto role : thread_roles) {
            all_thread_roles = all_thread_roles
                && csv_sample_count_matching(
                    cpu_text, ",thread," + std::to_string(account) + ","
                        + std::string{role} + ",") >= 2;
        }
    }
    runner.expect(
        queue_text.find("capacity,high_watermark,oldest_age_ns,dropped")
                != std::string::npos
            && all_queue_roles
            && csv_sample_count(queue_text) >= 2,
        "queue evidence must time-sample every queue in the account runtime path");
    runner.expect(
        cpu_text.find("scope,account_index,role,tid,user_cpu_ns,system_cpu_ns,voluntary_context_switches,nonvoluntary_context_switches")
                != std::string::npos
            && csv_sample_count_matching(
                cpu_text, ",process,-1,process,") >= 2
            && csv_sample_count_matching(
                cpu_text, ",thread,-1,producer,") >= 2
            && csv_sample_count_matching(
                cpu_text, ",thread,-1,sampler,") >= 2
            && all_thread_roles
            && csv_sample_count(cpu_text) >= 2,
        "CPU evidence must contain per-thread account and callback time series");
    std::ifstream report{output_path / "report.md"};
    const std::string report_text{
        std::istreambuf_iterator<char>{report},
        std::istreambuf_iterator<char>{}};
    bool all_reported_queues = true;
    for (const auto role : queue_roles) {
        all_reported_queues = all_reported_queues
            && report_text.find(std::string{"| "} + std::string{role} + " |")
                != std::string::npos;
    }
    runner.expect(
        report_text.find("CPU 使用情况") != std::string::npos
            && report_text.find("上下文切换") != std::string::npos,
        "report must summarize process and per-thread CPU evidence");
    runner.expect(
        report_text.find("运行队列汇总") != std::string::npos
            && all_reported_queues,
        "report must summarize all five runtime queue kinds");
    runner.expect(
        report_text.find("样本不足") != std::string::npos,
        "short benchmark must not present four samples as credible tail evidence");
    std::ifstream latency{output_path / "latency_raw.csv"};
    const std::string latency_text{
        std::istreambuf_iterator<char>{latency},
        std::istreambuf_iterator<char>{}};
    runner.expect(
        csv_row_count_containing(latency_text, ",callback_to_state,") >= 8
            && csv_row_count_containing(
                latency_text, ",simulated_end_to_end,") >= 4,
        "end-to-end samples must include each account trade callback and state update");
    runner.expect(
        event_text.find("\"market_events_enqueued\"") != std::string::npos
            && event_text.find("\"market_events_processed\"") != std::string::npos
            && event_text.find("\"order_requests\"") != std::string::npos
            && event_text.find("\"order_acceptances\"") != std::string::npos
            && event_text.find("\"order_rejections\"") != std::string::npos
            && event_text.find("\"cancel_requests\"") != std::string::npos
            && event_text.find("\"cancel_acceptances\"") != std::string::npos
            && event_text.find("\"trades\"") != std::string::npos
            && event_text.find("\"traded_volume\"") != std::string::npos
            && event_text.find("\"freeze_transitions\"") != std::string::npos
            && event_text.find("\"recovery_completions\"") != std::string::npos,
        "event evidence must preserve the complete per-account lifecycle");
    runner.expect(
        manifest_text.find("test-password") == std::string::npos,
        "benchmark metadata must not expose account credentials");
    std::filesystem::remove_all(output_path);
}

}

int main()
{
    test_support::TestRunner runner{"telemetry"};
    test_trace_queue_is_nonblocking_and_counts_drops(runner);
    test_trace_queue_reserves_capacity_for_order_facts(runner);
    test_async_journal_round_trip_and_clean_marker(runner);
    test_v3_diagnostics_round_trip_and_reject_malformed_fields(runner);
    test_abnormal_exit_after_start_keeps_a_complete_empty_journal(runner);
    test_truncated_journal_is_not_a_clean_restart(runner);
    test_structurally_complete_abnormal_journal_allows_counter_recovery(runner);
    test_dropped_trace_journal_is_not_a_restart_source(runner);
    test_best_effort_drop_does_not_poison_restart_source(runner);
    test_restart_image_keeps_only_unresolved_orders(runner);
    test_v1_journal_remains_auditable_but_cannot_restore_daily_limits(runner);
    test_latency_statistics_include_tail_and_jitter(runner);
    test_performance_queue_is_fixed_and_counts_drops(runner);
    test_performance_recorder_writes_raw_samples(runner);
    test_offline_benchmark_writes_complete_evidence(runner);
    return runner.finish();
}
