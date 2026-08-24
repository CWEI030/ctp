#include "ctp/trader_client.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
            std::cout << "[ok] all trader client tests passed\n";
        }
        return failures_ == 0 ? 0 : 1;
    }

private:
    int failures_{0};
};

struct FakeMetrics {
    std::vector<std::string> calls;
    int authenticate_calls{0};
    int login_calls{0};
    int account_calls{0};
    int position_calls{0};
    int release_calls{0};
    int authenticate_request_id{0};
    int login_request_id{0};
    int account_request_id{0};
    int position_request_id{0};
    std::string front;
    std::string broker_id;
    std::string user_id;
    std::string app_id;
    std::string auth_code;
    std::string password;
    std::string account_broker_id;
    std::string account_investor_id;
    std::string position_broker_id;
    std::string position_investor_id;
    THOST_TE_RESUME_TYPE private_mode{THOST_TERT_RESTART};
    THOST_TE_RESUME_TYPE public_mode{THOST_TERT_RESTART};
    int private_sequence{0};
    std::atomic<bool> authenticate_request_active{false};
    std::atomic<bool> released_during_authenticate_request{false};
};

class FakeTraderApi final : public ctp::TraderApi {
public:
    explicit FakeTraderApi(std::shared_ptr<FakeMetrics> metrics)
        : metrics_(std::move(metrics))
    {
    }

    void register_spi(CThostFtdcTraderSpi* spi) override
    {
        metrics_->calls.push_back("spi");
        spi_ = spi;
    }

    void subscribe_private_topic(
        THOST_TE_RESUME_TYPE resume_type, int sequence) override
    {
        metrics_->calls.push_back("private");
        metrics_->private_mode = resume_type;
        metrics_->private_sequence = sequence;
    }

    void subscribe_public_topic(THOST_TE_RESUME_TYPE resume_type) override
    {
        metrics_->calls.push_back("public");
        metrics_->public_mode = resume_type;
    }

    void register_front(const std::string& front) override
    {
        metrics_->calls.push_back("front");
        metrics_->front = front;
    }

    void init() override
    {
        metrics_->calls.push_back("init");
        if (on_init) {
            on_init(*this);
        }
    }

    int request_authenticate(
        CThostFtdcReqAuthenticateField* request, int request_id) override
    {
        metrics_->authenticate_request_active = true;
        ++metrics_->authenticate_calls;
        metrics_->authenticate_request_id = request_id;
        metrics_->broker_id = request->BrokerID;
        metrics_->user_id = request->UserID;
        metrics_->app_id = request->AppID;
        metrics_->auth_code = request->AuthCode;
        if (on_authenticate) {
            on_authenticate(*this);
        }
        metrics_->authenticate_request_active = false;
        return authenticate_return_code;
    }

    int request_user_login(
        CThostFtdcReqUserLoginField* request, int request_id) override
    {
        ++metrics_->login_calls;
        metrics_->login_request_id = request_id;
        metrics_->password = request->Password;
        if (on_login) {
            on_login(*this);
        }
        return login_return_code;
    }

    int request_trading_account(
        CThostFtdcQryTradingAccountField* request, int request_id) override
    {
        ++metrics_->account_calls;
        metrics_->account_request_id = request_id;
        metrics_->account_broker_id = request->BrokerID;
        metrics_->account_investor_id = request->InvestorID;
        if (on_account) {
            on_account(*this);
        }
        return account_return_code;
    }

    int request_investor_position(
        CThostFtdcQryInvestorPositionField* request, int request_id) override
    {
        ++metrics_->position_calls;
        metrics_->position_request_id = request_id;
        metrics_->position_broker_id = request->BrokerID;
        metrics_->position_investor_id = request->InvestorID;
        if (on_position) {
            on_position(*this);
        }
        return position_return_code;
    }

    void release() override
    {
        if (metrics_->authenticate_request_active) {
            metrics_->released_during_authenticate_request = true;
        }
        ++metrics_->release_calls;
        spi_ = nullptr;
    }

    CThostFtdcTraderSpi* spi() const { return spi_; }

