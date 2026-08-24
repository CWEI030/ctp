#include "ctp/interrupt.hpp"

#include <csignal>
#include <iostream>

namespace {

volatile std::sig_atomic_t previous_handler_called = 0;

void previous_handler(int) noexcept
{
    previous_handler_called = 1;
}

}

int main()
{
    const auto original_handler = std::signal(SIGINT, previous_handler);
    {
        ctp::SigintHandler handler;
        if (!handler.installed()) {
            std::cerr << "[fail] SIGINT handler must install\n";
            return 1;
        }
        if (handler.stop_requested()) {
            std::cerr << "[fail] a new handler must start without a stop request\n";
            return 1;
        }

        std::raise(SIGINT);
        if (!handler.stop_requested()) {
            std::cerr << "[fail] SIGINT must set the stop request\n";
            return 1;
        }
    }

    std::raise(SIGINT);
    std::signal(SIGINT, original_handler);
    if (!previous_handler_called) {
        std::cerr << "[fail] destruction must restore the previous handler\n";
        return 1;
    }

    std::cout << "[ok] all interrupt tests passed\n";
    return 0;
}
