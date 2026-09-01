#pragma once

#include <iostream>
#include <string_view>

namespace test_support {

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
