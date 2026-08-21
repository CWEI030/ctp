#include "ctp/config.hpp"

#include "ThostFtdcUserApiDataType.h"

#include <charconv>
#include <cstdlib>
#include <limits>
#include <unordered_set>
#include <utility>

namespace ctp {
namespace {

static_assert(sizeof(TThostFtdcBrokerIDType) == kBrokerIdCapacity);
static_assert(sizeof(TThostFtdcUserIDType) == kUserIdCapacity);
static_assert(sizeof(TThostFtdcPasswordType) == kPasswordCapacity);
static_assert(sizeof(TThostFtdcInstrumentIDType) == kInstrumentIdCapacity);
static_assert(sizeof(TThostFtdcAuthCodeType) == kAuthCodeCapacity);
static_assert(sizeof(TThostFtdcAppIDType) == kAppIdCapacity);

struct Profile {
    std::string_view name;
    std::string_view market_front;
    std::string_view trader_front;
};

constexpr Profile kProfiles[]{
    {"simnow-1", "tcp://180.168.146.187:10211", "tcp://180.168.146.187:10201"},
    {"simnow-2", "tcp://180.168.146.187:10212", "tcp://180.168.146.187:10202"},
    {"simnow-7x24", "tcp://182.254.243.31:40011", "tcp://182.254.243.31:40001"},
};

ConfigResult failure(std::string message)
{
    return {std::nullopt, std::move(message)};
}

const Profile* find_profile(std::string_view name)
{
    for (const auto& profile : kProfiles) {
        if (profile.name == name) {
            return &profile;
        }
    }
    return nullptr;
}

std::optional<std::string> read_required(
    const EnvironmentReader& reader,
    std::string_view name)
{
    auto value = reader(name);
    if (!value || value->empty()) {
        return std::nullopt;
    }
    return value;
}

bool fits_field(std::string_view value, std::size_t capacity)
{
    return !value.empty() && value.size() < capacity;
}

bool is_tcp_front(std::string_view value)
{
    constexpr std::string_view prefix{"tcp://"};
    return value.size() > prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::optional<int> parse_positive_int(std::string_view text)
{
    int value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last || value <= 0) {
        return std::nullopt;
    }
    return value;
}

}

RuntimeConfig::RuntimeConfig(
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
    int ticks)
    : mode_(mode),
      profile_(std::move(profile)),
      broker_id_(std::move(broker_id)),
      user_id_(std::move(user_id)),
      password_(std::move(password)),
      app_id_(std::move(app_id)),
      auth_code_(std::move(auth_code)),
      market_front_(std::move(market_front)),
      trader_front_(std::move(trader_front)),
      instrument_(std::move(instrument)),
      ticks_(ticks)
{
}

ConfigResult parse_config(
    const std::vector<std::string_view>& arguments,
    const EnvironmentReader& read_environment)
{
    if (arguments.empty()) {
        return failure("mode must be market or account");
    }

    Mode mode;
    if (arguments.front() == "market") {
        mode = Mode::Market;
    } else if (arguments.front() == "account") {
        mode = Mode::Account;
    } else {
        return failure("mode must be market or account");
    }

    std::string profile_name{"simnow-7x24"};
    std::string instrument;
    std::string ticks_text;
    std::unordered_set<std::string_view> seen_options;

    for (std::size_t index = 1; index < arguments.size(); index += 2) {
        const auto option = arguments[index];
        if (option == "--password") {
            return failure("--password is forbidden; use CTP_PASSWORD");
        }
        if (option != "--profile" && option != "--instrument" && option != "--ticks") {
            return failure("unknown command-line option");
        }
        if (!seen_options.insert(option).second) {
            return failure("duplicate command-line option");
        }
        if (index + 1 >= arguments.size()) {
            return failure("command-line option is missing a value");
        }

        const auto value = arguments[index + 1];
        if (option == "--profile") {
            profile_name = std::string{value};
        } else if (option == "--instrument") {
            instrument = std::string{value};
        } else {
            ticks_text = std::string{value};
        }
    }

    if (mode == Mode::Account && (!instrument.empty() || !ticks_text.empty())) {
        return failure("account mode does not accept market options");
    }
    if (mode == Mode::Market && instrument.empty()) {
        return failure("instrument is required in market mode");
    }
    if (mode == Mode::Market && !fits_field(instrument, kInstrumentIdCapacity)) {
        return failure("instrument exceeds the CTP field capacity");
    }

    int ticks = 0;
    if (mode == Mode::Market) {
        const auto parsed_ticks = parse_positive_int(ticks_text);
        if (!parsed_ticks) {
            return failure("ticks must be a positive integer");
        }
        ticks = *parsed_ticks;
    }

    const Profile* profile = find_profile(profile_name);
    if (profile == nullptr) {
        return failure("profile is unknown");
    }

    std::string broker_id = read_environment("CTP_BROKER_ID").value_or("9999");
    const auto user_id = read_required(read_environment, "CTP_USER_ID");
    const auto password = read_required(read_environment, "CTP_PASSWORD");
    if (!user_id) {
        return failure("CTP_USER_ID is required");
    }
    if (!password) {
        return failure("CTP_PASSWORD is required");
    }
    if (!fits_field(broker_id, kBrokerIdCapacity)) {
        return failure("CTP_BROKER_ID exceeds the CTP field capacity");
    }
    if (!fits_field(*user_id, kUserIdCapacity)) {
        return failure("CTP_USER_ID exceeds the CTP field capacity");
    }
    if (!fits_field(*password, kPasswordCapacity)) {
        return failure("CTP_PASSWORD exceeds the CTP field capacity");
    }

    std::string app_id = read_environment("CTP_APP_ID").value_or("");
    std::string auth_code = read_environment("CTP_AUTH_CODE").value_or("");
    if (mode == Mode::Account) {
        if (app_id.empty()) {
            return failure("CTP_APP_ID is required in account mode");
        }
        if (auth_code.empty()) {
            return failure("CTP_AUTH_CODE is required in account mode");
        }
        if (!fits_field(app_id, kAppIdCapacity)) {
            return failure("CTP_APP_ID exceeds the CTP field capacity");
        }
        if (!fits_field(auth_code, kAuthCodeCapacity)) {
            return failure("CTP_AUTH_CODE exceeds the CTP field capacity");
        }
    }

    std::string market_front =
        read_environment("CTP_MD_FRONT").value_or(std::string{profile->market_front});
    std::string trader_front =
        read_environment("CTP_TD_FRONT").value_or(std::string{profile->trader_front});
    if (!is_tcp_front(market_front)) {
        return failure("CTP_MD_FRONT must use tcp://");
    }
    if (!is_tcp_front(trader_front)) {
        return failure("CTP_TD_FRONT must use tcp://");
    }

    RuntimeConfig config{
        mode,
        std::move(profile_name),
        std::move(broker_id),
        *user_id,
        *password,
        std::move(app_id),
        std::move(auth_code),
        std::move(market_front),
        std::move(trader_front),
        std::move(instrument),
        ticks};
    return {std::move(config), {}};
}

EnvironmentReader system_environment()
{
    return [](std::string_view name) -> std::optional<std::string> {
        const std::string owned_name{name};
        const char* value = std::getenv(owned_name.c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string{value};
    };
}

}
