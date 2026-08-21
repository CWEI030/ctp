#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace ctp {

template <std::size_t N>
bool copy_to_field(char (&destination)[N], std::string_view source)
{
    if (source.size() >= N) {
        return false;
    }

    std::fill(std::begin(destination), std::end(destination), '\0');
    std::copy(source.begin(), source.end(), destination);
    return true;
}

template <std::size_t N>
std::string field_text(const char (&field)[N])
{
    const auto end = std::find(std::begin(field), std::end(field), '\0');
    return std::string{std::begin(field), end};
}

}
