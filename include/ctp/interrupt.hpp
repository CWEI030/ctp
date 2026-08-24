#pragma once

#include <functional>

namespace ctp {

using StopRequested = std::function<bool()>;

class SigintHandler final {
public:
    SigintHandler() noexcept;
    ~SigintHandler();

    SigintHandler(const SigintHandler&) = delete;
    SigintHandler& operator=(const SigintHandler&) = delete;

    bool installed() const noexcept;
    bool stop_requested() const noexcept;

private:
    using Handler = void (*)(int);

    Handler previous_{nullptr};
    bool installed_{false};
};

}
