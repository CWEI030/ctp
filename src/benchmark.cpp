#include "ctp/telemetry.hpp"

#include "ctp/field.hpp"
#include "ctp/strategy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <cstdio>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include <sched.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace ctp {
namespace {

std::int64_t steady_now_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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

struct RuntimeQueueSnapshot {
    std::size_t depth{0};
    std::size_t capacity{0};
    std::size_t high_watermark{0};
    std::int64_t oldest_age_ns{0};
    std::uint64_t dropped{0};
};

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value) noexcept;

struct OfflineRequest {
    CThostFtdcInputOrderField order{};
};

// 柜台替身也通过独立线程回调，避免把同步函数调用误测成交易回报链路。
class OfflineTraderApi final : public TraderApi {
public:
    OfflineTraderApi()
    {
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { run(); });
    }
    ~OfflineTraderApi() override { release(); }

    void register_spi(CThostFtdcTraderSpi* spi) override
    {
        spi_.store(spi, std::memory_order_release);
    }
    void subscribe_private_topic(THOST_TE_RESUME_TYPE, int) override {}
    void subscribe_public_topic(THOST_TE_RESUME_TYPE) override {}
    void register_front(const std::string&) override {}
    void init() override {}
    int request_authenticate(CThostFtdcReqAuthenticateField*, int) override { return 0; }
    int request_user_login(CThostFtdcReqUserLoginField*, int) override { return 0; }
    int request_settlement_query(CThostFtdcQrySettlementInfoConfirmField*, int) override { return 0; }
    int request_settlement_confirmation(CThostFtdcSettlementInfoConfirmField*, int) override { return 0; }
    int request_trading_account(CThostFtdcQryTradingAccountField*, int) override { return 0; }
    int request_investor_position(CThostFtdcQryInvestorPositionField*, int) override { return 0; }
    int request_order_insert(CThostFtdcInputOrderField* request, int) override
    {
        order_calls_.fetch_add(1, std::memory_order_relaxed);
        OfflineRequest queued{};
        queued.order = *request;
        outstanding_.fetch_add(1, std::memory_order_release);
        if (!requests_.try_push(queued)) {
            requests_.record_drop();
            outstanding_.fetch_sub(1, std::memory_order_release);
            return -1;
        }
        return 0;
    }
    int request_order_action(CThostFtdcInputOrderActionField*, int) override { return 0; }
    int request_order_query(CThostFtdcQryOrderField*, int) override { return 0; }
    int request_trade_query(CThostFtdcQryTradeField*, int) override { return 0; }
    void release() override
    {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        spi_.store(nullptr, std::memory_order_release);
    }

    RuntimeQueueSnapshot queue_snapshot() const noexcept
    {
        return {requests_.depth(), requests_.capacity(),
                requests_.high_watermark(),
                requests_.oldest_age_ns(steady_now_ns()),
                requests_.dropped_count()};
    }
    std::uint64_t order_calls() const noexcept
    {
        return order_calls_.load(std::memory_order_relaxed);
    }
    std::int32_t worker_tid() const noexcept
    {
        return worker_tid_.load(std::memory_order_acquire);
    }
    std::uint64_t outstanding() const noexcept
    {
        return outstanding_.load(std::memory_order_acquire);
    }

private:
    void run()
    {
        worker_tid_.store(
            static_cast<std::int32_t>(::syscall(SYS_gettid)),
            std::memory_order_release);
        OfflineRequest request{};
        std::uint64_t trade_sequence = 0;
        while (running_.load(std::memory_order_acquire)
               || requests_.depth() != 0) {
            if (!requests_.try_pop(request)) {
                std::this_thread::yield();
                continue;
            }
            auto* spi = spi_.load(std::memory_order_acquire);
            if (spi == nullptr) {
                outstanding_.fetch_sub(1, std::memory_order_release);
                continue;
            }
            CThostFtdcOrderField order{};
            copy_to_field(order.OrderRef, std::string_view{request.order.OrderRef});
            order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
            order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
            order.VolumeTotal = request.order.VolumeTotalOriginal;
            spi->OnRtnOrder(&order);

            CThostFtdcTradeField trade{};
            copy_to_field(trade.OrderRef, std::string_view{request.order.OrderRef});
            copy_to_field(trade.TradingDay, "20260909");
            copy_to_field(trade.ExchangeID, "SIM");
            copy_to_field(trade.InstrumentID,
                          std::string_view{request.order.InstrumentID});
            std::snprintf(trade.TradeID, sizeof(trade.TradeID), "%llu",
                          static_cast<unsigned long long>(++trade_sequence));
            trade.Direction = request.order.Direction;
            trade.OffsetFlag = request.order.CombOffsetFlag[0];
            trade.Volume = request.order.VolumeTotalOriginal;
            spi->OnRtnTrade(&trade);
            outstanding_.fetch_sub(1, std::memory_order_release);
        }
    }

    SpscQueue<OfflineRequest, 1024> requests_;
    std::atomic<CThostFtdcTraderSpi*> spi_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> order_calls_{0};
    std::atomic<std::uint64_t> outstanding_{0};
    std::atomic<std::int32_t> worker_tid_{0};
    std::thread worker_;
};

