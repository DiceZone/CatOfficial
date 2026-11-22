// CatOfficial：把 QQ 官方机器人中继成标准 OneBot V11。
// 用法：catofficial [配置文件路径]，默认读取程序所在目录的 config.json；不存在时生成并照常启动，
// 可以在管理面板里添加机器人与连接。

#include "config.h"
#include "hub.h"
#include "logbuf.h"
#include "options.h"
#include "paths.h"
#include "platform/instance_guard.h"
#include "platform/startup_guard.h"
#include "platform/tray_win.h"
#include "server.h"
#include "store.h"
#include "webui.h"
#include "update/update_service.h"
#include "files/file_store.h"
#include "terminal/terminal.h"

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
#include "platform/posix_update.h"
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

/// 是否由启动器 / 管理程序拉起（发行包里正常双击 catofficial 就是这样）。
bool managedLaunch() {
#ifdef _WIN32
    wchar_t managed[8]{};
    const DWORD length = GetEnvironmentVariableW(L"CATOFFICIAL_MANAGED", managed, 8);
    std::error_code ec;
    return length > 0 && std::wstring(managed) == L"1" && std::filesystem::is_regular_file("catofficial.exe", ec);
#else
    return catofficial::app::update::posix::managedLaunch(std::filesystem::current_path());
#endif
}

