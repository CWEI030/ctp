#include "ctp/telemetry.hpp"

#include "ctp/field.hpp"
#include "ctp/strategy.hpp"

#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include <sched.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace ctp {
namespace {

std::int64_t steady_now_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t thread_cpu_now_ns() noexcept
{
    timespec value{};
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0
        ? value.tv_sec * 1'000'000'000LL + value.tv_nsec : 0;
}

std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept
{
    return (value >> bits) | (value << (32 - bits));
}

std::string sha256_file(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    const std::uint64_t bit_count = bytes.size() * 8ULL;
    bytes.push_back(0x80);
    while (bytes.size() % 64 != 56) bytes.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(bit_count >> shift));
    }
    constexpr std::uint32_t constants[64]{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<std::uint32_t, 8> hash{
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
        std::uint32_t words[64]{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto base = offset + index * 4;
            words[index] = (static_cast<std::uint32_t>(bytes[base]) << 24)
                | (static_cast<std::uint32_t>(bytes[base + 1]) << 16)
                | (static_cast<std::uint32_t>(bytes[base + 2]) << 8)
                | bytes[base + 3];
        }
        for (std::size_t index = 16; index < 64; ++index) {
            const auto s0 = rotate_right(words[index - 15], 7)
                ^ rotate_right(words[index - 15], 18)
                ^ (words[index - 15] >> 3);
            const auto s1 = rotate_right(words[index - 2], 17)
                ^ rotate_right(words[index - 2], 19)
                ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0
                + words[index - 7] + s1;
        }
        auto a = hash[0]; auto b = hash[1]; auto c = hash[2]; auto d = hash[3];
        auto e = hash[4]; auto f = hash[5]; auto g = hash[6]; auto h = hash[7];
        for (std::size_t index = 0; index < 64; ++index) {
            const auto sum1 = rotate_right(e, 6) ^ rotate_right(e, 11)
                ^ rotate_right(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto temporary1 = h + sum1 + choose
                + constants[index] + words[index];
            const auto sum0 = rotate_right(a, 2) ^ rotate_right(a, 13)
                ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temporary1;
            d = c; c = b; b = a; a = temporary1 + temporary2;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
        hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
    }
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const auto value : hash) result << std::setw(8) << value;
    return result.str();
}

std::string read_first_line(const std::filesystem::path& path)
{
    std::ifstream input{path};
    std::string value;
    std::getline(input, value);
    return value;
}

std::string git_commit()
{
    const auto head = read_first_line(".git/HEAD");
    constexpr std::string_view prefix{"ref: "};
    if (head.rfind(prefix, 0) != 0) return head;
    return read_first_line(std::filesystem::path{".git"} / head.substr(prefix.size()));
}

std::string cpu_model()
{
    std::ifstream input{"/proc/cpuinfo"};
    std::string line;
    while (std::getline(input, line)) {
        constexpr std::string_view prefix{"model name"};
        if (line.rfind(prefix, 0) == 0) {
            const auto separator = line.find(':');
            return separator == std::string::npos ? line : line.substr(separator + 2);
        }
    }
    return "unknown";
}

std::string json_escape(std::string_view value)
{
    std::string result;
    for (const char character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

std::string shell_quote(std::string_view value)
{
    std::string result{"'"};
    for (const char character : value) {
        if (character == '\'') result += "'\\''";
        else result.push_back(character);
    }
    result.push_back('\'');
    return result;
}

class OfflineTraderApi final : public TraderApi {
public:
    explicit OfflineTraderApi(std::uint64_t& calls) : calls_(calls) {}

    void register_spi(CThostFtdcTraderSpi* spi) override { spi_ = spi; }
    void subscribe_private_topic(THOST_TE_RESUME_TYPE, int) override {}
    void subscribe_public_topic(THOST_TE_RESUME_TYPE) override {}
    void register_front(const std::string&) override {}
    void init() override {}
    int request_authenticate(CThostFtdcReqAuthenticateField*, int) override { return 0; }
    int request_user_login(CThostFtdcReqUserLoginField*, int) override { return 0; }
    int request_trading_account(CThostFtdcQryTradingAccountField*, int) override { return 0; }
    int request_investor_position(CThostFtdcQryInvestorPositionField*, int) override { return 0; }
    int request_order_insert(CThostFtdcInputOrderField* request, int) override
    {
        ++calls_;
        CThostFtdcOrderField report{};
        copy_to_field(report.OrderRef, std::string_view{request->OrderRef});
        report.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
        report.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
        report.VolumeTotal = request->VolumeTotalOriginal;
        if (spi_ != nullptr) spi_->OnRtnOrder(&report);
        return 0;
    }
    int request_order_action(CThostFtdcInputOrderActionField*, int) override { return 0; }
    int request_order_query(CThostFtdcQryOrderField*, int) override { return 0; }
    int request_trade_query(CThostFtdcQryTradeField*, int) override { return 0; }
    void release() override { spi_ = nullptr; }

private:
    std::uint64_t& calls_;
    CThostFtdcTraderSpi* spi_{nullptr};
};

struct BenchmarkAccount {
    BenchmarkAccount(
        const AccountConfig& account,
        const StrategyConfig& strategy_config,
        RiskLimits limits,
        std::size_t capacity)
        : strategy(strategy_config),
          session(
              account,
              limits,
              std::make_unique<OfflineTraderApi>(order_calls),
              capacity,
              16,
              capacity,
              64)
    {
        session.activate(1, 1, "0");
    }

    ThresholdStrategy strategy;
    std::uint64_t order_calls{0};
    std::uint64_t signals{0};
    std::uint64_t submitted{0};
    std::uint64_t rejected{0};
    AccountTradingSession session;
};

bool load_replay(
    const std::filesystem::path& path,
    std::vector<ReplayEvent>& events,
    std::string& message)
{
    std::ifstream input{path};
    std::string line;
    if (!std::getline(input, line) || line != kReplayCsvHeader) {
        message = "replay header is invalid";
        return false;
    }
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto parsed = parse_replay_csv_line(line);
        if (parsed.code != ReplayParseCode::Parsed) {
            message = "replay row is invalid";
            return false;
        }
        events.push_back(parsed.event);
    }
    if (events.empty()) {
        message = "replay contains no events";
        return false;
    }
    return true;
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value) noexcept
{
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

std::array<std::vector<std::int64_t>, 4> read_latency_samples(
    const std::filesystem::path& path)
{
    std::array<std::vector<std::int64_t>, 4> values;
    std::ifstream input{path};
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        const auto last_comma = line.rfind(',');
        const auto stage_begin = last_comma == std::string::npos
            ? std::string::npos : line.rfind(',', last_comma - 1);
        if (stage_begin == std::string::npos) continue;
        const auto stage = std::string_view{line}.substr(
            stage_begin + 1, last_comma - stage_begin - 1);
        std::int64_t latency = 0;
        if (!parse_integer(
                std::string_view{line}.substr(last_comma + 1), latency)) continue;
        std::size_t index = 0;
        if (stage == "signal_to_order_call") index = 1;
        else if (stage == "callback_to_state") index = 2;
        else if (stage == "simulated_end_to_end") index = 3;
        else if (stage != "market_to_signal") continue;
        values[index].push_back(latency);
    }
    return values;
}

void write_statistics(
    std::ostream& output,
    std::string_view name,
    const LatencyStatistics& value)
{
    output << "| " << name << " | " << value.count << " | "
           << value.minimum_ns << " | " << value.p50_ns << " | "
           << value.p95_ns << " | " << value.p99_ns << " | "
           << value.p999_ns << " | " << value.maximum_ns << " | "
           << std::fixed << std::setprecision(2) << value.mean_ns << " | "
           << value.standard_deviation_ns << " | "
           << value.jitter_p99_p50_ns << " | "
           << value.jitter_p999_p50_ns << " | "
           << value.maximum_pause_ns << " |\n";
}

}

int run_benchmark(
    const RuntimeConfig& config,
    std::ostream& output,
    std::ostream& error)
{
    if (config.mode() != Mode::Benchmark) {
        error << "[error] benchmark mode is required\n";
        return 2;
    }
    const auto& settings = config.benchmark();
    std::vector<ReplayEvent> replay;
    std::string load_error;
    if (!load_replay(settings.input_path, replay, load_error)) {
        error << "[error] " << load_error << '\n';
        return 3;
    }
    if (replay.front().market.status != MarketDataStatus::Valid
        || replay.front().market.last_price_ticks <= 0
        || replay.front().market.last_price_ticks
            == std::numeric_limits<std::int64_t>::max()) {
        error << "[error] first replay event cannot establish a safe threshold\n";
        return 3;
    }
    const std::size_t account_count = settings.account_count == 0
        ? config.accounts().size() : settings.account_count;
    std::error_code directory_error;
    std::filesystem::create_directories(settings.output_path, directory_error);
    if (directory_error) {
        error << "[error] benchmark output directory cannot be created\n";
        return 3;
    }

    StrategyConfig strategy_config{};
    strategy_config.instrument = replay.front().market.instrument;
    strategy_config.threshold_ticks = replay.front().market.last_price_ticks + 1;
    strategy_config.protection_ticks = 1;
    strategy_config.max_market_age_ns = std::numeric_limits<std::int64_t>::max();
    strategy_config.max_signals_per_run = std::numeric_limits<std::uint32_t>::max();
    strategy_config.retrigger_below_count = 1;

    RiskLimits limits{};
    limits.allowed_instrument = strategy_config.instrument;
    limits.max_market_age_ns = std::numeric_limits<std::int64_t>::max();
    limits.max_slippage_ticks = 10;
    limits.margin_per_lot = 1;
    limits.max_daily_signals = std::numeric_limits<std::uint32_t>::max();
    limits.max_daily_orders = std::numeric_limits<std::uint32_t>::max();
    limits.max_daily_cancels = std::numeric_limits<std::uint32_t>::max();
    limits.max_active_open_orders = std::numeric_limits<std::int32_t>::max();
    limits.max_net_open_position = std::numeric_limits<std::int32_t>::max();

    constexpr std::uint64_t kMaximumEvents = 50'000'000;
    const std::uint64_t base_seconds =
        static_cast<std::uint64_t>(settings.warmup_seconds)
        + settings.duration_seconds;
    if ((settings.rate_per_second != 0
         && base_seconds > kMaximumEvents / settings.rate_per_second)
        || (settings.burst_rate_per_second != 0
            && settings.burst_seconds
                > kMaximumEvents / settings.burst_rate_per_second)) {
        error << "[error] benchmark event count is outside the safe range\n";
        return 3;
    }
    const std::uint64_t base_events = settings.rate_per_second == 0
        ? replay.size() * 1000
        : settings.rate_per_second * base_seconds;
    const std::uint64_t burst_events =
        settings.burst_rate_per_second * settings.burst_seconds;
    if (base_events > kMaximumEvents || burst_events > kMaximumEvents
        || base_events + burst_events == 0
        || base_events > kMaximumEvents - burst_events) {
        error << "[error] benchmark event count is outside the safe range\n";
        return 3;
    }
    constexpr std::uint64_t kSubmitStride = 1024;
    const auto session_capacity = static_cast<std::size_t>(
        (base_events + burst_events) / kSubmitStride + 32);
    std::vector<std::unique_ptr<BenchmarkAccount>> accounts;
    accounts.reserve(account_count);
    for (std::size_t index = 0; index < account_count; ++index) {
        accounts.push_back(std::make_unique<BenchmarkAccount>(
            config.accounts()[index], strategy_config, limits, session_capacity));
    }

    const auto output_directory = std::filesystem::path{settings.output_path};
    const auto raw_path = output_directory / "latency_raw.csv";
    AsyncPerformanceRecorder recorder{raw_path};
    if (!recorder.start()) {
        error << "[error] latency raw file cannot be opened\n";
        return 3;
    }
    std::ofstream cpu{output_directory / "cpu_raw.csv"};
    cpu << "point,mono_ns,process_cpu_ticks,benchmark_thread_cpu_ns,writer_thread_cpu_ns\n";
    const auto start_ns = steady_now_ns();
    const auto start_cpu = std::clock();
    cpu << "start," << start_ns << ',' << start_cpu << ','
        << thread_cpu_now_ns() << ",0\n";
    std::uint64_t sample_sequence = 0;
    std::uint64_t delivered_events = 0;
    const std::uint64_t warmup_events =
        settings.rate_per_second * settings.warmup_seconds;

    const auto run_events = [&](std::uint64_t count, std::uint64_t rate) {
        auto next = std::chrono::steady_clock::now();
        const auto interval = rate == 0
            ? std::chrono::nanoseconds{0}
            : std::chrono::nanoseconds{1'000'000'000LL
                / static_cast<std::int64_t>(rate)};
        for (std::uint64_t event_index = 0; event_index < count; ++event_index) {
            auto market = replay[delivered_events % replay.size()].market;
            market.market_seq = delivered_events + 1;
            market.recv_mono_ns = steady_now_ns();
            for (std::size_t account_index = 0;
                 account_index < accounts.size(); ++account_index) {
                auto& account = *accounts[account_index];
                const auto market_begin = steady_now_ns();
                const auto decision = account.strategy.on_market(
                    market, market.recv_mono_ns);
                const auto decision_end = steady_now_ns();
                const bool measured = delivered_events >= warmup_events;
                if (measured) recorder.try_record({
                    ++sample_sequence, decision_end,
                    decision_end - market_begin,
                    static_cast<std::uint32_t>(account_index),
                    PerformanceStage::MarketToSignal});
                if (!decision.has_intent) continue;
                ++account.signals;
                if ((account.signals - 1) % kSubmitStride != 0) continue;
                RiskSnapshot risk{};
                risk.enabled = true;
                risk.authenticated = true;
                risk.logged_in = true;
                risk.reconciled = true;
                risk.trading_window_open = true;
                risk.market_valid = true;
                risk.funds_known = true;
                risk.positions_known = true;
                risk.now_ns = market.recv_mono_ns;
                risk.market_receive_ns = market.recv_mono_ns;
                risk.bid_price_ticks = market.bid_price_ticks;
                risk.ask_price_ticks = market.ask_price_ticks;
                risk.available_funds = std::numeric_limits<std::int64_t>::max();
                const auto submit_begin = steady_now_ns();
                const auto submit = account.session.submit(decision.intent, risk);
                const auto submit_end = steady_now_ns();
                if (submit.code == SubmitCode::Submitted) ++account.submitted;
                else ++account.rejected;
                if (measured) recorder.try_record({
                    ++sample_sequence, submit_end, submit_end - submit_begin,
                    static_cast<std::uint32_t>(account_index),
                    PerformanceStage::SignalToOrderCall});
                const auto callback_begin = steady_now_ns();
                account.session.drain_callbacks();
                const auto callback_end = steady_now_ns();
                if (measured) {
                    recorder.try_record({
                        ++sample_sequence, callback_end,
                        callback_end - callback_begin,
                        static_cast<std::uint32_t>(account_index),
                        PerformanceStage::CallbackToState});
                    recorder.try_record({
                        ++sample_sequence, callback_end,
                        callback_end - market_begin,
                        static_cast<std::uint32_t>(account_index),
                        PerformanceStage::SimulatedEndToEnd});
                }
            }
            ++delivered_events;
            if (rate != 0) {
                next += interval;
                std::this_thread::sleep_until(next);
            }
        }
    };
    run_events(base_events, settings.rate_per_second);
    run_events(burst_events, settings.burst_rate_per_second);
    const auto finish_ns = steady_now_ns();
    recorder.stop();
    const auto queue = recorder.snapshot();
    cpu << "finish," << finish_ns << ',' << std::clock() << ','
        << thread_cpu_now_ns() << ',' << queue.writer_thread_cpu_ns << '\n';
    cpu.close();

    std::ofstream queue_output{output_directory / "queue_raw.csv"};
    queue_output << "mono_ns,account_index,queue,depth,high_watermark,dropped,oldest_backlog_ns\n";
    for (std::size_t index = 0; index < account_count; ++index) {
        queue_output << finish_ns << ',' << index << ",performance,"
                     << queue.depth << ',' << queue.high_watermark << ','
                     << queue.dropped << ",0\n";
    }
    queue_output.close();

    std::ofstream events{output_directory / "events_summary.json"};
    events << "{\n  \"delivered_market_events\": " << delivered_events
           << ",\n  \"accounts\": [\n";
    for (std::size_t index = 0; index < accounts.size(); ++index) {
        const auto& account = *accounts[index];
        events << "    {\"index\": " << index
               << ", \"signals\": " << account.signals
               << ", \"submitted\": " << account.submitted
               << ", \"rejected\": " << account.rejected
               << ", \"api_order_calls\": " << account.order_calls << "}"
               << (index + 1 == accounts.size() ? "\n" : ",\n");
    }
    events << "  ]\n}\n";
    events.close();

    const auto elapsed_ns = std::max<std::int64_t>(1, finish_ns - start_ns);
    utsname system{};
    uname(&system);
    cpu_set_t affinity{};
    CPU_ZERO(&affinity);
    const auto affinity_count = sched_getaffinity(
        0, sizeof(affinity), &affinity) == 0 ? CPU_COUNT(&affinity) : 0;
    const auto memory_bytes = static_cast<std::uint64_t>(
        std::max<long>(0, sysconf(_SC_PHYS_PAGES)))
        * static_cast<std::uint64_t>(
            std::max<long>(0, sysconf(_SC_PAGESIZE)));
    const auto input_sha256 = sha256_file(settings.input_path);
    std::ofstream manifest{output_directory / "manifest.json"};
    manifest << "{\n"
             << "  \"format_version\": 1,\n"
             << "  \"mode\": \"offline_replay\",\n"
             << "  \"accounts\": " << account_count << ",\n"
             << "  \"git_commit\": \"" << git_commit() << "\",\n"
             << "  \"build_type\": \"" << CTP_BUILD_TYPE << "\",\n"
             << "  \"compiler\": \"" << json_escape(__VERSION__) << "\",\n"
             << "  \"system\": \"" << json_escape(system.sysname) << ' '
             << json_escape(system.release) << ' ' << json_escape(system.machine)
             << "\",\n"
             << "  \"cpu_model\": \"" << json_escape(cpu_model()) << "\",\n"
             << "  \"hardware_threads\": "
             << std::thread::hardware_concurrency() << ",\n"
             << "  \"affinity_cpu_count\": " << affinity_count << ",\n"
             << "  \"memory_bytes\": " << memory_bytes << ",\n"
             << "  \"input\": \"" << json_escape(settings.input_path) << "\",\n"
             << "  \"input_sha256\": \"" << input_sha256 << "\",\n"
             << "  \"rate_per_second\": " << settings.rate_per_second << ",\n"
             << "  \"warmup_seconds\": " << settings.warmup_seconds << ",\n"
             << "  \"duration_seconds\": " << settings.duration_seconds << ",\n"
             << "  \"burst_rate_per_second\": "
             << settings.burst_rate_per_second << ",\n"
             << "  \"burst_seconds\": " << settings.burst_seconds << ",\n"
             << "  \"elapsed_ns\": " << elapsed_ns << ",\n"
             << "  \"market_events_per_second\": " << std::fixed
             << std::setprecision(2)
             << delivered_events * 1'000'000'000.0 / elapsed_ns << ",\n"
             << "  \"performance_samples_dropped\": " << queue.dropped << "\n"
             << "}\n";
    manifest.close();

    const auto raw = read_latency_samples(raw_path);
    std::ofstream report{output_directory / "report.md"};
    report << "# CTP 离线基准报告\n\n"
           << "该结果使用离线回放和柜台替身，不能代表真实 CTP 网络延迟。\n\n"
           << "| 链路 | 数量 | 最小 | P50 | P95 | P99 | P99.9 | 最大 | 平均 | 标准差 | P99-P50 | P99.9-P50 | 最大停顿 |\n"
           << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    constexpr std::string_view names[]{
        "行情到策略", "信号到报单调用", "回报到状态", "模拟端到端"};
    for (std::size_t index = 0; index < raw.size(); ++index) {
        write_statistics(report, names[index], compute_latency_statistics(raw[index]));
    }
    report << "\n- 行情吞吐：" << std::fixed << std::setprecision(2)
           << delivered_events * 1'000'000'000.0 / elapsed_ns << " 条/秒\n"
           << "- 性能采样队列最高水位：" << queue.high_watermark << "\n"
           << "- 性能采样丢弃：" << queue.dropped << "\n";
    report.close();

    std::ofstream reproduce{output_directory / "reproduce.sh"};
    reproduce << "#!/usr/bin/env bash\nset -euo pipefail\n"
              << "./build/ctp_client benchmark --config "
              << shell_quote(settings.config_path)
              << " --input " << shell_quote(settings.input_path) << " --output "
              << shell_quote(settings.output_path) << " --accounts " << account_count
              << " --rate " << settings.rate_per_second
              << " --warmup-seconds " << settings.warmup_seconds
              << " --duration-seconds " << settings.duration_seconds;
    if (settings.burst_seconds != 0) {
        reproduce << " --burst-rate " << settings.burst_rate_per_second
                  << " --burst-seconds " << settings.burst_seconds;
    }
    reproduce << '\n';
    reproduce.close();
    std::filesystem::permissions(
        output_directory / "reproduce.sh",
        std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write
            | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace,
        directory_error);
    constexpr std::string_view evidence_files[]{
        "manifest.json", "latency_raw.csv", "queue_raw.csv", "cpu_raw.csv",
        "events_summary.json", "report.md", "reproduce.sh"};
    std::ofstream checksums{output_directory / "SHA256SUMS"};
    for (const auto name : evidence_files) {
        checksums << sha256_file(output_directory / name) << "  " << name << '\n';
    }
    output << "[ok] benchmark evidence: " << settings.output_path << '\n';
    return 0;
}

}
