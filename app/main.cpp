// CatOfficial：把 QQ 官方机器人中继成标准 OneBot V11。
// 用法：catofficial [配置文件路径]，默认读取当前目录的 config.json；不存在时生成并照常启动，
// 可以在管理面板里添加机器人与连接。

#include "config.h"
#include "hub.h"
#include "logbuf.h"
#include "paths.h"
#include "server.h"
#include "store.h"
#include "webui.h"

#include <curl/curl.h>
#include <drogon/drogon.h>
#include <ixwebsocket/IXNetSystem.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
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

catofficial::Logger makeLogger() {
    auto mu = std::make_shared<std::mutex>();
    return [mu](catofficial::LogLevel level, const std::string& message) {
        if (levelRank(level) < catofficial::app::LogBuffer::get().minimum()) return;
        catofficial::app::LogBuffer::get().push(level, message);
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

/// 面板请求重启：等端口释放后以相同参数重新启动自己。
void relaunch(char** argv) {
#ifdef _WIN32
    (void)argv;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = GetCommandLineW();
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    SetEnvironmentVariableW(L"CATOFFICIAL_RESTARTED", L"1");
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
#else
    setenv("CATOFFICIAL_RESTARTED", "1", 1);
    execv("/proc/self/exe", argv);
    execvp(argv[0], argv);
#endif
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    namespace fs = std::filesystem;
    using namespace catofficial::app;
    // 由面板重启时，等上一个进程释放端口。
    if (const char* restarted = std::getenv("CATOFFICIAL_RESTARTED"); restarted && std::string(restarted) == "1")
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    curl_global_init(CURL_GLOBAL_DEFAULT);
    ix::initNetSystem();
    std::error_code ec;
    const fs::path configPath = fs::absolute(fromUtf8(argc > 1 ? argv[1] : "config.json"), ec);
    if (!fs::exists(configPath)) {
        std::ofstream(configPath, std::ios::binary) << kConfigTemplate;
        std::cout << "已生成配置文件 " << utf8(configPath) << "。" << std::endl;
    }

    Config config;
    try {
        std::ifstream in(configPath, std::ios::binary);
        config = parseConfig(nlohmann::json::parse(in));
    } catch (const std::exception& e) {
        std::cerr << "读取配置失败：" << e.what() << std::endl;
        return 1;
    }

    LogBuffer::get().setMinimum(config.logLevel);
    const auto log = makeLogger();
    LogBuffer::get().setLogger(log);
    const bool newPassword = WebUi::ensurePassword(configPath, config);
    fs::path dataDir = fromUtf8(config.dataDir);
    if (dataDir.is_relative()) dataDir = configPath.parent_path() / dataDir;
    fs::create_directories(dataDir, ec);

    std::shared_ptr<Store> store;
    try {
        store = std::make_shared<Store>(utf8(dataDir / "catofficial.db"));
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    auto hub = std::make_shared<Hub>(config, store, log);
    hub->init();
    hub->loadBindings(dataDir / "bindings.json");
    Server::setup(hub);
    WebUi::setup(configPath, dataDir, hub);

    log(catofficial::LogLevel::info, std::string("CatOfficial ") + kVersion + " 启动中");
    int officials = 0, legacies = 0;
    for (const auto& b : config.bots) if (b.enabled) (b.official() ? officials : legacies)++;
    log(catofficial::LogLevel::info, "官方机器人 " + std::to_string(officials) + " 个，原 QQ 机器人 " + std::to_string(legacies) + " 个");
    if (!officials) log(catofficial::LogLevel::warn, "还没有启用的官方机器人，请在管理面板的「登录管理」里添加");
    if (config.webhookPort)
        log(catofficial::LogLevel::info, "官方 Webhook 入口：" + config.webhookHost + ":" + std::to_string(config.webhookPort) + config.webhookPath);
    if (config.webuiEnabled) {
        const auto host = config.webuiHost == "0.0.0.0" ? std::string("127.0.0.1") : config.webuiHost;
        log(catofficial::LogLevel::info, "管理面板：http://" + host + ":" + std::to_string(config.webuiPort) + "/");
        if (newPassword) log(catofficial::LogLevel::info, "已生成管理面板密码：" + config.webuiPassword + "（已写入配置文件 webui.password，可在面板中修改）");
        if (config.webuiHost != "127.0.0.1" && config.webuiHost != "localhost" && config.webuiHost != "::1")
            log(catofficial::LogLevel::warn, "管理面板监听在 " + config.webuiHost + "，其他机器也能访问，请使用足够强的密码");
    }

    drogon::app().setLogLevel(trantor::Logger::kWarn).setThreadNum(2);
    drogon::app().getLoop()->queueInLoop([hub] {
        hub->start();
        Server::startClients();
    });
    drogon::app().run();
    hub->stop();
    curl_global_cleanup();
    if (WebUi::restartRequested()) {
        log(catofficial::LogLevel::info, "正在重启");
        relaunch(argv);
    }
    return 0;
}
