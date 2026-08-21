#include "ctp/trader_client.hpp"

#include <chrono>
#include <functional>
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
    int release_calls{0};
    int authenticate_request_id{0};
    int login_request_id{0};
    std::string front;
    std::string broker_id;
    std::string user_id;
    std::string app_id;
    std::string auth_code;
    std::string password;
    THOST_TE_RESUME_TYPE private_mode{THOST_TERT_RESTART};
    THOST_TE_RESUME_TYPE public_mode{THOST_TERT_RESTART};
    int private_sequence{0};
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
        ++metrics_->authenticate_calls;
        metrics_->authenticate_request_id = request_id;
        metrics_->broker_id = request->BrokerID;
        metrics_->user_id = request->UserID;
        metrics_->app_id = request->AppID;
        metrics_->auth_code = request->AuthCode;
        if (on_authenticate) {
            on_authenticate(*this);
        }
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

    void release() override
    {
        ++metrics_->release_calls;
        spi_ = nullptr;
    }

    CThostFtdcTraderSpi* spi() const { return spi_; }

    std::function<void(FakeTraderApi&)> on_init;
    std::function<void(FakeTraderApi&)> on_authenticate;
    std::function<void(FakeTraderApi&)> on_login;
    int authenticate_return_code{0};
    int login_return_code{0};
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

    ctp::TraderResult result;
    {
        ctp::TraderClient client{account_config(), std::move(api)};
        result = client.run(std::chrono::milliseconds{20});
    }

    runner.expect(
        result.state == ctp::TraderState::ReadyForQuery,
        "successful authentication and login must become query-ready");
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
    runner.expect(metrics->authenticate_request_id == 1, "authentication ID must be 1");
    runner.expect(metrics->login_request_id == 2, "login ID must be 2");
    runner.expect(metrics->front == "tcp://127.0.0.1:10002", "trader front must be used");
    runner.expect(metrics->broker_id == "9999", "BrokerID must be copied");
    runner.expect(metrics->user_id == "123456", "UserID must be copied");
    runner.expect(metrics->app_id == "test-app", "AppID must be copied");
    runner.expect(metrics->auth_code == "secret-auth-code", "AuthCode must be copied");
    runner.expect(metrics->password == "secret-password", "Password must be copied");
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

void test_exit_codes_and_safe_output(TestRunner& runner)
{
    runner.expect(ctp::trader_exit_code(ctp::TraderState::ReadyForQuery) == 0,
                  "query-ready must exit 0");
    runner.expect(ctp::trader_exit_code(ctp::TraderState::TimedOut) == 4,
                  "timeout must exit 4");
    runner.expect(ctp::trader_exit_code(ctp::TraderState::LoginFailed) == 5,
                  "login failure must exit 5");

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
}

}

int main()
{
    TestRunner runner;
    test_success_advances_once(runner);
    test_immediate_and_business_failures(runner);
    test_login_failure_and_missing_response(runner);
    test_wrong_ids_disconnect_and_timeout(runner);
    test_exit_codes_and_safe_output(runner);
    return runner.finish();
}
