#include "ctp/config.hpp"
#include "ctp/interrupt.hpp"
#include "ctp/market_client.hpp"
#include "ctp/telemetry.hpp"
#include "ctp/trader_client.hpp"

#include <chrono>
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
    if (config.mode() == ctp::Mode::Benchmark) {
        return ctp::run_benchmark(config, std::cout, std::cerr);
    }
    if (config.mode() == ctp::Mode::Engine) {
        std::cerr
            << "[error] engine live runtime is not connected yet; "
               "offline market distribution is available\n";
        return 3;
    }

    ctp::SigintHandler sigint;
    if (!sigint.installed()) {
        std::cerr << "[error] failed to install SIGINT handler\n";
        return 3;
    }

    const ctp::StopRequested stop_requested = [&sigint] {
        return sigint.stop_requested();
    };
    if (config.mode() == ctp::Mode::Market) {
        return ctp::run_market(
            config, std::chrono::seconds{15}, stop_requested);
    }

    // Engine 模式已在上方返回；这里只有兼容保留的单账户查询模式。
    return ctp::run_account(
        config, std::chrono::seconds{15}, stop_requested);
}
