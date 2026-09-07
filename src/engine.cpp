#include "ctp/engine.hpp"

#include "ctp/field.hpp"
#include "ctp/market_client.hpp"
#include "ctp/strategy.hpp"
#include "ctp/telemetry.hpp"
#include "ctp/trader_client.hpp"
#include "ctp/trading.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

namespace ctp {
namespace {

bool has_tcp_front(std::string_view value) noexcept
{
    return value.size() > 6 && value.substr(0, 6) == "tcp://";
}

bool copy_instrument(
    const char (&source)[kInstrumentIdCapacity],
    std::array<char, kInstrumentIdCapacity>& destination) noexcept
{
    for (std::size_t index = 0; index < destination.size(); ++index) {
        destination[index] = source[index];
        if (source[index] == '\0') {
            return index != 0;
        }
    }
    destination.back() = '\0';
    return false;
}

bool parse_exchange_time_ms(
    const char* update_time,
    int update_millisec,
    std::int64_t& result) noexcept
{
    const auto digit = [update_time](std::size_t index) {
        return update_time[index] >= '0' && update_time[index] <= '9';
    };
    if (!digit(0) || !digit(1) || update_time[2] != ':'
        || !digit(3) || !digit(4) || update_time[5] != ':'
        || !digit(6) || !digit(7) || update_time[8] != '\0'
        || update_millisec < 0 || update_millisec > 999) {
        return false;
    }

    const int hour = (update_time[0] - '0') * 10 + update_time[1] - '0';
    const int minute = (update_time[3] - '0') * 10 + update_time[4] - '0';
    const int second = (update_time[6] - '0') * 10 + update_time[7] - '0';
    if (hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    result = ((hour * 60LL + minute) * 60LL + second) * 1000LL
             + update_millisec;
    return true;
}

bool price_to_ticks(
    double price,
    double minimum_price_increment,
    std::int64_t& result) noexcept
{
    if (!std::isfinite(price) || price <= 0
        || !std::isfinite(minimum_price_increment)
        || minimum_price_increment <= 0) {
        return false;
    }

    const long double ratio =
        static_cast<long double>(price)
        / static_cast<long double>(minimum_price_increment);
    if (ratio > static_cast<long double>(std::numeric_limits<std::int64_t>::max())
        || ratio < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return false;
    }

    const auto rounded = static_cast<std::int64_t>(std::round(ratio));
    const long double rebuilt =
        static_cast<long double>(rounded)
        * static_cast<long double>(minimum_price_increment);
    const long double tolerance =
        std::fmax(1.0e-9L, std::fabs(static_cast<long double>(price)) * 1.0e-12L);
    if (std::fabs(rebuilt - static_cast<long double>(price)) > tolerance) {
        return false;
    }

    result = rounded;
    return true;
}

}

struct MarketIngress::AccountChannel {
    std::string account_id;
    SpscQueue<MarketEvent, kMarketQueueCapacity> queue;
    std::atomic<bool> overflowed{false};
};

MarketIngress::MarketIngress(
    const std::vector<AccountConfig>& accounts,
    double minimum_price_increment)
    : channels_(std::make_unique<AccountChannel[]>(accounts.size())),
      account_count_(accounts.size()),
      minimum_price_increment_(minimum_price_increment)
{
    for (std::size_t index = 0; index < account_count_; ++index) {
        channels_[index].account_id = accounts[index].alias();
    }
}

MarketIngress::~MarketIngress() = default;

void MarketIngress::start() noexcept
{
    accepting_.store(true, std::memory_order_release);
}

void MarketIngress::stop() noexcept
{
    accepting_.store(false, std::memory_order_release);
}

std::size_t MarketIngress::account_count() const noexcept
{
    return account_count_;
}

std::string_view MarketIngress::account_id(std::size_t account_index) const noexcept
{
    if (account_index >= account_count_) {
        return {};
    }
    return channels_[account_index].account_id;
}

bool MarketIngress::try_pop(
    std::size_t account_index,
    MarketEvent& event) noexcept
{
    return account_index < account_count_
           && channels_[account_index].queue.try_pop(event);
}

MarketQueueSnapshot MarketIngress::snapshot(
    std::size_t account_index) const noexcept
{
    if (account_index >= account_count_) {
        return {};
    }

    const auto& channel = channels_[account_index];
    return {
        channel.queue.depth(),
        channel.queue.high_watermark(),
        channel.queue.dropped_count(),
        channel.overflowed.load(std::memory_order_acquire)};
}

MarketPublishResult MarketIngress::ingest(
    const CThostFtdcDepthMarketDataField* tick,
    std::int64_t recv_mono_ns) noexcept
{
    if (!accepting_.load(std::memory_order_acquire)) {
        return {};
    }

    const MarketEvent event = normalize(tick, recv_mono_ns);
    MarketPublishResult result;
    for (std::size_t index = 0; index < account_count_; ++index) {
        auto& channel = channels_[index];
        if (channel.overflowed.load(std::memory_order_relaxed)) {
            channel.queue.record_drop();
            ++result.overflowed;
            continue;
        }

        if (channel.queue.try_push(event)) {
            ++result.published;
            continue;
        }

        channel.overflowed.store(true, std::memory_order_release);
        ++result.overflowed;
    }
    return result;
}

void MarketIngress::OnRtnDepthMarketData(
    CThostFtdcDepthMarketDataField* tick)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto recv_mono_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    static_cast<void>(ingest(tick, recv_mono_ns));
}

MarketEvent MarketIngress::normalize(
    const CThostFtdcDepthMarketDataField* tick,
    std::int64_t recv_mono_ns) noexcept
{
    MarketEvent event;
    event.market_seq = next_market_seq_++;
    event.recv_mono_ns = recv_mono_ns;
    if (tick == nullptr) {
        event.status = MarketDataStatus::NullData;
        return event;
    }

    event.bid_volume = tick->BidVolume1;
    event.ask_volume = tick->AskVolume1;
    event.volume = tick->Volume;
    if (!copy_instrument(tick->InstrumentID, event.instrument)) {
        event.status = MarketDataStatus::InvalidInstrument;
        return event;
    }
    if (!parse_exchange_time_ms(
            tick->UpdateTime,
            tick->UpdateMillisec,
            event.exchange_time_ms)) {
        event.status = MarketDataStatus::InvalidTime;
        return event;
    }
    if (!price_to_ticks(
            tick->LastPrice,
            minimum_price_increment_,
            event.last_price_ticks)
        || !price_to_ticks(
            tick->BidPrice1,
            minimum_price_increment_,
            event.bid_price_ticks)
        || !price_to_ticks(
            tick->AskPrice1,
            minimum_price_increment_,
            event.ask_price_ticks)) {
        event.status = MarketDataStatus::InvalidPrice;
        return event;
    }
    if (event.bid_volume < 0 || event.ask_volume < 0 || event.volume < 0
        || event.bid_price_ticks > event.ask_price_ticks) {
        event.status = MarketDataStatus::InvalidBook;
        return event;
    }

    event.status = MarketDataStatus::Valid;
    return event;
}

namespace {

enum class LiveMarketState : std::uint8_t {
    Idle,
    Connecting,
    LoggingIn,
    Subscribing,
    Running,
    Disconnected,
    Failed,
};

class LiveMarketConnection final : public CThostFtdcMdSpi {
public:
    LiveMarketConnection(
        const RuntimeConfig& config,
        MarketIngress& ingress,
        std::unique_ptr<MarketApi> api)
        : account_(config.accounts().front()),
          live_(config.live()),
          ingress_(ingress),
          api_(std::move(api))
    {
        copy_to_field(instrument_, live_.instrument);
    }

