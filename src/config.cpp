#include "ctp/config.hpp"

#include "ThostFtdcUserApiDataType.h"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
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

struct AccountSection {
    std::string alias;
    std::unordered_map<std::string, std::string> fields;
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

std::string trim(std::string_view text)
{
    constexpr std::string_view whitespace{" \t\r"};
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(whitespace);
    return std::string{text.substr(first, last - first + 1)};
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

ConfigResult parse_engine_config(
    const std::vector<std::string_view>& arguments,
    const EnvironmentReader& read_environment)
{
    std::string engine_mode;
    std::string config_path;
    std::unordered_set<std::string_view> seen_options;

    for (std::size_t index = 1; index < arguments.size(); index += 2) {
        const auto option = arguments[index];
        if (option != "--mode" && option != "--config") {
            return failure("unknown engine command-line option");
        }
        if (!seen_options.insert(option).second) {
            return failure("duplicate engine command-line option");
        }
        if (index + 1 >= arguments.size()) {
            return failure("engine command-line option is missing a value");
        }

        if (option == "--mode") {
            engine_mode = std::string{arguments[index + 1]};
        } else {
            config_path = std::string{arguments[index + 1]};
        }
    }

    if (engine_mode != "live") {
        return failure("engine --mode must be live");
    }
    if (config_path.empty()) {
        return failure("engine --config is required");
    }

    std::error_code status_error;
    const auto file_status =
        std::filesystem::symlink_status(config_path, status_error);
    if (status_error) {
        return failure("account config file status cannot be read");
    }
    if (!std::filesystem::is_regular_file(file_status)) {
        return failure("account config path must be a regular file");
    }

    constexpr auto required_permissions =
        std::filesystem::perms::owner_read
        | std::filesystem::perms::owner_write;
    const auto actual_permissions =
        file_status.permissions() & std::filesystem::perms::mask;
    if (actual_permissions != required_permissions) {
        return failure("account config file permissions must be 0600");
    }

    std::ifstream input{config_path};
    if (!input) {
        return failure("account config file cannot be opened");
    }

    std::vector<AccountSection> sections;
    std::unordered_set<std::string> aliases;
    AccountSection* current_section = nullptr;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        const std::string cleaned = trim(line);
        if (cleaned.empty() || cleaned.front() == '#' || cleaned.front() == ';') {
            continue;
        }

        if (cleaned.front() == '[') {
            if (cleaned.back() != ']') {
                return failure("account config has an invalid section header");
            }

            constexpr std::string_view prefix{"account."};
            const std::string section_name =
                cleaned.substr(1, cleaned.size() - 2);
            if (section_name.size() <= prefix.size()
                || section_name.compare(0, prefix.size(), prefix) != 0) {
                return failure("account config section must use [account.alias]");
            }

            std::string alias = section_name.substr(prefix.size());
            if (!aliases.insert(alias).second) {
                return failure("account config contains a duplicate alias");
            }
            sections.push_back({std::move(alias), {}});
            current_section = &sections.back();
            continue;
        }

        if (current_section == nullptr) {
            return failure("account config field appears before a section");
        }

        const auto separator = cleaned.find('=');
        if (separator == std::string::npos) {
            return failure(
                "account config line " + std::to_string(line_number)
                + " must use key=value");
        }

        std::string key = trim(std::string_view{cleaned}.substr(0, separator));
        std::string value = trim(std::string_view{cleaned}.substr(separator + 1));
        if (key.empty()) {
            return failure("account config contains an empty field name");
        }
        if (!current_section->fields.emplace(std::move(key), std::move(value)).second) {
            return failure("account config contains a duplicate field");
        }
    }

    std::vector<AccountConfig> accounts;
    accounts.reserve(sections.size());
    for (auto& section : sections) {
        const auto enabled = section.fields.find("enabled");
        if (enabled == section.fields.end()) {
            return failure("account '" + section.alias + "' is missing enabled");
        }
        if (enabled->second == "false") {
            continue;
        }
        if (enabled->second != "true") {
            return failure("account '" + section.alias + "' enabled must be true or false");
        }

        constexpr std::string_view required_fields[]{
            "broker_id", "user_id", "password", "app_id", "auth_code", "trader_front"};
        for (const auto field : required_fields) {
            const auto found = section.fields.find(std::string{field});
            if (found == section.fields.end() || found->second.empty()) {
                return failure(
                    "account '" + section.alias + "' is missing " + std::string{field});
            }
        }

        struct FixedField {
            std::string_view name;
            std::string_view environment_suffix;
            std::size_t capacity;
        };
        constexpr FixedField fixed_fields[]{
            {"broker_id", "BROKER_ID", kBrokerIdCapacity},
            {"user_id", "USER_ID", kUserIdCapacity},
            {"password", "PASSWORD", kPasswordCapacity},
            {"app_id", "APP_ID", kAppIdCapacity},
            {"auth_code", "AUTH_CODE", kAuthCodeCapacity},
        };
        for (const auto& field : fixed_fields) {
            const std::string environment_name =
                "CTP_ACCOUNT_" + section.alias + "_"
                + std::string{field.environment_suffix};
            if (auto override_value = read_environment(environment_name)) {
                if (!fits_field(*override_value, field.capacity)) {
                    return failure(
                        "environment override '" + environment_name + "' is invalid");
                }
                section.fields.at(std::string{field.name}) = std::move(*override_value);
            }

            if (!fits_field(section.fields.at(std::string{field.name}), field.capacity)) {
                return failure(
                    "account '" + section.alias + "' " + std::string{field.name}
                    + " exceeds CTP field capacity");
            }
        }

        const std::string trader_front_environment =
            "CTP_ACCOUNT_" + section.alias + "_TRADER_FRONT";
        if (auto override_value = read_environment(trader_front_environment)) {
            if (!is_tcp_front(*override_value)) {
                return failure(
                    "environment override '" + trader_front_environment + "' is invalid");
            }
            section.fields.at("trader_front") = std::move(*override_value);
        }

        if (!is_tcp_front(section.fields.at("trader_front"))) {
            return failure("account '" + section.alias + "' trader_front must use tcp://");
        }

        accounts.emplace_back(
            section.alias,
            section.fields.at("broker_id"),
            section.fields.at("user_id"),
            section.fields.at("password"),
            section.fields.at("app_id"),
            section.fields.at("auth_code"),
            section.fields.at("trader_front"));
    }

    if (accounts.empty()) {
        return failure("at least one enabled account is required");
    }

    RuntimeConfig config{
        Mode::Engine,
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        0,
        std::move(accounts)};
    return {std::move(config), {}};
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

AccountConfig::AccountConfig(
    std::string alias,
    std::string broker_id,
    std::string user_id,
    std::string password,
    std::string app_id,
    std::string auth_code,
    std::string trader_front)
    : alias_(std::move(alias)),
      broker_id_(std::move(broker_id)),
      user_id_(std::move(user_id)),
      password_(std::move(password)),
      app_id_(std::move(app_id)),
      auth_code_(std::move(auth_code)),
      trader_front_(std::move(trader_front))
{
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
    int ticks,
    std::vector<AccountConfig> accounts)
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
      ticks_(ticks),
      accounts_(std::move(accounts))
{
}

ConfigResult parse_config(
    const std::vector<std::string_view>& arguments,
    const EnvironmentReader& read_environment)
{
    if (arguments.empty()) {
        return failure("mode must be market or account");
    }

    if (arguments.front() == "engine") {
        return parse_engine_config(arguments, read_environment);
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
