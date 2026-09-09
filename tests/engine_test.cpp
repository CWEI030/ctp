#include "ctp/engine.hpp"
#include "ctp/field.hpp"
#include "ctp/market_client.hpp"
#include "ctp/telemetry.hpp"
#include "ctp/trading.hpp"
#define CTP_TEST_DEFINE_ALLOCATION_OPERATORS
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

std::vector<ctp::AccountConfig> make_accounts(std::size_t count)
{
    std::vector<ctp::AccountConfig> accounts;
    accounts.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::string suffix = std::to_string(index + 1);
        accounts.emplace_back(
            "account" + suffix,
            "9999",
            "user" + suffix,
            "password" + suffix,
            "app" + suffix,
            "auth" + suffix,
            "tcp://127.0.0.1:4100" + suffix);
    }
    return accounts;
}

CThostFtdcDepthMarketDataField make_tick(int millisec = 7)
{
    CThostFtdcDepthMarketDataField tick{};
    ctp::copy_to_field(tick.InstrumentID, "IF2609");
    ctp::copy_to_field(tick.UpdateTime, "09:30:01");
    tick.UpdateMillisec = millisec;
    tick.LastPrice = 100.0 + millisec * 0.2;
    tick.BidPrice1 = 99.8 + millisec * 0.2;
    tick.AskPrice1 = 100.2 + millisec * 0.2;
    tick.BidVolume1 = 10 + millisec;
    tick.AskVolume1 = 20 + millisec;
    tick.Volume = 1000 + millisec;
    return tick;
}

struct FakeLiveMarketState {
    std::atomic<CThostFtdcMdSpi*> spi{nullptr};
    std::atomic<bool> subscribed{false};
    std::atomic<int> release_calls{0};
    int login_error{0};
    int subscription_error{0};
    std::string broker_id;
    std::string user_id;
    std::string password;
};

class FakeLiveMarketApi final : public ctp::MarketApi {
public:
    explicit FakeLiveMarketApi(std::shared_ptr<FakeLiveMarketState> state)
        : state_(std::move(state))
    {
    }

    void register_spi(CThostFtdcMdSpi* spi) override
    {
        state_->spi.store(spi, std::memory_order_release);
    }

    void register_front(const std::string&) override {}

    void init() override
    {
        state_->spi.load(std::memory_order_acquire)->OnFrontConnected();
    }

    int request_user_login(
        CThostFtdcReqUserLoginField* request,
        int request_id) override
    {
        state_->broker_id = request->BrokerID;
        state_->user_id = request->UserID;
        state_->password = request->Password;
        CThostFtdcRspInfoField info{};
        info.ErrorID = state_->login_error;
        state_->spi.load(std::memory_order_acquire)->OnRspUserLogin(
            nullptr, &info, request_id, true);
        return 0;
    }

    int subscribe_market_data(char* instruments[], int count) override
    {
        CThostFtdcSpecificInstrumentField response{};
        if (count == 1) ctp::copy_to_field(response.InstrumentID, instruments[0]);
        CThostFtdcRspInfoField info{};
        info.ErrorID = state_->subscription_error;
        if (info.ErrorID == 0) {
            state_->subscribed.store(true, std::memory_order_release);
        }
        state_->spi.load(std::memory_order_acquire)->OnRspSubMarketData(
            &response, &info, 1, true);
        return 0;
    }

