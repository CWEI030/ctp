#include "ctp/config.hpp"

#include "ThostFtdcUserApiDataType.h"

#include <charconv>
#include <cmath>
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

bool is_safe_account_alias(std::string_view value)
{
    if (value.empty() || value.size() > 64) return false;
    for (const char character : value) {
        const bool ascii_letter =
            (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z');
        const bool ascii_digit = character >= '0' && character <= '9';
        if (!ascii_letter && !ascii_digit
            && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

bool parse_clock_ms(std::string_view text, std::int32_t& result) noexcept
{
    if (text.size() != 8 || text[2] != ':' || text[5] != ':') return false;
    const auto digit = [](char value) { return value >= '0' && value <= '9'; };
    if (!digit(text[0]) || !digit(text[1]) || !digit(text[3])
        || !digit(text[4]) || !digit(text[6]) || !digit(text[7])) {
        return false;
    }
    const int hour = (text[0] - '0') * 10 + text[1] - '0';
    const int minute = (text[3] - '0') * 10 + text[4] - '0';
    const int second = (text[6] - '0') * 10 + text[7] - '0';
    if (hour > 23 || minute > 59 || second > 59) return false;
    result = (hour * 3'600 + minute * 60 + second) * 1'000;
    return true;
}

bool parse_trading_windows(
    std::string_view text,
    std::vector<TradingWindow>& windows)
{
    if (text.empty()) return false;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto comma = text.find(',', begin);
        const auto item = text.substr(
            begin,
            comma == std::string_view::npos ? text.size() - begin : comma - begin);
        const auto separator = item.find('-');
        TradingWindow window{};
        if (separator == std::string_view::npos
            || item.find('-', separator + 1) != std::string_view::npos
            || !parse_clock_ms(item.substr(0, separator), window.start_ms)
            || !parse_clock_ms(item.substr(separator + 1), window.end_ms)
            || window.start_ms == window.end_ms) {
            return false;
        }
        windows.push_back(window);
        if (comma == std::string_view::npos) return true;
        begin = comma + 1;
        if (begin == text.size()) return false;
    }
    return false;
}

ConfigResult parse_engine_config(
    const std::vector<std::string_view>& arguments,
    const EnvironmentReader& read_environment)
{
    std::string engine_mode;
    std::string config_path;
    bool check_only = false;
    bool allow_orders = false;
    bool acceptance = false;
    std::unordered_set<std::string_view> seen_options;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const auto option = arguments[index];
        if (option != "--mode" && option != "--config" && option != "--check"
            && option != "--allow-orders" && option != "--acceptance") {
            return failure("unknown engine command-line option");
        }
        if (!seen_options.insert(option).second) {
            return failure("duplicate engine command-line option");
        }
        if (option == "--check" || option == "--allow-orders"
            || option == "--acceptance") {
            if (option == "--check") check_only = true;
            else if (option == "--allow-orders") allow_orders = true;
            else acceptance = true;
            continue;
        }
        if (index + 1 >= arguments.size()) {
            return failure("engine command-line option is missing a value");
        }
        ++index;

        if (option == "--mode") {
            engine_mode = std::string{arguments[index]};
        } else {
            config_path = std::string{arguments[index]};
        }
    }

    if (engine_mode != "live") {
        return failure("engine --mode must be live");
    }
    if (config_path.empty()) {
        config_path = "config/accounts.local.ini";
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
    std::unordered_map<std::string, std::string> engine_fields;
    std::unordered_map<std::string, std::string> strategy_fields;
    std::unordered_map<std::string, std::string> risk_fields;
    std::unordered_set<std::string> control_sections;
    std::unordered_set<std::string> aliases;
    std::unordered_map<std::string, std::string>* current_fields = nullptr;
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
            if (section_name == "engine" || section_name == "strategy"
                || section_name == "risk") {
                if (!control_sections.insert(section_name).second) {
                    return failure("account config contains a duplicate section");
                }
                current_fields = section_name == "engine" ? &engine_fields
                    : section_name == "strategy" ? &strategy_fields
                    : &risk_fields;
                continue;
            }
            if (section_name.size() <= prefix.size()
                || section_name.compare(0, prefix.size(), prefix) != 0) {
                return failure("unknown account config section");
            }

            std::string alias = section_name.substr(prefix.size());
            if (!is_safe_account_alias(alias)) {
                return failure("account config contains an invalid alias");
            }
            if (!aliases.insert(alias).second) {
                return failure("account config contains a duplicate alias");
            }
            sections.push_back({std::move(alias), {}});
            current_fields = &sections.back().fields;
            continue;
        }

        if (current_fields == nullptr) {
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
        if (!current_fields->emplace(std::move(key), std::move(value)).second) {
            return failure("account config contains a duplicate field");
        }
    }

    LiveConfig live{};
    live.config_path = config_path;
    live.check_only = check_only;
    live.allow_orders = allow_orders;
    const auto reject_unknown = [](const auto& fields, const auto& allowed) {
        for (const auto& [name, ignored] : fields) {
            (void)ignored;
            if (allowed.find(name) == allowed.end()) return name;
        }
        return std::string{};
    };
    const std::unordered_set<std::string> allowed_engine{
        "profile", "market_front", "exchange_id", "instrument",
        "minimum_price_increment"};
    const std::unordered_set<std::string> allowed_strategy{
        "enabled", "trigger_price_ticks", "entry_protection_ticks",
        "cancel_after_market_ticks", "max_signals_per_run",
        "close_reprice_after_market_ticks", "max_close_reprices"};
    const std::unordered_set<std::string> allowed_risk{
        "max_order_volume", "max_net_position", "max_active_open_orders",
        "max_orders_per_day", "max_cancels_per_day",
        "max_order_rate_per_second", "max_price_deviation_ticks",
        "margin_per_lot", "min_available_funds", "trading_windows",
        "market_stale_after_ms", "kill_switch"};
    std::string unknown = reject_unknown(engine_fields, allowed_engine);
    if (unknown.empty()) {
        unknown = reject_unknown(strategy_fields, allowed_strategy);
    }
    if (unknown.empty()) {
        unknown = reject_unknown(risk_fields, allowed_risk);
    }
    if (!unknown.empty()) {
        return failure("unknown live config field '" + unknown + "'");
    }
    const auto value = [](const auto& fields, std::string_view name) {
        const auto found = fields.find(std::string{name});
        return found == fields.end() ? std::string_view{} : std::string_view{found->second};
    };
    const auto parse_i64 = [](std::string_view text, std::int64_t& output) {
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), output);
        return !text.empty() && parsed.ec == std::errc{}
            && parsed.ptr == text.data() + text.size();
    };
    const auto parse_u32 = [](std::string_view text, std::uint32_t& output) {
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), output);
        return !text.empty() && parsed.ec == std::errc{}
            && parsed.ptr == text.data() + text.size();
    };
    const auto parse_bool = [](std::string_view text, bool& output) {
        if (text == "true") output = true;
        else if (text == "false") output = false;
        else return false;
        return true;
    };
    const auto parse_double = [](std::string_view text, double& output) {
        if (text.empty()) return false;
        std::string owned{text};
        char* end = nullptr;
        output = std::strtod(owned.c_str(), &end);
        return end == owned.c_str() + owned.size() && std::isfinite(output);
    };
    live.profile = std::string{value(engine_fields, "profile")};
    live.market_front = std::string{value(engine_fields, "market_front")};
    live.exchange_id = std::string{value(engine_fields, "exchange_id")};
    live.instrument = std::string{value(engine_fields, "instrument")};
    if (const auto text = value(engine_fields, "minimum_price_increment");
        !text.empty() && (!parse_double(text, live.minimum_price_increment)
                          || live.minimum_price_increment <= 0.0)) {
        return failure("minimum_price_increment must be positive");
    }
    if (!live.profile.empty()) {
        const Profile* selected = nullptr;
        for (const auto& profile : kProfiles) {
            if (profile.name == live.profile) selected = &profile;
        }
        if (selected == nullptr) return failure("unknown engine profile");
        if (live.market_front.empty()) {
            live.market_front = std::string{selected->market_front};
        }
    }
    if (!live.market_front.empty() && !is_tcp_front(live.market_front)) {
        return failure("engine market_front must use tcp://");
    }
    if (live.instrument.size() >= kInstrumentIdCapacity) {
        return failure("engine instrument exceeds CTP field capacity");
    }
    if (const auto text = value(strategy_fields, "enabled");
        !text.empty() && !parse_bool(text, live.strategy_enabled)) {
        return failure("strategy enabled must be true or false");
    }
    if (const auto text = value(strategy_fields, "trigger_price_ticks");
        !text.empty() && !parse_i64(text, live.trigger_price_ticks)) {
        return failure("trigger_price_ticks must be an integer");
    }
    if (const auto text = value(strategy_fields, "entry_protection_ticks");
        !text.empty() && (!parse_i64(text, live.entry_protection_ticks)
                          || live.entry_protection_ticks < 0)) {
        return failure("entry_protection_ticks must be non-negative");
    }
    struct UnsignedField {
        std::string_view name;
        std::uint32_t* target;
    };
    const UnsignedField strategy_unsigned[]{
        {"cancel_after_market_ticks", &live.cancel_after_market_ticks},
        {"max_signals_per_run", &live.max_signals_per_run},
        {"close_reprice_after_market_ticks", &live.close_reprice_after_market_ticks},
        {"max_close_reprices", &live.max_close_reprices},
    };
    for (const auto& field : strategy_unsigned) {
        const auto text = value(strategy_fields, field.name);
        if (!text.empty() && !parse_u32(text, *field.target)) {
            return failure(std::string{field.name} + " must be a non-negative integer");
        }
    }
    struct PositiveRiskField {
        std::string_view name;
        std::uint32_t* target;
    };
    std::uint32_t max_order_volume = 0;
    std::uint32_t max_net_position = 0;
    std::uint32_t max_active_orders = 0;
    const PositiveRiskField risk_unsigned[]{
        {"max_order_volume", &max_order_volume},
        {"max_net_position", &max_net_position},
        {"max_active_open_orders", &max_active_orders},
        {"max_orders_per_day", &live.max_orders_per_day},
        {"max_cancels_per_day", &live.max_cancels_per_day},
        {"max_order_rate_per_second", &live.max_order_rate_per_second},
    };
    for (const auto& field : risk_unsigned) {
        const auto text = value(risk_fields, field.name);
        if (!text.empty() && (!parse_u32(text, *field.target) || *field.target == 0)) {
            return failure(std::string{field.name} + " must be a positive integer");
        }
    }
    if (max_order_volume > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
        || max_net_position > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
        || max_active_orders > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return failure("risk position limit exceeds int32 capacity");
    }
    live.max_order_volume = static_cast<std::int32_t>(max_order_volume);
    live.max_net_position = static_cast<std::int32_t>(max_net_position);
    live.max_active_open_orders = static_cast<std::int32_t>(max_active_orders);
    if (const auto text = value(risk_fields, "max_price_deviation_ticks");
        !text.empty() && (!parse_i64(text, live.max_price_deviation_ticks)
                          || live.max_price_deviation_ticks < 0)) {
        return failure("max_price_deviation_ticks must be non-negative");
    }
    if (const auto text = value(risk_fields, "market_stale_after_ms"); !text.empty()) {
        std::int64_t milliseconds = 0;
        if (!parse_i64(text, milliseconds) || milliseconds <= 0
            || milliseconds > std::numeric_limits<std::int64_t>::max() / 1'000'000) {
            return failure("market_stale_after_ms must be a positive integer");
        }
        live.market_stale_after_ns = milliseconds * 1'000'000;
    }
    if (const auto text = value(risk_fields, "min_available_funds"); !text.empty()) {
        double amount = 0.0;
        if (!parse_double(text, amount) || amount < 0.0
            || amount * 100.0 > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return failure("min_available_funds must be a non-negative amount");
        }
        live.minimum_available_funds =
            static_cast<std::int64_t>(std::llround(amount * 100.0));
    }
    if (const auto text = value(risk_fields, "margin_per_lot"); !text.empty()) {
        double amount = 0.0;
        if (!parse_double(text, amount) || amount <= 0.0
            || amount * 100.0 > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return failure("margin_per_lot must be a positive amount");
        }
        live.margin_per_lot =
            static_cast<std::int64_t>(std::llround(amount * 100.0));
        if (live.margin_per_lot <= 0) {
            return failure("margin_per_lot must be at least 0.01");
        }
    }
    if (const auto text = value(risk_fields, "trading_windows"); !text.empty()
        && !parse_trading_windows(text, live.trading_windows)) {
        return failure("trading_windows must contain valid HH:MM:SS-HH:MM:SS ranges");
    }
    if (const auto text = value(risk_fields, "kill_switch");
        !text.empty() && !parse_bool(text, live.kill_switch)) {
        return failure("risk kill_switch must be true or false");
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

    live.acceptance = acceptance;
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
        std::move(accounts),
        {},
        std::move(live)};
    return {std::move(config), {}};
}