struct BenchmarkAccount {
    struct PendingOrder {
        std::uint64_t client_order_id{0};
        std::int64_t market_receive_ns{0};
        bool measured{false};
    };

    BenchmarkAccount(
        const AccountConfig& account,
        const StrategyConfig& strategy_config,
        RiskLimits limits,
        std::size_t capacity,
        std::uint32_t submit_stride_value,
        std::size_t index,
        const std::filesystem::path& output_path)
        : account_index(index),
          trace(output_path / ("trace_account_" + std::to_string(index) + ".csv"),
                account.alias()),
          recorder(output_path / ("latency_account_" + std::to_string(index) + ".csv")),
          strategy(strategy_config),
          session(
              account,
              limits,
              make_api(api),
              capacity,
              capacity,
              capacity,
              1024,
              1,
              1,
              {},
              &trace,
              index + 1)
    {
        submit_stride = submit_stride_value;
        pending_orders.reserve(capacity);
        started = trace.start() && recorder.start();
        session.activate(1, 1, "0", "20260909");
    }

    static std::unique_ptr<TraderApi> make_api(OfflineTraderApi*& pointer)
    {
        auto api = std::make_unique<OfflineTraderApi>();
        pointer = api.get();
        return api;
    }

    void run(MarketIngress& ingress, const std::atomic<bool>& producer_done,
             std::uint64_t warmup_events)
    {
        worker_tid.store(static_cast<std::int32_t>(::syscall(SYS_gettid)),
                         std::memory_order_release);
        MarketEvent market{};
        std::uint64_t sample_sequence = 0;
        while (!producer_done.load(std::memory_order_acquire)
               || ingress.snapshot(account_index).depth != 0
               || api->outstanding() != 0
               || session.callback_queue_snapshot().depth != 0) {
            const bool callback_measured = pending_head < pending_orders.size()
                && pending_orders[pending_head].measured;
            const auto callback_begin = steady_now_ns();
            const auto callbacks = session.drain_callbacks(1);
            const auto callback_end = steady_now_ns();
            if (callbacks != 0 && callback_measured) {
                recorder.try_record({++sample_sequence, callback_end,
                    callback_end - callback_begin,
                    static_cast<std::uint32_t>(account_index),
                    PerformanceStage::CallbackToState});
            }
            if (callbacks != 0 && pending_head < pending_orders.size()) {
                OrderSnapshot snapshot{};
                const auto& pending = pending_orders[pending_head];
                if (session.order_snapshot(pending.client_order_id, snapshot)
                    && snapshot.state == OrderState::Filled) {
                    if (pending.measured) {
                        recorder.try_record({++sample_sequence, callback_end,
                            callback_end - pending.market_receive_ns,
                            static_cast<std::uint32_t>(account_index),
                            PerformanceStage::SimulatedEndToEnd});
                    }
                    ++pending_head;
                }
            }
            if (!ingress.try_pop(account_index, market)) {
                std::this_thread::yield();
                continue;
            }
            const auto current = processed.fetch_add(1, std::memory_order_relaxed) + 1;
            session.trace_market(market);
            const auto begin = steady_now_ns();
            const auto decision = strategy.on_market(market, market.recv_mono_ns);
            const auto decision_end = steady_now_ns();
            const bool measured = current > warmup_events;
            if (measured) recorder.try_record({++sample_sequence, decision_end,
                decision_end - begin, static_cast<std::uint32_t>(account_index),
                PerformanceStage::MarketToSignal});
            if (!decision.has_intent) continue;
            const auto signal_count = signals.fetch_add(1, std::memory_order_relaxed) + 1;
            session.trace_signal(market, decision.intent, decision_end);
            if ((signal_count - 1) % submit_stride != 0) continue;
            RiskSnapshot risk{};
            risk.enabled = risk.authenticated = risk.logged_in = true;
            risk.reconciled = risk.trading_window_open = risk.market_valid = true;
            risk.funds_known = risk.positions_known = true;
            risk.now_ns = risk.market_receive_ns = market.recv_mono_ns;
            risk.bid_price_ticks = market.bid_price_ticks;
            risk.ask_price_ticks = market.ask_price_ticks;
            risk.available_funds = std::numeric_limits<std::int64_t>::max();
            const auto submit_begin = steady_now_ns();
            const auto submit = session.submit(decision.intent, risk);
            const auto submit_end = steady_now_ns();
            if (submit.code == SubmitCode::Submitted) {
                submitted.fetch_add(1, std::memory_order_relaxed);
                pending_orders.push_back({
                    submit.client_order_id, market.recv_mono_ns, measured});
            } else {
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
            if (measured) {
                recorder.try_record({++sample_sequence, submit_end,
                    submit_end - submit_begin,
                    static_cast<std::uint32_t>(account_index),
                    PerformanceStage::SignalToOrderCall});
            }
        }
        session.drain_callbacks();
    }

    void stop_writers()
    {
        trace.stop();
        recorder.stop();
    }

    std::size_t account_index{0};
    OfflineTraderApi* api{nullptr};
    AsyncTraceJournal trace;
    AsyncPerformanceRecorder recorder;
    ThresholdStrategy strategy;
    AccountTradingSession session;
    bool started{false};
    std::atomic<std::uint64_t> processed{0};
    std::atomic<std::uint64_t> signals{0};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::int32_t> worker_tid{0};
    std::uint32_t submit_stride{1024};
    std::vector<PendingOrder> pending_orders;
    std::size_t pending_head{0};
    std::thread worker;
};

struct CpuPoint {
    std::uint64_t user_ns{0};
    std::uint64_t system_ns{0};
    std::uint64_t voluntary_switches{0};
    std::uint64_t nonvoluntary_switches{0};
};

bool read_cpu_point(std::int32_t tid, bool process, CpuPoint& point)
{
    if (tid <= 0) return false;
    const auto base = process
        ? std::filesystem::path{"/proc/self"}
        : std::filesystem::path{"/proc/self/task"} / std::to_string(tid);
    std::ifstream stat{base / "stat"};
    std::string line;
    if (!std::getline(stat, line)) return false;
    const auto end_name = line.rfind(')');
    if (end_name == std::string::npos || end_name + 2 >= line.size()) return false;
    std::istringstream fields{line.substr(end_name + 2)};
    std::string value;
    std::vector<std::string> columns;
    while (fields >> value) columns.push_back(value);
    // 去掉 pid/comm 后，数组下标 11、12 对应 /proc stat 的 utime、stime。
    if (columns.size() <= 12) return false;
    std::uint64_t user_ticks = 0;
    std::uint64_t system_ticks = 0;
    if (!parse_integer(columns[11], user_ticks)
        || !parse_integer(columns[12], system_ticks)) return false;
    const auto ticks_per_second = std::max<long>(1, sysconf(_SC_CLK_TCK));
    point.user_ns = user_ticks * 1'000'000'000ULL / ticks_per_second;
    point.system_ns = system_ticks * 1'000'000'000ULL / ticks_per_second;

    std::ifstream status{base / "status"};
    while (std::getline(status, line)) {
        constexpr std::string_view voluntary{"voluntary_ctxt_switches:"};
        constexpr std::string_view involuntary{"nonvoluntary_ctxt_switches:"};
        if (line.rfind(voluntary, 0) == 0) {
            parse_integer(std::string_view{line}.substr(voluntary.size() + 1),
                          point.voluntary_switches);
        } else if (line.rfind(involuntary, 0) == 0) {
            parse_integer(std::string_view{line}.substr(involuntary.size() + 1),
                          point.nonvoluntary_switches);
        }
    }
    return true;
}

// 采样线程属于控制面；它只读取原子快照，不进入账户实时处理热路径。
class BenchmarkSampler {
public:
    BenchmarkSampler(MarketIngress& ingress,
                     std::vector<std::unique_ptr<BenchmarkAccount>>& accounts,
                     const std::filesystem::path& output_path,
                     const std::atomic<std::int32_t>& producer_tid)
        : ingress_(ingress), accounts_(accounts), producer_tid_(producer_tid),
          queue_(output_path / "queue_raw.csv"),
          cpu_(output_path / "cpu_raw.csv")
    {
        queue_ << "mono_ns,account_index,queue,depth,capacity,high_watermark,oldest_age_ns,dropped\n";
        cpu_ << "sample_mono_ns,scope,account_index,role,tid,user_cpu_ns,system_cpu_ns,voluntary_context_switches,nonvoluntary_context_switches\n";
        cpu_rows_.reserve(3 + accounts_.size() * 4);
    }

