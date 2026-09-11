#pragma once

#ifdef CTP_TEST_WITH_TRADER_FAKE
#include "ctp/trader_client.hpp"
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support {

inline std::atomic<bool> count_allocations{false};
inline std::atomic<std::size_t> allocation_count{0};

inline void* allocate(std::size_t size)
{
    if (count_allocations.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}

class AllocationProbe {
public:
    AllocationProbe()
    {
        allocation_count.store(0, std::memory_order_relaxed);
        count_allocations.store(true, std::memory_order_relaxed);
    }

    ~AllocationProbe()
    {
        stop();
    }

    void stop() noexcept
    {
        count_allocations.store(false, std::memory_order_relaxed);
    }

    std::size_t count() const noexcept
    {
        return allocation_count.load(std::memory_order_relaxed);
    }
};

class TestRunner {
public:
    explicit TestRunner(std::string_view suite_name = "tests")
        : suite_name_(suite_name)
    {
    }

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
            std::cout << "[ok] all " << suite_name_ << " tests passed\n";
        }
        return failures_ == 0 ? 0 : 1;
    }

private:
    std::string_view suite_name_;
    int failures_{0};
};

#ifdef CTP_TEST_WITH_TRADER_FAKE
// 各交易测试共用同一个可观察替身，避免每个批次重复实现 CTP 接口。
struct FakeTraderMetrics {
    std::vector<std::string> calls;
    int authenticate_calls{0};
    int login_calls{0};
    int settlement_query_calls{0};
    int settlement_confirmation_calls{0};
    int settlement_query_request_id{0};
    int settlement_confirmation_request_id{0};
    CThostFtdcQrySettlementInfoConfirmField last_settlement_query{};
    CThostFtdcSettlementInfoConfirmField last_settlement_confirmation{};
    int account_calls{0};
    int position_calls{0};
    std::atomic<int> order_insert_calls{0};
    int order_action_calls{0};
    int order_query_calls{0};
    int trade_query_calls{0};
    int release_calls{0};
    int authenticate_request_id{0};
    int login_request_id{0};
    int account_request_id{0};
    int position_request_id{0};
    int order_insert_request_id{0};
    int order_action_request_id{0};
    int order_query_request_id{0};
    int trade_query_request_id{0};
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
    CThostFtdcInputOrderField last_order{};
    CThostFtdcInputOrderActionField last_action{};
    THOST_TE_RESUME_TYPE private_mode{THOST_TERT_RESTART};
    THOST_TE_RESUME_TYPE public_mode{THOST_TERT_RESTART};
    int private_sequence{0};
    std::atomic<bool> authenticate_request_active{false};
    std::atomic<bool> released_during_authenticate_request{false};
};

class FakeTraderApi final : public ctp::TraderApi {
public:
    explicit FakeTraderApi(std::shared_ptr<FakeTraderMetrics> metrics)
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
        if (on_init) on_init(*this);
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
        if (on_authenticate) on_authenticate(*this);
        metrics_->authenticate_request_active = false;
        return authenticate_return_code;
    }

    int request_user_login(
        CThostFtdcReqUserLoginField* request, int request_id) override
    {
        ++metrics_->login_calls;
        metrics_->login_request_id = request_id;
        metrics_->password = request->Password;
        if (on_login) on_login(*this);
        return login_return_code;
    }

    int request_trading_account(
        CThostFtdcQryTradingAccountField* request, int request_id) override
    {
        ++metrics_->account_calls;
        metrics_->account_request_id = request_id;
        metrics_->account_broker_id = request->BrokerID;
        metrics_->account_investor_id = request->InvestorID;
        if (on_account) on_account(*this);
        return account_return_code;
    }

    int request_investor_position(
        CThostFtdcQryInvestorPositionField* request, int request_id) override
    {
        ++metrics_->position_calls;
        metrics_->position_request_id = request_id;
        metrics_->position_broker_id = request->BrokerID;
        metrics_->position_investor_id = request->InvestorID;
        if (on_position) on_position(*this);
        return position_return_code;
    }

    int request_order_insert(
        CThostFtdcInputOrderField* request, int request_id) override
    {
        ++metrics_->order_insert_calls;
        metrics_->order_insert_request_id = request_id;
        metrics_->last_order = *request;
        if (on_order_insert) on_order_insert(*this);
        return order_insert_return_code;
    }

    int request_order_action(
        CThostFtdcInputOrderActionField* request, int request_id) override
    {
        ++metrics_->order_action_calls;
        metrics_->order_action_request_id = request_id;
        metrics_->last_action = *request;
        if (on_order_action) on_order_action(*this);
        return order_action_return_code;
    }

    int request_settlement_query(
        CThostFtdcQrySettlementInfoConfirmField* request, int request_id) override
    {
        ++metrics_->settlement_query_calls;
        metrics_->settlement_query_request_id = request_id;
        metrics_->last_settlement_query = *request;
        if (on_settlement_query) on_settlement_query(*this);
        return settlement_query_return_code;
    }

    int request_settlement_confirmation(
        CThostFtdcSettlementInfoConfirmField* request, int request_id) override
    {
        ++metrics_->settlement_confirmation_calls;
        metrics_->settlement_confirmation_request_id = request_id;
        metrics_->last_settlement_confirmation = *request;
        if (on_settlement_confirmation) on_settlement_confirmation(*this);
        return settlement_confirmation_return_code;
    }

    int request_order_query(CThostFtdcQryOrderField*, int request_id) override
    {
        ++metrics_->order_query_calls;
        metrics_->order_query_request_id = request_id;
        if (on_order_query) on_order_query(*this);
        return order_query_return_code;
    }

    int request_trade_query(CThostFtdcQryTradeField*, int request_id) override
    {
        ++metrics_->trade_query_calls;
        metrics_->trade_query_request_id = request_id;
        if (on_trade_query) on_trade_query(*this);
        return trade_query_return_code;
    }

    void release() override
    {
        if (metrics_->authenticate_request_active) {
            metrics_->released_during_authenticate_request = true;
        }
        ++metrics_->release_calls;
        spi_ = nullptr;
    }

    CThostFtdcTraderSpi* spi() const noexcept { return spi_; }

    std::function<void(FakeTraderApi&)> on_init;
    std::function<void(FakeTraderApi&)> on_authenticate;
    std::function<void(FakeTraderApi&)> on_login;
    std::function<void(FakeTraderApi&)> on_settlement_query;
    std::function<void(FakeTraderApi&)> on_settlement_confirmation;
    int settlement_query_return_code{0};
    int settlement_confirmation_return_code{0};
    std::function<void(FakeTraderApi&)> on_account;
    std::function<void(FakeTraderApi&)> on_position;
    std::function<void(FakeTraderApi&)> on_order_insert;
    std::function<void(FakeTraderApi&)> on_order_action;
    std::function<void(FakeTraderApi&)> on_order_query;
    std::function<void(FakeTraderApi&)> on_trade_query;
    int authenticate_return_code{0};
    int login_return_code{0};
    int account_return_code{0};
    int position_return_code{0};
    int order_insert_return_code{0};
    int order_action_return_code{0};
    int order_query_return_code{0};
    int trade_query_return_code{0};

private:
    std::shared_ptr<FakeTraderMetrics> metrics_;
    CThostFtdcTraderSpi* spi_{nullptr};
};
#endif

}

#ifdef CTP_TEST_DEFINE_ALLOCATION_OPERATORS
void* operator new(std::size_t size)
{
    return test_support::allocate(size);
}

void* operator new[](std::size_t size)
{
    return test_support::allocate(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}
#endif
