#include "ctp/trader_client.hpp"

#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>

namespace ctp {
namespace {

constexpr int kAuthenticationRequestId = 1;
constexpr int kLoginRequestId = 2;
constexpr int kMissingAuthenticationResponse = -1;
constexpr int kMissingLoginResponse = -2;
constexpr int kInvalidAuthenticationFields = -3;
constexpr int kInvalidLoginFields = -4;

class CtpTraderApi final : public TraderApi {
public:
    explicit CtpTraderApi(CThostFtdcTraderApi* api) : api_(api) {}

    ~CtpTraderApi() override
    {
        release();
    }

    void register_spi(CThostFtdcTraderSpi* spi) override
    {
        api_->RegisterSpi(spi);
    }

    void subscribe_private_topic(
        THOST_TE_RESUME_TYPE resume_type, int sequence) override
    {
        api_->SubscribePrivateTopic(resume_type, sequence);
    }

    void subscribe_public_topic(THOST_TE_RESUME_TYPE resume_type) override
    {
        api_->SubscribePublicTopic(resume_type);
    }

    void register_front(const std::string& front) override
    {
        front_ = front;
        api_->RegisterFront(front_.data());
    }

    void init() override
    {
        api_->Init();
    }

    int request_authenticate(
        CThostFtdcReqAuthenticateField* request, int request_id) override
    {
        return api_->ReqAuthenticate(request, request_id);
    }

    int request_user_login(
        CThostFtdcReqUserLoginField* request, int request_id) override
    {
        return api_->ReqUserLogin(request, request_id);
    }

    void release() override
    {
        if (api_ == nullptr) {
            return;
        }
        api_->Release();
        api_ = nullptr;
    }

private:
    CThostFtdcTraderApi* api_;
    std::string front_;
};

std::unique_ptr<TraderApi> create_trader_api()
{
    std::error_code error;
    std::filesystem::create_directories("flow/trader", error);
    if (error) {
        return nullptr;
    }

    auto* api = CThostFtdcTraderApi::CreateFtdcTraderApi("flow/trader/");
    if (api == nullptr) {
        return nullptr;
    }
    return std::make_unique<CtpTraderApi>(api);
}

}

int report_trader_result(
    const TraderResult& result,
    std::ostream& output,
    std::ostream& error)
{
    if (result.state == TraderState::ReadyForQuery) {
        output << "[ok] trader authentication and login completed; "
                  "ready for queries\n";
        return 0;
    }
    if (result.state == TraderState::TimedOut) {
        error << "[error] trader operation timed out\n";
    } else if (result.state == TraderState::Disconnected) {
        error << "[error] trader front disconnected: reason="
              << result.error_code << '\n';
    } else {
        error << "[error] trader authentication or login failed: code="
              << result.error_code;
        if (!result.error_message.empty()) {
            error << " message=" << result.error_message;
        }
        error << '\n';
    }
    return trader_exit_code(result.state);
}

TraderClient::TraderClient(
    const RuntimeConfig& config,
    std::unique_ptr<TraderApi> api)
    : config_(config), api_(std::move(api))
{
}

TraderClient::~TraderClient()
{
    release_api();
}

TraderResult TraderClient::run(std::chrono::milliseconds timeout)
{
    api_->register_spi(this);
    api_->subscribe_private_topic(THOST_TERT_QUICK, 1);
    api_->subscribe_public_topic(THOST_TERT_QUICK);
    api_->register_front(config_.trader_front());
    api_->init();

    TraderResult result;
    {
        std::unique_lock<std::mutex> lock{mutex_};
        if (!condition_.wait_for(lock, timeout, [this] { return is_terminal(); })) {
            result_.state = TraderState::TimedOut;
            result_.error_message = "trader operation timed out";
        }
        result = result_;
    }

    release_api();
    return result;
}

void TraderClient::OnFrontConnected()
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != TraderState::Connecting) {
            return;
        }
        result_.state = TraderState::AuthenticationPending;
    }

    CThostFtdcReqAuthenticateField request{};
    if (!copy_to_field(request.BrokerID, config_.broker_id()) ||
        !copy_to_field(request.UserID, config_.user_id()) ||
        !copy_to_field(request.AppID, config_.app_id()) ||
        !copy_to_field(request.AuthCode, config_.auth_code())) {
        finish(
            TraderState::AuthenticationFailed,
            kInvalidAuthenticationFields,
            "validated authentication fields could not be copied");
        return;
    }

    const int code = api_->request_authenticate(
        &request, kAuthenticationRequestId);
    if (code != 0) {
        finish(
            TraderState::AuthenticationFailed,
            code,
            "ReqAuthenticate rejected the request");
    }
}

