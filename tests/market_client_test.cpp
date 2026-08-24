#include "ctp/market_client.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace {

class TestRunner {
public:
    void expect(bool condition, std::string_view message)
    {
        if (!condition) {
            ++failures_;
            std::cerr << "[fail] " << message << '\n';
        }
    }

    int finish() const
    {
        if (failures_ == 0) {
            std::cout << "[ok] all market client tests passed\n";
        }
        return failures_ == 0 ? 0 : 1;
    }

private:
    int failures_{0};
};

struct FakeMetrics {
    int register_spi_calls{0};
    int init_calls{0};
    int login_calls{0};
    int subscribe_calls{0};
    int subscribe_count{0};
    int release_calls{0};
    std::string front;
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string subscribed_instrument;
    int request_id{0};
    std::atomic<bool> login_request_active{false};
    std::atomic<bool> released_during_login_request{false};
};

class FakeMarketApi final : public ctp::MarketApi {
public:
    explicit FakeMarketApi(std::shared_ptr<FakeMetrics> metrics)
        : metrics_(std::move(metrics))
    {
    }

    void register_spi(CThostFtdcMdSpi* spi) override
    {
        ++metrics_->register_spi_calls;
        spi_ = spi;
    }

    void register_front(const std::string& front) override
    {
        metrics_->front = front;
    }

    void init() override
    {
        ++metrics_->init_calls;
        if (on_init) {
            on_init(*this);
        }
    }

    int request_user_login(
        CThostFtdcReqUserLoginField* request,
        int request_id) override
    {
        metrics_->login_request_active = true;
        ++metrics_->login_calls;
        metrics_->broker_id = request->BrokerID;
        metrics_->user_id = request->UserID;
        metrics_->password = request->Password;
        metrics_->request_id = request_id;
        if (on_login_request) {
            on_login_request(*this);
        }
        metrics_->login_request_active = false;
        return login_return_code;
    }

    int subscribe_market_data(char* instruments[], int count) override
    {
        ++metrics_->subscribe_calls;
        metrics_->subscribe_count = count;
        if (count > 0 && instruments != nullptr && instruments[0] != nullptr) {
            metrics_->subscribed_instrument = instruments[0];
        }
        if (on_subscribe) {
            on_subscribe(*this);
        }
        return subscribe_return_code;
    }

    void release() override
    {
        if (metrics_->login_request_active) {
            metrics_->released_during_login_request = true;
        }
        ++metrics_->release_calls;
        spi_ = nullptr;
    }

    CThostFtdcMdSpi* spi() const { return spi_; }

    std::function<void(FakeMarketApi&)> on_init;
    std::function<void(FakeMarketApi&)> on_login_request;
    std::function<void(FakeMarketApi&)> on_subscribe;
    int login_return_code{0};
    int subscribe_return_code{0};

private:
    std::shared_ptr<FakeMetrics> metrics_;
    CThostFtdcMdSpi* spi_{nullptr};
};

ctp::RuntimeConfig market_config()
{
    return {
        ctp::Mode::Market,
        "simnow-7x24",
        "9999",
        "123456",
        "example-password",
        "",
        "",
        "tcp://127.0.0.1:10001",
        "tcp://127.0.0.1:10002",
        "IF2609",
        5};
}

void respond_with_success(FakeMarketApi& api)
{
    CThostFtdcRspUserLoginField response{};
    CThostFtdcRspInfoField info{};
    api.spi()->OnRspUserLogin(&response, &info, 1, true);
}

void respond_to_subscription(
    FakeMarketApi& api, int error_code, std::string_view error_message)
{
    CThostFtdcSpecificInstrumentField instrument{};
    CThostFtdcRspInfoField info{};
    ctp::copy_to_field(instrument.InstrumentID, "IF2609");
    info.ErrorID = error_code;
    ctp::copy_to_field(info.ErrorMsg, error_message);
    api.spi()->OnRspSubMarketData(&instrument, &info, 0, true);
}

void emit_ticks(FakeMarketApi& api, int count)
{
    for (int index = 0; index < count; ++index) {
        CThostFtdcDepthMarketDataField tick{};
        ctp::copy_to_field(tick.InstrumentID, "IF2609");
        ctp::copy_to_field(tick.UpdateTime, "09:30:00");
        tick.UpdateMillisec = index;
        tick.LastPrice = 100.0 + index;
        tick.BidPrice1 = 99.0 + index;
        tick.AskPrice1 = 101.0 + index;
        tick.Volume = 1000 + index;
        api.spi()->OnRtnDepthMarketData(&tick);
    }
}

