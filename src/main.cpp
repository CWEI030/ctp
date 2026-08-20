#include "ThostFtdcMdApi.h"
#include "ThostFtdcTraderApi.h"
#include "ctp/config.hpp"

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
        CThostFtdcMdApi* market_api = CThostFtdcMdApi::CreateFtdcMdApi();
        if (market_api == nullptr) {
            std::cerr << "[error] failed to create market API\n";
            return 3;
        }

        market_api->Release();
        std::cout << "[ok] market configuration validated and API lifecycle completed\n";
        return 0;
    }

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