    ~LiveMarketConnection()
    {
        ingress_.stop();
        if (api_) {
            api_->register_spi(nullptr);
            api_->release();
        }
    }

    bool start()
    {
        if (!api_) return false;
        state_.store(LiveMarketState::Connecting, std::memory_order_release);
        api_->register_spi(this);
        api_->register_front(live_.market_front);
        api_->init();
        return true;
    }

    LiveMarketState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    void OnFrontConnected() override
    {
        state_.store(LiveMarketState::LoggingIn, std::memory_order_release);
        CThostFtdcReqUserLoginField request{};
        copy_to_field(request.BrokerID, account_.broker_id());
        copy_to_field(request.UserID, account_.user_id());
        copy_to_field(request.Password, account_.password());
        if (api_->request_user_login(&request, next_request_id_++) != 0) {
            fail();
        }
    }

    void OnFrontDisconnected(int) override
    {
        // 给每个账户发送一条无效事件，禁止策略跨断线沿用旧基线。
        static_cast<void>(ingress_.ingest(nullptr, monotonic_now_ns()));
        ingress_.stop();
        state_.store(LiveMarketState::Disconnected, std::memory_order_release);
    }

    void OnRspUserLogin(
        CThostFtdcRspUserLoginField*,
        CThostFtdcRspInfoField* info,
        int,
        bool is_last) override
    {
        if (!is_last) return;
        if (info != nullptr && info->ErrorID != 0) {
            fail();
            return;
        }
        state_.store(LiveMarketState::Subscribing, std::memory_order_release);
        char* instruments[]{instrument_};
        if (api_->subscribe_market_data(instruments, 1) != 0) fail();
    }