enum class MarketWaitPhase {
    Connecting,
    Login,
    Subscription,
    Tick,
};

std::unique_ptr<FakeMarketApi> market_api_waiting_at(
    const std::shared_ptr<FakeMetrics>& metrics,
    MarketWaitPhase phase,
    bool disconnect)
{
    auto api = std::make_unique<FakeMarketApi>(metrics);
    if (phase == MarketWaitPhase::Connecting) {
        if (disconnect) {
            api->on_init = [](FakeMarketApi& fake) {
                fake.spi()->OnFrontDisconnected(0x2001);
            };
        }
        return api;
    }

    api->on_init = [](FakeMarketApi& fake) {
        fake.spi()->OnFrontConnected();
    };
    if (phase == MarketWaitPhase::Login) {
        if (disconnect) {
            api->on_login_request = [](FakeMarketApi& fake) {
                fake.spi()->OnFrontDisconnected(0x2001);
            };
        }
        return api;
    }

    api->on_login_request = respond_with_success;
    if (phase == MarketWaitPhase::Subscription) {
        if (disconnect) {
            api->on_subscribe = [](FakeMarketApi& fake) {
                fake.spi()->OnFrontDisconnected(0x2001);
            };
        }
        return api;
    }

    api->on_subscribe = [disconnect](FakeMarketApi& fake) {
        respond_to_subscription(fake, 0, {});
        if (disconnect) {
            fake.spi()->OnFrontDisconnected(0x2001);
        }
    };
    return api;
}

void test_subscription_response_is_required(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    api->on_init = [](FakeMarketApi& fake) { fake.spi()->OnFrontConnected(); };
    api->on_login_request = respond_with_success;

    ctp::MarketClient client{market_config(), std::move(api)};
    const auto result = client.run(std::chrono::milliseconds{2});

    runner.expect(
        result.state == ctp::MarketState::TimedOut,
        "missing subscription response must not report success");
}

void test_subscription_business_failure(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    api->on_init = [](FakeMarketApi& fake) { fake.spi()->OnFrontConnected(); };
    api->on_login_request = respond_with_success;
    api->on_subscribe = [](FakeMarketApi& fake) {
        respond_to_subscription(fake, 9, "unknown instrument");
    };

    ctp::MarketClient client{market_config(), std::move(api)};
    const auto result = client.run(std::chrono::milliseconds{20});

    runner.expect(
        result.state == ctp::MarketState::SubscriptionFailed,
        "subscription business error must fail the operation");
    runner.expect(result.error_code == 9, "subscription ErrorID must be preserved");
    runner.expect(
        result.error_message == "unknown instrument",
        "subscription ErrorMsg must be preserved");
}

void test_immediate_subscription_failure(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    api->subscribe_return_code = -3;
    api->on_init = [](FakeMarketApi& fake) { fake.spi()->OnFrontConnected(); };
    api->on_login_request = respond_with_success;

    ctp::MarketClient client{market_config(), std::move(api)};
    const auto result = client.run(std::chrono::milliseconds{20});

    runner.expect(
        result.state == ctp::MarketState::SubscriptionFailed,
        "immediate subscription rejection must use its own failure state");
    runner.expect(result.error_code == -3, "subscription return code must be kept");
    runner.expect(
        ctp::market_exit_code(result.state) == 6,
        "subscription failure must map to exit code 6");
    runner.expect(metrics->release_calls == 1, "subscription failure must release once");
}

void test_insufficient_ticks_time_out(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    api->on_init = [](FakeMarketApi& fake) { fake.spi()->OnFrontConnected(); };
    api->on_login_request = respond_with_success;
    api->on_subscribe = [](FakeMarketApi& fake) {
        respond_to_subscription(fake, 0, {});
        emit_ticks(fake, 4);
    };

    ctp::MarketClient client{market_config(), std::move(api)};
    const auto result = client.run(std::chrono::milliseconds{2});

    runner.expect(
        result.state == ctp::MarketState::TimedOut,
        "fewer than configured ticks must not complete");
}

