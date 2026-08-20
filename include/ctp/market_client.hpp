#pragma once

#include "ThostFtdcMdApi.h"
#include "ctp/config.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace ctp {

enum class MarketState {
    Connecting,
    LoginPending,
    LoginSucceeded,
    LoginFailed,
    Disconnected,
    TimedOut,
};

struct MarketResult {
    MarketState state{MarketState::Connecting};
    int error_code{0};
    std::string error_message;
};

class MarketApi {
public:
    virtual ~MarketApi() = default;

    virtual void register_spi(CThostFtdcMdSpi* spi) = 0;
    virtual void register_front(const std::string& front) = 0;
    virtual void init() = 0;
    virtual int request_user_login(
        CThostFtdcReqUserLoginField* request,
        int request_id) = 0;
    virtual void release() = 0;
};

class MarketClient final : public CThostFtdcMdSpi {
public:
    MarketClient(
        const RuntimeConfig& config,
        std::unique_ptr<MarketApi> api);
    ~MarketClient();

    MarketResult run(std::chrono::milliseconds timeout);

    void OnFrontConnected() override;
    void OnFrontDisconnected(int reason) override;
    void OnRspUserLogin(
        CThostFtdcRspUserLoginField* response,
        CThostFtdcRspInfoField* info,
        int request_id,
        bool is_last) override;

private:
    bool is_terminal() const;
    void finish(MarketState state, int error_code, std::string message);
    void release_api();

    RuntimeConfig config_;
    std::unique_ptr<MarketApi> api_;
    std::mutex mutex_;
    std::condition_variable condition_;
    MarketResult result_;
    bool released_{false};
};

int run_market(
    const RuntimeConfig& config,
    std::chrono::milliseconds timeout = std::chrono::seconds{15});
int market_exit_code(MarketState state);

}
