#include "ThostFtdcMdApi.h"
#include "ThostFtdcTraderApi.h"

#include <iostream>

int main()
{
    CThostFtdcMdApi* market_api =
        CThostFtdcMdApi::CreateFtdcMdApi();
    if (market_api == nullptr) {
        std::cerr << "[error] failed to create market API\n";
        return 1;
    }

    market_api->Release();
    market_api = nullptr;
    std::cout << "[ok] market API created and released\n";

    CThostFtdcTraderApi* trader_api =
        CThostFtdcTraderApi::CreateFtdcTraderApi();
    if (trader_api == nullptr) {
        std::cerr << "[error] failed to create trader API\n";
        return 2;
    }

    trader_api->Release();
    trader_api = nullptr;
    std::cout << "[ok] trader API created and released\n";

    return 0;
}
