#pragma once

#include "ThostFtdcTraderApi.h"
#include "ctp/config.hpp"

#include <chrono>
#include <condition_variable>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>

namespace ctp {

enum class TraderState {
    Connecting,
    AuthenticationPending,
    LoginPending,
    ReadyForQuery,
    AuthenticationFailed,
    LoginFailed,
    Disconnected,
    TimedOut,
};

struct TraderResult {
    TraderState state{TraderState::Connecting};
    int error_code{0};
    std::string error_message;
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
    virtual void release() = 0;
};

class TraderClient final : public CThostFtdcTraderSpi {
public:
    TraderClient(
        const RuntimeConfig& config,
        std::unique_ptr<TraderApi> api);
    ~TraderClient();

    TraderResult run(std::chrono::milliseconds timeout);

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
    std::chrono::milliseconds timeout = std::chrono::seconds{15});
int trader_exit_code(TraderState state);

}
