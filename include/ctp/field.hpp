#pragma once

#include <algorithm>
#include <array>
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
bool copy_to_field(std::array<char, N>& destination, std::string_view source)
{
    if (source.size() >= N) {
        return false;
    }

    destination.fill('\0');
    std::copy(source.begin(), source.end(), destination.begin());
    return true;
}

template <std::size_t N>
std::string field_text(const char (&field)[N])
{
    const auto end = std::find(std::begin(field), std::end(field), '\0');
    return std::string{std::begin(field), end};
}

template <std::size_t N>
std::string field_text(const std::array<char, N>& field)
{
    const auto end = std::find(field.begin(), field.end(), '\0');
    return std::string{field.begin(), end};
}

}
