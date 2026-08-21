#include "ctp/config.hpp"
#include "ctp/market_client.hpp"
#include "ctp/trader_client.hpp"

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char* argv[])
{
    std::vector<std::string_view> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const auto result = ctp::parse_config(arguments, ctp::system_environment());
    if (!result.config) {
        std::cerr << "[error] invalid configuration: " << result.error << '\n';
        return 2;
    }

    const auto& config = *result.config;
    if (config.mode() == ctp::Mode::Market) {
        return ctp::run_market(config);
    }

    return ctp::run_account(config);
}