void test_login_starts_single_subscription(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    api->on_init = [](FakeMarketApi& fake) {
        fake.spi()->OnFrontConnected();
    };
    api->on_login_request = [](FakeMarketApi& fake) {
        respond_with_success(fake);
    };

    ctp::MarketClient client{market_config(), std::move(api)};
    client.run(std::chrono::milliseconds{2});

    runner.expect(metrics->subscribe_calls == 1, "login must subscribe once");
    runner.expect(metrics->subscribe_count == 1, "one instrument must be subscribed");
    runner.expect(
        metrics->subscribed_instrument == "IF2609",
        "configured instrument must be subscribed");
}

void test_success_and_lifecycle(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    api->on_init = [](FakeMarketApi& fake) {
        fake.spi()->OnFrontConnected();
        fake.spi()->OnFrontConnected();
    };
    api->on_login_request = [](FakeMarketApi& fake) {
        respond_with_success(fake);
        fake.spi()->OnFrontDisconnected(0x2001);
    };
    api->on_subscribe = [](FakeMarketApi& fake) {
        respond_to_subscription(fake, 0, {});
        emit_ticks(fake, 6);
    };

    ctp::MarketResult result;
    {
        ctp::MarketClient client{market_config(), std::move(api)};
        result = client.run(std::chrono::milliseconds{20});
    }

    runner.expect(
        result.state == ctp::MarketState::Completed,
        "configured tick count must finish in Completed");
    runner.expect(result.ticks.size() == 5, "exactly five ticks must be retained");
    runner.expect(result.ticks.front().last_price == 100.0, "first price must match");
    runner.expect(result.ticks.back().volume == 1004, "late tick must be ignored");
    runner.expect(metrics->register_spi_calls == 1, "SPI must be registered once");
    runner.expect(metrics->init_calls == 1, "API must be initialized once");
    runner.expect(metrics->login_calls == 1, "duplicate connect must not repeat login");
    runner.expect(metrics->request_id == 1, "first login request ID must be 1");
    runner.expect(metrics->broker_id == "9999", "BrokerID must be copied");
    runner.expect(metrics->user_id == "123456", "UserID must be copied");
    runner.expect(
        metrics->password == "example-password",
        "Password must be copied without being logged");
    runner.expect(
        metrics->front == "tcp://127.0.0.1:10001",
        "configured market front must be registered");
    runner.expect(metrics->release_calls == 1, "API must be released exactly once");
}

void test_immediate_request_failure(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    api->login_return_code = -2;
    api->on_init = [](FakeMarketApi& fake) { fake.spi()->OnFrontConnected(); };

    ctp::MarketClient client{market_config(), std::move(api)};
    const auto result = client.run(std::chrono::milliseconds{20});

    runner.expect(
        result.state == ctp::MarketState::LoginFailed,
        "immediate request rejection must fail login");
    runner.expect(result.error_code == -2, "request return code must be preserved");
    runner.expect(metrics->release_calls == 1, "rejected request must release once");
}

void test_business_login_failure(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    api->on_init = [](FakeMarketApi& fake) { fake.spi()->OnFrontConnected(); };
    api->on_login_request = [](FakeMarketApi& fake) {
        CThostFtdcRspInfoField info{};
        info.ErrorID = 7;
        ctp::copy_to_field(info.ErrorMsg, "invalid credentials");
        fake.spi()->OnRspUserLogin(nullptr, &info, 1, true);
    };

    ctp::MarketClient client{market_config(), std::move(api)};
    const auto result = client.run(std::chrono::milliseconds{20});

    runner.expect(
        result.state == ctp::MarketState::LoginFailed,
        "CTP business error must fail login");
    runner.expect(result.error_code == 7, "CTP ErrorID must be preserved");
    runner.expect(
        result.error_message == "invalid credentials",
        "CTP ErrorMsg must be preserved");
    runner.expect(metrics->release_calls == 1, "business failure must release once");
}