    bool good() const noexcept { return queue_.good() && cpu_.good(); }

    bool start()
    {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] {
            sampler_tid_.store(static_cast<std::int32_t>(::syscall(SYS_gettid)),
                               std::memory_order_release);
            while (running_.load(std::memory_order_acquire)) {
                if (sample()) publish_complete_sample();
                std::unique_lock lock{sample_mutex_};
                sample_condition_.wait_for(lock, std::chrono::milliseconds{100},
                    [this] { return !running_.load(std::memory_order_acquire); });
            }
            if (sample()) publish_complete_sample();
        });

        // 短基准也必须拥有可比较的时间序列；基准计时从两个完整基线样本之后开始。
        std::unique_lock lock{sample_mutex_};
        return sample_condition_.wait_for(lock, std::chrono::seconds{5},
            [this] { return complete_samples_ >= 2; });
    }

    void stop()
    {
        running_.store(false, std::memory_order_release);
        sample_condition_.notify_all();
        if (thread_.joinable()) thread_.join();
        queue_.flush();
        cpu_.flush();
    }

private:
    struct CpuSampleRow {
        std::string_view scope;
        std::int64_t account;
        std::string_view role;
        std::int32_t tid;
        CpuPoint point;
    };

    void queue_row(std::int64_t now, std::size_t index, std::string_view name,
                   const RuntimeQueueSnapshot& snapshot)
    {
        queue_ << now << ',' << index << ',' << name << ',' << snapshot.depth
               << ',' << snapshot.capacity << ',' << snapshot.high_watermark
               << ',' << snapshot.oldest_age_ns << ',' << snapshot.dropped << '\n';
    }

    static bool capture_cpu_row(std::vector<CpuSampleRow>& rows,
                                std::string_view scope, std::int64_t account,
                                std::string_view role, std::int32_t tid)
    {
        CpuPoint point{};
        if (!read_cpu_point(tid, scope == "process", point)) return false;
        rows.push_back({scope, account, role, tid, point});
        return true;
    }

    void cpu_row(std::int64_t now, const CpuSampleRow& row)
    {
        cpu_ << now << ',' << row.scope << ',' << row.account << ',' << row.role
             << ',' << row.tid << ',' << row.point.user_ns << ','
             << row.point.system_ns << ',' << row.point.voluntary_switches << ','
             << row.point.nonvoluntary_switches << '\n';
    }

    bool sample()
    {
        const auto now = steady_now_ns();
        cpu_rows_.clear();
        bool complete = capture_cpu_row(cpu_rows_, "process", -1, "process",
            static_cast<std::int32_t>(getpid()));
        complete = capture_cpu_row(cpu_rows_, "thread", -1, "producer",
            producer_tid_.load(std::memory_order_acquire)) && complete;
        complete = capture_cpu_row(cpu_rows_, "thread", -1, "sampler",
            sampler_tid_.load(std::memory_order_acquire)) && complete;
        for (std::size_t index = 0; index < accounts_.size(); ++index) {
            auto& account = *accounts_[index];
            const auto trace = account.trace.snapshot();
            const auto latency = account.recorder.snapshot();
            complete = capture_cpu_row(cpu_rows_, "thread", index, "account_worker",
                account.worker_tid.load(std::memory_order_acquire)) && complete;
            complete = capture_cpu_row(cpu_rows_, "thread", index, "trader_callback",
                account.api->worker_tid()) && complete;
            complete = capture_cpu_row(cpu_rows_, "thread", index, "trace_writer",
                trace.writer_tid) && complete;
            complete = capture_cpu_row(cpu_rows_, "thread", index, "latency_writer",
                latency.writer_tid) && complete;
        }
        if (!complete) return false;

        std::size_t cpu_index = 0;
        cpu_row(now, cpu_rows_[cpu_index++]);
        cpu_row(now, cpu_rows_[cpu_index++]);
        cpu_row(now, cpu_rows_[cpu_index++]);
        for (std::size_t index = 0; index < accounts_.size(); ++index) {
            auto& account = *accounts_[index];
            const auto market = ingress_.snapshot(index);
            queue_row(now, index, "market_ingress",
                {market.depth, market.capacity, market.high_watermark,
                 market.oldest_age_ns, market.dropped});
            const auto api = account.api->queue_snapshot();
            queue_row(now, index, "trader_request", api);
            const auto callback = account.session.callback_queue_snapshot();
            queue_row(now, index, "trading_callback",
                {callback.depth, callback.capacity, callback.high_watermark,
                 callback.oldest_age_ns, callback.dropped});
            const auto trace = account.trace.snapshot();
            queue_row(now, index, "trace",
                {trace.depth, trace.capacity, trace.high_watermark,
                 trace.oldest_age_ns, trace.dropped});
            const auto latency = account.recorder.snapshot();
            queue_row(now, index, "latency",
                {latency.depth, latency.capacity, latency.high_watermark,
                 latency.oldest_age_ns, latency.dropped});

            cpu_row(now, cpu_rows_[cpu_index++]);
            cpu_row(now, cpu_rows_[cpu_index++]);
            cpu_row(now, cpu_rows_[cpu_index++]);
            cpu_row(now, cpu_rows_[cpu_index++]);
        }
        return queue_.good() && cpu_.good();
    }

    void publish_complete_sample()
    {
        {
            std::lock_guard lock{sample_mutex_};
            ++complete_samples_;
        }
        sample_condition_.notify_all();
    }

    MarketIngress& ingress_;
    std::vector<std::unique_ptr<BenchmarkAccount>>& accounts_;
    const std::atomic<std::int32_t>& producer_tid_;
    std::ofstream queue_;
    std::ofstream cpu_;
    std::atomic<bool> running_{false};
    std::atomic<std::int32_t> sampler_tid_{0};
    std::mutex sample_mutex_;
    std::condition_variable sample_condition_;
    std::size_t complete_samples_{0};
    std::vector<CpuSampleRow> cpu_rows_;
    std::thread thread_;
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
           << value.maximum_pause_ns << " | ";
    if (value.count < 200) output << "样本不足：P95";
    else if (value.count < 1000) output << "样本不足：P99";
    else if (value.count < 10000) output << "样本不足：P99.9";
    else output << "P95/P99/P99.9 样本充足";
    output << " |\n";
}