    void OnRspSubMarketData(
        CThostFtdcSpecificInstrumentField*,
        CThostFtdcRspInfoField* info,
        int,
        bool is_last) override
    {
        if (info != nullptr && info->ErrorID != 0) {
            fail();
            return;
        }
        if (is_last) {
            ingress_.start();
            state_.store(LiveMarketState::Running, std::memory_order_release);
        }
    }

    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* tick) override
    {
        ingress_.OnRtnDepthMarketData(tick);
    }

private:
    static std::int64_t monotonic_now_ns() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void fail() noexcept
    {
        ingress_.stop();
        state_.store(LiveMarketState::Failed, std::memory_order_release);
    }

    const AccountConfig& account_;
    const LiveConfig& live_;
    MarketIngress& ingress_;
    std::unique_ptr<MarketApi> api_;
    char instrument_[kInstrumentIdCapacity]{};
    std::atomic<LiveMarketState> state_{LiveMarketState::Idle};
    int next_request_id_{1};
};

class LiveAccountWorker final {
public:
    LiveAccountWorker(
        const RuntimeConfig& config,
        std::size_t account_index,
        MarketIngress& ingress,
        std::unique_ptr<TraderApi> api,
        const std::filesystem::path& trace_directory,
        std::uint64_t run_id)
        : live_(config.live()),
          account_index_(account_index),
          ingress_(ingress),
          strategy_(make_strategy(config.live()))
    {
        if (!trace_directory.empty()) {
            journal_ = std::make_unique<AsyncTraceJournal>(
                trace_directory
                    / (config.accounts()[account_index].alias() + ".csv"),
                config.accounts()[account_index].alias());
            if (!journal_->start()) {
                journal_.reset();
                initialized_ = false;
            }
        }
        session_ = std::make_unique<AccountTradingSession>(
            config.accounts()[account_index],
            make_risk_limits(config.live()),
            std::move(api),
            256,
            1024,
            64,
            1024,
            1,
            1,
            make_close_policy(config.live()),
            journal_.get(),
            run_id,
            config.live().minimum_price_increment);
    }

    ~LiveAccountWorker()
    {
        join();
    }

    void start(std::atomic<bool>& stopping)
    {
        thread_ = std::thread([this, &stopping] { run(stopping); });
    }

    bool initialized() const noexcept { return initialized_; }

    void join()
    {
        if (thread_.joinable()) thread_.join();
    }

    bool ready() const noexcept
    {
        return ready_.load(std::memory_order_acquire);
    }

    bool failed() const noexcept
    {
        return failed_.load(std::memory_order_acquire);
    }

    std::uint64_t submitted() const noexcept
    {
        return submitted_.load(std::memory_order_relaxed);
    }

private:
    static StrategyConfig make_strategy(const LiveConfig& live)
    {
        StrategyConfig config{};
        copy_to_field(config.instrument, live.instrument);
        config.threshold_ticks = live.trigger_price_ticks;
        config.protection_ticks = live.entry_protection_ticks;
        config.max_market_age_ns = live.market_stale_after_ns;
        config.max_signals_per_run = live.max_signals_per_run;
        return config;
    }

    static RiskLimits make_risk_limits(const LiveConfig& live)
    {
        RiskLimits limits{};
        copy_to_field(limits.allowed_instrument, live.instrument);
        limits.max_market_age_ns = live.market_stale_after_ns;
        limits.max_slippage_ticks = live.max_price_deviation_ticks;
        limits.minimum_available_after_order = live.minimum_available_funds;
        limits.max_daily_signals = live.max_signals_per_run;
        limits.max_daily_orders = live.max_orders_per_day;
        limits.max_daily_cancels = live.max_cancels_per_day;
        limits.max_order_volume = live.max_order_volume;
        limits.max_active_open_orders = live.max_active_open_orders;
        limits.max_net_open_position = live.max_net_position;
        return limits;
    }

