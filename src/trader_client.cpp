#include "ctp/trader_client.hpp"

#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>

namespace ctp {
namespace {

constexpr int kAuthenticationRequestId = 1;
constexpr int kLoginRequestId = 2;
constexpr int kTradingAccountRequestId = 3;
constexpr int kInvestorPositionRequestId = 4;
constexpr int kMissingAuthenticationResponse = -1;
constexpr int kMissingLoginResponse = -2;
constexpr int kInvalidAuthenticationFields = -3;
constexpr int kInvalidLoginFields = -4;
constexpr int kMissingTradingAccountResponse = -5;
constexpr int kInvalidTradingAccountFields = -6;
constexpr int kInvalidInvestorPositionFields = -7;
constexpr auto kStopCheckInterval = std::chrono::milliseconds{50};

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

    int request_trading_account(
        CThostFtdcQryTradingAccountField* request, int request_id) override
    {
        return api_->ReqQryTradingAccount(request, request_id);
    }

    int request_investor_position(
        CThostFtdcQryInvestorPositionField* request, int request_id) override
    {
        return api_->ReqQryInvestorPosition(request, request_id);
    }

    int request_order_insert(
        CThostFtdcInputOrderField* request, int request_id) override
    {
        return api_->ReqOrderInsert(request, request_id);
    }

    int request_order_action(
        CThostFtdcInputOrderActionField* request, int request_id) override
    {
        return api_->ReqOrderAction(request, request_id);
    }

    int request_order_query(
        CThostFtdcQryOrderField* request, int request_id) override
    {
        return api_->ReqQryOrder(request, request_id);
    }

    int request_trade_query(
        CThostFtdcQryTradeField* request, int request_id) override
    {
        return api_->ReqQryTrade(request, request_id);
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

std::string masked_identifier(const std::string& value)
{
    if (value.size() <= 2) {
        return std::string(value.size(), '*');
    }
    return std::string(value.size() - 2, '*') + value.substr(value.size() - 2);
}

std::unique_ptr<TraderApi> create_ctp_trader_api(
    const std::string& flow_directory)
{
    std::error_code error;
    std::filesystem::create_directories(flow_directory, error);
    if (error) {
        return nullptr;
    }

    std::string ctp_path = flow_directory;
    if (ctp_path.empty() || ctp_path.back() != '/') {
        ctp_path.push_back('/');
    }
    auto* api = CThostFtdcTraderApi::CreateFtdcTraderApi(ctp_path.c_str());
    if (api == nullptr) {
        return nullptr;
    }
    return std::make_unique<CtpTraderApi>(api);
}

const char* direction_text(char direction)
{
    switch (direction) {
    case THOST_FTDC_PD_Net:
        return "net";
    case THOST_FTDC_PD_Long:
        return "long";
    case THOST_FTDC_PD_Short:
        return "short";
    default:
        return "unknown";
    }
}

}

std::unique_ptr<TraderApi> create_trader_api(
    const std::string& flow_directory)
{
    return create_ctp_trader_api(flow_directory);
}

int report_trader_result(
    const TraderResult& result,
    std::ostream& output,
    std::ostream& error)
{
    if (result.state == TraderState::Completed) {
        if (result.account) {
            output << "account=" << masked_identifier(result.account->account_id)
                   << " balance=" << result.account->balance
                   << " available=" << result.account->available
                   << " margin=" << result.account->current_margin << '\n';
        }
        for (const auto& position : result.positions) {
            output << "position instrument=" << position.instrument_id
                   << " direction=" << direction_text(position.direction)
                   << " total=" << position.position
                   << " today=" << position.today_position
                   << " yesterday=" << position.yesterday_position << '\n';
        }
        output << "[ok] account queries completed: positions="
               << result.positions.size() << '\n';
        return 0;
    }
    if (result.state == TraderState::TimedOut) {
        error << "[error] trader operation timed out\n";
    } else if (result.state == TraderState::Interrupted) {
        error << "[error] trader operation interrupted\n";
    } else if (result.state == TraderState::Disconnected) {
        error << "[error] trader front disconnected: reason="
              << result.error_code << '\n';
    } else if (result.state == TraderState::QueryFailed) {
        error << "[error] trader query failed: code=" << result.error_code;
        if (!result.error_message.empty()) {
            error << " message=" << result.error_message;
        }
        error << '\n';
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

TraderResult TraderClient::run(
    std::chrono::milliseconds timeout,
    const StopRequested& stop_requested)
{
    api_->register_spi(this);
    api_->subscribe_private_topic(THOST_TERT_QUICK, 1);
    api_->subscribe_public_topic(THOST_TERT_QUICK);
    api_->register_front(config_.trader_front());
    api_->init();

    TraderResult result;
    {
        std::unique_lock<std::mutex> lock{mutex_};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!is_terminal()) {
            if (stop_requested && stop_requested()) {
                result_.state = TraderState::Interrupted;
                result_.error_message = "trader operation interrupted";
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                result_.state = TraderState::TimedOut;
                result_.error_message = "trader operation timed out";
                break;
            }

            auto wake_at = now + kStopCheckInterval;
            if (wake_at > deadline) {
                wake_at = deadline;
            }
            condition_.wait_until(
                lock, wake_at, [this] { return is_terminal(); });
        }
        result = result_;
    }

    release_api();
    return result;
}

void TraderClient::OnFrontConnected()
{
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

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

    if (!can_request(TraderState::AuthenticationPending)) {
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
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

    finish(TraderState::Disconnected, reason, "trader front disconnected");
}

void TraderClient::OnRspAuthenticate(
    CThostFtdcRspAuthenticateField* response,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

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

    if (!can_request(TraderState::LoginPending)) {
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
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

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

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != TraderState::LoginPending) {
            return;
        }
        result_.state = TraderState::TradingAccountPending;
    }

    CThostFtdcQryTradingAccountField request{};
    if (!copy_to_field(request.BrokerID, config_.broker_id()) ||
        !copy_to_field(request.InvestorID, config_.user_id())) {
        finish(
            TraderState::QueryFailed,
            kInvalidTradingAccountFields,
            "validated trading account fields could not be copied");
        return;
    }

    if (!can_request(TraderState::TradingAccountPending)) {
        return;
    }
    const int code = api_->request_trading_account(
        &request, kTradingAccountRequestId);
    if (code != 0) {
        finish(
            TraderState::QueryFailed,
            code,
            "ReqQryTradingAccount rejected the request");
    }
}

void TraderClient::OnRspQryTradingAccount(
    CThostFtdcTradingAccountField* response,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != TraderState::TradingAccountPending ||
            request_id != kTradingAccountRequestId) {
            return;
        }
    }