    void release() override
    {
        state_->release_calls.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::shared_ptr<FakeLiveMarketState> state_;
};

ctp::RuntimeConfig make_live_config(std::size_t account_count)
{
    ctp::LiveConfig live{};
    live.market_front = "tcp://127.0.0.1:41213";
    live.exchange_id = "CFFEX";
    live.instrument = "IF2609";
    live.minimum_price_increment = 0.2;
    live.allow_orders = true;
    live.strategy_enabled = true;
    live.trigger_price_ticks = 4'000;
    live.entry_protection_ticks = 2;
    live.cancel_after_market_ticks = 20;
    live.max_signals_per_run = 1;
    live.close_reprice_after_market_ticks = 5;
    live.max_close_reprices = 1;
    live.max_order_volume = 1;
    live.max_net_position = 1;
    live.max_active_open_orders = 1;
    live.max_orders_per_day = 2;
    live.max_cancels_per_day = 2;
    live.max_order_rate_per_second = 2;
    live.max_price_deviation_ticks = 2;
    live.margin_per_lot = 1'000'000;
    live.minimum_available_funds = 1'000'000;
    live.trading_windows.push_back({9 * 3'600'000, 10 * 3'600'000});
    live.market_stale_after_ns = 1'000'000'000;
    live.kill_switch = false;
    return {
        ctp::Mode::Engine,
        "simnow",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        0,
        make_accounts(account_count),
        {},
        std::move(live)};
}

std::unique_ptr<test_support::FakeTraderApi> make_recovering_trader(
    const std::shared_ptr<test_support::FakeTraderMetrics>& metrics,
    bool reject_authentication)
{
    auto api = std::make_unique<test_support::FakeTraderApi>(metrics);
    api->on_init = [](auto& self) { self.spi()->OnFrontConnected(); };
    api->on_authenticate = [metrics, reject_authentication](auto& self) {
        CThostFtdcRspInfoField info{};
        info.ErrorID = reject_authentication ? 7 : 0;
        self.spi()->OnRspAuthenticate(
            nullptr, &info, metrics->authenticate_request_id, true);
    };
    api->on_login = [metrics](auto& self) {
        CThostFtdcRspUserLoginField response{};
        response.FrontID = 3;
        response.SessionID = 9;
        ctp::copy_to_field(response.MaxOrderRef, "20");
        ctp::copy_to_field(response.TradingDay, "20260909");
        self.spi()->OnRspUserLogin(
            &response, nullptr, metrics->login_request_id, true);
    };
    api->on_order_query = [metrics](auto& self) {
        self.spi()->OnRspQryOrder(
            nullptr, nullptr, metrics->order_query_request_id, true);
    };
    api->on_trade_query = [metrics](auto& self) {
        self.spi()->OnRspQryTrade(
            nullptr, nullptr, metrics->trade_query_request_id, true);
    };
    api->on_position = [metrics](auto& self) {
        self.spi()->OnRspQryInvestorPosition(
            nullptr, nullptr, metrics->position_request_id, true);
    };
    api->on_account = [metrics](auto& self) {
        CThostFtdcTradingAccountField account{};
        account.Available = 100'000.0;
        self.spi()->OnRspQryTradingAccount(
            &account, nullptr, metrics->account_request_id, true);
    };
    return api;
}

void test_fixed_event_and_queue(test_support::TestRunner& runner)
{
    runner.expect(
        std::is_trivially_copyable<ctp::MarketEvent>::value,
        "MarketEvent must be trivially copyable");

    ctp::SpscQueue<int, 3> queue;
    runner.expect(queue.try_push(10), "first queue slot must accept an event");
    runner.expect(queue.try_push(20), "second queue slot must accept an event");
    runner.expect(queue.try_push(30), "all declared queue slots must be usable");
    runner.expect(!queue.try_push(40), "a full queue must reject without overwrite");
    runner.expect(queue.depth() == 3, "full queue depth must equal capacity");
    runner.expect(queue.high_watermark() == 3, "high-water mark must reach capacity");
    runner.expect(queue.dropped_count() == 1, "full push must count one drop");

    int value = 0;
    runner.expect(queue.try_pop(value) && value == 10, "queue must pop in FIFO order");
    runner.expect(queue.try_pop(value) && value == 20, "queue must retain middle event");
    runner.expect(queue.try_pop(value) && value == 30, "queue must retain last event");
    runner.expect(!queue.try_pop(value), "empty queue must return immediately");
}

void test_variable_account_assembly(test_support::TestRunner& runner)
{
    for (std::size_t count = 1; count <= 5; ++count) {
        ctp::MarketIngress ingress{make_accounts(count), 0.2};
        runner.expect(
            ingress.account_count() == count,
            "ingress must create one independent channel per configured account");
        runner.expect(
            ingress.account_id(count - 1) == "account" + std::to_string(count),
            "account channel order must follow the immutable config order");
    }
}

void test_normalization_and_four_account_distribution(
    test_support::TestRunner& runner)
{
    ctp::MarketIngress ingress{make_accounts(4), 0.2};
    ingress.start();
    auto tick = make_tick(0);
    const auto result = ingress.ingest(&tick, 123456789);

    runner.expect(result.published == 4, "one valid tick must reach all four accounts");
    runner.expect(result.overflowed == 0, "available queues must not report overflow");
    for (std::size_t account = 0; account < 4; ++account) {
        ctp::MarketEvent event{};
        runner.expect(ingress.try_pop(account, event), "each account must receive the tick");
        runner.expect(event.market_seq == 1, "all accounts must receive the same sequence");
        runner.expect(event.recv_mono_ns == 123456789, "receive time must be retained");
        runner.expect(event.exchange_time_ms == 34201000, "exchange time must use milliseconds");
        runner.expect(event.last_price_ticks == 500, "last price must use integer ticks");
        runner.expect(event.bid_price_ticks == 499, "bid price must use integer ticks");
        runner.expect(event.ask_price_ticks == 501, "ask price must use integer ticks");
        runner.expect(
            std::string_view{event.instrument.data()} == "IF2609",
            "the fixed event must retain the instrument identifier");
        runner.expect(
            event.status == ctp::MarketDataStatus::Valid,
            "valid book data must retain an explicit valid status");

        event.instrument[0] = 'X';
        if (account + 1 < 4) {
            runner.expect(
                ingress.snapshot(account + 1).depth == 1,
                "changing one popped copy must not alter another account queue");
        }
    }
}

void test_slow_account_does_not_block_other_accounts(
    test_support::TestRunner& runner)
{
    for (std::size_t slow_account = 0; slow_account < 4; ++slow_account) {
        ctp::MarketIngress ingress{make_accounts(4), 0.2};
        ingress.start();

        for (std::size_t sequence = 0;
             sequence < ctp::kMarketQueueCapacity + 2;
             ++sequence) {
            auto tick = make_tick(static_cast<int>(sequence % 100));
            const auto result =
                ingress.ingest(&tick, static_cast<std::int64_t>(sequence));

            for (std::size_t account = 0; account < 4; ++account) {
                if (account == slow_account) {
                    continue;
                }
                ctp::MarketEvent event{};
                runner.expect(
                    ingress.try_pop(account, event),
                    "active accounts must continue while one account is paused");
                runner.expect(
                    event.market_seq == sequence + 1,
                    "active account sequence must remain continuous");
            }

            if (sequence >= ctp::kMarketQueueCapacity) {
                runner.expect(
                    result.published == 3 && result.overflowed == 1,
                    "overflow result must identify only the paused account");
            }
        }

        const auto slow = ingress.snapshot(slow_account);
        runner.expect(slow.overflowed, "only the paused account must be marked overflowed");
        runner.expect(slow.dropped == 2, "paused account must count every omitted event");
        for (std::size_t account = 0; account < 4; ++account) {
            if (account == slow_account) {
                continue;
            }
            const auto active = ingress.snapshot(account);
            runner.expect(!active.overflowed, "other accounts must remain healthy");
            runner.expect(active.dropped == 0, "other accounts must not inherit drops");
        }
    }
}

void expect_status_for_all_accounts(
    test_support::TestRunner& runner,
    ctp::MarketIngress& ingress,
    ctp::MarketDataStatus expected,
    std::string_view message)
{
    for (std::size_t account = 0; account < ingress.account_count(); ++account) {
        ctp::MarketEvent event{};
        runner.expect(ingress.try_pop(account, event), message);
        runner.expect(event.status == expected, message);
    }
}

void test_lifecycle_invalid_data_and_hot_path_allocation(
    test_support::TestRunner& runner)
{
    ctp::MarketIngress ingress{make_accounts(2), 0.2};
    auto tick = make_tick(0);
    runner.expect(
        ingress.ingest(&tick, 1).published == 0,
        "ingress must ignore callbacks before start");

    ingress.start();
    runner.expect(
        ingress.ingest(nullptr, 2).published == 2,
        "null callbacks must become explicit events");
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::NullData,
        "null callbacks must have NullData status");