    static AutoClosePolicy make_close_policy(const LiveConfig& live)
    {
        AutoClosePolicy policy{};
        policy.enabled = true;
        policy.entry_timeout_market_events = live.cancel_after_market_ticks;
        policy.close_protection_ticks = live.entry_protection_ticks;
        policy.close_reprice_interval_market_events =
            live.close_reprice_after_market_ticks;
        policy.max_close_reprices = live.max_close_reprices;
        return policy;
    }

    static std::int64_t now_ns() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    RiskSnapshot risk_snapshot(const MarketEvent& market) noexcept
    {
        const auto recovery = session_->recovery_snapshot();
        const auto execution = session_->execution_snapshot();
        PositionSnapshot position{};
        const bool positions_known = session_->position_snapshot(
            live_.instrument, position)
            || recovery.phase == RecoveryPhase::Ready;
        RiskSnapshot risk{};
        risk.enabled = true;
        risk.authenticated = recovery.phase == RecoveryPhase::Ready;
        risk.logged_in = recovery.phase == RecoveryPhase::Ready;
        risk.reconciled = recovery.phase == RecoveryPhase::Ready;
        risk.frozen = execution.frozen;
        risk.trading_window_open = true;
        risk.global_kill_switch = live_.kill_switch || !live_.allow_orders;
        risk.market_valid = market.status == MarketDataStatus::Valid;
        risk.funds_known = recovery.funds_known;
        risk.positions_known = positions_known;
        risk.now_ns = now_ns();
        risk.market_receive_ns = market.recv_mono_ns;
        risk.bid_price_ticks = market.bid_price_ticks;
        risk.ask_price_ticks = market.ask_price_ticks;
        risk.available_funds = recovery.available_funds;
        risk.long_position = position.long_quantity;
        risk.short_position = position.short_quantity;
        risk.closable_long = position.long_quantity;
        risk.closable_short = position.short_quantity;
        risk.active_open_orders = execution.active_open_orders;
        return risk;
    }