    if (info != nullptr && info->ErrorID != 0) {
        finish(TraderState::QueryFailed, info->ErrorID, field_text(info->ErrorMsg));
        return;
    }

    bool missing_response = false;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != TraderState::TradingAccountPending) {
            return;
        }
        if (response != nullptr) {
            result_.account = TradingAccountSummary{
                field_text(response->AccountID),
                response->Balance,
                response->Available,
                response->CurrMargin};
        }
        if (!is_last) {
            return;
        }
        missing_response = !result_.account.has_value();
        if (!missing_response) {
            result_.state = TraderState::InvestorPositionPending;
        }
    }

    if (missing_response) {
        finish(
            TraderState::QueryFailed,
            kMissingTradingAccountResponse,
            "trading account response is missing");
        return;
    }

    CThostFtdcQryInvestorPositionField request{};
    if (!copy_to_field(request.BrokerID, config_.broker_id()) ||
        !copy_to_field(request.InvestorID, config_.user_id())) {
        finish(
            TraderState::QueryFailed,
            kInvalidInvestorPositionFields,
            "validated investor position fields could not be copied");
        return;
    }

    if (!can_request(TraderState::InvestorPositionPending)) {
        return;
    }
    const int code = api_->request_investor_position(
        &request, kInvestorPositionRequestId);
    if (code != 0) {
        finish(
            TraderState::QueryFailed,
            code,
            "ReqQryInvestorPosition rejected the request");
    }
}

void TraderClient::OnRspQryInvestorPosition(
    CThostFtdcInvestorPositionField* response,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != TraderState::InvestorPositionPending ||
            request_id != kInvestorPositionRequestId) {
            return;
        }
    }

    if (info != nullptr && info->ErrorID != 0) {
        finish(TraderState::QueryFailed, info->ErrorID, field_text(info->ErrorMsg));
        return;
    }

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != TraderState::InvestorPositionPending) {
            return;
        }
        if (response != nullptr) {
            result_.positions.push_back(PositionSummary{
                field_text(response->InstrumentID),
                response->PosiDirection,
                response->Position,
                response->TodayPosition,
                response->YdPosition});
        }
        if (!is_last) {
            return;
        }
        result_.state = TraderState::Completed;
    }
    condition_.notify_one();
}

bool TraderClient::is_terminal() const
{
    return result_.state == TraderState::Completed ||
           result_.state == TraderState::AuthenticationFailed ||
           result_.state == TraderState::LoginFailed ||
           result_.state == TraderState::QueryFailed ||
           result_.state == TraderState::Disconnected ||
           result_.state == TraderState::TimedOut ||
           result_.state == TraderState::Interrupted;
}

TraderClient::CallbackGuard::CallbackGuard(TraderClient& client)
    : client_(client), entered_(client_.enter_callback())
{
}

TraderClient::CallbackGuard::~CallbackGuard()
{
    if (entered_) {
        client_.leave_callback();
    }
}

TraderClient::CallbackGuard::operator bool() const
{
    return entered_;
}

bool TraderClient::enter_callback()
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (releasing_ || released_) {
        return false;
    }
    ++active_callbacks_;
    return true;
}

void TraderClient::leave_callback()
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        --active_callbacks_;
    }
    condition_.notify_all();
}

bool TraderClient::can_request(TraderState expected_state)
{
    std::lock_guard<std::mutex> lock{mutex_};
    return !releasing_ && !released_ && result_.state == expected_state;
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
    {
        std::unique_lock<std::mutex> lock{mutex_};
        if (released_) {
            return;
        }
        if (releasing_) {
            condition_.wait(lock, [this] { return released_; });
            return;
        }
        releasing_ = true;
        condition_.wait(lock, [this] {
            return active_callbacks_ == 0;
        });
    }
    api_->release();
    {
        std::lock_guard<std::mutex> lock{mutex_};
        released_ = true;
    }
    condition_.notify_all();
}

int run_account(
    const RuntimeConfig& config,
    std::chrono::milliseconds timeout,
    const StopRequested& stop_requested)
{
    auto api = create_trader_api("flow/trader");
    if (!api) {
        std::cerr << "[error] failed to create trader API or flow directory\n";
        return 3;
    }

    TraderClient client{config, std::move(api)};
    return report_trader_result(
        client.run(timeout, stop_requested), std::cout, std::cerr);
}

int trader_exit_code(TraderState state)
{
    if (state == TraderState::Completed) {
        return 0;
    }
    if (state == TraderState::Interrupted) {
        return 130;
    }
    if (state == TraderState::Disconnected || state == TraderState::TimedOut) {
        return 4;
    }
    if (state == TraderState::QueryFailed) {
        return 6;
    }
    return 5;
}

}