    std::function<void(FakeTraderApi&)> on_init;
    std::function<void(FakeTraderApi&)> on_authenticate;
    std::function<void(FakeTraderApi&)> on_login;
    std::function<void(FakeTraderApi&)> on_account;
    std::function<void(FakeTraderApi&)> on_position;
    int authenticate_return_code{0};
    int login_return_code{0};
    int account_return_code{0};
    int position_return_code{0};
private:
    std::shared_ptr<FakeMetrics> metrics_;
    CThostFtdcTraderSpi* spi_{nullptr};
};

ctp::RuntimeConfig account_config()
{
    return {
        ctp::Mode::Account,
        "simnow-7x24",
        "9999",
        "123456",
        "secret-password",
        "test-app",
        "secret-auth-code",
        "tcp://127.0.0.1:10001",
        "tcp://127.0.0.1:10002",
        "",
        0};
}

void authenticate_success(FakeTraderApi& api, bool is_last = true)
{
    CThostFtdcRspAuthenticateField response{};
    CThostFtdcRspInfoField info{};
    api.spi()->OnRspAuthenticate(&response, &info, 1, is_last);
}

void login_success(FakeTraderApi& api, bool is_last = true)
{
    CThostFtdcRspUserLoginField response{};
    CThostFtdcRspInfoField info{};
    api.spi()->OnRspUserLogin(&response, &info, 2, is_last);
}

void account_success(FakeTraderApi& api, bool is_last = true)
{
    CThostFtdcTradingAccountField response{};
    ctp::copy_to_field(response.AccountID, "123456");
    response.Balance = 100000.5;
    response.Available = 80000.25;
    response.CurrMargin = 15000.75;
    CThostFtdcRspInfoField info{};
    api.spi()->OnRspQryTradingAccount(&response, &info, 3, is_last);
}

void position_success(
    FakeTraderApi& api,
    std::string_view instrument,
    char direction,
    int position,
    int today,
    int yesterday,
    bool is_last)
{
    CThostFtdcInvestorPositionField response{};
    ctp::copy_to_field(response.InstrumentID, instrument);
    response.PosiDirection = direction;
    response.Position = position;
    response.TodayPosition = today;
    response.YdPosition = yesterday;
    CThostFtdcRspInfoField info{};
    api.spi()->OnRspQryInvestorPosition(&response, &info, 4, is_last);
}

enum class TraderWaitPhase {
    Connecting,
    Authentication,
    Login,
    TradingAccount,
    InvestorPosition,
};

std::unique_ptr<FakeTraderApi> trader_api_waiting_at(
    const std::shared_ptr<FakeMetrics>& metrics,
    TraderWaitPhase phase,
    bool disconnect)
{
    auto api = std::make_unique<FakeTraderApi>(metrics);
    if (phase == TraderWaitPhase::Connecting) {
        if (disconnect) {
            api->on_init = [](FakeTraderApi& value) {
                value.spi()->OnFrontDisconnected(0x2001);
            };
        }
        return api;
    }

    api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontConnected();
    };
    if (phase == TraderWaitPhase::Authentication) {
        if (disconnect) {
            api->on_authenticate = [](FakeTraderApi& value) {
                value.spi()->OnFrontDisconnected(0x2001);
            };
        }
        return api;
    }

    api->on_authenticate = [](FakeTraderApi& value) {
        authenticate_success(value);
    };
    if (phase == TraderWaitPhase::Login) {
        if (disconnect) {
            api->on_login = [](FakeTraderApi& value) {
                value.spi()->OnFrontDisconnected(0x2001);
            };
        }
        return api;
    }

    api->on_login = [](FakeTraderApi& value) {
        login_success(value);
    };
    if (phase == TraderWaitPhase::TradingAccount) {
        if (disconnect) {
            api->on_account = [](FakeTraderApi& value) {
                value.spi()->OnFrontDisconnected(0x2001);
            };
        }
        return api;
    }

    api->on_account = [](FakeTraderApi& value) {
        account_success(value);
    };
    if (disconnect) {
        api->on_position = [](FakeTraderApi& value) {
            value.spi()->OnFrontDisconnected(0x2001);
        };
    }
    return api;
}

