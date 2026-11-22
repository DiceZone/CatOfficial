#pragma once
// 网页终端（移植自 BDC 公寓守护进程的 terminal）与 MCSM 式的程序托管。
//
// 终端跑在 CatOfficial 所在的机器上，网页只是个窗口——关掉页面不影响它继续跑，回来接着看。
// 输出走长轮询：请求挂着，有输出立刻回来，没有就等到超时再发一个（与 BDC 相同）。
// 托管的程序也跑在伪终端里，控制台看到的就是它真正的输出，可以直接输入指令。

#include "pty.h"
#include "../files/file_store.h"

#include <catofficial/log.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace catofficial::app::terminal {

using json = nlohmann::json;

/// 一段滚动输出（保留最近 256 KB）。读的人带着游标来：有新内容立刻返回，没有就挂着等。
class OutputBuffer : public std::enable_shared_from_this<OutputBuffer> {
public:
    using Reply = std::function<void(const std::string& data, std::int64_t cursor, bool alive)>;
    void append(const char* data, std::size_t size);
    void append(const std::string& text) { append(text.data(), text.size()); }
    void setAlive(bool alive);
    /// 每次追加的内容同时交给 @p sink（托管程序的日志落盘用）。
    void setSink(std::function<void(const char*, std::size_t)> sink) { std::lock_guard lock(mu_); sink_ = std::move(sink); }
    /// 有内容（或已结束）时立刻调用 @p reply；否则最多等 @p waitSeconds 秒。
    void read(std::int64_t cursor, double waitSeconds, Reply reply);
    std::int64_t total() const;

private:
    struct Waiter { std::uint64_t id; std::int64_t cursor; Reply reply; };
    void flushWaiters();
    mutable std::mutex mu_;
    std::string buf_;
    std::int64_t total_ = 0;
    bool alive_ = true;
    std::uint64_t nextWaiter_ = 1;
    std::vector<Waiter> waiters_;
    std::function<void(const char*, std::size_t)> sink_;
};

/// 一个终端会话。
struct Session {
    std::string id, label, shell, opened;
    std::unique_ptr<Pty> pty;
    std::shared_ptr<OutputBuffer> output = std::make_shared<OutputBuffer>();
    std::thread pump;
    std::chrono::steady_clock::time_point lastRead = std::chrono::steady_clock::now();
    std::atomic<bool> done{false};
    std::chrono::steady_clock::time_point doneAt{};
    ~Session();
};

class SessionManager {
public:
    explicit SessionManager(std::filesystem::path cwd, Logger log) : cwd_(std::move(cwd)), log_(std::move(log)) {}
    ~SessionManager();
    std::shared_ptr<Session> open(const std::string& label, int cols, int rows, std::string& error);
    std::shared_ptr<Session> get(const std::string& id);
    bool close(const std::string& id);
    json list();
    /// 收掉闲置太久的会话（两小时没人看，或已结束十分钟）。定时调用。
    void reap();
    void closeAll();

private:
    std::filesystem::path cwd_;
    Logger log_;
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
};

/// 托管程序（实例）的配置，存在 data/programs.json。
///
/// cwd 是实例目录：默认只能填 data/instances 下的文件夹名称（留空按名称自动起一个）；
/// 用 --files-full-access 启动时才允许填安装目录以外的绝对路径。
struct ProgramConfig {
    std::string id, name, command, cwd;
    std::vector<std::pair<std::string, std::string>> env;
    bool autoStart = false;
    bool autoRestart = true;
    int restartDelaySeconds = 5;
    std::string stopCommand;          // 正常停止时输入的指令，如 stop；空则发送 Ctrl+C
    int stopTimeoutSeconds = 15;      // 超时后强制结束
    json toJson() const;
    static ProgramConfig fromJson(const json& j);
};

/// 日志设置（所有实例共用）：保存多少天、每个实例最多占多少空间。超出的从最旧的删起。
struct ProgramLogSettings {
    int retentionDays = 30;           // 0 表示不按天数删
    int maxMegabytes = 100;           // 每个实例的日志上限
    json toJson() const { return json{{"log_retention_days", retentionDays}, {"log_max_mb", maxMegabytes}}; }
};

