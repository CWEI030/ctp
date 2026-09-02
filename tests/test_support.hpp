#pragma once

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string_view>

namespace test_support {

inline std::atomic<bool> count_allocations{false};
inline std::atomic<std::size_t> allocation_count{0};

inline void* allocate(std::size_t size)
{
    if (count_allocations.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}

class AllocationProbe {
public:
    AllocationProbe()
    {
        allocation_count.store(0, std::memory_order_relaxed);
        count_allocations.store(true, std::memory_order_relaxed);
    }

    ~AllocationProbe()
    {
        stop();
    }

    void stop() noexcept
    {
        count_allocations.store(false, std::memory_order_relaxed);
    }

    std::size_t count() const noexcept
    {
        return allocation_count.load(std::memory_order_relaxed);
    }
};

class TestRunner {
public:
    explicit TestRunner(std::string_view suite_name = "tests")
        : suite_name_(suite_name)
    {
    }

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
            std::cout << "[ok] all " << suite_name_ << " tests passed\n";
        }
        return failures_ == 0 ? 0 : 1;
    }

private:
    std::string_view suite_name_;
    int failures_{0};
};

}

#ifdef CTP_TEST_DEFINE_ALLOCATION_OPERATORS
void* operator new(std::size_t size)
{
    return test_support::allocate(size);
}

void* operator new[](std::size_t size)
{
    return test_support::allocate(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}
#endif