void test_success_advances_once(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeTraderApi>(metrics);
    api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontConnected();
        value.spi()->OnFrontConnected();
    };
    api->on_authenticate = [](FakeTraderApi& value) {
        authenticate_success(value, false);
        authenticate_success(value);
        authenticate_success(value);
    };
    api->on_login = [](FakeTraderApi& value) {
        login_success(value, false);
        login_success(value);
        login_success(value);
        value.spi()->OnFrontDisconnected(0x2001);
    };
    api->on_account = [](FakeTraderApi& value) {
        account_success(value, false);
        value.spi()->OnRspQryTradingAccount(nullptr, nullptr, 3, true);
        account_success(value);
    };
    api->on_position = [](FakeTraderApi& value) {
        position_success(value, "rb2610", THOST_FTDC_PD_Long, 3, 1, 2, false);
        position_success(value, "ag2612", THOST_FTDC_PD_Short, 2, 2, 0, true);
        position_success(value, "late", THOST_FTDC_PD_Net, 9, 9, 0, true);
    };

    ctp::TraderResult result;
    {
        ctp::TraderClient client{account_config(), std::move(api)};
        result = client.run(std::chrono::milliseconds{20});
    }

    runner.expect(
        result.state == ctp::TraderState::Completed,
        "successful serial queries must complete");
    runner.expect(
        metrics->calls == std::vector<std::string>{
            "spi", "private", "public", "front", "init"},
        "TraderApi initialization order must match the SDK contract");
    runner.expect(
        metrics->private_mode == THOST_TERT_QUICK &&
            metrics->public_mode == THOST_TERT_QUICK &&
            metrics->private_sequence == 1,
        "public and private topics must use quick mode");
    runner.expect(metrics->authenticate_calls == 1, "authentication must run once");
    runner.expect(metrics->login_calls == 1, "login must run once");
    runner.expect(metrics->account_calls == 1, "account query must run once");
    runner.expect(metrics->position_calls == 1, "position query must run once");
    runner.expect(metrics->authenticate_request_id == 1, "authentication ID must be 1");
    runner.expect(metrics->login_request_id == 2, "login ID must be 2");
    runner.expect(metrics->account_request_id == 3, "account query ID must be 3");
    runner.expect(metrics->position_request_id == 4, "position query ID must be 4");
    runner.expect(metrics->front == "tcp://127.0.0.1:10002", "trader front must be used");
    runner.expect(metrics->broker_id == "9999", "BrokerID must be copied");
    runner.expect(metrics->user_id == "123456", "UserID must be copied");
    runner.expect(metrics->app_id == "test-app", "AppID must be copied");
    runner.expect(metrics->auth_code == "secret-auth-code", "AuthCode must be copied");
    runner.expect(metrics->password == "secret-password", "Password must be copied");
    runner.expect(metrics->account_broker_id == "9999" &&
                      metrics->account_investor_id == "123456",
                  "account query identity must be copied");
    runner.expect(metrics->position_broker_id == "9999" &&
                      metrics->position_investor_id == "123456",
                  "position query identity must be copied");
    runner.expect(result.account.has_value(), "account summary must be collected");
    runner.expect(result.account && result.account->available == 80000.25,
                  "available funds must survive the callback");
    runner.expect(result.positions.size() == 2,
                  "all position rows before completion must be collected");
    runner.expect(result.positions.size() == 2 &&
                      result.positions[1].instrument_id == "ag2612" &&
                      result.positions[1].position == 2,
                  "position row fields must survive the callback");
    runner.expect(metrics->release_calls == 1, "TraderApi must be released once");
}

void test_immediate_and_business_failures(TestRunner& runner)
{
    auto rejected_metrics = std::make_shared<FakeMetrics>();
    auto rejected_api = std::make_unique<FakeTraderApi>(rejected_metrics);
    rejected_api->authenticate_return_code = -2;
    rejected_api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontConnected();
    };
    ctp::TraderClient rejected{account_config(), std::move(rejected_api)};
    const auto rejected_result = rejected.run(std::chrono::milliseconds{20});
    runner.expect(
        rejected_result.state == ctp::TraderState::AuthenticationFailed,
        "immediate authentication rejection must fail");
    runner.expect(rejected_result.error_code == -2, "request rejection code must survive");

    auto failed_metrics = std::make_shared<FakeMetrics>();
    auto failed_api = std::make_unique<FakeTraderApi>(failed_metrics);
    failed_api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontConnected();
    };
    failed_api->on_authenticate = [](FakeTraderApi& value) {
        CThostFtdcRspInfoField info{};
        info.ErrorID = 7;
        ctp::copy_to_field(info.ErrorMsg, "authentication denied");
        value.spi()->OnRspAuthenticate(nullptr, &info, 1, true);
    };
    ctp::TraderClient failed{account_config(), std::move(failed_api)};
    const auto failed_result = failed.run(std::chrono::milliseconds{20});
    runner.expect(
        failed_result.state == ctp::TraderState::AuthenticationFailed,
        "authentication business error must fail");
    runner.expect(failed_result.error_code == 7, "business ErrorID must survive");
    runner.expect(
        failed_result.error_message == "authentication denied",
        "business ErrorMsg must survive");
    runner.expect(rejected_metrics->release_calls == 1,
                  "rejected authentication must release TraderApi once");
    runner.expect(failed_metrics->release_calls == 1,
                  "failed authentication must release TraderApi once");
}

