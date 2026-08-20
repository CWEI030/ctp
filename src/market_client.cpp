#include "ctp/market_client.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>

namespace ctp {
namespace {

constexpr int kLoginRequestId = 1;
constexpr int kMissingLoginResponse = -1;
constexpr int kInvalidLoginFields = -2;

template <std::size_t N>
std::string field_text(const char (&field)[N])
{
    const auto end = std::find(std::begin(field), std::end(field), '\0');
    return std::string{std::begin(field), end};
}

class CtpMarketApi final : public MarketApi {
public:
    explicit CtpMarketApi(CThostFtdcMdApi* api) : api_(api) {}

    ~CtpMarketApi() override
    {
        release();
    }

    void register_spi(CThostFtdcMdSpi* spi) override
    {
        api_->RegisterSpi(spi);
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

    int request_user_login(
        CThostFtdcReqUserLoginField* request,
        int request_id) override
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
    CThostFtdcMdApi* api_;
    std::string front_;
};

std::unique_ptr<MarketApi> create_market_api()
{
    std::error_code error;
    std::filesystem::create_directories("flow/md", error);
    if (error) {
        return nullptr;
    }

    CThostFtdcMdApi* api = CThostFtdcMdApi::CreateFtdcMdApi("flow/md/");
    if (api == nullptr) {
        return nullptr;
    }
    return std::make_unique<CtpMarketApi>(api);
}

}

MarketClient::MarketClient(
    const RuntimeConfig& config,
    std::unique_ptr<MarketApi> api)
    : config_(config), api_(std::move(api))
{
}

MarketClient::~MarketClient()
{
    release_api();
}

MarketResult MarketClient::run(std::chrono::milliseconds timeout)
{
    api_->register_spi(this);
    api_->register_front(config_.market_front());
    api_->init();

    MarketResult result;
    {
        std::unique_lock<std::mutex> lock{mutex_};
        if (!condition_.wait_for(lock, timeout, [this] { return is_terminal(); })) {
            result_.state = MarketState::TimedOut;
            result_.error_message = "market login timed out";
        }
        result = result_;
    }

    release_api();
    return result;
}

void MarketClient::OnFrontConnected()
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != MarketState::Connecting) {
            return;
        }
        result_.state = MarketState::LoginPending;
    }

    CThostFtdcReqUserLoginField request{};
    if (!copy_to_field(request.BrokerID, config_.broker_id()) ||
        !copy_to_field(request.UserID, config_.user_id()) ||
        !copy_to_field(request.Password, config_.password())) {
        finish(
            MarketState::LoginFailed,
            kInvalidLoginFields,
            "validated login fields could not be copied");
        return;
    }

    const int return_code = api_->request_user_login(&request, kLoginRequestId);
    if (return_code != 0) {
        finish(
            MarketState::LoginFailed,
            return_code,
            "ReqUserLogin rejected the request");
    }
}

void MarketClient::OnFrontDisconnected(int reason)
{
    finish(MarketState::Disconnected, reason, "market front disconnected");
}

void MarketClient::OnRspUserLogin(
    CThostFtdcRspUserLoginField* response,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    static_cast<void>(is_last);

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != MarketState::LoginPending ||
            request_id != kLoginRequestId) {
            return;
        }
    }

    if (info != nullptr && info->ErrorID != 0) {
        finish(
            MarketState::LoginFailed,
            info->ErrorID,
            field_text(info->ErrorMsg));
        return;
    }

    if (response == nullptr) {
        finish(
            MarketState::LoginFailed,
            kMissingLoginResponse,
            "login response is missing");
        return;
    }

    finish(MarketState::LoginSucceeded, 0, {});
}

bool MarketClient::is_terminal() const
{
    return result_.state == MarketState::LoginSucceeded ||
           result_.state == MarketState::LoginFailed ||
           result_.state == MarketState::Disconnected ||
           result_.state == MarketState::TimedOut;
}

void MarketClient::finish(
    MarketState state,
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

void MarketClient::release_api()
{
    if (released_) {
        return;
    }
    api_->release();
    released_ = true;
}

int run_market(const RuntimeConfig& config, std::chrono::milliseconds timeout)
{
    auto api = create_market_api();
    if (!api) {
        std::cerr << "[error] failed to create market API or flow directory\n";
        return 3;
    }

    MarketClient client{config, std::move(api)};
    const auto result = client.run(timeout);

    if (result.state == MarketState::LoginSucceeded) {
        std::cout << "[ok] market login succeeded\n";
        return market_exit_code(result.state);
    }
    if (result.state == MarketState::TimedOut) {
        std::cerr << "[error] market login timed out\n";
        return market_exit_code(result.state);
    }
    if (result.state == MarketState::Disconnected) {
        std::cerr << "[error] market front disconnected: reason="
                  << result.error_code << '\n';
        return market_exit_code(result.state);
    }

    std::cerr << "[error] market login failed: code=" << result.error_code;
    if (!result.error_message.empty()) {
        std::cerr << " message=" << result.error_message;
    }
    std::cerr << '\n';
    return market_exit_code(result.state);
}

int market_exit_code(MarketState state)
{
    if (state == MarketState::LoginSucceeded) {
        return 0;
    }
    if (state == MarketState::Disconnected ||
        state == MarketState::TimedOut) {
        return 4;
    }
    return 5;
}

}