    tick.InstrumentID[0] = '\0';
    runner.expect(
        ingress.ingest(&tick, 3).published == 2,
        "invalid market data must remain visible to every account");
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::InvalidInstrument,
        "empty instrument must be marked invalid");

    tick = make_tick(0);
    ctp::copy_to_field(tick.UpdateTime, "25:00:00");
    ingress.ingest(&tick, 4);
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::InvalidTime,
        "invalid exchange time must be classified");

    tick = make_tick(0);
    tick.LastPrice = std::numeric_limits<double>::quiet_NaN();
    ingress.ingest(&tick, 5);
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::InvalidPrice,
        "non-finite price must be classified");

    tick = make_tick(0);
    tick.BidPrice1 = tick.AskPrice1 + 1.0;
    ingress.ingest(&tick, 6);
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::InvalidBook,
        "crossed book must be marked invalid instead of silently discarded");

    tick = make_tick(0);
    ingress.ingest(&tick, 7);
    for (std::size_t account = 0; account < 2; ++account) {
        ctp::MarketEvent event{};
        ingress.try_pop(account, event);
    }

    test_support::AllocationProbe allocation_probe;
    ingress.OnRtnDepthMarketData(&tick);
    allocation_probe.stop();
    runner.expect(
        ingress.snapshot(0).depth == 1 && ingress.snapshot(1).depth == 1,
        "preheated callback must still publish normally");
    runner.expect(
        allocation_probe.count() == 0,
        "preheated callback must not allocate memory");

    ingress.stop();
    runner.expect(
        ingress.ingest(&tick, 8).published == 0,
        "ingress must ignore callbacks after stop");
}

ctp::MarketDataStatus normalize_price_status(
    CThostFtdcDepthMarketDataField tick,
    double minimum_price_increment)
{
    ctp::MarketIngress ingress{make_accounts(1), minimum_price_increment};
    ingress.start();
    ingress.ingest(&tick, 1);
    ctp::MarketEvent event{};
    ingress.try_pop(0, event);
    return event.status;
}

void test_price_normalization_boundaries(test_support::TestRunner& runner)
{
    auto tick = make_tick(0);
    tick.LastPrice = 100.20000000000002;
    runner.expect(
        normalize_price_status(tick, 0.2) == ctp::MarketDataStatus::Valid,
        "binary floating representation near an exact tick must normalize");

    tick = make_tick(0);
    tick.LastPrice = 100.1;
    runner.expect(
        normalize_price_status(tick, 0.2)
            == ctp::MarketDataStatus::InvalidPrice,
        "a half-tick price must not be rounded into a tradable price");

    tick = make_tick(0);
    tick.LastPrice = -100.0;
    runner.expect(
        normalize_price_status(tick, 0.2)
            == ctp::MarketDataStatus::InvalidPrice,
        "a negative source price must be rejected");

    tick = make_tick(0);
    tick.AskPrice1 = std::numeric_limits<double>::infinity();
    runner.expect(
        normalize_price_status(tick, 0.2)
            == ctp::MarketDataStatus::InvalidPrice,
        "an infinite source price must be rejected");

    tick = make_tick(0);
    runner.expect(
        normalize_price_status(tick, 0.0)
            == ctp::MarketDataStatus::InvalidPrice,
        "a non-positive minimum price increment must reject normalization");

    tick = make_tick(0);
    tick.BidPrice1 = std::numeric_limits<double>::max();
    runner.expect(
        normalize_price_status(tick, 0.2)
            == ctp::MarketDataStatus::InvalidPrice,
        "a tick conversion outside int64 range must be rejected");
}