void test_disconnect_and_timeout(TestRunner& runner)
{
    auto disconnect_metrics = std::make_shared<FakeMetrics>();
    auto disconnect_api = std::make_unique<FakeMarketApi>(disconnect_metrics);
    disconnect_api->on_init = [](FakeMarketApi& fake) {
        fake.spi()->OnFrontDisconnected(0x2001);
    };

    ctp::MarketClient disconnected{market_config(), std::move(disconnect_api)};
    const auto disconnect_result =
        disconnected.run(std::chrono::milliseconds{20});
    runner.expect(
        disconnect_result.state == ctp::MarketState::Disconnected,
        "disconnect must wake the waiting thread");
    runner.expect(
        disconnect_result.error_code == 0x2001,
        "disconnect reason must be preserved");
    runner.expect(
        disconnect_metrics->release_calls == 1,
        "disconnect must release once");

    auto timeout_metrics = std::make_shared<FakeMetrics>();
    auto timeout_api = std::make_unique<FakeMarketApi>(timeout_metrics);
    ctp::MarketClient timed_out{market_config(), std::move(timeout_api)};
    const auto timeout_result = timed_out.run(std::chrono::milliseconds{2});
    runner.expect(
        timeout_result.state == ctp::MarketState::TimedOut,
        "missing callbacks must produce a finite timeout");
    runner.expect(timeout_metrics->release_calls == 1, "timeout must release once");
}

void test_interruption_stops_market_client(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    ctp::MarketClient client{market_config(), std::move(api)};

    const auto result = client.run(
        std::chrono::milliseconds{100}, [] { return true; });
    client.OnFrontConnected();

    runner.expect(
        result.state == ctp::MarketState::Interrupted,
        "stop request must interrupt the market client");
    runner.expect(
        ctp::market_exit_code(result.state) == 130,
        "market interruption must map to exit code 130");
    runner.expect(metrics->login_calls == 0,
                  "late callbacks must not advance after interruption");
    runner.expect(metrics->release_calls == 1,
                  "interruption must release MarketApi once");
}

void test_all_market_wait_phases_terminate(TestRunner& runner)
{
    const MarketWaitPhase phases[]{
        MarketWaitPhase::Connecting,
        MarketWaitPhase::Login,
        MarketWaitPhase::Subscription,
        MarketWaitPhase::Tick};

    for (const auto phase : phases) {
        auto timeout_metrics = std::make_shared<FakeMetrics>();
        ctp::MarketClient timed_out{
            market_config(),
            market_api_waiting_at(timeout_metrics, phase, false)};
        const auto timeout_result =
            timed_out.run(std::chrono::milliseconds{2});
        runner.expect(timeout_result.state == ctp::MarketState::TimedOut,
                      "every market wait phase must time out");
        runner.expect(timeout_metrics->release_calls == 1,
                      "every market timeout must release once");

        auto interrupt_metrics = std::make_shared<FakeMetrics>();
        ctp::MarketClient interrupted{
            market_config(),
            market_api_waiting_at(interrupt_metrics, phase, false)};
        const auto interrupt_result = interrupted.run(
            std::chrono::milliseconds{100}, [] { return true; });
        runner.expect(interrupt_result.state == ctp::MarketState::Interrupted,
                      "every market wait phase must be interruptible");
        runner.expect(interrupt_metrics->release_calls == 1,
                      "every market interruption must release once");

        auto disconnect_metrics = std::make_shared<FakeMetrics>();
        ctp::MarketClient disconnected{
            market_config(),
            market_api_waiting_at(disconnect_metrics, phase, true)};
        const auto disconnect_result =
            disconnected.run(std::chrono::milliseconds{100});
        runner.expect(disconnect_result.state == ctp::MarketState::Disconnected,
                      "every market wait phase must handle disconnection");
        runner.expect(disconnect_metrics->release_calls == 1,
                      "every market disconnection must release once");
    }
}

void test_release_waits_for_active_login_request(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    std::promise<void> initialized;
    std::promise<void> login_entered;
    std::promise<void> allow_login_return;
    auto allow_login_future = allow_login_return.get_future().share();
    api->on_init = [&initialized](FakeMarketApi&) { initialized.set_value(); };
    api->on_login_request = [&login_entered, allow_login_future](FakeMarketApi&) {
        login_entered.set_value();
        allow_login_future.wait();
    };

    auto client = std::make_unique<ctp::MarketClient>(
        market_config(), std::move(api));
    std::atomic<bool> stop_requested{false};
    auto run = std::async(std::launch::async, [&] {
        return client->run(std::chrono::seconds{2}, [&] {
            return stop_requested.load();
        });
    });
    initialized.get_future().wait();

    auto callback = std::async(
        std::launch::async, [&client] { client->OnFrontConnected(); });
    login_entered.get_future().wait();
    stop_requested = true;
    runner.expect(
        run.wait_for(std::chrono::milliseconds{250}) ==
            std::future_status::timeout,
        "interruption must wait for the active login request to return");
    allow_login_return.set_value();

    callback.get();
    const auto result = run.get();
    runner.expect(result.state == ctp::MarketState::Interrupted,
                  "active login request must still allow bounded interruption");
    runner.expect(!metrics->released_during_login_request,
                  "MarketApi must not be released during an active request");
    runner.expect(metrics->release_calls == 1,
                  "concurrent interruption must release MarketApi once");
}

