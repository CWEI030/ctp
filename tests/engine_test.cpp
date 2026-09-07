#include "ctp/engine.hpp"
#include "ctp/field.hpp"
#include "ctp/trading.hpp"
#define CTP_TEST_DEFINE_ALLOCATION_OPERATORS
#include "test_support.hpp"

#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace {

std::vector<ctp::AccountConfig> make_accounts(std::size_t count)
{
    std::vector<ctp::AccountConfig> accounts;
    accounts.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::string suffix = std::to_string(index + 1);
        accounts.emplace_back(
            "account" + suffix,
            "9999",
            "user" + suffix,
            "password" + suffix,
            "app" + suffix,
            "auth" + suffix,
            "tcp://127.0.0.1:4100" + suffix);
    }
    return accounts;
}

CThostFtdcDepthMarketDataField make_tick(int millisec = 7)
{
    CThostFtdcDepthMarketDataField tick{};
    ctp::copy_to_field(tick.InstrumentID, "IF2609");
    ctp::copy_to_field(tick.UpdateTime, "09:30:01");
    tick.UpdateMillisec = millisec;
    tick.LastPrice = 100.0 + millisec * 0.2;
    tick.BidPrice1 = 99.8 + millisec * 0.2;
    tick.AskPrice1 = 100.2 + millisec * 0.2;
    tick.BidVolume1 = 10 + millisec;
    tick.AskVolume1 = 20 + millisec;
    tick.Volume = 1000 + millisec;
    return tick;
}

void test_fixed_event_and_queue(test_support::TestRunner& runner)
{
    runner.expect(
        std::is_trivially_copyable<ctp::MarketEvent>::value,
        "MarketEvent must be trivially copyable");

    ctp::SpscQueue<int, 3> queue;
    runner.expect(queue.try_push(10), "first queue slot must accept an event");
    runner.expect(queue.try_push(20), "second queue slot must accept an event");
    runner.expect(queue.try_push(30), "all declared queue slots must be usable");
    runner.expect(!queue.try_push(40), "a full queue must reject without overwrite");
    runner.expect(queue.depth() == 3, "full queue depth must equal capacity");
    runner.expect(queue.high_watermark() == 3, "high-water mark must reach capacity");
    runner.expect(queue.dropped_count() == 1, "full push must count one drop");

    int value = 0;
    runner.expect(queue.try_pop(value) && value == 10, "queue must pop in FIFO order");
    runner.expect(queue.try_pop(value) && value == 20, "queue must retain middle event");
    runner.expect(queue.try_pop(value) && value == 30, "queue must retain last event");
    runner.expect(!queue.try_pop(value), "empty queue must return immediately");
}

void test_variable_account_assembly(test_support::TestRunner& runner)
{
    for (std::size_t count = 1; count <= 5; ++count) {
        ctp::MarketIngress ingress{make_accounts(count), 0.2};
        runner.expect(
            ingress.account_count() == count,
            "ingress must create one independent channel per configured account");
        runner.expect(
            ingress.account_id(count - 1) == "account" + std::to_string(count),
            "account channel order must follow the immutable config order");
    }
}

void test_normalization_and_four_account_distribution(
    test_support::TestRunner& runner)
{
    ctp::MarketIngress ingress{make_accounts(4), 0.2};
    ingress.start();
    auto tick = make_tick(0);
    const auto result = ingress.ingest(&tick, 123456789);

    runner.expect(result.published == 4, "one valid tick must reach all four accounts");
    runner.expect(result.overflowed == 0, "available queues must not report overflow");
    for (std::size_t account = 0; account < 4; ++account) {
        ctp::MarketEvent event{};
        runner.expect(ingress.try_pop(account, event), "each account must receive the tick");
        runner.expect(event.market_seq == 1, "all accounts must receive the same sequence");
        runner.expect(event.recv_mono_ns == 123456789, "receive time must be retained");
        runner.expect(event.exchange_time_ms == 34201000, "exchange time must use milliseconds");
        runner.expect(event.last_price_ticks == 500, "last price must use integer ticks");
        runner.expect(event.bid_price_ticks == 499, "bid price must use integer ticks");
        runner.expect(event.ask_price_ticks == 501, "ask price must use integer ticks");
        runner.expect(
            std::string_view{event.instrument.data()} == "IF2609",
            "the fixed event must retain the instrument identifier");
        runner.expect(
            event.status == ctp::MarketDataStatus::Valid,
            "valid book data must retain an explicit valid status");

        event.instrument[0] = 'X';
        if (account + 1 < 4) {
            runner.expect(
                ingress.snapshot(account + 1).depth == 1,
                "changing one popped copy must not alter another account queue");
        }
    }
}

