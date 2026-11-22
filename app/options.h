#pragma once
// 启动参数。每一项都可以写成命令行参数，也可以用环境变量注入（容器、面板托管时更方便）：
// 开关类参数在命令行或环境变量任一处打开即生效；带值的参数以命令行为准。
//
//   命令行                         环境变量                          作用
//   [配置文件]                     CATOFFICIAL_CONFIG                配置文件路径，默认安装目录的 config.json
//   --files-full-access            CATOFFICIAL_FILES_FULL_ACCESS     文件管理开放整台机器（默认只开放安装目录）
//   --disable-files                CATOFFICIAL_DISABLE_FILES         关闭文件管理
//   --disable-terminal             CATOFFICIAL_DISABLE_TERMINAL      关闭网页终端
//   --disable-programs             CATOFFICIAL_DISABLE_PROGRAMS      关闭程序托管
//   --max-programs=N               CATOFFICIAL_MAX_PROGRAMS          最多托管多少个程序（默认 32，0 表示不能再添加）
//   --container                    CATOFFICIAL_CONTAINER             容器模式（与 Dice!Next 相同）：在线更新只提示新版本，不下载、不安装

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace catofficial::app {

struct Options {
    std::string config;              // 配置文件（原样，调用方按启动时的当前目录解析）
    bool filesFullAccess = false;
    bool disableFiles = false;
    bool disableTerminal = false;
    bool disablePrograms = false;
    int maxPrograms = 32;
    bool container = false;
    std::vector<std::string> flags;  // 命令行里出现的开关，重启时原样带上
    std::vector<std::string> errors; // 认不出的参数、格式不对的值
};

inline constexpr int kMaxProgramsLimit = 1000;

/// 环境变量的真假：1 / true / yes / on 为真（不分大小写）。
inline bool truthy(const std::string& value) {
    std::string v;
    for (char c : value) if (!std::isspace(static_cast<unsigned char>(c))) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

/// @p env 取环境变量（测试时可替换）；返回空串表示没设。
inline Options parseOptions(const std::vector<std::string>& args, const std::function<std::string(const char*)>& env) {
    Options o;
    auto readInt = [&](const std::string& text, const std::string& name, int& out) {
        try {
            std::size_t used = 0;
            const int value = std::stoi(text, &used);
            if (used != text.size() || value < 0 || value > kMaxProgramsLimit) throw std::out_of_range(name);
            out = value;
        } catch (...) {
            o.errors.push_back(name + " 的值必须是 0 到 " + std::to_string(kMaxProgramsLimit) + " 之间的整数：" + text);
        }
    };
    // 先读环境变量，再让命令行覆盖
    o.config = env("CATOFFICIAL_CONFIG");
    o.filesFullAccess = truthy(env("CATOFFICIAL_FILES_FULL_ACCESS"));
    o.disableFiles = truthy(env("CATOFFICIAL_DISABLE_FILES"));
    o.disableTerminal = truthy(env("CATOFFICIAL_DISABLE_TERMINAL"));
    o.disablePrograms = truthy(env("CATOFFICIAL_DISABLE_PROGRAMS"));
    o.container = truthy(env("CATOFFICIAL_CONTAINER"));
    if (const auto max = env("CATOFFICIAL_MAX_PROGRAMS"); !max.empty()) readInt(max, "CATOFFICIAL_MAX_PROGRAMS", o.maxPrograms);

    bool configFromArgs = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg.rfind("--", 0) != 0) {
            if (!configFromArgs) { o.config = arg; configFromArgs = true; }
            else o.errors.push_back("多余的参数：" + arg);
            continue;
        }
        const auto eq = arg.find('=');
        const auto name = arg.substr(0, eq);
        std::string value = eq == std::string::npos ? std::string() : arg.substr(eq + 1);
        if (name == "--files-full-access") o.filesFullAccess = true;
        else if (name == "--disable-files") o.disableFiles = true;
        else if (name == "--disable-terminal") o.disableTerminal = true;
        else if (name == "--disable-programs") o.disablePrograms = true;
        else if (name == "--container") o.container = true;
        else if (name == "--max-programs") {
            // 也接受「--max-programs 5」这种写法
            if (eq == std::string::npos && i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) value = args[++i];
            readInt(value, "--max-programs", o.maxPrograms);
            o.flags.push_back("--max-programs=" + value);
            continue;
        } else {
            o.errors.push_back("认不出的参数：" + arg);
            continue;
        }
        o.flags.push_back(arg);
    }
    return o;
}

inline Options parseOptions(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return parseOptions(args, [](const char* name) {
#ifdef _WIN32
        // 用宽字符读再转 UTF-8：配置文件路径里有中文时，按本地代码页读会坏掉
        const std::wstring wide(name, name + std::char_traits<char>::length(name));
        const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
        if (!size) return std::string();
        std::wstring value(size, L'\0');
        const DWORD length = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
        value.resize(length);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), bytes, nullptr, nullptr);
        return out;
#else
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
#endif
    });
}

}  // namespace catofficial::app