ConfigResult parse_benchmark_config(
    const std::vector<std::string_view>& arguments,
    const EnvironmentReader& read_environment)
{
    BenchmarkConfig benchmark{};
    std::string config_path{"config/accounts.local.ini"};
    std::unordered_set<std::string_view> seen_options;

    const auto parse_unsigned = [](std::string_view text, auto& value) {
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), value);
        return !text.empty() && parsed.ec == std::errc{}
            && parsed.ptr == text.data() + text.size();
    };

    for (std::size_t index = 1; index < arguments.size(); index += 2) {
        const auto option = arguments[index];
        if (index + 1 >= arguments.size()) {
            return failure("benchmark command-line option is missing a value");
        }
        if (!seen_options.insert(option).second) {
            return failure("duplicate benchmark command-line option");
        }
        const auto value = arguments[index + 1];
        if (option == "--config") config_path = std::string{value};
        else if (option == "--input") benchmark.input_path = std::string{value};
        else if (option == "--output") benchmark.output_path = std::string{value};
        else if (option == "--accounts") {
            if (!parse_unsigned(value, benchmark.account_count)
                || benchmark.account_count == 0) {
                return failure("benchmark accounts must be a positive integer");
            }
        } else if (option == "--rate") {
            if (!parse_unsigned(value, benchmark.rate_per_second)) {
                return failure("benchmark rate must be a non-negative integer");
            }
        } else if (option == "--warmup-seconds") {
            if (!parse_unsigned(value, benchmark.warmup_seconds)) {
                return failure("benchmark warmup seconds must be a non-negative integer");
            }
        } else if (option == "--duration-seconds") {
            if (!parse_unsigned(value, benchmark.duration_seconds)
                || benchmark.duration_seconds == 0) {
                return failure("benchmark duration seconds must be a positive integer");
            }
        } else if (option == "--burst-rate") {
            if (!parse_unsigned(value, benchmark.burst_rate_per_second)) {
                return failure("benchmark burst rate must be a non-negative integer");
            }
        } else if (option == "--burst-seconds") {
            if (!parse_unsigned(value, benchmark.burst_seconds)) {
                return failure("benchmark burst seconds must be a non-negative integer");
            }
        } else {
            return failure("unknown benchmark command-line option");
        }
    }
    if (benchmark.input_path.empty() || benchmark.output_path.empty()) {
        return failure("benchmark requires --input and --output");
    }
    if ((benchmark.burst_rate_per_second == 0) != (benchmark.burst_seconds == 0)) {
        return failure("benchmark burst rate and seconds must be specified together");
    }

    benchmark.config_path = config_path;
    const std::vector<std::string_view> engine_arguments{
        "engine", "--mode", "live", "--config", config_path};
    auto accounts_result = parse_engine_config(engine_arguments, read_environment);
    if (!accounts_result.config) return accounts_result;
    auto accounts = accounts_result.config->accounts();
    if (benchmark.account_count != 0 && benchmark.account_count > accounts.size()) {
        return failure("benchmark accounts exceeds enabled account count");
    }

    RuntimeConfig config{
        Mode::Benchmark, {}, {}, {}, {}, {}, {}, {}, {}, {}, 0,
        std::move(accounts), std::move(benchmark)};
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
    std::vector<AccountConfig> accounts,
    BenchmarkConfig benchmark,
    LiveConfig live)
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
      accounts_(std::move(accounts)),
      benchmark_(std::move(benchmark)),
      live_(std::move(live))
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
    if (arguments.front() == "benchmark") {
        return parse_benchmark_config(arguments, read_environment);
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
