#include "ctp/interrupt.hpp"

#include <csignal>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_sigint(int) noexcept
{
    stop_requested = 1;
}

}

namespace ctp {

SigintHandler::SigintHandler() noexcept
{
    ::stop_requested = 0;
    previous_ = std::signal(SIGINT, handle_sigint);
    installed_ = previous_ != SIG_ERR;
}

SigintHandler::~SigintHandler()
{
    if (installed_) {
        std::signal(SIGINT, previous_);
    }
}

bool SigintHandler::installed() const noexcept
{
    return installed_;
}

bool SigintHandler::stop_requested() const noexcept
{
    return ::stop_requested != 0;
}

}
