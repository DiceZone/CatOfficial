#pragma once

// 日志回调。CatOfficial 自身不写日志，只把要记的内容交给调用方。

#include <functional>
#include <string>

namespace catofficial {

enum class LogLevel { debug, info, warn, error };
using Logger = std::function<void(LogLevel, const std::string&)>;

}  // namespace catofficial
