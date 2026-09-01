#pragma once

#include "ThostFtdcMdApi.h"
#include "ctp/config.hpp"
#include "ctp/interrupt.hpp"

#include <chrono>
#include <condition_variable>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ctp {

enum class MarketState {
    Connecting,
    LoginPending,
    SubscriptionPending,
    TickPending,
    Completed,
    LoginFailed,
    SubscriptionFailed,
    Disconnected,
    TimedOut,
    Interrupted,
};

struct MarketTick {
    std::string instrument;
    std::string update_time;
    int update_millisec{0};
    double last_price{0};
    double bid_price{0};
    double ask_price{0};
    int volume{0};
};

struct MarketResult {
    MarketState state{MarketState::Connecting};
    int error_code{0};
    std::string error_message;
    std::vector<MarketTick> ticks;
};

std::string format_market_tick(const MarketTick& tick);
int report_market_result(
    const MarketResult& result,
    std::ostream& output,
    std::ostream& error);

class MarketApi {
public:
    virtual ~MarketApi() = default;

    virtual void register_spi(CThostFtdcMdSpi* spi) = 0;
    virtual void register_front(const std::string& front) = 0;
    virtual void init() = 0;
    virtual int request_user_login(
        CThostFtdcReqUserLoginField* request,
        int request_id) = 0;
    virtual int subscribe_market_data(
        char* instruments[],
        int count) = 0;
    virtual void release() = 0;
};

// 新引擎复用现有真实行情适配器；目录创建和 SDK 对象分配只发生在控制面。
std::unique_ptr<MarketApi> create_market_api();

class MarketClient final : public CThostFtdcMdSpi {
public:
    MarketClient(
        const RuntimeConfig& config,
        std::unique_ptr<MarketApi> api);
    ~MarketClient();

    MarketResult run(
        std::chrono::milliseconds timeout,
        const StopRequested& stop_requested = {});

    void OnFrontConnected() override;
    void OnFrontDisconnected(int reason) override;
    void OnRspUserLogin(
        CThostFtdcRspUserLoginField* response,
        CThostFtdcRspInfoField* info,
        int request_id,
        bool is_last) override;
    void OnRspSubMarketData(
        CThostFtdcSpecificInstrumentField* instrument,
        CThostFtdcRspInfoField* info,
        int request_id,
        bool is_last) override;
    void OnRtnDepthMarketData(
        CThostFtdcDepthMarketDataField* tick) override;

private:
    class CallbackGuard {
    public:
        explicit CallbackGuard(MarketClient& client);
        ~CallbackGuard();

        CallbackGuard(const CallbackGuard&) = delete;
        CallbackGuard& operator=(const CallbackGuard&) = delete;

        explicit operator bool() const;

    private:
        MarketClient& client_;
        bool entered_;
    };

    bool is_terminal() const;
    bool enter_callback();
    void leave_callback();
    bool can_request(MarketState expected_state);
    void finish(MarketState state, int error_code, std::string message);
    void release_api();

    RuntimeConfig config_;
    std::unique_ptr<MarketApi> api_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    MarketResult result_;
    // Release 前等待完整回调退出，避免 SPI 对象仍被访问时结束其生命周期。
    std::size_t active_callbacks_{0};
    bool releasing_{false};
    bool released_{false};
};

int run_market(
    const RuntimeConfig& config,
    std::chrono::milliseconds timeout = std::chrono::seconds{15},
    const StopRequested& stop_requested = {});
int market_exit_code(MarketState state);

}
