#include "ctp/market_client.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
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
    int release_calls{0};
    std::string front;
    std::string broker_id;
    std::string user_id;
    std::string password;
    int request_id{0};
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
        ++metrics_->login_calls;
        metrics_->broker_id = request->BrokerID;
        metrics_->user_id = request->UserID;
        metrics_->password = request->Password;
        metrics_->request_id = request_id;
        if (on_login_request) {
            on_login_request(*this);
        }
        return login_return_code;
    }

    void release() override
    {
        ++metrics_->release_calls;
        spi_ = nullptr;
    }

    CThostFtdcMdSpi* spi() const { return spi_; }

    std::function<void(FakeMarketApi&)> on_init;
    std::function<void(FakeMarketApi&)> on_login_request;
    int login_return_code{0};

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

    ctp::MarketResult result;
    {
        ctp::MarketClient client{market_config(), std::move(api)};
        result = client.run(std::chrono::milliseconds{20});
    }

    runner.expect(
        result.state == ctp::MarketState::LoginSucceeded,
        "successful response must finish in LoginSucceeded");
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
        ctp::market_exit_code(ctp::MarketState::LoginSucceeded) == 0,
        "successful login must map to exit code 0");
    runner.expect(
        ctp::market_exit_code(ctp::MarketState::Disconnected) == 4,
        "disconnect must map to exit code 4");
    runner.expect(
        ctp::market_exit_code(ctp::MarketState::TimedOut) == 4,
        "timeout must map to exit code 4");
    runner.expect(
        ctp::market_exit_code(ctp::MarketState::LoginFailed) == 5,
        "login failure must map to exit code 5");
}

}

int main()
{
    TestRunner runner;
    test_success_and_lifecycle(runner);
    test_immediate_request_failure(runner);
    test_business_login_failure(runner);
    test_disconnect_and_timeout(runner);
    test_null_and_late_callbacks(runner);
    test_exit_codes(runner);
    return runner.finish();
}
