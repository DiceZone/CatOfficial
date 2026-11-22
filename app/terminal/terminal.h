#pragma once
// 网页终端（移植自 BDC 公寓守护进程的 terminal）与 MCSM 式的程序托管。
//
// 终端跑在 CatOfficial 所在的机器上，网页只是个窗口——关掉页面不影响它继续跑，回来接着看。
// 输出走长轮询：请求挂着，有输出立刻回来，没有就等到超时再发一个（与 BDC 相同）。
// 托管的程序也跑在伪终端里，控制台看到的就是它真正的输出，可以直接输入指令。

#include "pty.h"

#include <catofficial/log.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
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

/// 托管的程序（配置部分，存在 data/programs.json）。
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

/// 启动命令的第一项是相对路径、而那个文件就在工作目录里时，换成它的绝对路径。
/// Windows 的 CreateProcessW 找程序时不看子进程的工作目录（只看 CatOfficial 自己的目录、当前目录、System32 与 PATH），
/// 不换的话「工作目录里的 xxx.exe」永远找不到；POSIX 的 sh 也只按 PATH 找不带斜杠的名字。
std::string resolveProgramCommand(const std::string& command, const std::filesystem::path& cwd);

class ProgramManager {
public:
    /// @p maxPrograms 最多托管多少个（启动参数 --max-programs，默认 32；已有的超出部分照常保留，只是不能再添加）。
    ProgramManager(std::filesystem::path configFile, std::filesystem::path installRoot, Logger log, int maxPrograms = 32);
    int maxPrograms() const { return maxPrograms_; }
    ~ProgramManager();
    json list();
    json save(const json& body, std::string& error);    // 新增或修改
    bool remove(const std::string& id, std::string& error);
    bool start(const std::string& id, std::string& error);
    bool stop(const std::string& id, bool force, std::string& error);
    bool restart(const std::string& id, std::string& error);
    bool input(const std::string& id, const std::string& data);
    void resize(const std::string& id, int cols, int rows);
    bool read(const std::string& id, std::int64_t cursor, double wait, OutputBuffer::Reply reply);
    void clearOutput(const std::string& id);
    /// CatOfficial 启动后拉起设置了「随 CatOfficial 启动」的程序。
    void autoStart();
    /// 退出前停止所有程序（先按设置正常停止，超时强制结束）。
    void stopAll();

private:
    struct Runtime;
    std::shared_ptr<Runtime> find(const std::string& id);
    bool launch(const std::shared_ptr<Runtime>& runtime, std::string& error);
    void onExit(const std::shared_ptr<Runtime>& runtime, int code);
    void persist();
    json describe(const std::shared_ptr<Runtime>& runtime);
    std::filesystem::path workingDirectory(const ProgramConfig& config) const;

    std::filesystem::path file_, root_;
    Logger log_;
    std::mutex mu_;
    std::vector<std::shared_ptr<Runtime>> programs_;
    int cols_ = 120, rows_ = 32;
    int maxPrograms_ = 32;
};

}  // namespace catofficial::app::terminal
