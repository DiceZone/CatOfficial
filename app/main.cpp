// CatOfficial：把 QQ 官方机器人中继成标准 OneBot V11。
// 用法：catofficial [配置文件路径]，默认读取当前目录的 config.json；不存在时生成模板。

#include "config.h"
#include "paths.h"
#include "relay.h"
#include "server.h"
#include "store.h"

#include <drogon/drogon.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

int levelRank(catofficial::LogLevel level) {
    switch (level) {
        case catofficial::LogLevel::debug: return 0;
        case catofficial::LogLevel::info: return 1;
        case catofficial::LogLevel::warn: return 2;
        default: return 3;
    }
}

int configRank(const std::string& name) {
    if (name == "debug") return 0;
    if (name == "warn") return 2;
    if (name == "error") return 3;
    return 1;
}

catofficial::Logger makeLogger(int minimum) {
    auto mu = std::make_shared<std::mutex>();
    return [mu, minimum](catofficial::LogLevel level, const std::string& message) {
        if (levelRank(level) < minimum) return;
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
        static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        std::lock_guard lock(*mu);
        std::cout << "[" << stamp << "] [" << names[levelRank(level)] << "] " << message << std::endl;
    };
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    namespace fs = std::filesystem;
    const fs::path configPath = catofficial::app::fromUtf8(argc > 1 ? argv[1] : "config.json");
    if (!fs::exists(configPath)) {
        std::ofstream(configPath, std::ios::binary) << catofficial::app::kConfigTemplate;
        std::cout << "已生成配置模板 " << catofficial::app::utf8(configPath) << "，填写 bot.app_id 与 bot.app_secret 后重新启动。" << std::endl;
        return 1;
    }

    catofficial::app::Config config;
    try {
        std::ifstream in(configPath, std::ios::binary);
        config = catofficial::app::parseConfig(nlohmann::json::parse(in));
    } catch (const std::exception& e) {
        std::cerr << "读取配置失败：" << e.what() << std::endl;
        return 1;
    }

    const auto log = makeLogger(configRank(config.logLevel));
    const fs::path dataDir = catofficial::app::fromUtf8(config.dataDir);
    std::error_code ec;
    fs::create_directories(dataDir, ec);

    std::shared_ptr<catofficial::app::Store> store;
    try {
        store = std::make_shared<catofficial::app::Store>(catofficial::app::utf8(dataDir / "catofficial.db"));
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    auto relay = std::make_shared<catofficial::app::Relay>(config, store, log);
    relay->init();
    relay->loadBindings(dataDir / "bindings.json");
    catofficial::app::Server::setup(relay);

    log(catofficial::LogLevel::info, std::string("CatOfficial ") + catofficial::app::kVersion + " 启动中");
    for (const auto& l : config.listeners)
        log(catofficial::LogLevel::info, "OneBot " + std::string(l.type == "http" ? "HTTP API" : "正向 WebSocket") + "：" + l.host + ":" + std::to_string(l.port));
    for (const auto& p : config.httpPost) log(catofficial::LogLevel::info, "OneBot HTTP POST 上报：" + p.url);
    for (const auto& w : config.wsReverse) log(catofficial::LogLevel::info, "OneBot 反向 WebSocket：" + w.url);
    if (config.webhook)
        log(catofficial::LogLevel::info, "官方事件经 Webhook 接收：" + config.webhookHost + ":" + std::to_string(config.webhookPort) + config.webhookPath);

    drogon::app().setLogLevel(trantor::Logger::kWarn).setThreadNum(2);
    drogon::app().getLoop()->queueInLoop([relay] {
        relay->start();
        catofficial::app::Server::startClients();
    });
    drogon::app().run();
    relay->stop();
    return 0;
}
