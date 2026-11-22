// CatOfficial：把 QQ 官方机器人中继成标准 OneBot V11。
// 用法：catofficial [配置文件路径]，默认读取程序所在目录的 config.json；不存在时生成并照常启动，
// 可以在管理面板里添加机器人与连接。

#include "config.h"
#include "hub.h"
#include "logbuf.h"
#include "paths.h"
#include "platform/tray_win.h"
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
void relaunch(char** argv, const std::filesystem::path& configPath) {
#ifdef _WIN32
    (void)argv;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // 当前目录已切到程序所在目录，配置文件改用绝对路径传下去。
    std::wstring command = L"\"" + std::wstring(exe) + L"\" \"" + configPath.wstring() + L"\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    SetEnvironmentVariableW(L"CATOFFICIAL_RESTARTED", L"1");
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
#else
    (void)configPath;
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
    // 命令行给的路径按启动时的当前目录解析；之后在 Windows 上切到程序所在目录，
    // 这样双击、开机自启动（当前目录是 System32）都读写程序旁边的 config.json 与 data。
    fs::path configPath = argc > 1 ? fs::absolute(fromUtf8(argv[1]), ec) : fs::path();
#ifdef _WIN32
    {
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH) fs::current_path(fs::path(std::wstring(exe, n)).parent_path(), ec);
    }
#endif
    if (configPath.empty()) configPath = fs::absolute("config.json", ec);
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
    bool passwordSaved = true;
    const bool newPassword = WebUi::ensurePassword(configPath, config, passwordSaved);
    // 数据目录固定为配置文件旁的 data。
    const fs::path dataDir = configPath.parent_path() / "data";
    fs::create_directories(dataDir, ec);

    std::shared_ptr<Store> store;
    try {
        store = std::make_shared<Store>(utf8(dataDir / "catofficial.db"));
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    ImageCache::get().setDays(config.imageCacheDays);
    ImageCache::get().start(dataDir / "cache" / "images", log);
    sysinfo::HistorySampler::instance().start(dataDir);

    auto hub = std::make_shared<Hub>(config, store, log);
    hub->setReplies(std::make_shared<ReplyService>(dataDir / "replies.json"));
    hub->init();
    hub->loadBindings(dataDir / "bindings.json");
    Server::setup(hub);
    WebUi::setup(configPath, dataDir, hub);

    log(catofficial::LogLevel::info, std::string("CatOfficial ") + kVersion + " 启动中");
    int officials = 0, legacies = 0;
    for (const auto& b : config.bots) if (b.enabled) (b.official() ? officials : legacies)++;
    log(catofficial::LogLevel::info, "官方机器人 " + std::to_string(officials) + " 个，OneBot 机器人 " + std::to_string(legacies) + " 个");
    if (!officials) log(catofficial::LogLevel::warn, "还没有启用的官方机器人，请在管理面板的「登录管理」里添加");
    if (config.webhookStandalone)
        log(catofficial::LogLevel::info, "官方 Webhook 入口：" + config.webhookHost + ":" + std::to_string(config.webhookPort) + config.webhookPath);
    else if (config.webuiEnabled)
        log(catofficial::LogLevel::info, "官方 Webhook 入口：面板端口 " + std::to_string(config.webuiPort) + config.webhookPath);
    if (config.webuiEnabled) {
        const auto host = config.webuiHost == "0.0.0.0" ? std::string("127.0.0.1") : config.webuiHost;
        log(catofficial::LogLevel::info, "管理面板：http://" + host + ":" + std::to_string(config.webuiPort) + "/");
        if (newPassword && passwordSaved)
            log(catofficial::LogLevel::info, "已生成管理面板密码：" + config.webuiPassword + "（已写入配置文件 webui.password，可在面板中修改）");
        else if (newPassword)
            log(catofficial::LogLevel::warn, "已生成管理面板密码：" + config.webuiPassword + "，但写入配置文件失败，下次启动会重新生成。请确认程序目录可写");
        if (config.webuiHost != "127.0.0.1" && config.webuiHost != "localhost" && config.webuiHost != "::1")
            log(catofficial::LogLevel::warn, "管理面板监听在 " + config.webuiHost + "，其他机器也能访问，请使用足够强的密码");
    }

    drogon::app().setLogLevel(trantor::Logger::kWarn).setThreadNum(2);
    drogon::app().getLoop()->queueInLoop([hub] {
        hub->start();
        Server::startClients();
    });
    // Windows 托盘：打开应用目录 / 显示·隐藏控制台 / 打开网页面板 / 开机自启动 / 退出。
    // 默认启动即最小化到托盘（隐藏控制台、弹气泡、禁用控制台的关闭按钮）；tray.start_hidden=false 时保留控制台。
    // 刚生成面板密码的那次启动保留控制台，让人看得到密码。
    startSystemTray(config.webuiPort, [] { drogon::app().quit(); }, config.trayStartHidden && !newPassword, config.trayText);

    drogon::app().run();
    stopSystemTray();
    hub->stop();
    ImageCache::get().stop();
    curl_global_cleanup();
    if (WebUi::restartRequested()) {
        log(catofficial::LogLevel::info, "正在重启");
        relaunch(argv, configPath);
    }
    return 0;
}
