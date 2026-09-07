#include "ctp/config.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class TestRunner {
public:
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
            std::cout << "[ok] all config tests passed\n";
        }
        return failures_ == 0 ? 0 : 1;
    }

private:
    int failures_{0};
};

using Environment = std::unordered_map<std::string, std::string>;

class TemporaryAccountFile {
public:
    explicit TemporaryAccountFile(std::string_view contents)
    {
        static std::size_t sequence = 0;
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("ctp-accounts-" + std::to_string(timestamp) + "-"
               + std::to_string(sequence++) + ".ini");

        std::ofstream output{path_};
        output << contents;
        output.close();
        std::filesystem::permissions(
            path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
    }

    ~TemporaryAccountFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryAccountFile(const TemporaryAccountFile&) = delete;
    TemporaryAccountFile& operator=(const TemporaryAccountFile&) = delete;

    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

class TemporaryDefaultConfigDirectory {
public:
    explicit TemporaryDefaultConfigDirectory(std::string_view contents)
        : original_path_(std::filesystem::current_path())
    {
        static std::size_t sequence = 0;
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path()
            / ("ctp-default-config-" + std::to_string(timestamp) + "-"
               + std::to_string(sequence++));
        const auto config_directory = root_ / "config";
        std::filesystem::create_directories(config_directory);

        const auto config_path = config_directory / "accounts.local.ini";
        std::ofstream output{config_path};
        output << contents;
        output.close();
        std::filesystem::permissions(
            config_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
        std::filesystem::current_path(root_);
    }

    ~TemporaryDefaultConfigDirectory()
    {
        std::error_code ignored;
        std::filesystem::current_path(original_path_, ignored);
        std::filesystem::remove_all(root_, ignored);
    }

    TemporaryDefaultConfigDirectory(const TemporaryDefaultConfigDirectory&) = delete;
    TemporaryDefaultConfigDirectory& operator=(
        const TemporaryDefaultConfigDirectory&) = delete;

private:
    std::filesystem::path original_path_;
    std::filesystem::path root_;
};

std::string make_accounts_ini(std::size_t enabled_accounts, bool add_disabled = false)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < enabled_accounts; ++index) {
        const auto number = index + 1;
        output << "[account.account" << number << "]\n"
               << "enabled=true\n"
               << "broker_id=9999\n"
               << "user_id=user" << number << "\n"
               << "password=test-password-" << number << "\n"
               << "app_id=test-app-" << number << "\n"
               << "auth_code=test-auth-" << number << "\n"
               << "trader_front=tcp://127.0.0.1:" << (41000 + number) << "\n\n";
    }
    if (add_disabled) {
        output << "[account.disabled]\n"
               << "enabled=false\n";
    }
    return output.str();
}

std::string make_live_ini(std::size_t enabled_accounts)
{
    return
        "[engine]\n"
        "profile=simnow-7x24\n"
        "market_front=tcp://127.0.0.1:42001\n"
        "exchange_id=CFFEX\n"
        "instrument=IF2609\n"
        "minimum_price_increment=0.2\n\n"
        "[strategy]\n"
        "enabled=true\n"
        "trigger_price_ticks=4000\n"
        "entry_protection_ticks=2\n"
        "cancel_after_market_ticks=10\n"
        "max_signals_per_run=1\n"
        "close_reprice_after_market_ticks=10\n"
        "max_close_reprices=3\n\n"
        "[risk]\n"
        "max_order_volume=1\n"
        "max_net_position=1\n"
        "max_active_open_orders=1\n"
        "max_orders_per_day=2\n"
        "max_cancels_per_day=4\n"
        "max_order_rate_per_second=1\n"
        "max_price_deviation_ticks=2\n"
        "min_available_funds=10000.50\n"
        "market_stale_after_ms=1000\n"
        "kill_switch=false\n\n"
        + make_accounts_ini(enabled_accounts);
}

std::string replace_account_field(
    std::string ini,
    std::string_view field,
    std::string_view replacement)
{
    const std::string prefix = std::string{field} + "=";
    const auto field_begin = ini.find(prefix);
    if (field_begin == std::string::npos) {
        return ini;
    }

    const auto value_begin = field_begin + prefix.size();
    const auto value_end = ini.find('\n', value_begin);
    ini.replace(value_begin, value_end - value_begin, replacement);
    return ini;
}

ctp::EnvironmentReader environment_reader(Environment values)
{
    return [values = std::move(values)](std::string_view name)
               -> std::optional<std::string> {
        const auto found = values.find(std::string{name});
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

ctp::ConfigResult parse(
    std::vector<std::string_view> arguments,
    Environment environment)
{
    return ctp::parse_config(arguments, environment_reader(std::move(environment)));
}

void expect_error_contains(
    TestRunner& runner,
    const ctp::ConfigResult& result,
    std::string_view expected)
{
    runner.expect(!result.config.has_value(), "invalid input must not create config");
    runner.expect(
        result.error.find(expected) != std::string::npos,
        "error must identify the invalid field");
}

void test_valid_market(TestRunner& runner)
{
    const auto result = parse(
        {"market", "--profile", "simnow-1", "--instrument", "IF2609", "--ticks", "5"},
        {{"CTP_USER_ID", "123456"}, {"CTP_PASSWORD", "example-password"}});

    runner.expect(result.config.has_value(), "valid market input must create config");
    if (!result.config) {
        return;
    }

    runner.expect(result.config->mode() == ctp::Mode::Market, "mode must be market");
    runner.expect(result.config->profile() == "simnow-1", "profile must be retained");
    runner.expect(result.config->instrument() == "IF2609", "instrument must be retained");
    runner.expect(result.config->ticks() == 5, "ticks must be parsed");
    runner.expect(result.config->broker_id() == "9999", "SimNow BrokerID must default to 9999");
    runner.expect(
        result.config->market_front() == "tcp://180.168.146.187:10211",
        "simnow-1 market front must match the profile");
}

void test_valid_account(TestRunner& runner)
{
    const auto result = parse(
        {"account", "--profile", "simnow-7x24"},
        {{"CTP_USER_ID", "123456"},
         {"CTP_PASSWORD", "example-password"},
         {"CTP_APP_ID", "example-app"},
         {"CTP_AUTH_CODE", "example-code"}});

    runner.expect(result.config.has_value(), "valid account input must create config");
    if (!result.config) {
        return;
    }

    runner.expect(result.config->mode() == ctp::Mode::Account, "mode must be account");
    runner.expect(result.config->instrument().empty(), "account mode must not have instrument");
    runner.expect(result.config->ticks() == 0, "account mode must not have ticks");
    runner.expect(
        result.config->market_front() == "tcp://182.254.243.31:40011",
        "7x24 market front must match the profile");
    runner.expect(
        result.config->trader_front() == "tcp://182.254.243.31:40001",
        "7x24 trader front must match the profile");
}

void test_engine_accepts_variable_account_count(TestRunner& runner)
{
    for (std::size_t count = 1; count <= 5; ++count) {
        TemporaryAccountFile file{make_accounts_ini(count, true)};
        const std::string path = file.path();
        const auto result = parse(
            {"engine", "--mode", "live", "--config", path},
            {});

        runner.expect(
            result.config.has_value(),
            "engine must accept every enabled account count from one through five");
        if (!result.config) {
            continue;
        }

        runner.expect(result.config->mode() == ctp::Mode::Engine, "mode must be engine");
        runner.expect(
            result.config->accounts().size() == count,
            "disabled sections must not count as enabled accounts");

        for (std::size_t index = 0; index < count; ++index) {
            const auto number = index + 1;
            const auto& account = result.config->accounts()[index];

            runner.expect(
                account.alias() == "account" + std::to_string(number),
                "each account alias must come from its INI section name");
            runner.expect(
                account.broker_id() == "9999",
                "each account broker id must be retained");
            runner.expect(
                account.user_id() == "user" + std::to_string(number),
                "each account user id must be retained");
            runner.expect(
                account.password() == "test-password-" + std::to_string(number),
                "each account password must be retained");
            runner.expect(
                account.app_id() == "test-app-" + std::to_string(number),
                "each account app id must be retained");
            runner.expect(
                account.auth_code() == "test-auth-" + std::to_string(number),
                "each account auth code must be retained");
            runner.expect(
                account.trader_front()
                    == "tcp://127.0.0.1:" + std::to_string(41000 + number),
                "each account trader front must be retained");
        }
    }
}

void test_engine_parses_live_runtime_and_explicit_order_gate(TestRunner& runner)
{
    TemporaryAccountFile file{make_live_ini(1)};
    const auto result = parse(
        {"engine", "--mode", "live", "--config", file.path(),
         "--allow-orders"},
        {});

    runner.expect(result.config.has_value(), "complete live runtime must parse");
    if (!result.config) return;
    const auto& live = result.config->live();
    runner.expect(
        live.allow_orders
            && live.market_front == "tcp://127.0.0.1:42001"
            && live.exchange_id == "CFFEX"
            && live.instrument == "IF2609"
            && live.minimum_price_increment == 0.2
            && live.strategy_enabled
            && live.trigger_price_ticks == 4000
            && live.minimum_available_funds == 1'000'050
            && !live.kill_switch,
        "live sections and the command-line order gate must be retained");

    const auto observed = parse(
        {"engine", "--mode", "live", "--config", file.path()}, {});
    runner.expect(
        observed.config.has_value() && !observed.config->live().allow_orders,
        "orders must remain disabled unless --allow-orders is explicit");
}

void test_benchmark_configuration(TestRunner& runner)
{
    TemporaryAccountFile file{make_accounts_ini(4)};
    const std::string path = file.path();
    const auto result = parse(
        {"benchmark", "--config", path, "--input", "market.csv",
         "--output", "runtime/performance/run-1", "--accounts", "2",
         "--rate", "1000", "--warmup-seconds", "1",
         "--duration-seconds", "3", "--burst-rate", "2000",
         "--burst-seconds", "1"},
        {});

    runner.expect(result.config.has_value(), "valid benchmark options must parse");
    if (!result.config) return;
    const auto& benchmark = result.config->benchmark();
    runner.expect(
        result.config->mode() == ctp::Mode::Benchmark
            && result.config->accounts().size() == 4
            && benchmark.config_path == path
            && benchmark.account_count == 2
            && benchmark.rate_per_second == 1000
            && benchmark.duration_seconds == 3
            && benchmark.burst_rate_per_second == 2000,
        "benchmark options and account configuration must share one immutable config");

    const auto missing = parse(
        {"benchmark", "--config", path, "--input", "market.csv"}, {});
    expect_error_contains(runner, missing, "--input and --output");
    const auto too_many = parse(
        {"benchmark", "--config", path, "--input", "market.csv",
         "--output", "result", "--accounts", "5"}, {});
    expect_error_contains(runner, too_many, "exceeds enabled account count");
}

void test_engine_uses_default_account_config_path(TestRunner& runner)
{
    TemporaryDefaultConfigDirectory directory{make_accounts_ini(2)};
    const auto result = parse({"engine", "--mode", "live"}, {});

    runner.expect(
        result.config.has_value(),
        "engine without --config must read config/accounts.local.ini");
    if (!result.config) {
        return;
    }
    runner.expect(
        result.config->accounts().size() == 2,
        "default account config must retain every enabled account");
}

void test_engine_rejects_zero_enabled_accounts(TestRunner& runner)
{
    TemporaryAccountFile file{make_accounts_ini(0, true)};
    const std::string path = file.path();
    const auto result = parse(
        {"engine", "--mode", "live", "--config", path},
        {});

    expect_error_contains(runner, result, "enabled account");
}

void test_engine_requires_private_regular_config_file(TestRunner& runner)
{
    TemporaryAccountFile file{make_accounts_ini(1)};
    const std::string path = file.path();
    constexpr std::filesystem::perms invalid_permissions[]{
        std::filesystem::perms::owner_read,
        std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write
            | std::filesystem::perms::group_read,
        std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write
            | std::filesystem::perms::others_read,
    };

    for (const auto permissions : invalid_permissions) {
        std::filesystem::permissions(
            path,
            permissions,
            std::filesystem::perm_options::replace);
        expect_error_contains(
            runner,
            parse({"engine", "--mode", "live", "--config", path}, {}),
            "0600");
    }

    const std::string directory = std::filesystem::temp_directory_path().string();
    expect_error_contains(
        runner,
        parse({"engine", "--mode", "live", "--config", directory}, {}),
        "regular file");
}

void test_engine_rejects_invalid_account_schema(TestRunner& runner)
{
    {
        std::string unsafe_alias = make_accounts_ini(1);
        unsafe_alias.replace(
            unsafe_alias.find("account.account1"),
            std::string{"account.account1"}.size(),
            "account../outside");
        TemporaryAccountFile file{unsafe_alias};
        expect_error_contains(
            runner,
            parse({"engine", "--mode", "live", "--config", file.path()}, {}),
            "alias");
    }

    {
        TemporaryAccountFile file{make_accounts_ini(1) + make_accounts_ini(1)};
        expect_error_contains(
            runner,
            parse({"engine", "--mode", "live", "--config", file.path()}, {}),
            "duplicate alias");
    }

    {
        const std::string without_password =
            replace_account_field(make_accounts_ini(1), "password", "");
        TemporaryAccountFile file{without_password};
        expect_error_contains(
            runner,
            parse({"engine", "--mode", "live", "--config", file.path()}, {}),
            "password");
    }

    {
        const std::string invalid_front = replace_account_field(
            make_accounts_ini(1),
            "trader_front",
            "http://127.0.0.1:41001");
        TemporaryAccountFile file{invalid_front};
        expect_error_contains(
            runner,
            parse({"engine", "--mode", "live", "--config", file.path()}, {}),
            "trader_front");
    }
}

void test_engine_rejects_oversized_fields_without_leaking_values(TestRunner& runner)
{
    struct FieldBoundary {
        std::string_view name;
        std::size_t capacity;
        char secret_character;
    };

    constexpr FieldBoundary boundaries[]{
        {"broker_id", ctp::kBrokerIdCapacity, 'b'},
        {"user_id", ctp::kUserIdCapacity, 'u'},
        {"password", ctp::kPasswordCapacity, 'p'},
        {"app_id", ctp::kAppIdCapacity, 'a'},
        {"auth_code", ctp::kAuthCodeCapacity, 'c'},
    };

    for (const auto& boundary : boundaries) {
        const std::string secret_value(
            boundary.capacity,
            boundary.secret_character);
        const std::string invalid_ini = replace_account_field(
            make_accounts_ini(1),
            boundary.name,
            secret_value);
        TemporaryAccountFile file{invalid_ini};
        const auto result = parse(
            {"engine", "--mode", "live", "--config", file.path()},
            {});

        expect_error_contains(runner, result, boundary.name);
        runner.expect(
            result.error.find(secret_value) == std::string::npos,
            "account config error must not contain the rejected field value");
    }
}

void test_engine_environment_overrides_only_the_selected_account(TestRunner& runner)
{
    TemporaryAccountFile file{make_accounts_ini(2)};
    const auto result = parse(
        {"engine", "--mode", "live", "--config", file.path()},
        {
            {"CTP_ACCOUNT_account1_BROKER_ID", "8888"},
            {"CTP_ACCOUNT_account1_USER_ID", "override-user-1"},
            {"CTP_ACCOUNT_account1_PASSWORD", "override-password-1"},
            {"CTP_ACCOUNT_account1_APP_ID", "override-app-1"},
            {"CTP_ACCOUNT_account1_AUTH_CODE", "override-auth-1"},
            {"CTP_ACCOUNT_account2_TRADER_FRONT", "tcp://127.0.0.1:42002"},
        });

    runner.expect(result.config.has_value(), "valid account overrides must be accepted");
    if (!result.config) {
        return;
    }

    const auto& accounts = result.config->accounts();
    runner.expect(accounts.size() == 2, "overrides must not change the account count");
    runner.expect(
        accounts[0].broker_id() == "8888",
        "account1 broker id override must be applied to account1");
    runner.expect(
        accounts[0].user_id() == "override-user-1",
        "account1 user id override must be applied to account1");
    runner.expect(
        accounts[0].password() == "override-password-1",
        "account1 password override must be applied to account1");
    runner.expect(
        accounts[0].app_id() == "override-app-1",
        "account1 app id override must be applied to account1");
    runner.expect(
        accounts[0].auth_code() == "override-auth-1",
        "account1 auth code override must be applied to account1");
    runner.expect(
        accounts[0].trader_front() == "tcp://127.0.0.1:41001",
        "account2 front override must not modify account1");
    runner.expect(
        accounts[1].password() == "test-password-2",
        "account1 password override must not modify account2");
    runner.expect(
        accounts[1].auth_code() == "test-auth-2",
        "account1 auth code override must not modify account2");
    runner.expect(
        accounts[1].trader_front() == "tcp://127.0.0.1:42002",
        "account2 front override must be applied to account2");
}

void test_engine_rejects_invalid_environment_override_without_leaking_it(
    TestRunner& runner)
{
    const std::string secret_value(ctp::kPasswordCapacity, 's');
    TemporaryAccountFile file{make_accounts_ini(1)};
    const auto result = parse(
        {"engine", "--mode", "live", "--config", file.path()},
        {{"CTP_ACCOUNT_account1_PASSWORD", secret_value}});

    expect_error_contains(runner, result, "CTP_ACCOUNT_account1_PASSWORD");
    runner.expect(
        result.error.find(secret_value) == std::string::npos,
        "environment override error must not contain the rejected value");
}

void test_invalid_inputs(TestRunner& runner)
{
    const Environment market_env{
        {"CTP_USER_ID", "123456"}, {"CTP_PASSWORD", "example-password"}};

    expect_error_contains(runner, parse({}, market_env), "mode");
    expect_error_contains(runner, parse({"unknown"}, market_env), "mode");
    expect_error_contains(
        runner,
        parse({"market", "--profile", "unknown", "--instrument", "IF2609", "--ticks", "5"}, market_env),
        "profile");
    expect_error_contains(
        runner,
        parse({"market", "--instrument", "IF2609", "--ticks", "0"}, market_env),
        "ticks");
    expect_error_contains(
        runner,
        parse({"market", "--instrument", "IF2609", "--ticks", "abc"}, market_env),
        "ticks");
    expect_error_contains(
        runner,
        parse({"market", "--ticks", "5"}, market_env),
        "instrument");
    expect_error_contains(
        runner,
        parse({"market", "--instrument", "IF2609", "--ticks", "5", "--password", "secret"}, market_env),
        "--password");
    expect_error_contains(
        runner,
        parse({"market", "--instrument", "IF2609", "--ticks", "5", "--ticks", "6"}, market_env),
        "duplicate");
    expect_error_contains(
        runner,
        parse({"account", "--instrument", "IF2609"}, market_env),
        "account");

    expect_error_contains(
        runner,
        parse(
            {"market", "--instrument", "IF2609", "--ticks", "5"},
            {{"CTP_PASSWORD", "example-password"}}),
        "CTP_USER_ID");
    expect_error_contains(
        runner,
        parse(
            {"market", "--instrument", "IF2609", "--ticks", "5"},
            {{"CTP_USER_ID", "123456"}}),
        "CTP_PASSWORD");
    expect_error_contains(
        runner,
        parse(
            {"account"},
            {{"CTP_USER_ID", "123456"}, {"CTP_PASSWORD", "example-password"}}),
        "CTP_APP_ID");
}

void test_field_boundaries(TestRunner& runner)
{
    const Environment base{
        {"CTP_USER_ID", "123456"}, {"CTP_PASSWORD", "example-password"}};
    const std::vector<std::string_view> args{
        "market", "--instrument", "IF2609", "--ticks", "5"};

    auto environment = base;
    environment["CTP_BROKER_ID"] = std::string(11, 'b');
    expect_error_contains(runner, parse(args, environment), "CTP_BROKER_ID");

    environment = base;
    environment["CTP_USER_ID"] = std::string(16, 'u');
    expect_error_contains(runner, parse(args, environment), "CTP_USER_ID");

    environment = base;
    environment["CTP_PASSWORD"] = std::string(41, 'p');
    expect_error_contains(runner, parse(args, environment), "CTP_PASSWORD");

    environment = base;
    environment["CTP_MD_FRONT"] = "http://example.invalid:1";
    expect_error_contains(runner, parse(args, environment), "CTP_MD_FRONT");

    char destination[4]{'x', 'x', 'x', '\0'};
    runner.expect(ctp::copy_to_field(destination, "abc"), "N-1 bytes must fit");
    runner.expect(std::strcmp(destination, "abc") == 0, "copy must add NUL terminator");

    char unchanged[4]{'x', 'x', 'x', '\0'};
    runner.expect(!ctp::copy_to_field(unchanged, "abcd"), "N bytes must be rejected");
    runner.expect(std::strcmp(unchanged, "xxx") == 0, "failed copy must not modify destination");
}

}

int main()
{
    TestRunner runner;
    test_valid_market(runner);
    test_valid_account(runner);
    test_engine_accepts_variable_account_count(runner);
    test_engine_parses_live_runtime_and_explicit_order_gate(runner);
    test_benchmark_configuration(runner);
    test_engine_uses_default_account_config_path(runner);
    test_engine_rejects_zero_enabled_accounts(runner);
    test_engine_requires_private_regular_config_file(runner);
    test_engine_rejects_invalid_account_schema(runner);
    test_engine_rejects_oversized_fields_without_leaking_values(runner);
    test_engine_environment_overrides_only_the_selected_account(runner);
    test_engine_rejects_invalid_environment_override_without_leaking_it(runner);
    test_invalid_inputs(runner);
    test_field_boundaries(runner);
    return runner.finish();
}