/// 一个实例的控制台日志：data/instance-logs/<实例 id>/<日期>.log，单个文件太大时切成 <日期>-2.log …
/// 存的是原始的终端输出（含颜色控制符），面板里能原样重放；崩溃、重启 CatOfficial 之后都还在。
class ProgramLog {
public:
    ProgramLog(std::filesystem::path dir, std::function<ProgramLogSettings()> settings)
        : dir_(std::move(dir)), settings_(std::move(settings)) {}
    void append(const char* data, std::size_t size);
    /// 按天数与大小上限删掉旧文件。
    void prune();
    json list() const;
    /// 日志文件的真实路径（名字不对、不存在返回空）。
    std::filesystem::path file(const std::string& name) const;
    /// 最近一份日志的结尾（启动时载回控制台）。
    std::string tail(std::size_t bytes) const;
    const std::filesystem::path& dir() const { return dir_; }

private:
    void openCurrent();
    std::filesystem::path dir_;
    std::function<ProgramLogSettings()> settings_;
    mutable std::mutex mu_;
    std::string currentDate_;
    std::filesystem::path current_;
    std::uint64_t currentBytes_ = 0;
    std::FILE* stream_ = nullptr;
public:
    ~ProgramLog();
};

/// 启动命令的第一项是相对路径、而那个文件就在工作目录里时，换成它的绝对路径。
/// Windows 的 CreateProcessW 找程序时不看子进程的工作目录（只看 CatOfficial 自己的目录、当前目录、System32 与 PATH），
/// 不换的话「工作目录里的 xxx.exe」永远找不到；POSIX 的 sh 也只按 PATH 找不带斜杠的名字。
std::string resolveProgramCommand(const std::string& command, const std::filesystem::path& cwd);

class ProgramManager {
public:
    /// @p maxPrograms 最多托管多少个（启动参数 --max-programs，默认 32；已有的超出部分照常保留，只是不能再添加）。
    /// @p fullAccess 是否用 --files-full-access 启动（才允许实例目录放在 data/instances 以外）。
    /// @p store 删除实例目录、导入压缩包时把旧的移进回收站。
    ProgramManager(std::filesystem::path configFile, std::filesystem::path installRoot, Logger log, int maxPrograms,
                   bool fullAccess, std::shared_ptr<files::FileStore> store);
    int maxPrograms() const { return maxPrograms_; }
    bool fullAccess() const { return fullAccess_; }
    ~ProgramManager();
    json list();
    json save(const json& body, std::string& error);    // 新增或修改
    /// 删除托管设置；@p removeFiles 时把实例目录与日志一起移进回收站。
    bool remove(const std::string& id, bool removeFiles, std::string& error);
    bool start(const std::string& id, std::string& error);
    bool stop(const std::string& id, bool force, std::string& error);
    bool restart(const std::string& id, std::string& error);
    bool input(const std::string& id, const std::string& data);
    void resize(const std::string& id, int cols, int rows);
    bool read(const std::string& id, std::int64_t cursor, double wait, OutputBuffer::Reply reply);
    void clearOutput(const std::string& id);
    /// 导入压缩包（.zip / .tar.gz）作为实例目录：旧目录整个移进回收站，压缩包外面只套了一层文件夹时自动去掉。
    bool importArchive(const std::string& id, const std::filesystem::path& archive, const std::string& fileName,
                       const std::string& encoding, std::string& error);
    json logList(const std::string& id);
    std::filesystem::path logFile(const std::string& id, const std::string& name);
    ProgramLogSettings logSettings() const;
    bool saveLogSettings(const json& body, std::string& error);
    /// 定时调用：按设置清理各实例的旧日志。
    void pruneLogs();
    /// CatOfficial 启动后拉起设置了「随 CatOfficial 启动」的程序。
    void autoStart();
    /// 退出前停止所有程序（先按设置正常停止，超时强制结束）。
    void stopAll();

private:
    struct Runtime;
    std::shared_ptr<Runtime> find(const std::string& id);
    std::shared_ptr<Runtime> makeRuntime(const ProgramConfig& config);
    bool launch(const std::shared_ptr<Runtime>& runtime, std::string& error);
    void onExit(const std::shared_ptr<Runtime>& runtime, int code);
    void persist();
    json describe(const std::shared_ptr<Runtime>& runtime);
    std::filesystem::path workingDirectory(const ProgramConfig& config) const;
    /// 检查并规范实例目录的写法；不合规时写 @p error 返回 false。
    bool normalizeFolder(ProgramConfig& config, std::string& error);
    void migrateLegacyFolders();

    std::filesystem::path file_, root_, instances_, logs_;
    Logger log_;
    int maxPrograms_ = 32;
    bool fullAccess_ = false;
    std::shared_ptr<files::FileStore> store_;
    mutable std::mutex mu_;
    std::vector<std::shared_ptr<Runtime>> programs_;
    ProgramLogSettings logSettings_;
    int cols_ = 120, rows_ = 32;
};

}  // namespace catofficial::app::terminal