ctp::RiskLimits isolation_limits()
{
    ctp::RiskLimits limits{};
    ctp::copy_to_field(limits.allowed_instrument, "IF2609");
    limits.max_market_age_ns = 1'000'000;
    limits.max_slippage_ticks = 2;
    limits.margin_per_lot = 100;
    limits.minimum_available_after_order = 100;
    limits.max_daily_signals = 10;
    limits.max_daily_orders = 10;
    limits.max_daily_cancels = 10;
    limits.max_active_open_orders = 1;
    limits.max_net_open_position = 1;
    return limits;
}

ctp::RiskSnapshot isolation_snapshot()
{
    ctp::RiskSnapshot snapshot{};
    snapshot.enabled = true;
    snapshot.authenticated = true;
    snapshot.logged_in = true;
    snapshot.reconciled = true;
    snapshot.trading_window_open = true;
    snapshot.market_valid = true;
    snapshot.funds_known = true;
    snapshot.positions_known = true;
    snapshot.now_ns = 2'000'000;
    snapshot.market_receive_ns = 1'500'000;
    snapshot.bid_price_ticks = 499;
    snapshot.ask_price_ticks = 501;
    snapshot.available_funds = 200;
    return snapshot;
}

ctp::OrderIntent isolation_intent(std::uint64_t signal_id)
{
    ctp::OrderIntent intent{};
    intent.signal_id = signal_id;
    ctp::copy_to_field(intent.instrument, "IF2609");
    intent.direction = ctp::Direction::Buy;
    intent.offset = ctp::Offset::Open;
    intent.quantity = 1;
    intent.limit_price_ticks = 502;
    return intent;
}

