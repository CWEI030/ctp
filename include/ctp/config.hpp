#pragma once

#include "ctp/field.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ctp {

enum class Mode {
    Market,
    Account,
    Engine,
    Benchmark,
};

struct BenchmarkConfig {
    std::string config_path;
    std::string input_path;
    std::string output_path;
    std::size_t account_count{0};
    std::uint64_t rate_per_second{0};
    std::uint32_t warmup_seconds{0};
    std::uint32_t duration_seconds{1};
    std::uint64_t burst_rate_per_second{0};
    std::uint32_t burst_seconds{0};
};

inline constexpr std::size_t kBrokerIdCapacity = 11;
inline constexpr std::size_t kUserIdCapacity = 16;
inline constexpr std::size_t kPasswordCapacity = 41;
inline constexpr std::size_t kInstrumentIdCapacity = 81;
inline constexpr std::size_t kAuthCodeCapacity = 17;
inline constexpr std::size_t kAppIdCapacity = 33;

class AccountConfig {
public:
    AccountConfig(
        std::string alias,
        std::string broker_id,
        std::string user_id,
        std::string password,
        std::string app_id,
        std::string auth_code,
        std::string trader_front);

    const std::string& alias() const { return alias_; }
    const std::string& broker_id() const { return broker_id_; }
    const std::string& user_id() const { return user_id_; }
    const std::string& password() const { return password_; }
    const std::string& app_id() const { return app_id_; }
    const std::string& auth_code() const { return auth_code_; }
    const std::string& trader_front() const { return trader_front_; }

private:
    std::string alias_;
    std::string broker_id_;
    std::string user_id_;
    std::string password_;
    std::string app_id_;
    std::string auth_code_;
    std::string trader_front_;
};

class RuntimeConfig {
public:
    RuntimeConfig(
        Mode mode,
        std::string profile,
        std::string broker_id,
        std::string user_id,
        std::string password,
        std::string app_id,
        std::string auth_code,
        std::string market_front,
        std::string trader_front,
        std::string instrument,
        int ticks,
        std::vector<AccountConfig> accounts = {},
        BenchmarkConfig benchmark = {});

    Mode mode() const { return mode_; }
    const std::string& profile() const { return profile_; }
    const std::string& broker_id() const { return broker_id_; }
    const std::string& user_id() const { return user_id_; }
    const std::string& password() const { return password_; }
    const std::string& app_id() const { return app_id_; }
    const std::string& auth_code() const { return auth_code_; }
    const std::string& market_front() const { return market_front_; }
    const std::string& trader_front() const { return trader_front_; }
    const std::string& instrument() const { return instrument_; }
    int ticks() const { return ticks_; }
    const std::vector<AccountConfig>& accounts() const { return accounts_; }
    const BenchmarkConfig& benchmark() const { return benchmark_; }

private:
    Mode mode_;
    std::string profile_;
    std::string broker_id_;
    std::string user_id_;
    std::string password_;
    std::string app_id_;
    std::string auth_code_;
    std::string market_front_;
    std::string trader_front_;
    std::string instrument_;
    int ticks_;
    std::vector<AccountConfig> accounts_;
    BenchmarkConfig benchmark_;
};

struct ConfigResult {
    std::optional<RuntimeConfig> config;
    std::string error;
};

using EnvironmentReader =
    std::function<std::optional<std::string>(std::string_view)>;

ConfigResult parse_config(
    const std::vector<std::string_view>& arguments,
    const EnvironmentReader& read_environment);

EnvironmentReader system_environment();

}