std::vector<std::string_view> split_csv(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto comma = line.find(',', begin);
        fields.push_back(line.substr(begin, comma == std::string_view::npos
            ? line.size() - begin : comma - begin));
        if (comma == std::string_view::npos) break;
        begin = comma + 1;
    }
    return fields;
}

struct QueueSummary {
    std::uint64_t maximum_depth{0};
    std::uint64_t maximum_high_watermark{0};
    std::uint64_t maximum_oldest_age_ns{0};
    std::uint64_t maximum_dropped{0};
    std::int64_t first_drop_mono_ns{-1};
};

using QueueKey = std::pair<std::string, std::size_t>;

std::map<QueueKey, QueueSummary> read_queue_summaries(
    const std::filesystem::path& path)
{
    std::map<QueueKey, QueueSummary> result;
    std::ifstream input{path};
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        const auto fields = split_csv(line);
        if (fields.size() != 8) continue;
        std::int64_t mono_ns = 0;
        std::size_t account = 0;
        std::uint64_t depth = 0, high_watermark = 0, oldest = 0, dropped = 0;
        if (!parse_integer(fields[0], mono_ns)
            || !parse_integer(fields[1], account)
            || !parse_integer(fields[3], depth)
            || !parse_integer(fields[5], high_watermark)
            || !parse_integer(fields[6], oldest)
            || !parse_integer(fields[7], dropped)) continue;
        auto& summary = result[{std::string{fields[2]}, account}];
        summary.maximum_depth = std::max(summary.maximum_depth, depth);
        summary.maximum_high_watermark = std::max(
            summary.maximum_high_watermark, high_watermark);
        summary.maximum_oldest_age_ns = std::max(
            summary.maximum_oldest_age_ns, oldest);
        summary.maximum_dropped = std::max(summary.maximum_dropped, dropped);
        if (dropped != 0 && summary.first_drop_mono_ns < 0) {
            summary.first_drop_mono_ns = mono_ns;
        }
    }
    return result;
}