void test_slow_account_does_not_block_other_accounts(
    test_support::TestRunner& runner)
{
    for (std::size_t slow_account = 0; slow_account < 4; ++slow_account) {
        ctp::MarketIngress ingress{make_accounts(4), 0.2};
        ingress.start();

        for (std::size_t sequence = 0;
             sequence < ctp::kMarketQueueCapacity + 2;
             ++sequence) {
            auto tick = make_tick(static_cast<int>(sequence % 100));
            const auto result =
                ingress.ingest(&tick, static_cast<std::int64_t>(sequence));

            for (std::size_t account = 0; account < 4; ++account) {
                if (account == slow_account) {
                    continue;
                }
                ctp::MarketEvent event{};
                runner.expect(
                    ingress.try_pop(account, event),
                    "active accounts must continue while one account is paused");
                runner.expect(
                    event.market_seq == sequence + 1,
                    "active account sequence must remain continuous");
            }

            if (sequence >= ctp::kMarketQueueCapacity) {
                runner.expect(
                    result.published == 3 && result.overflowed == 1,
                    "overflow result must identify only the paused account");
            }
        }

        const auto slow = ingress.snapshot(slow_account);
        runner.expect(slow.overflowed, "only the paused account must be marked overflowed");
        runner.expect(slow.dropped == 2, "paused account must count every omitted event");
        for (std::size_t account = 0; account < 4; ++account) {
            if (account == slow_account) {
                continue;
            }
            const auto active = ingress.snapshot(account);
            runner.expect(!active.overflowed, "other accounts must remain healthy");
            runner.expect(active.dropped == 0, "other accounts must not inherit drops");
        }
    }
}

void expect_status_for_all_accounts(
    test_support::TestRunner& runner,
    ctp::MarketIngress& ingress,
    ctp::MarketDataStatus expected,
    std::string_view message)
{
    for (std::size_t account = 0; account < ingress.account_count(); ++account) {
        ctp::MarketEvent event{};
        runner.expect(ingress.try_pop(account, event), message);
        runner.expect(event.status == expected, message);
    }
}

void test_lifecycle_invalid_data_and_hot_path_allocation(
    test_support::TestRunner& runner)
{
    ctp::MarketIngress ingress{make_accounts(2), 0.2};
    auto tick = make_tick(0);
    runner.expect(
        ingress.ingest(&tick, 1).published == 0,
        "ingress must ignore callbacks before start");

    ingress.start();
    runner.expect(
        ingress.ingest(nullptr, 2).published == 2,
        "null callbacks must become explicit events");
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::NullData,
        "null callbacks must have NullData status");

    tick.InstrumentID[0] = '\0';
    runner.expect(
        ingress.ingest(&tick, 3).published == 2,
        "invalid market data must remain visible to every account");
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::InvalidInstrument,
        "empty instrument must be marked invalid");

    tick = make_tick(0);
    ctp::copy_to_field(tick.UpdateTime, "25:00:00");
    ingress.ingest(&tick, 4);
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::InvalidTime,
        "invalid exchange time must be classified");

    tick = make_tick(0);
    tick.LastPrice = std::numeric_limits<double>::quiet_NaN();
    ingress.ingest(&tick, 5);
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::InvalidPrice,
        "non-finite price must be classified");

    tick = make_tick(0);
    tick.BidPrice1 = tick.AskPrice1 + 1.0;
    ingress.ingest(&tick, 6);
    expect_status_for_all_accounts(
        runner, ingress, ctp::MarketDataStatus::InvalidBook,
        "crossed book must be marked invalid instead of silently discarded");

    tick = make_tick(0);
    ingress.ingest(&tick, 7);
    for (std::size_t account = 0; account < 2; ++account) {
        ctp::MarketEvent event{};
        ingress.try_pop(account, event);
    }

    test_support::AllocationProbe allocation_probe;
    ingress.OnRtnDepthMarketData(&tick);
    allocation_probe.stop();
    runner.expect(
        ingress.snapshot(0).depth == 1 && ingress.snapshot(1).depth == 1,
        "preheated callback must still publish normally");
    runner.expect(
        allocation_probe.count() == 0,
        "preheated callback must not allocate memory");

    ingress.stop();
    runner.expect(
        ingress.ingest(&tick, 8).published == 0,
        "ingress must ignore callbacks after stop");
}