    void run(std::atomic<bool>& stopping) noexcept
    {
        session_->start();
        while (!stopping.load(std::memory_order_acquire)) {
            session_->drain_callbacks();
            const auto recovery = session_->recovery_snapshot();
            ready_.store(
                recovery.phase == RecoveryPhase::Ready,
                std::memory_order_release);
            if (recovery.phase == RecoveryPhase::Frozen) {
                failed_.store(true, std::memory_order_release);
            }
            if (ingress_.snapshot(account_index_).overflowed) {
                session_->mark_fault(AccountFault::MarketQueueOverflow);
                failed_.store(true, std::memory_order_release);
            }

            MarketEvent market{};
            std::size_t drained = 0;
            while (drained < 256 && ingress_.try_pop(account_index_, market)) {
                ++drained;
                if (recovery.phase != RecoveryPhase::Ready) continue;
                auto risk = risk_snapshot(market);
                session_->trace_market(market);
                static_cast<void>(session_->on_market(market, risk));
                if (!live_.strategy_enabled || !live_.allow_orders) continue;
                const auto decision = strategy_.on_market(market, risk.now_ns);
                if (!decision.has_intent) continue;
                session_->trace_signal(decision.intent, risk.now_ns);
                if (orders_in_window_ >= live_.max_order_rate_per_second
                    && risk.now_ns - rate_window_start_ns_ < 1'000'000'000) {
                    continue;
                }
                if (risk.now_ns - rate_window_start_ns_ >= 1'000'000'000) {
                    rate_window_start_ns_ = risk.now_ns;
                    orders_in_window_ = 0;
                }
                const auto result = session_->submit(decision.intent, risk);
                if (result.code == SubmitCode::Submitted) {
                    ++orders_in_window_;
                    submitted_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (drained == 0) std::this_thread::yield();
        }
        session_->drain_callbacks();
        session_.reset();
        if (journal_) journal_->stop();
    }

    const LiveConfig& live_;
    std::size_t account_index_{0};
    MarketIngress& ingress_;
    ThresholdStrategy strategy_;
    std::unique_ptr<AsyncTraceJournal> journal_;
    std::unique_ptr<AccountTradingSession> session_;
    std::thread thread_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> submitted_{0};
    std::int64_t rate_window_start_ns_{0};
    std::uint32_t orders_in_window_{0};
    bool initialized_{true};
};

std::string validate_live_config(const RuntimeConfig& config)
{
    const auto& live = config.live();
    if (config.accounts().empty()) return "no enabled account";
    if (!has_tcp_front(live.market_front)) {
        return "market_front is missing or invalid";
    }
    if (live.instrument.empty()) return "instrument is missing";
    if (!std::isfinite(live.minimum_price_increment)
        || live.minimum_price_increment <= 0.0) {
        return "minimum_price_increment is missing or invalid";
    }
    if (!live.allow_orders) return {};
    if (!live.strategy_enabled) return "--allow-orders requires strategy enabled=true";
    if (live.kill_switch) return "--allow-orders requires risk kill_switch=false";
    if (live.trigger_price_ticks <= 0 || live.max_signals_per_run == 0
        || live.cancel_after_market_ticks == 0
        || live.close_reprice_after_market_ticks == 0
        || live.max_order_volume < 1 || live.max_net_position < 1
        || live.max_active_open_orders < 1 || live.max_orders_per_day < 1
        || live.max_cancels_per_day < 1
        || live.max_order_rate_per_second < 1
        || live.market_stale_after_ns <= 0) {
        return "order-enabled strategy and risk limits must be positive";
    }
    return {};
}

}

int run_live_engine(
    const RuntimeConfig& config,
    std::ostream& output,
    std::ostream& error,
    const std::function<bool()>& stop_requested,
    LiveEngineDependencies dependencies)
{
    const auto invalid = validate_live_config(config);
    if (!invalid.empty()) {
        error << "[error] live engine configuration: " << invalid << '\n';
        return 2;
    }
    if (!stop_requested) {
        error << "[error] live engine requires a stop predicate\n";
        return 2;
    }
    if (!dependencies.create_market) {
        dependencies.create_market = [] { return create_market_api(); };
    }
    if (!dependencies.create_trader) {
        dependencies.create_trader = [](const std::string& path) {
            return create_trader_api(path);
        };
    }

    const auto run_id = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::filesystem::path trace_directory;
    if (!dependencies.trace_root.empty()) {
        trace_directory = std::filesystem::path{dependencies.trace_root}
            / std::to_string(run_id);
        std::error_code directory_error;
        std::filesystem::create_directories(trace_directory, directory_error);
        if (directory_error) {
            error << "[error] cannot create trace directory\n";
            return 3;
        }
    }

    MarketIngress ingress{
        config.accounts(), config.live().minimum_price_increment};
    std::vector<std::unique_ptr<LiveAccountWorker>> workers;
    workers.reserve(config.accounts().size());
    for (std::size_t index = 0; index < config.accounts().size(); ++index) {
        const auto flow_directory = std::string{"runtime/flow/"}
            + config.accounts()[index].alias() + '/';
        auto api = dependencies.create_trader(flow_directory);
        if (!api) {
            error << "[error] cannot create trader API for account index "
                  << index << '\n';
            return 3;
        }
        workers.push_back(std::make_unique<LiveAccountWorker>(
            config, index, ingress, std::move(api), trace_directory, run_id));
        if (!workers.back()->initialized()) {
            error << "[error] cannot start trace journal for account index "
                  << index << '\n';
            return 3;
        }
    }

    std::atomic<bool> stopping{false};
    for (auto& worker : workers) worker->start(stopping);
    LiveMarketConnection market{
        config, ingress, dependencies.create_market()};
    if (!market.start()) {
        stopping.store(true, std::memory_order_release);
        for (auto& worker : workers) worker->join();
        error << "[error] cannot create market API\n";
        return 3;
    }

    output << "[info] live engine started with " << workers.size()
           << " independent account worker(s); orders="
           << (config.live().allow_orders ? "enabled" : "disabled") << '\n';

    while (!stop_requested()) {
        if (market.state() == LiveMarketState::Failed) {
            error << "[error] market login or subscription failed\n";
            stopping.store(true, std::memory_order_release);
            for (auto& worker : workers) worker->join();
            return 3;
        }
        std::this_thread::yield();
    }
    stopping.store(true, std::memory_order_release);
    ingress.stop();
    for (auto& worker : workers) worker->join();

    std::size_t ready = 0;
    std::size_t failed = 0;
    std::uint64_t submitted = 0;
    for (const auto& worker : workers) {
        if (worker->ready()) ++ready;
        if (worker->failed()) ++failed;
        submitted += worker->submitted();
    }
    output << "[info] live engine stopped; ready=" << ready
           << ", failed=" << failed << ", submitted=" << submitted << '\n';
    return 0;
}

}