void TraderClient::OnFrontDisconnected(int reason)
{
    finish(TraderState::Disconnected, reason, "trader front disconnected");
}

void TraderClient::OnRspAuthenticate(
    CThostFtdcRspAuthenticateField* response,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != TraderState::AuthenticationPending ||
            request_id != kAuthenticationRequestId) {
            return;
        }
    }

    if (info != nullptr && info->ErrorID != 0) {
        finish(
            TraderState::AuthenticationFailed,
            info->ErrorID,
            field_text(info->ErrorMsg));
        return;
    }
    if (!is_last) {
        return;
    }
    if (response == nullptr) {
        finish(
            TraderState::AuthenticationFailed,
            kMissingAuthenticationResponse,
            "authentication response is missing");
        return;
    }

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != TraderState::AuthenticationPending) {
            return;
        }
        result_.state = TraderState::LoginPending;
    }

    CThostFtdcReqUserLoginField request{};
    if (!copy_to_field(request.BrokerID, config_.broker_id()) ||
        !copy_to_field(request.UserID, config_.user_id()) ||
        !copy_to_field(request.Password, config_.password())) {
        finish(
            TraderState::LoginFailed,
            kInvalidLoginFields,
            "validated login fields could not be copied");
        return;
    }

    const int code = api_->request_user_login(&request, kLoginRequestId);
    if (code != 0) {
        finish(
            TraderState::LoginFailed,
            code,
            "ReqUserLogin rejected the request");
    }
}

void TraderClient::OnRspUserLogin(
    CThostFtdcRspUserLoginField* response,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != TraderState::LoginPending ||
            request_id != kLoginRequestId) {
            return;
        }
    }

    if (info != nullptr && info->ErrorID != 0) {
        finish(TraderState::LoginFailed, info->ErrorID, field_text(info->ErrorMsg));
        return;
    }
    if (!is_last) {
        return;
    }
    if (response == nullptr) {
        finish(
            TraderState::LoginFailed,
            kMissingLoginResponse,
            "login response is missing");
        return;
    }

    finish(TraderState::ReadyForQuery, 0, {});
}

bool TraderClient::is_terminal() const
{
    return result_.state == TraderState::ReadyForQuery ||
           result_.state == TraderState::AuthenticationFailed ||
           result_.state == TraderState::LoginFailed ||
           result_.state == TraderState::Disconnected ||
           result_.state == TraderState::TimedOut;
}

void TraderClient::finish(
    TraderState state,
    int error_code,
    std::string message)
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (is_terminal()) {
            return;
        }
        result_.state = state;
        result_.error_code = error_code;
        result_.error_message = std::move(message);
    }
    condition_.notify_one();
}

void TraderClient::release_api()
{
    if (released_) {
        return;
    }
    api_->release();
    released_ = true;
}

int run_account(const RuntimeConfig& config, std::chrono::milliseconds timeout)
{
    auto api = create_trader_api();
    if (!api) {
        std::cerr << "[error] failed to create trader API or flow directory\n";
        return 3;
    }

    TraderClient client{config, std::move(api)};
    return report_trader_result(client.run(timeout), std::cout, std::cerr);
}

int trader_exit_code(TraderState state)
{
    if (state == TraderState::ReadyForQuery) {
        return 0;
    }
    if (state == TraderState::Disconnected || state == TraderState::TimedOut) {
        return 4;
    }
    return 5;
}

}