ctp::MarketDataStatus normalize_price_status(
    CThostFtdcDepthMarketDataField tick,
    double minimum_price_increment)
{
    ctp::MarketIngress ingress{make_accounts(1), minimum_price_increment};
    ingress.start();
    ingress.ingest(&tick, 1);
    ctp::MarketEvent event{};
    ingress.try_pop(0, event);
    return event.status;
}

void test_price_normalization_boundaries(test_support::TestRunner& runner)
{
    auto tick = make_tick(0);
    tick.LastPrice = 100.20000000000002;
    runner.expect(
        normalize_price_status(tick, 0.2) == ctp::MarketDataStatus::Valid,
        "binary floating representation near an exact tick must normalize");

    tick = make_tick(0);
    tick.LastPrice = 100.1;
    runner.expect(
        normalize_price_status(tick, 0.2)
            == ctp::MarketDataStatus::InvalidPrice,
        "a half-tick price must not be rounded into a tradable price");

    tick = make_tick(0);
    tick.LastPrice = -100.0;
    runner.expect(
        normalize_price_status(tick, 0.2)
            == ctp::MarketDataStatus::InvalidPrice,
        "a negative source price must be rejected");

    tick = make_tick(0);
    tick.AskPrice1 = std::numeric_limits<double>::infinity();
    runner.expect(
        normalize_price_status(tick, 0.2)
            == ctp::MarketDataStatus::InvalidPrice,
        "an infinite source price must be rejected");

    tick = make_tick(0);
    runner.expect(
        normalize_price_status(tick, 0.0)
            == ctp::MarketDataStatus::InvalidPrice,
        "a non-positive minimum price increment must reject normalization");

    tick = make_tick(0);
    tick.BidPrice1 = std::numeric_limits<double>::max();
    runner.expect(
        normalize_price_status(tick, 0.2)
            == ctp::MarketDataStatus::InvalidPrice,
        "a tick conversion outside int64 range must be rejected");
}

ctp::RiskLimits isolation_limits()
{
    ctp::RiskLimits limits{};
    ctp::copy_to_field(limits.allowed_instrument, "IF2609");
    limits.max_market_age_ns = 1'000'000;
    limits.max_slippage_ticks = 2;
    limits.margin_per_lot = 100;
    limits.minimum_available_after_order = 100;
    limits.max_daily_signals = 10;
    limits.max_daily_orders = 10;
    limits.max_daily_cancels = 10;
    limits.max_active_open_orders = 1;
    limits.max_net_open_position = 1;
    return limits;
}

ctp::RiskSnapshot isolation_snapshot()
{
    ctp::RiskSnapshot snapshot{};
    snapshot.enabled = true;
    snapshot.authenticated = true;
    snapshot.logged_in = true;
    snapshot.reconciled = true;
    snapshot.trading_window_open = true;
    snapshot.market_valid = true;
    snapshot.funds_known = true;
    snapshot.positions_known = true;
    snapshot.now_ns = 2'000'000;
    snapshot.market_receive_ns = 1'500'000;
    snapshot.bid_price_ticks = 499;
    snapshot.ask_price_ticks = 501;
    snapshot.available_funds = 200;
    return snapshot;
}