void test_four_account_fault_matrix(test_support::TestRunner& runner)
{
    constexpr std::array expected_faults{
        ctp::AccountFault::Disconnected,
        ctp::AccountFault::MarketQueueOverflow,
        ctp::AccountFault::CallbackQueueOverflow,
        ctp::AccountFault::UnknownCallback,
    };

    for (std::size_t target = 0; target < expected_faults.size(); ++target) {
        auto accounts = make_accounts(4);
        std::array<std::unique_ptr<ctp::AccountTradingSession>, 4> sessions;
        std::array<test_support::FakeTraderApi*, 4> api_views{};
        for (std::size_t account = 0; account < sessions.size(); ++account) {
            auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
            auto api = std::make_unique<test_support::FakeTraderApi>(metrics);
            api_views[account] = api.get();
            sessions[account] = std::make_unique<ctp::AccountTradingSession>(
                accounts[account], isolation_limits(), std::move(api),
                8, 8, 8, 1);
        }

        if (target == 0) {
            api_views[target]->spi()->OnFrontDisconnected(0x1001);
            sessions[target]->drain_callbacks();
        } else if (target == 1) {
            ctp::MarketIngress ingress{accounts, 0.2};
            ingress.start();
            for (std::size_t sequence = 0;
                 sequence <= ctp::kMarketQueueCapacity;
                 ++sequence) {
                auto tick = make_tick(static_cast<int>(sequence % 100));
                ingress.ingest(&tick, static_cast<std::int64_t>(sequence));
                for (std::size_t account = 0; account < sessions.size(); ++account) {
                    if (account == target) continue;
                    ctp::MarketEvent ignored{};
                    ingress.try_pop(account, ignored);
                }
            }
            if (ingress.snapshot(target).overflowed) {
                sessions[target]->mark_fault(
                    ctp::AccountFault::MarketQueueOverflow);
            }
        } else if (target == 2) {
            CThostFtdcOrderField callback{};
            ctp::copy_to_field(callback.OrderRef, "999");
            callback.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
            api_views[target]->spi()->OnRtnOrder(&callback);
            api_views[target]->spi()->OnRtnOrder(&callback);
            sessions[target]->drain_callbacks();
        } else {
            CThostFtdcOrderField callback{};
            ctp::copy_to_field(callback.OrderRef, "999");
            callback.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
            api_views[target]->spi()->OnRtnOrder(&callback);
            sessions[target]->drain_callbacks();
        }

        for (std::size_t account = 0; account < sessions.size(); ++account) {
            const auto state = sessions[account]->execution_snapshot();
            const auto submitted = sessions[account]->submit(
                isolation_intent(1'000 + target * 10 + account),
                isolation_snapshot());
            if (account == target) {
                runner.expect(
                    state.frozen && state.reconciliation_required
                        && state.fault == expected_faults[target]
                        && state.alert_count == 1
                        && submitted.code == ctp::SubmitCode::RiskRejected
                        && submitted.risk_reason
                            == ctp::RiskRejectReason::AccountFrozen,
                    "the selected account alone must expose its stable fault");
            } else {
                runner.expect(
                    !state.frozen && !state.reconciliation_required
                        && state.fault == ctp::AccountFault::None
                        && submitted.code == ctp::SubmitCode::Submitted,
                    "unaffected accounts must retain independent order flow");
            }
        }
    }
}

void test_live_runner_isolates_one_failed_account(
    test_support::TestRunner& runner)
{
    auto config = make_live_config(4);
    auto market = std::make_shared<FakeLiveMarketState>();
    std::vector<std::shared_ptr<test_support::FakeTraderMetrics>> traders;
    for (std::size_t index = 0; index < 4; ++index) {
        traders.push_back(
            std::make_shared<test_support::FakeTraderMetrics>());
    }

    ctp::LiveEngineDependencies dependencies;
    dependencies.trace_root.clear();
    dependencies.create_market = [market] {
        return std::make_unique<FakeLiveMarketApi>(market);
    };
    std::size_t next_account = 0;
    dependencies.create_trader = [&traders, &next_account](const std::string&) {
        const auto index = next_account++;
        return make_recovering_trader(traders[index], index == 0);
    };

    std::atomic<bool> stop{false};
    std::thread feeder([market, &stop] {
        while (!market->subscribed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        for (int pair = 0; pair < 20; ++pair) {
            auto below = make_tick(pair * 2);
            below.LastPrice = 799.8;
            below.BidPrice1 = 799.6;
            below.AskPrice1 = 800.0;
            market->spi.load(std::memory_order_acquire)
                ->OnRtnDepthMarketData(&below);

            auto crossing = make_tick(pair * 2 + 1);
            crossing.LastPrice = 800.0;
            crossing.BidPrice1 = 799.8;
            crossing.AskPrice1 = 800.2;
            market->spi.load(std::memory_order_acquire)
                ->OnRtnDepthMarketData(&crossing);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        stop.store(true, std::memory_order_release);
    });

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = ctp::run_live_engine(
        config,
        output,
        error,
        [&stop] { return stop.load(std::memory_order_acquire); },
        std::move(dependencies));
    feeder.join();

    runner.expect(exit_code == 0, "one account failure must not stop the engine");
    runner.expect(
        traders[0]->order_insert_calls == 0,
        "the authentication-failed account must never submit an order");
    for (std::size_t index = 1; index < traders.size(); ++index) {
        runner.expect(
            traders[index]->account_calls == 1
                && traders[index]->order_insert_calls == 1,
            "each healthy account must recover and submit independently");
        runner.expect(
            traders[index]->last_order.VolumeTotalOriginal == 1
                && traders[index]->last_order.LimitPrice == 800.6,
            "each healthy account must submit the configured one-lot price");
    }
    runner.expect(
        output.str().find("ready=3, failed=1, submitted=3")
            != std::string::npos,
        "the final summary must preserve per-account outcomes");
    runner.expect(
        market->release_calls.load(std::memory_order_relaxed) == 1,
        "the shared market API must be released exactly once");
}

int run_single_account_signal(const ctp::RuntimeConfig& config)
{
    auto market = std::make_shared<FakeLiveMarketState>();
    auto trader = std::make_shared<test_support::FakeTraderMetrics>();
    ctp::LiveEngineDependencies dependencies;
    dependencies.trace_root.clear();
    dependencies.create_market = [market] {
        return std::make_unique<FakeLiveMarketApi>(market);
    };
    dependencies.create_trader = [trader](const std::string&) {
        return make_recovering_trader(trader, false);
    };

    std::atomic<bool> stop{false};
    std::thread feeder([market, &stop] {
        while (!market->subscribed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        for (int pair = 0; pair < 20; ++pair) {
            auto below = make_tick(pair * 2);
            below.LastPrice = 799.8;
            below.BidPrice1 = 799.6;
            below.AskPrice1 = 800.0;
            market->spi.load(std::memory_order_acquire)
                ->OnRtnDepthMarketData(&below);
            auto crossing = make_tick(pair * 2 + 1);
            crossing.LastPrice = 800.0;
            crossing.BidPrice1 = 799.8;
            crossing.AskPrice1 = 800.2;
            market->spi.load(std::memory_order_acquire)
                ->OnRtnDepthMarketData(&crossing);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        stop.store(true, std::memory_order_release);
    });
    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = ctp::run_live_engine(
        config,
        output,
        error,
        [&stop] { return stop.load(std::memory_order_acquire); },
        std::move(dependencies));
    feeder.join();
    return exit_code == 0 ? trader->order_insert_calls : -1;
}

void test_live_runner_enforces_margin_and_exchange_time_window(
    test_support::TestRunner& runner)
{
    const auto rebuild = [](ctp::LiveConfig live) {
        return ctp::RuntimeConfig{
            ctp::Mode::Engine,
            "simnow", "", "", "", "", "", "", "", "", 0,
            make_accounts(1), {}, std::move(live)};
    };

    auto insufficient_margin = make_live_config(1).live();
    insufficient_margin.margin_per_lot = 10'000'000;
    insufficient_margin.minimum_available_funds = 1;
    runner.expect(
        run_single_account_signal(rebuild(std::move(insufficient_margin))) == 0,
        "online risk must reserve configured per-lot margin before CTP insert");

    auto closed_window = make_live_config(1).live();
    closed_window.trading_windows = {{10 * 3'600'000, 11 * 3'600'000}};
    runner.expect(
        run_single_account_signal(rebuild(std::move(closed_window))) == 0,
        "online risk must reject ticks outside configured exchange-time windows");
}

void test_live_runner_fails_over_first_market_login(
    test_support::TestRunner& runner)
{
    auto config = make_live_config(4);
    std::vector<std::shared_ptr<FakeLiveMarketState>> markets;
    for (std::size_t index = 0; index < 2; ++index) {
        markets.push_back(std::make_shared<FakeLiveMarketState>());
    }
    markets[0]->login_error = 7;

    std::vector<std::shared_ptr<test_support::FakeTraderMetrics>> traders;
    for (std::size_t index = 0; index < 4; ++index) {
        traders.push_back(
            std::make_shared<test_support::FakeTraderMetrics>());
    }

    ctp::LiveEngineDependencies dependencies;
    dependencies.trace_root.clear();
    std::size_t next_market = 0;
    dependencies.create_market = [&markets, &next_market] {
        return std::make_unique<FakeLiveMarketApi>(markets[next_market++]);
    };
    std::size_t next_account = 0;
    dependencies.create_trader = [&traders, &next_account](const std::string&) {
        const auto index = next_account++;
        return make_recovering_trader(traders[index], index == 0);
    };

    bool published = false;
    std::size_t polls = 0;
    const auto stop_requested = [&] {
        if (markets[1]->subscribed.load(std::memory_order_acquire)
            && !published) {
            auto below = make_tick(0);
            below.LastPrice = 799.8;
            below.BidPrice1 = 799.6;
            below.AskPrice1 = 800.0;
            markets[1]->spi.load(std::memory_order_acquire)
                ->OnRtnDepthMarketData(&below);
            auto crossing = make_tick(1);
            crossing.LastPrice = 800.0;
            crossing.BidPrice1 = 799.8;
            crossing.AskPrice1 = 800.2;
            markets[1]->spi.load(std::memory_order_acquire)
                ->OnRtnDepthMarketData(&crossing);
            published = true;
        }
        ++polls;
        return (traders[1]->order_insert_calls == 1
                && traders[2]->order_insert_calls == 1
                && traders[3]->order_insert_calls == 1)
            || polls == 2'000'000;
    };

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = ctp::run_live_engine(
        config, output, error, stop_requested, std::move(dependencies));

    runner.expect(
        exit_code == 0 && next_market == 2,
        "a failed first market login must fall through to the next account");
    runner.expect(
        markets[0]->user_id == "user1" && markets[0]->password == "password1"
            && markets[1]->user_id == "user2"
            && markets[1]->password == "password2",
        "market failover must try configured account credentials in order");
    runner.expect(
        markets[0]->release_calls.load(std::memory_order_relaxed) == 1
            && markets[1]->release_calls.load(std::memory_order_relaxed) == 1,
        "each market API must be detached and released exactly once");
    runner.expect(
        traders[0]->order_insert_calls == 0
            && traders[1]->order_insert_calls == 1
            && traders[2]->order_insert_calls == 1
            && traders[3]->order_insert_calls == 1,
        "market credential failover must preserve healthy account order flow");
    runner.expect(
        output.str().find("ready=3, failed=1, submitted=3")
            != std::string::npos,
        "market failover must preserve independent account outcomes");
}

void test_live_runner_fails_over_market_subscription(
    test_support::TestRunner& runner)
{
    auto config = make_live_config(2);
    auto first = std::make_shared<FakeLiveMarketState>();
    auto second = std::make_shared<FakeLiveMarketState>();
    first->subscription_error = 9;
    std::vector<std::shared_ptr<FakeLiveMarketState>> markets{first, second};

    ctp::LiveEngineDependencies dependencies;
    dependencies.trace_root.clear();
    std::size_t next_market = 0;
    dependencies.create_market = [&markets, &next_market] {
        return std::make_unique<FakeLiveMarketApi>(markets[next_market++]);
    };
    dependencies.create_trader = [](const std::string&) {
        return make_recovering_trader(
            std::make_shared<test_support::FakeTraderMetrics>(), false);
    };

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = ctp::run_live_engine(
        config,
        output,
        error,
        [second] { return second->subscribed.load(std::memory_order_acquire); },
        std::move(dependencies));

    runner.expect(
        exit_code == 0 && next_market == 2 && first->user_id == "user1"
            && second->user_id == "user2",
        "a failed subscription must use a fresh API with the next account");
    runner.expect(
        first->release_calls.load(std::memory_order_relaxed) == 1
            && second->release_calls.load(std::memory_order_relaxed) == 1,
        "subscription failover must release both API instances once");
}

void test_live_runner_stops_after_all_market_accounts_fail(
    test_support::TestRunner& runner)
{
    auto config = make_live_config(4);
    std::vector<std::shared_ptr<FakeLiveMarketState>> markets;
    for (std::size_t index = 0; index < 4; ++index) {
        auto state = std::make_shared<FakeLiveMarketState>();
        state->login_error = 10 + static_cast<int>(index);
        markets.push_back(std::move(state));
    }

    ctp::LiveEngineDependencies dependencies;
    dependencies.trace_root.clear();
    std::size_t next_market = 0;
    dependencies.create_market = [&markets, &next_market] {
        return std::make_unique<FakeLiveMarketApi>(markets[next_market++]);
    };
    dependencies.create_trader = [](const std::string&) {
        return make_recovering_trader(
            std::make_shared<test_support::FakeTraderMetrics>(), false);
    };

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = ctp::run_live_engine(
        config, output, error, [] { return false; }, std::move(dependencies));

    bool released_once = true;
    for (const auto& market : markets) {
        released_once = released_once
            && market->release_calls.load(std::memory_order_relaxed) == 1;
    }
    runner.expect(
        exit_code == 3 && next_market == 4 && released_once,
        "the engine must stop only after every market credential fails once");
    runner.expect(
        error.str().find("all market account credentials failed")
            != std::string::npos,
        "exhausted market failover must report the global failure boundary");
}

void test_live_runner_reopens_failover_after_running(
    test_support::TestRunner& runner)
{
    auto config = make_live_config(2);
    auto first = std::make_shared<FakeLiveMarketState>();
    auto second = std::make_shared<FakeLiveMarketState>();
    std::vector<std::shared_ptr<FakeLiveMarketState>> markets{first, second};

    ctp::LiveEngineDependencies dependencies;
    dependencies.trace_root.clear();
    std::size_t next_market = 0;
    dependencies.create_market = [&markets, &next_market] {
        return std::make_unique<FakeLiveMarketApi>(markets[next_market++]);
    };
    dependencies.create_trader = [](const std::string&) {
        return make_recovering_trader(
            std::make_shared<test_support::FakeTraderMetrics>(), false);
    };

    int running_polls = 0;
    const auto stop_requested = [&] {
        if (first->subscribed.load(std::memory_order_acquire)
            && running_polls++ == 1) {
            first->login_error = 11;
            auto* spi = first->spi.load(std::memory_order_acquire);
            spi->OnFrontDisconnected(0x1001);
            spi->OnFrontConnected();
        }
        return second->subscribed.load(std::memory_order_acquire);
    };

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = ctp::run_live_engine(
        config, output, error, stop_requested, std::move(dependencies));

    runner.expect(
        exit_code == 0 && next_market == 2 && second->user_id == "user2",
        "a reconnect login failure after running must start a fresh failover round");
    runner.expect(
        first->release_calls.load(std::memory_order_relaxed) == 1
            && second->release_calls.load(std::memory_order_relaxed) == 1,
        "reconnect failover must release each market API exactly once");
}

void test_live_runner_restores_latest_identity_before_market(
    test_support::TestRunner& runner)
{
    const std::filesystem::path trace_root{
        "/tmp/ctp_batch009_live_restart"};
    std::filesystem::remove_all(trace_root);
    std::filesystem::create_directories(trace_root / "100");
    {
        ctp::AsyncTraceJournal journal{
            trace_root / "100" / "account1.csv", "account1"};
        runner.expect(journal.start(), "restart fixture journal must start");
        ctp::TraceEvent submitted{};
        submitted.trace_id = {100, 2};
        submitted.sequence = 1;
        submitted.stage = ctp::TraceStage::OrderSubmitted;
        submitted.client_order_id = 9;
        submitted.order_ref = 21;
        submitted.limit_price_ticks = 4'002;
        submitted.quantity = 1;
        submitted.direction = static_cast<std::uint8_t>(ctp::Direction::Buy);
        submitted.offset = static_cast<std::uint8_t>(ctp::Offset::Open);
        submitted.purpose = static_cast<std::uint8_t>(ctp::OrderPurpose::Entry);
        ctp::copy_to_field(submitted.instrument, "IF2609");
        runner.expect(
            journal.try_record(submitted),
            "restart fixture must retain the unresolved order");
        ctp::TraceEvent checkpoint{};
        checkpoint.sequence = 2;
        checkpoint.stage = ctp::TraceStage::RestartCheckpoint;
        ctp::copy_to_field(checkpoint.trading_day, "20260909");
        checkpoint.daily_signals = 1;
        checkpoint.daily_orders = 1;
        runner.expect(
            journal.try_record(checkpoint),
            "restart fixture must retain daily risk counters");
        journal.stop();
    }

    auto config = make_live_config(1);
    auto market = std::make_shared<FakeLiveMarketState>();
    auto trader = std::make_shared<test_support::FakeTraderMetrics>();
    ctp::LiveEngineDependencies dependencies;
    dependencies.trace_root = trace_root.string();
    dependencies.create_market = [market] {
        return std::make_unique<FakeLiveMarketApi>(market);
    };
    dependencies.create_trader = [trader](const std::string&) {
        auto api = make_recovering_trader(trader, false);
        api->on_order_query = [trader](auto& self) {
            CThostFtdcOrderField order{};
            ctp::copy_to_field(order.OrderRef, "21");
            order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
            order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
            self.spi()->OnRspQryOrder(
                &order, nullptr, trader->order_query_request_id, true);
        };
        return api;
    };

    std::atomic<bool> stop{false};
    std::thread feeder([market, &stop] {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{2};
        while (!market->subscribed.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        auto below = make_tick(0);
        below.LastPrice = 799.8;
        below.BidPrice1 = 799.6;
        below.AskPrice1 = 800.0;
        market->spi.load(std::memory_order_acquire)
            ->OnRtnDepthMarketData(&below);
        auto crossing = make_tick(1);
        crossing.LastPrice = 800.0;
        crossing.BidPrice1 = 799.8;
        crossing.AskPrice1 = 800.2;
        market->spi.load(std::memory_order_acquire)
            ->OnRtnDepthMarketData(&crossing);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        stop.store(true, std::memory_order_release);
    });

    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = ctp::run_live_engine(
        config,
        output,
        error,
        [&stop] { return stop.load(std::memory_order_acquire); },
        std::move(dependencies));
    feeder.join();

    runner.expect(
        exit_code == 0 && trader->order_query_calls == 1
            && trader->account_calls == 1 && trader->order_insert_calls == 0
            && output.str().find("ready=1") != std::string::npos,
        "live restart must reconcile the old order and suppress duplicate insert");
    std::filesystem::remove_all(trace_root);
}

void test_live_runner_refuses_unsafe_latest_trace(
    test_support::TestRunner& runner)
{
    const std::filesystem::path trace_root{
        "/tmp/ctp_batch009_unsafe_restart"};
    std::filesystem::remove_all(trace_root);
    std::filesystem::create_directories(trace_root / "101");
    {
        std::ofstream damaged{trace_root / "101" / "account1.csv"};
        damaged << "truncated trace";
    }
    auto config = make_live_config(1);
    ctp::LiveEngineDependencies dependencies;
    dependencies.trace_root = trace_root.string();
    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = ctp::run_live_engine(
        config, output, error, [] { return true; }, std::move(dependencies));
    runner.expect(
        exit_code == 3
            && error.str().find("latest trace is not safe for restart")
                != std::string::npos,
        "order-enabled startup must reject a damaged latest account trace");
    std::filesystem::remove_all(trace_root);
}

void test_unsafe_restart_freezes_only_its_account(
    test_support::TestRunner& runner)
{
    const std::filesystem::path trace_root{
        "/tmp/ctp_risk_restart_account_isolation"};
    std::filesystem::remove_all(trace_root);
    std::filesystem::create_directories(trace_root / "101");
    {
        std::ofstream damaged{trace_root / "101" / "account1.csv"};
        damaged << "truncated trace";
    }

    auto config = make_live_config(2);
    auto market = std::make_shared<FakeLiveMarketState>();
    std::size_t trader_api_count = 0;
    ctp::LiveEngineDependencies dependencies;
    dependencies.trace_root = trace_root.string();
    dependencies.create_market = [market] {
        return std::make_unique<FakeLiveMarketApi>(market);
    };
    dependencies.create_trader = [&trader_api_count](const std::string&) {
        ++trader_api_count;
        return make_recovering_trader(
            std::make_shared<test_support::FakeTraderMetrics>(), false);
    };

    std::atomic<bool> stop{false};
    std::thread stopper([market, &stop] {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds{2};
        while (!market->subscribed.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        stop.store(true, std::memory_order_release);
    });
    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = ctp::run_live_engine(
        config,
        output,
        error,
        [&stop] { return stop.load(std::memory_order_acquire); },
        std::move(dependencies));
    stopper.join();

    runner.expect(
        exit_code == 0 && trader_api_count == 1
            && output.str().find("ready=1, failed=1") != std::string::npos,
        "an unsafe account restart must not create its trader API or block a peer");
    std::filesystem::remove_all(trace_root);
}

void test_live_validation_rejects_unresolved_placeholders(
    test_support::TestRunner& runner)
{
    auto live = make_live_config(1).live();
    live.market_front = "tcp://<MARKET_HOST>:<PORT>";
    ctp::RuntimeConfig config{
        ctp::Mode::Engine,
        "simnow",
        "", "", "", "", "", "", "", "", 0,
        make_accounts(1),
        {},
        std::move(live)};
    runner.expect(
        ctp::validate_live_engine_config(config).find("placeholder")
            != std::string::npos,
        "live validation must reject unresolved configuration placeholders");
}

void test_live_validation_enforces_traceable_signal_capacity(
    test_support::TestRunner& runner)
{
    auto live = make_live_config(1).live();
    live.max_signals_per_run = ctp::kLiveSignalCapacity + 1;
    ctp::RuntimeConfig config{
        ctp::Mode::Engine,
        "simnow",
        "", "", "", "", "", "", "", "", 0,
        make_accounts(1),
        {},
        std::move(live)};
    runner.expect(
        !ctp::validate_live_engine_config(config).empty(),
        "live validation must reject runs larger than the trace capacity proof");
}

}

int main()
{
    test_support::TestRunner runner{"engine"};
    test_fixed_event_and_queue(runner);
    test_variable_account_assembly(runner);
    test_normalization_and_four_account_distribution(runner);
    test_slow_account_does_not_block_other_accounts(runner);
    test_lifecycle_invalid_data_and_hot_path_allocation(runner);
    test_price_normalization_boundaries(runner);
    test_four_account_fault_matrix(runner);
    test_live_runner_isolates_one_failed_account(runner);
    test_live_runner_enforces_margin_and_exchange_time_window(runner);
    test_live_runner_fails_over_first_market_login(runner);
    test_live_runner_fails_over_market_subscription(runner);
    test_live_runner_stops_after_all_market_accounts_fail(runner);
    test_live_runner_reopens_failover_after_running(runner);
    test_live_runner_restores_latest_identity_before_market(runner);
    test_live_runner_refuses_unsafe_latest_trace(runner);
    test_unsafe_restart_freezes_only_its_account(runner);
    test_live_validation_rejects_unresolved_placeholders(runner);
    test_live_validation_enforces_traceable_signal_capacity(runner);
    return runner.finish();
}
