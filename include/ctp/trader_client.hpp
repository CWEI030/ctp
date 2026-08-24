#pragma once

#include "ThostFtdcTraderApi.h"
#include "ctp/config.hpp"
#include "ctp/interrupt.hpp"

#include <chrono>
#include <condition_variable>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ctp {

enum class TraderState {
    Connecting,
    AuthenticationPending,
    LoginPending,
    TradingAccountPending,
    InvestorPositionPending,
    Completed,
    AuthenticationFailed,
    LoginFailed,
    QueryFailed,
    Disconnected,
    TimedOut,
    Interrupted,
};

struct TradingAccountSummary {
    std::string account_id;
    double balance{0.0};
    double available{0.0};
    double current_margin{0.0};
};

struct PositionSummary {
    std::string instrument_id;
    char direction{'\0'};
    int position{0};
    int today_position{0};
    int yesterday_position{0};
};

struct TraderResult {
    TraderState state{TraderState::Connecting};
    int error_code{0};
    std::string error_message;
    std::optional<TradingAccountSummary> account;
    std::vector<PositionSummary> positions;
};

int report_trader_result(
    const TraderResult& result,
    std::ostream& output,
    std::ostream& error);

class TraderApi {
public:
    virtual ~TraderApi() = default;

    virtual void register_spi(CThostFtdcTraderSpi* spi) = 0;
    virtual void subscribe_private_topic(
        THOST_TE_RESUME_TYPE resume_type, int sequence) = 0;
    virtual void subscribe_public_topic(THOST_TE_RESUME_TYPE resume_type) = 0;
    virtual void register_front(const std::string& front) = 0;
    virtual void init() = 0;
    virtual int request_authenticate(
        CThostFtdcReqAuthenticateField* request, int request_id) = 0;
    virtual int request_user_login(
        CThostFtdcReqUserLoginField* request, int request_id) = 0;
    virtual int request_trading_account(
        CThostFtdcQryTradingAccountField* request, int request_id) = 0;
    virtual int request_investor_position(
        CThostFtdcQryInvestorPositionField* request, int request_id) = 0;
    virtual void release() = 0;
};

class TraderClient final : public CThostFtdcTraderSpi {
public:
    TraderClient(
        const RuntimeConfig& config,
        std::unique_ptr<TraderApi> api);
    ~TraderClient();

    TraderResult run(
        std::chrono::milliseconds timeout,
        const StopRequested& stop_requested = {});

    void OnFrontConnected() override;
    void OnFrontDisconnected(int reason) override;
    void OnRspAuthenticate(
        CThostFtdcRspAuthenticateField* response,
        CThostFtdcRspInfoField* info,
        int request_id,
        bool is_last) override;
    void OnRspUserLogin(
        CThostFtdcRspUserLoginField* response,
        CThostFtdcRspInfoField* info,
        int request_id,
        bool is_last) override;
    void OnRspQryTradingAccount(
        CThostFtdcTradingAccountField* response,
        CThostFtdcRspInfoField* info,
        int request_id,
        bool is_last) override;
    void OnRspQryInvestorPosition(
        CThostFtdcInvestorPositionField* response,
        CThostFtdcRspInfoField* info,
        int request_id,
        bool is_last) override;

private:
    bool is_terminal() const;
    void finish(TraderState state, int error_code, std::string message);
    void release_api();

    RuntimeConfig config_;
    std::unique_ptr<TraderApi> api_;
    std::mutex mutex_;
    std::condition_variable condition_;
    TraderResult result_;
    bool released_{false};
};

int run_account(
    const RuntimeConfig& config,
    std::chrono::milliseconds timeout = std::chrono::seconds{15},
    const StopRequested& stop_requested = {});
int trader_exit_code(TraderState state);

}