/// 重启（面板「重启」或安装更新）。由启动器拉起时交给启动器：Windows 拉起
/// 「catofficial.exe --after <本进程 PID>」，它等本进程退出、安装待装的更新后再拉起新 core；
/// POSIX 以退出码 42 结束，由前台管理程序接手。开发时直接运行 core 则重新启动自己。
/// 返回 main 应使用的退出码。@p flags 是启动时带的开关（如 --files-full-access），重启后照样带上。
int relaunch(char** argv, const std::filesystem::path& configPath, bool configGiven, const std::vector<std::string>& flags) {
#ifdef _WIN32
    (void)argv;
    std::wstring command;
    std::wstring exe;
    std::wstring tail;
    for (const auto& flag : flags) tail += L" " + std::filesystem::path(catofficial::app::fromUtf8(flag)).wstring();
    if (managedLaunch()) {
        exe = std::filesystem::absolute("catofficial.exe").wstring();
        command = L"\"" + exe + L"\" --after " + std::to_wstring(GetCurrentProcessId()) + tail;
        if (configGiven) command += L" \"" + configPath.wstring() + L"\"";
    } else {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        exe = self;
        // 当前目录可能已切换，配置文件改用绝对路径传下去。
        command = L"\"" + exe + L"\"" + tail + L" \"" + configPath.wstring() + L"\"";
        SetEnvironmentVariableW(L"CATOFFICIAL_RESTARTED", L"1");
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    return 0;
#else
    (void)configPath; (void)configGiven; (void)flags;
    if (managedLaunch()) return catofficial::app::update::posix::kRestartExitCode;
    setenv("CATOFFICIAL_RESTARTED", "1", 1);
    execv("/proc/self/exe", argv);
    execvp(argv[0], argv);
    return 1;
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
    // 命令行给的路径按启动时的当前目录解析；之后在 Windows 上切到安装目录（与 Dice!Next 相同：
    // 发行包里是 app\ 的上一级），这样双击、开机自启动（当前目录是 System32）都读写安装目录里的
    // config.json 与 data。在临时目录（压缩包预览）里运行时拒绝启动。
    // 启动参数（每一项都可以用环境变量注入，见 options.h）。
    const Options options = parseOptions(argc, argv);
    if (!options.errors.empty()) {
        for (const auto& error : options.errors) std::cerr << "[CatOfficial] " << error << std::endl;
        std::cerr << "可用参数：[配置文件] --files-full-access --disable-files --disable-terminal --disable-programs --max-programs=N --container" << std::endl;
        return 1;
    }
    const auto& flags = options.flags;
    fs::path configPath = options.config.empty() ? fs::path() : fs::absolute(fromUtf8(options.config), ec);
    const bool configGiven = !configPath.empty();
    const bool filesFullAccess = options.filesFullAccess;
    // 容器模式：与 Dice!Next 的 DICENEXT_CONTAINER 相同，在线更新只提示新版本。写进环境变量，
    // 在线更新的容器检测、重启时拉起的启动器都认得出来。
    if (options.container) {
#ifdef _WIN32
        SetEnvironmentVariableW(L"CATOFFICIAL_CONTAINER", L"1");
#else
        setenv("CATOFFICIAL_CONTAINER", "1", 1);
#endif
    }
    if (!startup::prepareCoreLaunch()) return 2;
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

    // 单实例：同一套数据（同一 data 目录）只允许一个进程运行。
    if (!acquireInstanceLock(utf8(dataDir / ".instance.lock"))) {
        std::cerr << "\n[CatOfficial] 检测到同一套数据已有实例在运行（数据目录：" << utf8(dataDir)
                  << "）。\n  同一套数据不能同时运行多个进程。\n  若确认没有其他实例，请删除 "
                  << utf8(dataDir / ".instance.lock") << " 后重试。\n" << std::endl;
        return 1;
    }
    // 面板端口被占用时自动顺延到下一个空闲端口，并写回配置文件。
    if (config.webuiEnabled && portInUse(config.webuiPort, config.webuiHost)) {
        const uint16_t want = config.webuiPort;
        const uint16_t got = findAvailablePort(want);
        if (got != want && !portInUse(got, config.webuiHost)) {
            config.webuiPort = got;
            WebUi::saveWebuiPort(configPath, got);
            log(catofficial::LogLevel::warn, "面板端口 " + std::to_string(want) + " 已被占用，自动改用 " + std::to_string(got) + " 并写入配置文件");
        } else {
            log(catofficial::LogLevel::error, "面板端口 " + std::to_string(want) + " 已被占用，附近也没有空闲端口");
        }
    }

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

    // 文件管理（默认只开放安装目录，--files-full-access 开放整台机器）、网页终端与程序托管。
    const fs::path installRoot = configPath.parent_path();
    WebUi::Tools tools;
    // 用 --disable-files / --disable-terminal / --disable-programs 关掉的功能不创建：接口直接回「已关闭」，面板菜单里也不显示
    // 文件存储总是创建：程序托管删除实例目录、导入压缩包时要用它的回收站；--disable-files 只是不开放给面板
    const auto fileStore = std::make_shared<files::FileStore>(installRoot, filesFullAccess);
    if (!options.disableFiles) tools.files = fileStore;
    if (!options.disableTerminal) tools.sessions = std::make_shared<terminal::SessionManager>(installRoot, log);
    if (!options.disablePrograms)
        tools.programs = std::make_shared<terminal::ProgramManager>(dataDir / "programs.json", installRoot, log, options.maxPrograms, filesFullAccess, fileStore);
    WebUi::setTools(tools);
    // 上传最大 2 GB；超过 8 MB 的请求体由 drogon 落到临时文件，不占内存
    drogon::app().setClientMaxBodySize(files::kMaxStreamUploadBytes + (1 << 20)).setClientMaxMemoryBodySize(8 << 20);
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

    // 在线更新：检查、下载、校验在后台线程；安装交给启动器。
    auto updater = std::make_shared<update::UpdateService>(dataDir / "update.json", log,
        [] {
            WebUi::restartRequested() = true;
            drogon::app().getLoop()->queueInLoop([] { drogon::app().quit(); });
        },
        [weakHub = std::weak_ptr<Hub>(hub)](const std::string& event, const std::string& message) {
            if (auto h = weakHub.lock()) h->announce(event == "update_available" ? "CatOfficial 有新版本" : "CatOfficial 在线更新", message);
        });
    WebUi::setUpdater(updater);
    drogon::app().getLoop()->runAfter(20.0, [updater] { updater->tick(); });
    drogon::app().getLoop()->runEvery(60.0, [updater] { updater->tick(); });

    drogon::app().setLogLevel(trantor::Logger::kWarn).setThreadNum(2);
    drogon::app().getLoop()->queueInLoop([hub, tools] {
        hub->start();
        Server::startClients();
        if (tools.programs) tools.programs->autoStart();
    });
    if (options.disableFiles) log(catofficial::LogLevel::info, "文件管理已关闭（--disable-files）");
    else if (filesFullAccess) log(catofficial::LogLevel::warn, "文件管理已开放整台机器（--files-full-access）");
    if (options.disableTerminal) log(catofficial::LogLevel::info, "网页终端已关闭（--disable-terminal）");
    if (options.disablePrograms) log(catofficial::LogLevel::info, "程序托管已关闭（--disable-programs）");
    else if (options.maxPrograms != 32) log(catofficial::LogLevel::info, "程序托管最多 " + std::to_string(options.maxPrograms) + " 个（--max-programs）");
    if (options.container) log(catofficial::LogLevel::info, "容器模式（--container）：在线更新只提示新版本，不下载、不安装");
    // 回收站里过期的条目每小时清一次；闲置的终端会话每五分钟收一次
    // 回收站总要清（程序托管也往里放东西）；实例日志按设置每小时清一次
    drogon::app().getLoop()->runAfter(30.0, [fileStore] { fileStore->purgeExpiredTrash(); });
    drogon::app().getLoop()->runEvery(3600.0, [fileStore] { fileStore->purgeExpiredTrash(); });
    if (tools.programs) {
        drogon::app().getLoop()->runAfter(60.0, [programs = tools.programs] { programs->pruneLogs(); });
        drogon::app().getLoop()->runEvery(3600.0, [programs = tools.programs] { programs->pruneLogs(); });
    }
    if (tools.sessions) drogon::app().getLoop()->runEvery(300.0, [sessions = tools.sessions] { sessions->reap(); });
    // Windows 托盘：打开应用目录 / 显示·隐藏控制台 / 打开网页面板 / 开机自启动 / 退出。
    // 默认启动即最小化到托盘（隐藏控制台、弹气泡、禁用控制台的关闭按钮）；tray.start_hidden=false 时保留控制台。
    // 刚生成面板密码的那次启动保留控制台，让人看得到密码。
    startSystemTray(config.webuiPort, [] { drogon::app().quit(); }, config.trayStartHidden && !newPassword, config.trayText);

    drogon::app().run();
    stopSystemTray();
    WebUi::setUpdater(nullptr);
    updater.reset();
    // 先按各自的方式停掉托管的程序（给它们保存数据的时间），再关终端
    if (tools.programs) tools.programs->stopAll();
    if (tools.sessions) tools.sessions->closeAll();
    WebUi::setTools({});
    hub->stop();
    ImageCache::get().stop();
    curl_global_cleanup();
    if (WebUi::restartRequested()) {
        log(catofficial::LogLevel::info, "正在重启");
        return relaunch(argv, configPath, configGiven, flags);
    }
    return 0;
}