void test_null_and_late_callbacks(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeMarketApi>(metrics);
    api->on_init = [](FakeMarketApi& fake) { fake.spi()->OnFrontConnected(); };
    api->on_login_request = [](FakeMarketApi& fake) {
        fake.spi()->OnRspUserLogin(nullptr, nullptr, 1, true);
        respond_with_success(fake);
    };

    ctp::MarketClient client{market_config(), std::move(api)};
    const auto result = client.run(std::chrono::milliseconds{20});

    runner.expect(
        result.state == ctp::MarketState::LoginFailed,
        "missing login response must fail safely");
    runner.expect(metrics->release_calls == 1, "missing response must release once");
}

void test_exit_codes(TestRunner& runner)
{
    runner.expect(
        ctp::market_exit_code(ctp::MarketState::Completed) == 0,
        "completed market data must map to exit code 0");
    runner.expect(
        ctp::market_exit_code(ctp::MarketState::Disconnected) == 4,
        "disconnect must map to exit code 4");
    runner.expect(
        ctp::market_exit_code(ctp::MarketState::TimedOut) == 4,
        "timeout must map to exit code 4");
    runner.expect(
        ctp::market_exit_code(ctp::MarketState::LoginFailed) == 5,
        "login failure must map to exit code 5");
    runner.expect(
        ctp::market_exit_code(ctp::MarketState::SubscriptionFailed) == 6,
        "subscription failure must map to exit code 6");
}

void test_tick_format(TestRunner& runner)
{
    const ctp::MarketTick tick{
        "IF2609",
        "09:30:00",
        7,
        123.45,
        123.4,
        123.5,
        88};

    runner.expect(
        ctp::format_market_tick(tick) ==
            "time=09:30:00.007 instrument=IF2609 last=123.45 "
            "bid1=123.4 ask1=123.5 volume=88",
        "tick output must use the stable key-value format");
}

void test_completed_result_output(TestRunner& runner)
{
    ctp::MarketResult result;
    result.state = ctp::MarketState::Completed;
    result.ticks = {
        {"IF2609", "09:30:00", 7, 123.45, 123.4, 123.5, 88},
        {"IF2609", "09:30:01", 12, 123.55, 123.5, 123.6, 90}};
    std::ostringstream output;
    std::ostringstream error;

    const int exit_code = ctp::report_market_result(result, output, error);

    runner.expect(exit_code == 0, "completed market data must exit successfully");
    runner.expect(
        output.str() ==
            "time=09:30:00.007 instrument=IF2609 last=123.45 "
            "bid1=123.4 ask1=123.5 volume=88\n"
            "time=09:30:01.012 instrument=IF2609 last=123.55 "
            "bid1=123.5 ask1=123.6 volume=90\n"
            "[ok] market data completed: ticks=2\n",
        "completed result must print every tick and the retained count");
    runner.expect(error.str().empty(), "success must not write to stderr");
}

}

int main()
{
    TestRunner runner;
    test_login_starts_single_subscription(runner);
    test_subscription_response_is_required(runner);
    test_subscription_business_failure(runner);
    test_immediate_subscription_failure(runner);
    test_insufficient_ticks_time_out(runner);
    test_success_and_lifecycle(runner);
    test_immediate_request_failure(runner);
    test_business_login_failure(runner);
    test_disconnect_and_timeout(runner);
    test_interruption_stops_market_client(runner);
    test_all_market_wait_phases_terminate(runner);
    test_release_waits_for_active_login_request(runner);
    test_null_and_late_callbacks(runner);
    test_exit_codes(runner);
    test_tick_format(runner);
    test_completed_result_output(runner);
    return runner.finish();
}