void test_login_failure_and_missing_response(TestRunner& runner)
{
    auto failed_metrics = std::make_shared<FakeMetrics>();
    auto failed_api = std::make_unique<FakeTraderApi>(failed_metrics);
    failed_api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontConnected();
    };
    failed_api->on_authenticate = [](FakeTraderApi& value) {
        authenticate_success(value);
    };
    failed_api->on_login = [](FakeTraderApi& value) {
        CThostFtdcRspInfoField info{};
        info.ErrorID = 8;
        ctp::copy_to_field(info.ErrorMsg, "login denied");
        value.spi()->OnRspUserLogin(nullptr, &info, 2, true);
    };
    ctp::TraderClient failed{account_config(), std::move(failed_api)};
    const auto failed_result = failed.run(std::chrono::milliseconds{20});
    runner.expect(
        failed_result.state == ctp::TraderState::LoginFailed,
        "login business error must fail");
    runner.expect(failed_result.error_code == 8, "login ErrorID must survive");

    auto rejected_metrics = std::make_shared<FakeMetrics>();
    auto rejected_api = std::make_unique<FakeTraderApi>(rejected_metrics);
    rejected_api->login_return_code = -3;
    rejected_api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontConnected();
    };
    rejected_api->on_authenticate = [](FakeTraderApi& value) {
        authenticate_success(value);
    };
    ctp::TraderClient rejected{account_config(), std::move(rejected_api)};
    const auto rejected_result = rejected.run(std::chrono::milliseconds{20});
    runner.expect(rejected_result.state == ctp::TraderState::LoginFailed,
                  "immediate login rejection must fail");
    runner.expect(rejected_result.error_code == -3,
                  "login request rejection code must survive");

    auto missing_metrics = std::make_shared<FakeMetrics>();
    auto missing_api = std::make_unique<FakeTraderApi>(missing_metrics);
    missing_api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontConnected();
    };
    missing_api->on_authenticate = [](FakeTraderApi& value) {
        value.spi()->OnRspAuthenticate(nullptr, nullptr, 1, true);
    };
    ctp::TraderClient missing{account_config(), std::move(missing_api)};
    const auto missing_result = missing.run(std::chrono::milliseconds{20});
    runner.expect(
        missing_result.state == ctp::TraderState::AuthenticationFailed,
        "missing authentication response must fail safely");

    auto missing_login_metrics = std::make_shared<FakeMetrics>();
    auto missing_login_api = std::make_unique<FakeTraderApi>(missing_login_metrics);
    missing_login_api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontConnected();
    };
    missing_login_api->on_authenticate = [](FakeTraderApi& value) {
        authenticate_success(value);
    };
    missing_login_api->on_login = [](FakeTraderApi& value) {
        value.spi()->OnRspUserLogin(nullptr, nullptr, 2, true);
    };
    ctp::TraderClient missing_login{account_config(), std::move(missing_login_api)};
    const auto missing_login_result = missing_login.run(std::chrono::milliseconds{20});
    runner.expect(missing_login_result.state == ctp::TraderState::LoginFailed,
                  "missing login response must fail safely");
}

