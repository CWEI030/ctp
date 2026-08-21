#include "ctp/config.hpp"

#include <cstring>
#include <iostream>
#include <optional>
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
    test_invalid_inputs(runner);
    test_field_boundaries(runner);
    return runner.finish();
}
