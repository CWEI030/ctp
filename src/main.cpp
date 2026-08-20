#include "ThostFtdcTraderApi.h"
#include "ctp/config.hpp"
#include "ctp/market_client.hpp"

#include <iostream>
#include <string_view>
#include <vector>

namespace {

int run_account_lifecycle_probe()
{
    CThostFtdcTraderApi* trader_api =
        CThostFtdcTraderApi::CreateFtdcTraderApi();
    if (trader_api == nullptr) {
        std::cerr << "[error] failed to create trader API\n";
        return 3;
    }

    trader_api->Release();
    std::cout << "[ok] account configuration validated and API lifecycle completed\n";
    return 0;
}

}

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

    return run_account_lifecycle_probe();
}