void test_wrong_ids_disconnect_and_timeout(TestRunner& runner)
{
    auto timeout_metrics = std::make_shared<FakeMetrics>();
    auto timeout_api = std::make_unique<FakeTraderApi>(timeout_metrics);
    timeout_api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontConnected();
    };
    timeout_api->on_authenticate = [](FakeTraderApi& value) {
        CThostFtdcRspAuthenticateField response{};
        CThostFtdcRspInfoField info{};
        value.spi()->OnRspAuthenticate(&response, &info, 99, true);
    };
    ctp::TraderClient timed_out{account_config(), std::move(timeout_api)};
    const auto timeout_result = timed_out.run(std::chrono::milliseconds{2});
    runner.expect(
        timeout_result.state == ctp::TraderState::TimedOut,
        "wrong request ID must not advance and must time out");

    auto disconnect_metrics = std::make_shared<FakeMetrics>();
    auto disconnect_api = std::make_unique<FakeTraderApi>(disconnect_metrics);
    disconnect_api->on_init = [](FakeTraderApi& value) {
        value.spi()->OnFrontDisconnected(0x2001);
    };
    ctp::TraderClient disconnected{account_config(), std::move(disconnect_api)};
    const auto disconnect_result = disconnected.run(std::chrono::milliseconds{20});
    runner.expect(
        disconnect_result.state == ctp::TraderState::Disconnected,
        "disconnect must finish the operation");
    runner.expect(disconnect_result.error_code == 0x2001, "disconnect reason must survive");
}

void test_interruption_stops_trader_client(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeTraderApi>(metrics);
    ctp::TraderClient client{account_config(), std::move(api)};

    const auto result = client.run(
        std::chrono::milliseconds{100}, [] { return true; });
    client.OnFrontConnected();

    runner.expect(
        result.state == ctp::TraderState::Interrupted,
        "stop request must interrupt the trader client");
    runner.expect(
        ctp::trader_exit_code(result.state) == 130,
        "trader interruption must map to exit code 130");
    runner.expect(metrics->authenticate_calls == 0,
                  "late callbacks must not advance after interruption");
    runner.expect(metrics->release_calls == 1,
                  "interruption must release TraderApi once");
}

void test_all_trader_wait_phases_terminate(TestRunner& runner)
{
    const TraderWaitPhase phases[]{
        TraderWaitPhase::Connecting,
        TraderWaitPhase::Authentication,
        TraderWaitPhase::Login,
        TraderWaitPhase::TradingAccount,
        TraderWaitPhase::InvestorPosition};

    for (const auto phase : phases) {
        auto timeout_metrics = std::make_shared<FakeMetrics>();
        ctp::TraderClient timed_out{
            account_config(),
            trader_api_waiting_at(timeout_metrics, phase, false)};
        const auto timeout_result =
            timed_out.run(std::chrono::milliseconds{2});
        runner.expect(timeout_result.state == ctp::TraderState::TimedOut,
                      "every trader wait phase must time out");
        runner.expect(timeout_metrics->release_calls == 1,
                      "every trader timeout must release once");

        auto interrupt_metrics = std::make_shared<FakeMetrics>();
        ctp::TraderClient interrupted{
            account_config(),
            trader_api_waiting_at(interrupt_metrics, phase, false)};
        const auto interrupt_result = interrupted.run(
            std::chrono::milliseconds{100}, [] { return true; });
        runner.expect(interrupt_result.state == ctp::TraderState::Interrupted,
                      "every trader wait phase must be interruptible");
        runner.expect(interrupt_metrics->release_calls == 1,
                      "every trader interruption must release once");

        auto disconnect_metrics = std::make_shared<FakeMetrics>();
        ctp::TraderClient disconnected{
            account_config(),
            trader_api_waiting_at(disconnect_metrics, phase, true)};
        const auto disconnect_result =
            disconnected.run(std::chrono::milliseconds{100});
        runner.expect(disconnect_result.state == ctp::TraderState::Disconnected,
                      "every trader wait phase must handle disconnection");
        runner.expect(disconnect_metrics->release_calls == 1,
                      "every trader disconnection must release once");
    }
}