ctp::OrderIntent isolation_intent(std::uint64_t signal_id)
{
    ctp::OrderIntent intent{};
    intent.signal_id = signal_id;
    ctp::copy_to_field(intent.instrument, "IF2609");
    intent.direction = ctp::Direction::Buy;
    intent.offset = ctp::Offset::Open;
    intent.quantity = 1;
    intent.limit_price_ticks = 502;
    return intent;
}

void test_four_account_fault_matrix(test_support::TestRunner& runner)
{
    constexpr std::array expected_faults{
        ctp::AccountFault::Disconnected,
        ctp::AccountFault::MarketQueueOverflow,
        ctp::AccountFault::CallbackQueueOverflow,
        ctp::AccountFault::UnknownCallback,
    };

    for (std::size_t target = 0; target < expected_faults.size(); ++target) {
        auto accounts = make_accounts(4);
        std::array<std::unique_ptr<ctp::AccountTradingSession>, 4> sessions;
        std::array<test_support::FakeTraderApi*, 4> api_views{};
        for (std::size_t account = 0; account < sessions.size(); ++account) {
            auto metrics = std::make_shared<test_support::FakeTraderMetrics>();
            auto api = std::make_unique<test_support::FakeTraderApi>(metrics);
            api_views[account] = api.get();
            sessions[account] = std::make_unique<ctp::AccountTradingSession>(
                accounts[account], isolation_limits(), std::move(api),
                8, 8, 8, 1);
        }

        if (target == 0) {
            api_views[target]->spi()->OnFrontDisconnected(0x1001);
            sessions[target]->drain_callbacks();
        } else if (target == 1) {
            ctp::MarketIngress ingress{accounts, 0.2};
            ingress.start();
            for (std::size_t sequence = 0;
                 sequence <= ctp::kMarketQueueCapacity;
                 ++sequence) {
                auto tick = make_tick(static_cast<int>(sequence % 100));
                ingress.ingest(&tick, static_cast<std::int64_t>(sequence));
                for (std::size_t account = 0; account < sessions.size(); ++account) {
                    if (account == target) continue;
                    ctp::MarketEvent ignored{};
                    ingress.try_pop(account, ignored);
                }
            }
            if (ingress.snapshot(target).overflowed) {
                sessions[target]->mark_fault(
                    ctp::AccountFault::MarketQueueOverflow);
            }
        } else if (target == 2) {
            CThostFtdcOrderField callback{};
            ctp::copy_to_field(callback.OrderRef, "999");
            callback.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
            api_views[target]->spi()->OnRtnOrder(&callback);
            api_views[target]->spi()->OnRtnOrder(&callback);
            sessions[target]->drain_callbacks();
        } else {
            CThostFtdcOrderField callback{};
            ctp::copy_to_field(callback.OrderRef, "999");
            callback.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
            api_views[target]->spi()->OnRtnOrder(&callback);
            sessions[target]->drain_callbacks();
        }

        for (std::size_t account = 0; account < sessions.size(); ++account) {
            const auto state = sessions[account]->execution_snapshot();
            const auto submitted = sessions[account]->submit(
                isolation_intent(1'000 + target * 10 + account),
                isolation_snapshot());
            if (account == target) {
                runner.expect(
                    state.frozen && state.reconciliation_required
                        && state.fault == expected_faults[target]
                        && state.alert_count == 1
                        && submitted.code == ctp::SubmitCode::RiskRejected
                        && submitted.risk_reason
                            == ctp::RiskRejectReason::AccountFrozen,
                    "the selected account alone must expose its stable fault");
            } else {
                runner.expect(
                    !state.frozen && !state.reconciliation_required
                        && state.fault == ctp::AccountFault::None
                        && submitted.code == ctp::SubmitCode::Submitted,
                    "unaffected accounts must retain independent order flow");
            }
        }
    }
}

}

int main()
{
    test_support::TestRunner runner{"engine"};
    test_fixed_event_and_queue(runner);
    test_variable_account_assembly(runner);
    test_normalization_and_four_account_distribution(runner);
    test_slow_account_does_not_block_other_accounts(runner);
    test_lifecycle_invalid_data_and_hot_path_allocation(runner);
    test_price_normalization_boundaries(runner);
    test_four_account_fault_matrix(runner);
    return runner.finish();
}