struct CpuSummary {
    std::string scope;
    std::int64_t account{-1};
    std::string role;
    std::int32_t tid{0};
    std::uint64_t first_mono_ns{0};
    std::uint64_t last_mono_ns{0};
    CpuPoint first{};
    CpuPoint last{};
    double peak_percent{0.0};
    bool initialized{false};
};

std::map<std::string, CpuSummary> read_cpu_summaries(
    const std::filesystem::path& path)
{
    std::map<std::string, CpuSummary> result;
    std::ifstream input{path};
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        const auto fields = split_csv(line);
        if (fields.size() != 9) continue;
        std::uint64_t mono_ns = 0;
        std::int64_t account = -1;
        std::int32_t tid = 0;
        CpuPoint point{};
        if (!parse_integer(fields[0], mono_ns)
            || !parse_integer(fields[2], account)
            || !parse_integer(fields[4], tid)
            || !parse_integer(fields[5], point.user_ns)
            || !parse_integer(fields[6], point.system_ns)
            || !parse_integer(fields[7], point.voluntary_switches)
            || !parse_integer(fields[8], point.nonvoluntary_switches)) continue;
        const std::string key = std::string{fields[1]} + ":"
            + std::to_string(account) + ":" + std::string{fields[3]} + ":"
            + std::to_string(tid);
        auto& summary = result[key];
        if (!summary.initialized) {
            summary.scope = fields[1];
            summary.account = account;
            summary.role = fields[3];
            summary.tid = tid;
            summary.first_mono_ns = mono_ns;
            summary.first = point;
            summary.initialized = true;
        } else if (mono_ns > summary.last_mono_ns) {
            const auto wall = mono_ns - summary.last_mono_ns;
            const auto cpu = point.user_ns + point.system_ns
                - summary.last.user_ns - summary.last.system_ns;
            summary.peak_percent = std::max(
                summary.peak_percent, cpu * 100.0 / wall);
        }
        summary.last_mono_ns = mono_ns;
        summary.last = point;
    }
    return result;
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
    const auto session_capacity = static_cast<std::size_t>(
        (base_events + burst_events) / settings.submit_stride + 32);
    const auto output_directory = std::filesystem::path{settings.output_path};
    std::vector<std::unique_ptr<BenchmarkAccount>> accounts;
    accounts.reserve(account_count);
    for (std::size_t index = 0; index < account_count; ++index) {
        accounts.push_back(std::make_unique<BenchmarkAccount>(
            config.accounts()[index], strategy_config, limits, session_capacity,
            settings.submit_stride, index, output_directory));
        if (!accounts.back()->started) {
            error << "[error] account evidence writer cannot be opened\n";
            return 3;
        }
    }

    const auto raw_path = output_directory / "latency_raw.csv";
    std::vector<AccountConfig> benchmark_configs;
    benchmark_configs.reserve(account_count);
    for (std::size_t index = 0; index < account_count; ++index) {
        benchmark_configs.push_back(config.accounts()[index]);
    }
    MarketIngress ingress{benchmark_configs, 1.0};
    ingress.start();
    std::atomic<bool> producer_done{false};
    std::atomic<std::int32_t> producer_tid{
        static_cast<std::int32_t>(::syscall(SYS_gettid))};
    const std::uint64_t warmup_events =
        settings.rate_per_second * settings.warmup_seconds;
    for (auto& account : accounts) {
        account->worker = std::thread([&ingress, &producer_done, warmup_events,
                                       pointer = account.get()] {
            pointer->run(ingress, producer_done, warmup_events);
        });
    }
    BenchmarkSampler sampler{ingress, accounts, output_directory, producer_tid};
    if (!sampler.good()) {
        producer_done.store(true, std::memory_order_release);
        for (auto& account : accounts) if (account->worker.joinable()) account->worker.join();
        error << "[error] benchmark sampler output cannot be opened\n";
        return 3;
    }
    if (!sampler.start()) {
        sampler.stop();
        producer_done.store(true, std::memory_order_release);
        for (auto& account : accounts) {
            if (account->worker.joinable()) account->worker.join();
        }
        ingress.stop();
        error << "[error] benchmark sampler could not collect two complete samples\n";
        return 3;
    }
    const auto start_ns = steady_now_ns();
    std::uint64_t delivered_events = 0;

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
            ingress.publish(market);
            ++delivered_events;
            if (rate != 0) {
                next += interval;
                std::this_thread::sleep_until(next);
            }
        }
    };
    run_events(base_events, settings.rate_per_second);
    run_events(burst_events, settings.burst_rate_per_second);
    producer_done.store(true, std::memory_order_release);
    for (auto& account : accounts) {
        if (account->worker.joinable()) account->worker.join();
    }
    const auto finish_ns = steady_now_ns();
    sampler.stop();
    ingress.stop();
    std::uint64_t performance_dropped = 0;
    for (auto& account : accounts) {
        account->stop_writers();
        const auto snapshot = account->recorder.snapshot();
        performance_dropped += snapshot.dropped;
    }

    // 各账户独占 SPSC 延迟队列；完成后在控制面合并为约定的原始证据文件。
    std::ofstream raw_output{raw_path};
    raw_output << "sequence,mono_ns,account_index,stage,latency_ns\n";
    for (std::size_t index = 0; index < account_count; ++index) {
        std::ifstream shard{output_directory
            / ("latency_account_" + std::to_string(index) + ".csv")};
        std::string line;
        std::getline(shard, line);
        while (std::getline(shard, line)) raw_output << line << '\n';
    }
    raw_output.close();

    std::ofstream events{output_directory / "events_summary.json"};
    events << "{\n  \"delivered_market_events\": " << delivered_events
           << ",\n  \"accounts\": [\n";
    for (std::size_t index = 0; index < accounts.size(); ++index) {
        const auto& account = *accounts[index];
        const auto market = ingress.snapshot(index);
        const auto lifecycle = account.session.event_snapshot();
        events << "    {\"index\": " << index
               << ", \"market_events_enqueued\": "
               << delivered_events - market.dropped
               << ", \"market_events_processed\": " << account.processed.load()
               << ", \"signals\": " << account.signals.load()
               << ", \"submitted\": " << account.submitted.load()
               << ", \"rejected\": " << account.rejected.load()
               << ", \"api_order_calls\": " << account.api->order_calls()
               << ", \"order_requests\": " << lifecycle.order_requests
               << ", \"order_acceptances\": " << lifecycle.order_acceptances
               << ", \"order_rejections\": " << lifecycle.order_rejections
               << ", \"cancel_requests\": " << lifecycle.cancel_requests
               << ", \"cancel_acceptances\": " << lifecycle.cancel_acceptances
               << ", \"trades\": " << lifecycle.trades
               << ", \"traded_volume\": " << lifecycle.traded_volume
               << ", \"freeze_transitions\": " << lifecycle.freeze_transitions
               << ", \"recovery_completions\": " << lifecycle.recovery_completions
               << "}"
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
             << "  \"submit_stride\": " << settings.submit_stride << ",\n"
             << "  \"burst_rate_per_second\": "
             << settings.burst_rate_per_second << ",\n"
             << "  \"burst_seconds\": " << settings.burst_seconds << ",\n"
             << "  \"elapsed_ns\": " << elapsed_ns << ",\n"
             << "  \"market_events_per_second\": " << std::fixed
             << std::setprecision(2)
             << delivered_events * 1'000'000'000.0 / elapsed_ns << ",\n"
             << "  \"performance_samples_dropped\": "
             << performance_dropped << "\n"
             << "}\n";
    manifest.close();

    const auto raw = read_latency_samples(raw_path);
    const auto queue_summaries = read_queue_summaries(
        output_directory / "queue_raw.csv");
    const auto cpu_summaries = read_cpu_summaries(
        output_directory / "cpu_raw.csv");
    std::ofstream report{output_directory / "report.md"};
    report << "# CTP 离线基准报告\n\n"
           << "该结果使用离线回放和柜台替身，不能代表真实 CTP 网络延迟。\n\n"
           << "| 链路 | 数量 | 最小 | P50 | P95 | P99 | P99.9 | 最大 | 平均 | 标准差 | P99-P50 | P99.9-P50 | 最大停顿 | 尾分位可信度 |\n"
           << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|\n";
    constexpr std::string_view names[]{
        "行情到策略", "信号到报单调用", "回报到状态", "模拟端到端"};
    for (std::size_t index = 0; index < raw.size(); ++index) {
        write_statistics(report, names[index], compute_latency_statistics(raw[index]));
    }
    report << "\n## 账户吞吐与生命周期\n\n"
           << "| 账户 | 行情处理/秒 | 信号 | 提交 | 本地拒绝 | 接受 | 撤单请求 | 撤单接受 | 成交 | 冻结 | 恢复 |\n"
           << "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (std::size_t index = 0; index < accounts.size(); ++index) {
        const auto& account = *accounts[index];
        const auto lifecycle = account.session.event_snapshot();
        report << "| " << index << " | " << std::fixed << std::setprecision(2)
               << account.processed.load() * 1'000'000'000.0 / elapsed_ns
               << " | " << account.signals.load()
               << " | " << account.submitted.load()
               << " | " << account.rejected.load()
               << " | " << lifecycle.order_acceptances
               << " | " << lifecycle.cancel_requests
               << " | " << lifecycle.cancel_acceptances
               << " | " << lifecycle.trades
               << " | " << lifecycle.freeze_transitions
               << " | " << lifecycle.recovery_completions << " |\n";
    }

    report << "\n## CPU 使用情况\n\n"
           << "利用率按单个逻辑核等效值计算；进程值可超过 100%。上下文切换为采样首尾差值。\n\n"
           << "| 范围 | 账户 | 线程角色 | TID | 用户态 ms | 内核态 ms | 平均利用率 % | 峰值利用率 % | 主动上下文切换 | 被动上下文切换 |\n"
           << "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& [key, cpu] : cpu_summaries) {
        (void)key;
        const auto wall = cpu.last_mono_ns > cpu.first_mono_ns
            ? cpu.last_mono_ns - cpu.first_mono_ns : 0;
        const auto user = cpu.last.user_ns >= cpu.first.user_ns
            ? cpu.last.user_ns - cpu.first.user_ns : 0;
        const auto system_ns = cpu.last.system_ns >= cpu.first.system_ns
            ? cpu.last.system_ns - cpu.first.system_ns : 0;
        const auto voluntary = cpu.last.voluntary_switches
                >= cpu.first.voluntary_switches
            ? cpu.last.voluntary_switches - cpu.first.voluntary_switches : 0;
        const auto involuntary = cpu.last.nonvoluntary_switches
                >= cpu.first.nonvoluntary_switches
            ? cpu.last.nonvoluntary_switches
                - cpu.first.nonvoluntary_switches : 0;
        report << "| " << cpu.scope << " | " << cpu.account << " | "
               << cpu.role << " | " << cpu.tid << " | " << std::fixed
               << std::setprecision(2) << user / 1'000'000.0 << " | "
               << system_ns / 1'000'000.0 << " | "
               << (wall == 0 ? 0.0 : (user + system_ns) * 100.0 / wall)
               << " | " << cpu.peak_percent << " | " << voluntary << " | "
               << involuntary << " |\n";
    }

    report << "\n## 运行队列汇总\n\n"
           << "| 队列 | 账户 | 最大深度 | 最高水位 | 最大最旧事件年龄 ns | 最大丢弃 | 首次丢弃 mono_ns |\n"
           << "|---|---:|---:|---:|---:|---:|---:|\n";
    for (const auto& [key, queue] : queue_summaries) {
        report << "| " << key.first << " | " << key.second << " | "
               << queue.maximum_depth << " | " << queue.maximum_high_watermark
               << " | " << queue.maximum_oldest_age_ns << " | "
               << queue.maximum_dropped << " | ";
        if (queue.first_drop_mono_ns < 0) report << "无";
        else report << queue.first_drop_mono_ns;
        report << " |\n";
    }
    report << "\n- 性能样本丢弃合计：" << performance_dropped << "\n";
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
    reproduce << " --submit-stride " << settings.submit_stride;
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