void test_release_waits_for_active_authenticate_request(TestRunner& runner)
{
    auto metrics = std::make_shared<FakeMetrics>();
    auto api = std::make_unique<FakeTraderApi>(metrics);
    std::promise<void> initialized;
    std::promise<void> authenticate_entered;
    std::promise<void> allow_authenticate_return;
    auto allow_authenticate_future =
        allow_authenticate_return.get_future().share();
    api->on_init = [&initialized](FakeTraderApi&) { initialized.set_value(); };
    api->on_authenticate =
        [&authenticate_entered, allow_authenticate_future](FakeTraderApi&) {
            authenticate_entered.set_value();
            allow_authenticate_future.wait();
        };

    auto client = std::make_unique<ctp::TraderClient>(
        account_config(), std::move(api));
    std::atomic<bool> stop_requested{false};
    auto run = std::async(std::launch::async, [&] {
        return client->run(std::chrono::seconds{2}, [&] {
            return stop_requested.load();
        });
    });
    initialized.get_future().wait();

    auto callback = std::async(
        std::launch::async, [&client] { client->OnFrontConnected(); });
    authenticate_entered.get_future().wait();
    stop_requested = true;
    runner.expect(
        run.wait_for(std::chrono::milliseconds{250}) ==
            std::future_status::timeout,
        "interruption must wait for the active authentication request to return");
    allow_authenticate_return.set_value();

    callback.get();
    const auto result = run.get();
    runner.expect(result.state == ctp::TraderState::Interrupted,
                  "active authentication must still allow bounded interruption");
    runner.expect(!metrics->released_during_authenticate_request,
                  "TraderApi must not be released during an active request");
    runner.expect(metrics->release_calls == 1,
                  "concurrent interruption must release TraderApi once");
}

std::unique_ptr<FakeTraderApi> logged_in_api(
    const std::shared_ptr<FakeMetrics>& metrics)
{
    auto api = std::make_unique<FakeTraderApi>(metrics);
    api->on_init = [](FakeTraderApi& value) { value.spi()->OnFrontConnected(); };
    api->on_authenticate = [](FakeTraderApi& value) { authenticate_success(value); };
    api->on_login = [](FakeTraderApi& value) { login_success(value); };
    return api;
}

void test_query_failures_and_empty_positions(TestRunner& runner)
{
    auto account_rejected_metrics = std::make_shared<FakeMetrics>();
    auto account_rejected_api = logged_in_api(account_rejected_metrics);
    account_rejected_api->account_return_code = -5;
    ctp::TraderClient account_rejected{
        account_config(), std::move(account_rejected_api)};
    const auto account_rejected_result =
        account_rejected.run(std::chrono::milliseconds{20});
    runner.expect(account_rejected_result.state == ctp::TraderState::QueryFailed &&
                      account_rejected_result.error_code == -5,
                  "immediate account query rejection must fail");

    auto account_failed_metrics = std::make_shared<FakeMetrics>();
    auto account_failed_api = logged_in_api(account_failed_metrics);
    account_failed_api->on_account = [](FakeTraderApi& value) {
        CThostFtdcRspInfoField info{};
        info.ErrorID = 41;
        ctp::copy_to_field(info.ErrorMsg, "account denied");
        value.spi()->OnRspQryTradingAccount(nullptr, &info, 3, true);
    };
    ctp::TraderClient account_failed{
        account_config(), std::move(account_failed_api)};
    const auto account_failed_result =
        account_failed.run(std::chrono::milliseconds{20});
    runner.expect(account_failed_result.state == ctp::TraderState::QueryFailed &&
                      account_failed_result.error_code == 41,
                  "account business error must fail with its ErrorID");

    auto missing_metrics = std::make_shared<FakeMetrics>();
    auto missing_api = logged_in_api(missing_metrics);
    missing_api->on_account = [](FakeTraderApi& value) {
        value.spi()->OnRspQryTradingAccount(nullptr, nullptr, 3, true);
    };
    ctp::TraderClient missing{account_config(), std::move(missing_api)};
    const auto missing_result = missing.run(std::chrono::milliseconds{20});
    runner.expect(missing_result.state == ctp::TraderState::QueryFailed,
                  "missing final account response must fail safely");
    runner.expect(missing_metrics->position_calls == 0,
                  "failed account query must not start the position query");

    auto position_failed_metrics = std::make_shared<FakeMetrics>();
    auto position_failed_api = logged_in_api(position_failed_metrics);
    position_failed_api->on_account = [](FakeTraderApi& value) {
        account_success(value);
    };
    position_failed_api->on_position = [](FakeTraderApi& value) {
        CThostFtdcRspInfoField info{};
        info.ErrorID = 42;
        ctp::copy_to_field(info.ErrorMsg, "position denied");
        value.spi()->OnRspQryInvestorPosition(nullptr, &info, 4, true);
    };
    ctp::TraderClient position_failed{
        account_config(), std::move(position_failed_api)};
    const auto position_failed_result =
        position_failed.run(std::chrono::milliseconds{20});
    runner.expect(position_failed_result.state == ctp::TraderState::QueryFailed &&
                      position_failed_result.error_code == 42,
                  "position business error must fail with its ErrorID");

    auto position_rejected_metrics = std::make_shared<FakeMetrics>();
    auto position_rejected_api = logged_in_api(position_rejected_metrics);
    position_rejected_api->on_account = [](FakeTraderApi& value) {
        account_success(value);
    };
    position_rejected_api->position_return_code = -6;
    ctp::TraderClient position_rejected{
        account_config(), std::move(position_rejected_api)};
    const auto position_rejected_result =
        position_rejected.run(std::chrono::milliseconds{20});
    runner.expect(position_rejected_result.state == ctp::TraderState::QueryFailed &&
                      position_rejected_result.error_code == -6,
                  "immediate position query rejection must fail");

    auto empty_metrics = std::make_shared<FakeMetrics>();
    auto empty_api = logged_in_api(empty_metrics);
    empty_api->on_account = [](FakeTraderApi& value) { account_success(value); };
    empty_api->on_position = [](FakeTraderApi& value) {
        value.spi()->OnRspQryInvestorPosition(nullptr, nullptr, 4, true);
    };
    ctp::TraderClient empty{account_config(), std::move(empty_api)};
    const auto empty_result = empty.run(std::chrono::milliseconds{20});
    runner.expect(empty_result.state == ctp::TraderState::Completed &&
                      empty_result.positions.empty(),
                  "null final position response must mean zero positions");
}

