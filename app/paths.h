#pragma once

// UTF-8 字符串与 std::filesystem::path 互转（C++20 的 u8string 是 char8_t）。

#include <filesystem>
#include <string>

namespace catofficial::app {

inline std::string utf8(const std::filesystem::path& path) {
    const auto s = path.u8string();
    return std::string(s.begin(), s.end());
}

inline std::filesystem::path fromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

}  // namespace catofficial::app
