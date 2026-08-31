#pragma once

#include "ctp/field.hpp"

#include <cstddef>
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
        std::vector<AccountConfig> accounts = {});

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