void test_exit_codes_and_safe_output(TestRunner& runner)
{
    runner.expect(ctp::trader_exit_code(ctp::TraderState::Completed) == 0,
                  "completed queries must exit 0");
    runner.expect(ctp::trader_exit_code(ctp::TraderState::TimedOut) == 4,
                  "timeout must exit 4");
    runner.expect(ctp::trader_exit_code(ctp::TraderState::LoginFailed) == 5,
                  "login failure must exit 5");
    runner.expect(ctp::trader_exit_code(ctp::TraderState::QueryFailed) == 6,
                  "query failure must exit 6");

    ctp::TraderResult result;
    result.state = ctp::TraderState::LoginFailed;
    result.error_code = 8;
    result.error_message = "login denied";
    std::ostringstream output;
    std::ostringstream error;
    const int code = ctp::report_trader_result(result, output, error);
    const std::string text = output.str() + error.str();
    runner.expect(code == 5, "failed report must preserve the exit contract");
    runner.expect(text.find("secret-password") == std::string::npos,
                  "report must not contain the password");
    runner.expect(text.find("secret-auth-code") == std::string::npos,
                  "report must not contain the AuthCode");

    ctp::TraderResult success;
    success.state = ctp::TraderState::Completed;
    success.account = ctp::TradingAccountSummary{
        "123456", 100000.5, 80000.25, 15000.75};
    success.positions.push_back(
        ctp::PositionSummary{"rb2610", THOST_FTDC_PD_Long, 3, 1, 2});
    std::ostringstream success_output;
    std::ostringstream success_error;
    runner.expect(ctp::report_trader_result(
                      success, success_output, success_error) == 0,
                  "successful report must exit 0");
    const std::string success_text = success_output.str();
    runner.expect(success_text.find("account=****56") != std::string::npos,
                  "account identifier must be masked");
    runner.expect(success_text.find("instrument=rb2610") != std::string::npos &&
                      success_text.find("positions=1") != std::string::npos,
                  "successful report must include positions and their count");
    runner.expect(success_text.find("123456") == std::string::npos,
                  "successful report must not expose the complete account ID");
}

}

int main()
{
    TestRunner runner;
    test_success_advances_once(runner);
    test_immediate_and_business_failures(runner);
    test_login_failure_and_missing_response(runner);
    test_wrong_ids_disconnect_and_timeout(runner);
    test_interruption_stops_trader_client(runner);
    test_all_trader_wait_phases_terminate(runner);
    test_release_waits_for_active_authenticate_request(runner);
    test_query_failures_and_empty_positions(runner);
    test_exit_codes_and_safe_output(runner);
    return runner.finish();
}
