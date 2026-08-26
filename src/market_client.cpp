#include "ctp/market_client.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>
#include <utility>

namespace ctp {
namespace {

constexpr int kLoginRequestId = 1;
constexpr int kMissingLoginResponse = -1;
constexpr int kInvalidLoginFields = -2;
constexpr int kInvalidInstrument = -3;
constexpr int kMissingSubscriptionResponse = -4;
constexpr auto kStopCheckInterval = std::chrono::milliseconds{50};

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

    int subscribe_market_data(char* instruments[], int count) override
    {
        return api_->SubscribeMarketData(instruments, count);
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

std::string format_market_tick(const MarketTick& tick)
{
    std::ostringstream output;
    output << "time=" << tick.update_time << '.'
           << std::setfill('0') << std::setw(3) << tick.update_millisec
           << " instrument=" << tick.instrument
           << " last=" << tick.last_price
           << " bid1=" << tick.bid_price
           << " ask1=" << tick.ask_price
           << " volume=" << tick.volume;
    return output.str();
}

int report_market_result(
    const MarketResult& result,
    std::ostream& output,
    std::ostream& error)
{
    if (result.state == MarketState::Completed) {
        for (const auto& tick : result.ticks) {
            output << format_market_tick(tick) << '\n';
        }
        output << "[ok] market data completed: ticks="
               << result.ticks.size() << '\n';
        return market_exit_code(result.state);
    }
    if (result.state == MarketState::TimedOut) {
        error << "[error] market operation timed out: received="
              << result.ticks.size() << '\n';
        return market_exit_code(result.state);
    }
    if (result.state == MarketState::Disconnected) {
        error << "[error] market front disconnected: reason="
              << result.error_code << '\n';
        return market_exit_code(result.state);
    }
    if (result.state == MarketState::Interrupted) {
        error << "[error] market operation interrupted\n";
        return market_exit_code(result.state);
    }

    error << "[error] market operation failed: code=" << result.error_code;
    if (!result.error_message.empty()) {
        error << " message=" << result.error_message;
    }
    error << '\n';
    return market_exit_code(result.state);
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

MarketResult MarketClient::run(
    std::chrono::milliseconds timeout,
    const StopRequested& stop_requested)
{
    api_->register_spi(this);
    api_->register_front(config_.market_front());
    api_->init();

    MarketResult result;
    {
        std::unique_lock<std::mutex> lock{mutex_};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!is_terminal()) {
            if (stop_requested && stop_requested()) {
                result_.state = MarketState::Interrupted;
                result_.error_message = "market operation interrupted";
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                result_.state = MarketState::TimedOut;
                result_.error_message = "market operation timed out";
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

void MarketClient::OnFrontConnected()
{
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

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

    if (!can_request(MarketState::LoginPending)) {
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
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

    finish(MarketState::Disconnected, reason, "market front disconnected");
}

void MarketClient::OnRspUserLogin(
    CThostFtdcRspUserLoginField* response,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

    static_cast<void>(is_last);

    {
        std::lock_guard<std::mutex> lock{mutex_};
        // SimNow 行情登录回调可能返回 0，即使请求使用了非零编号。
        const bool matches_login_request =
            request_id == 0 || request_id == kLoginRequestId;
        if (result_.state != MarketState::LoginPending ||
            !matches_login_request) {
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

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != MarketState::LoginPending) {
            return;
        }
        result_.state = MarketState::SubscriptionPending;
    }

    std::string instrument = config_.instrument();
    if (instrument.empty()) {
        finish(
            MarketState::SubscriptionFailed,
            kInvalidInstrument,
            "validated instrument is empty");
        return;
    }

    char* instruments[] = {instrument.data()};
    if (!can_request(MarketState::SubscriptionPending)) {
        return;
    }
    const int return_code = api_->subscribe_market_data(instruments, 1);
    if (return_code != 0) {
        finish(
            MarketState::SubscriptionFailed,
            return_code,
            "SubscribeMarketData rejected the request");
        return;
    }
}

void MarketClient::OnRspSubMarketData(
    CThostFtdcSpecificInstrumentField* instrument,
    CThostFtdcRspInfoField* info,
    int request_id,
    bool is_last)
{
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

    static_cast<void>(request_id);
    static_cast<void>(is_last);

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != MarketState::SubscriptionPending) {
            return;
        }
    }

    if (info != nullptr && info->ErrorID != 0) {
        finish(
            MarketState::SubscriptionFailed,
            info->ErrorID,
            field_text(info->ErrorMsg));
        return;
    }

    if (instrument == nullptr) {
        finish(
            MarketState::SubscriptionFailed,
            kMissingSubscriptionResponse,
            "subscription response is missing");
        return;
    }

    if (field_text(instrument->InstrumentID) != config_.instrument()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != MarketState::SubscriptionPending) {
            return;
        }
        result_.state = MarketState::TickPending;
    }
}

void MarketClient::OnRtnDepthMarketData(
    CThostFtdcDepthMarketDataField* tick)
{
    CallbackGuard callback{*this};
    if (!callback) {
        return;
    }

    if (tick == nullptr) {
        return;
    }

    bool completed = false;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (result_.state != MarketState::TickPending ||
            field_text(tick->InstrumentID) != config_.instrument()) {
            return;
        }

        result_.ticks.push_back(MarketTick{
            field_text(tick->InstrumentID),
            field_text(tick->UpdateTime),
            tick->UpdateMillisec,
            tick->LastPrice,
            tick->BidPrice1,
            tick->AskPrice1,
            tick->Volume});

        if (result_.ticks.size() ==
            static_cast<std::size_t>(config_.ticks())) {
            result_.state = MarketState::Completed;
            completed = true;
        }
    }

    if (completed) {
        condition_.notify_one();
    }
}

bool MarketClient::is_terminal() const
{
    return result_.state == MarketState::Completed ||
           result_.state == MarketState::LoginFailed ||
           result_.state == MarketState::SubscriptionFailed ||
           result_.state == MarketState::Disconnected ||
           result_.state == MarketState::TimedOut ||
           result_.state == MarketState::Interrupted;
}

MarketClient::CallbackGuard::CallbackGuard(MarketClient& client)
    : client_(client), entered_(client_.enter_callback())
{
}

MarketClient::CallbackGuard::~CallbackGuard()
{
    if (entered_) {
        client_.leave_callback();
    }
}

MarketClient::CallbackGuard::operator bool() const
{
    return entered_;
}

bool MarketClient::enter_callback()
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (releasing_ || released_) {
        return false;
    }
    ++active_callbacks_;
    return true;
}

void MarketClient::leave_callback()
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        --active_callbacks_;
    }
    condition_.notify_all();
}

bool MarketClient::can_request(MarketState expected_state)
{
    std::lock_guard<std::mutex> lock{mutex_};
    return !releasing_ && !released_ && result_.state == expected_state;
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

int run_market(
    const RuntimeConfig& config,
    std::chrono::milliseconds timeout,
    const StopRequested& stop_requested)
{
    auto api = create_market_api();
    if (!api) {
        std::cerr << "[error] failed to create market API or flow directory\n";
        return 3;
    }

    MarketClient client{config, std::move(api)};
    const auto result = client.run(timeout, stop_requested);

    return report_market_result(result, std::cout, std::cerr);
}

int market_exit_code(MarketState state)
{
    if (state == MarketState::Completed) {
        return 0;
    }
    if (state == MarketState::Interrupted) {
        return 130;
    }
    if (state == MarketState::Disconnected ||
        state == MarketState::TimedOut) {
        return 4;
    }
    if (state == MarketState::SubscriptionFailed) {
        return 6;
    }
    return 5;
}

}
