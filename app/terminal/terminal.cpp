#include "terminal.h"

#include "../paths.h"
#include "../update/update_archive.h"

#include <drogon/drogon.h>

#ifdef _WIN32
#include <share.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <ctime>
#include <fstream>
#include <random>
#include <regex>
#include <set>

namespace catofficial::app::terminal {

namespace fs = std::filesystem;

namespace {
constexpr std::size_t kScrollbackBytes = 256 * 1024;
constexpr std::size_t kMaxSessions = 16;

std::string randomId() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; ++i) out += digits[rng() & 15];
    return out;
}

std::string nowRfc3339() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

/// 截掉半个 UTF-8 字符：滚动缓冲从中间砍断时，开头不要留下乱码。
std::size_t utf8Boundary(const std::string& text, std::size_t from) {
    while (from < text.size() && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80) ++from;
    return from;
}

void schedule(double seconds, std::function<void()> fn) {
    drogon::app().getLoop()->runAfter(seconds, std::move(fn));
}

#ifdef _WIN32
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
#endif
}  // namespace

// ─── OutputBuffer ─────────────────────────────────────────────

void OutputBuffer::append(const char* data, std::size_t size) {
    if (!size) return;
    std::function<void(const char*, std::size_t)> sink;
    {
        std::lock_guard lock(mu_);
        buf_.append(data, size);
        total_ += static_cast<std::int64_t>(size);
        if (buf_.size() > kScrollbackBytes) buf_.erase(0, buf_.size() - kScrollbackBytes);
        sink = sink_;
    }
    if (sink) sink(data, size);
    flushWaiters();
}

void OutputBuffer::setAlive(bool alive) {
    {
        std::lock_guard lock(mu_);
        alive_ = alive;
    }
    flushWaiters();
}

std::int64_t OutputBuffer::total() const {
    std::lock_guard lock(mu_);
    return total_;
}

void OutputBuffer::read(std::int64_t cursor, double waitSeconds, Reply reply) {
    std::uint64_t id = 0;
    bool ready = false;
    std::string data;
    std::int64_t next = 0;
    bool alive = true;
    {
        std::lock_guard lock(mu_);
        const std::int64_t start = total_ - static_cast<std::int64_t>(buf_.size());
        if (cursor < start || cursor > total_) cursor = start;
        if (cursor < total_ || !alive_) {
            data = buf_.substr(utf8Boundary(buf_, static_cast<std::size_t>(cursor - start)));
            next = total_;
            alive = alive_;
            ready = true;
        } else {
            id = nextWaiter_++;
            waiters_.push_back(Waiter{id, cursor, std::move(reply)});
        }
    }
    if (ready) { reply(data, next, alive); return; }   // 回调在锁外执行
    std::weak_ptr<OutputBuffer> weak = shared_from_this();
    schedule(waitSeconds, [weak, id] {
        auto self = weak.lock();
        if (!self) return;
        Reply expired;
        std::int64_t total = 0;
        bool alive = true;
        {
            std::lock_guard lock(self->mu_);
            const auto it = std::find_if(self->waiters_.begin(), self->waiters_.end(), [id](const Waiter& w) { return w.id == id; });
            if (it == self->waiters_.end()) return;
            expired = std::move(it->reply);
            self->waiters_.erase(it);
            total = self->total_;
            alive = self->alive_;
        }
        expired(std::string(), total, alive);
    });
}

void OutputBuffer::flushWaiters() {
    std::vector<std::pair<Reply, std::string>> ready;
    std::int64_t total = 0;
    bool alive = true;
    {
        std::lock_guard lock(mu_);
        total = total_;
        alive = alive_;
        const std::int64_t start = total_ - static_cast<std::int64_t>(buf_.size());
        for (auto it = waiters_.begin(); it != waiters_.end();) {
            if (it->cursor < total_ || !alive_) {
                auto cursor = it->cursor < start ? start : it->cursor;
                const auto from = utf8Boundary(buf_, static_cast<std::size_t>(cursor - start));
                ready.emplace_back(std::move(it->reply), buf_.substr(from));
                it = waiters_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& [reply, data] : ready) reply(data, total, alive);
}

// ─── 终端会话 ─────────────────────────────────────────────────

Session::~Session() {
    if (pty) pty->kill();
    if (pump.joinable()) pump.join();
}

SessionManager::~SessionManager() { closeAll(); }

std::shared_ptr<Session> SessionManager::open(const std::string& label, int cols, int rows, std::string& error) {
    {
        std::lock_guard lock(mu_);
        if (sessions_.size() >= kMaxSessions) { error = "终端会话数已达上限（16 个），请先关闭不用的"; return nullptr; }
    }
    auto session = std::make_shared<Session>();
    session->id = randomId();
    session->label = label;
    session->shell = defaultShell();
    session->opened = nowRfc3339();
    PtyOptions options;
#ifdef _WIN32
    options.commandLine = L"\"" + widen(session->shell) + L"\"";
#else
    options.argv = {session->shell, "-i"};
#endif
    options.cwd = utf8(cwd_);
    options.cols = cols;
    options.rows = rows;
    session->pty = Pty::start(options, error);
    if (!session->pty) return nullptr;
    std::weak_ptr<Session> weak = session;
    Session* raw = session.get();
    session->pump = std::thread([raw] {
        std::vector<char> chunk(8192);
        while (true) {
            const auto n = raw->pty->read(chunk.data(), chunk.size());
            if (n == 0) break;
            raw->output->append(chunk.data(), n);
        }
        raw->pty->wait();
        raw->doneAt = std::chrono::steady_clock::now();
        raw->done = true;
        raw->output->setAlive(false);
    });
    {
        std::lock_guard lock(mu_);
        sessions_[session->id] = session;
    }
    if (log_) log_(LogLevel::info, "已打开网页终端 " + (label.empty() ? session->id.substr(0, 8) : label) + "（" + session->shell + "）");
    return session;
}

std::shared_ptr<Session> SessionManager::get(const std::string& id) {
    std::lock_guard lock(mu_);
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) return nullptr;
    it->second->lastRead = std::chrono::steady_clock::now();
    return it->second;
}

bool SessionManager::close(const std::string& id) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mu_);
        const auto it = sessions_.find(id);
        if (it == sessions_.end()) return false;
        session = it->second;
        sessions_.erase(it);
    }
    session->pty->kill();
    // 等读线程收尾放到后台，别卡住请求
    std::thread([session] {}).detach();
    if (log_) log_(LogLevel::info, "已关闭网页终端 " + (session->label.empty() ? id.substr(0, 8) : session->label));
    return true;
}

json SessionManager::list() {
    std::lock_guard lock(mu_);
    json out = json::array();
    for (const auto& [id, s] : sessions_)
        out.push_back(json{{"id", id}, {"label", s->label}, {"shell", s->shell}, {"opened", s->opened}, {"alive", !s->done.load()}, {"bytes", s->output->total()}});
    return out;
}

void SessionManager::reap() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> dead;
    {
        std::lock_guard lock(mu_);
        for (const auto& [id, s] : sessions_) {
            if (now - s->lastRead > std::chrono::hours(2) || (s->done && now - s->doneAt > std::chrono::minutes(10))) dead.push_back(id);
        }
    }
    for (const auto& id : dead) close(id);
}

void SessionManager::closeAll() {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(mu_);
        for (const auto& [id, s] : sessions_) ids.push_back(id);
    }
    for (const auto& id : ids) close(id);
}

// ─── 程序托管 ─────────────────────────────────────────────────

std::string resolveProgramCommand(const std::string& command, const fs::path& cwd) {
    const auto start = command.find_first_not_of(" \t");
    if (start == std::string::npos) return command;
    std::string first, rest;
    if (command[start] == '"') {
        const auto end = command.find('"', start + 1);
        if (end == std::string::npos) return command;
        first = command.substr(start + 1, end - start - 1);
        rest = command.substr(end + 1);
    } else {
        const auto end = command.find_first_of(" \t", start);
        first = command.substr(start, end == std::string::npos ? std::string::npos : end - start);
        rest = end == std::string::npos ? std::string() : command.substr(end);
    }
    if (first.empty()) return command;
    const auto path = fromUtf8(first);
    if (path.is_absolute()) return command;
    std::vector<fs::path> candidates{cwd / path};
#ifdef _WIN32
    // 只看是不是以 .exe 结尾：DiceDriver.OneBot_1.1.1(16) 这种名字里的「.1(16)」也会被当成扩展名
    std::string lowered = first;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered.size() < 4 || lowered.compare(lowered.size() - 4, 4, ".exe") != 0) candidates.push_back(cwd / fromUtf8(first + ".exe"));
#endif
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec)) continue;
        const auto absolute = utf8(fs::absolute(candidate, ec).lexically_normal());
#ifdef _WIN32
        return "\"" + absolute + "\"" + rest;
#else
        // 交给 sh 执行：单引号括起来，路径里有空格、括号也不会被拆开
        std::string quoted = "'";
        for (char c : absolute) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return quoted + "'" + rest;
#endif
    }
    return command;   // 不在工作目录里：按原样交给系统去 PATH 里找（java、python 之类）
}

#ifndef _WIN32
void chmodExecutable(const fs::path& path) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) ::chmod(utf8(path).c_str(), 0755);
}

/// 导入压缩包后给 ELF 与脚本加上执行权限（zip 常常丢掉 +x；Android 上没有它就启动不了）。
void markExecutables(const fs::path& dir) {
    std::error_code ec;
    for (const auto& item : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!item.is_regular_file(ec)) continue;
        std::ifstream in(item.path(), std::ios::binary);
        char mag[4]{};
        in.read(mag, 4);
        const auto n = in.gcount();
        const bool elf = n >= 4 && mag[0] == 0x7f && mag[1] == 'E' && mag[2] == 'L' && mag[3] == 'F';
        const bool script = n >= 2 && mag[0] == '#' && mag[1] == '!';
        if (elf || script) chmodExecutable(item.path());
    }
}

void ensureCommandExecutable(const std::string& command, const fs::path& cwd) {
    const auto start = command.find_first_not_of(" \t");
    if (start == std::string::npos) return;
    std::string first;
    if (command[start] == '\'' || command[start] == '"') {
        const auto end = command.find(command[start], start + 1);
        if (end == std::string::npos) return;
        first = command.substr(start + 1, end - start - 1);
    } else {
        const auto end = command.find_first_of(" \t", start);
        first = command.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    if (first.empty() || (first.find('/') == std::string::npos && first[0] != '.')) return;
    auto file = fromUtf8(first);
    if (!file.is_absolute()) file = cwd / file;
    chmodExecutable(file);
}
#endif

json ProgramConfig::toJson() const {
    json envJson = json::object();
    for (const auto& [k, v] : env) envJson[k] = v;
    return json{{"id", id}, {"name", name}, {"command", command}, {"cwd", cwd}, {"env", envJson}, {"auto_start", autoStart},
                {"auto_restart", autoRestart}, {"restart_delay_seconds", restartDelaySeconds}, {"stop_command", stopCommand},
                {"stop_timeout_seconds", stopTimeoutSeconds}};
}

ProgramConfig ProgramConfig::fromJson(const json& j) {
    ProgramConfig c;
    c.id = j.value("id", std::string());
    c.name = j.value("name", std::string());
    c.command = j.value("command", std::string());
    c.cwd = j.value("cwd", std::string());
    if (j.contains("env") && j["env"].is_object())
        for (auto& [k, v] : j["env"].items()) if (v.is_string()) c.env.emplace_back(k, v.get<std::string>());
    c.autoStart = j.value("auto_start", false);
    c.autoRestart = j.value("auto_restart", true);
    c.restartDelaySeconds = std::clamp(j.value("restart_delay_seconds", 5), 1, 3600);
    c.stopCommand = j.value("stop_command", std::string());
    c.stopTimeoutSeconds = std::clamp(j.value("stop_timeout_seconds", 15), 1, 600);
    return c;
}

struct ProgramManager::Runtime {
    ProgramConfig config;
    std::mutex mu;
    std::string state = "stopped";   // stopped / running / stopping / restarting / crashed
    std::unique_ptr<Pty> pty;
    std::thread pump;
    std::shared_ptr<OutputBuffer> output = std::make_shared<OutputBuffer>();
    std::shared_ptr<ProgramLog> log;
    std::int64_t pid = 0;
    std::string startedAt;
    int exitCode = 0;
    bool hasExit = false;
    bool stopRequested = false;
    std::uint64_t generation = 0;     // 每次启动加一：过期的定时器（强制结束、自动重启）认得出不是自己那一次
    std::atomic<bool> exited{true};   // 读线程看到进程退出后置位
    std::deque<std::chrono::steady_clock::time_point> recentStarts;
    std::string error;
    ~Runtime() {
        if (pty) pty->kill();
        if (pump.joinable()) pump.join();
    }
};

// ─── 实例日志 ─────────────────────────────────────────────────

namespace {
const std::regex& logNamePattern() {
    static const std::regex pattern("^[0-9]{4}-[0-9]{2}-[0-9]{2}(-[0-9]+)?\\.log$");
    return pattern;
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

struct LogFileInfo { std::string name; std::uint64_t size; fs::file_time_type modified; };

std::vector<LogFileInfo> logFiles(const fs::path& dir) {
    std::vector<LogFileInfo> files;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(dir, ec)) {
        const auto name = utf8(item.path().filename());
        if (!item.is_regular_file(ec) || !std::regex_match(name, logNamePattern())) continue;
        files.push_back({name, static_cast<std::uint64_t>(item.file_size(ec)), item.last_write_time(ec)});
    }
    // 旧的在前：按修改时间，再按名字
    std::sort(files.begin(), files.end(), [](const LogFileInfo& a, const LogFileInfo& b) {
        return a.modified != b.modified ? a.modified < b.modified : a.name < b.name;
    });
    return files;
}
}  // namespace

ProgramLog::~ProgramLog() {
    std::lock_guard lock(mu_);
    if (stream_) std::fclose(stream_);
}

void ProgramLog::openCurrent() {
    const auto settings = settings_ ? settings_() : ProgramLogSettings{};
    // 单个文件最多占上限的四分之一，满了切下一份，清理时才能按份删掉旧的
    const std::uint64_t perFile = std::max<std::uint64_t>(1ULL << 20, static_cast<std::uint64_t>(settings.maxMegabytes) * (1ULL << 20) / 4);
    const auto date = today();
    if (stream_ && date == currentDate_ && currentBytes_ < perFile) return;
    if (stream_) { std::fclose(stream_); stream_ = nullptr; }
    std::error_code ec;
    fs::create_directories(dir_, ec);
    for (int part = 1; part < 10000; ++part) {
        const auto name = part == 1 ? date + ".log" : date + "-" + std::to_string(part) + ".log";
        const auto path = dir_ / fromUtf8(name);
        const auto size = fs::exists(path, ec) ? static_cast<std::uint64_t>(fs::file_size(path, ec)) : 0ULL;
        if (size >= perFile) continue;
#ifdef _WIN32
        // 允许别人同时读（面板查看、载回控制台）；_wfopen_s 打开的文件不让别的句柄读
        stream_ = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
#else
        stream_ = std::fopen(path.c_str(), "ab");
#endif
        currentDate_ = date;
        current_ = path;
        currentBytes_ = size;
        return;
    }
}

void ProgramLog::append(const char* data, std::size_t size) {
    std::lock_guard lock(mu_);
    openCurrent();
    if (!stream_) return;
    std::fwrite(data, 1, size, stream_);
    std::fflush(stream_);   // 崩溃时也要留下来
    currentBytes_ += size;
}

void ProgramLog::prune() {
    const auto settings = settings_ ? settings_() : ProgramLogSettings{};
    std::lock_guard lock(mu_);
    auto files = logFiles(dir_);
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    std::uint64_t total = 0;
    for (const auto& f : files) total += f.size;
    const std::uint64_t limit = static_cast<std::uint64_t>(settings.maxMegabytes) * (1ULL << 20);
    for (const auto& f : files) {
        const auto path = dir_ / fromUtf8(f.name);
        if (path == current_) continue;   // 正在写的那份不删
        const bool expired = settings.retentionDays > 0 && now - f.modified > std::chrono::hours(24 * settings.retentionDays);
        if (!expired && total <= limit) continue;
        fs::remove(path, ec);
        if (!ec) total -= f.size;
    }
}

json ProgramLog::list() const {
    std::lock_guard lock(mu_);
    json out = json::array();
    auto files = logFiles(dir_);
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        // 正在写的那份：目录里记的大小会滞后，用实际写入的字节数
        if (!current_.empty() && fromUtf8(it->name) == current_.filename()) it->size = currentBytes_;
        const auto system = std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(it->modified - fs::file_time_type::clock::now());
        out.push_back(json{{"name", it->name}, {"size", it->size}, {"modified", std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count()}});
    }
    return out;
}

fs::path ProgramLog::file(const std::string& name) const {
    if (!std::regex_match(name, logNamePattern())) return {};
    const auto path = dir_ / fromUtf8(name);
    std::error_code ec;
    return fs::is_regular_file(path, ec) ? path : fs::path();
}

std::string ProgramLog::tail(std::size_t bytes) const {
    std::lock_guard lock(mu_);
    const auto files = logFiles(dir_);
    if (files.empty()) return {};
    std::ifstream in(dir_ / fromUtf8(files.back().name), std::ios::binary);
    // 打开后再取大小：目录里记的大小对正在写的文件会滞后
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(std::max<std::streamoff>(0, in.tellg()));
    in.seekg(static_cast<std::streamoff>(size > bytes ? size - bytes : 0));
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // 从中间截断时跳过半个 UTF-8 字符
    return data.substr(utf8Boundary(data, 0));
}

// ─── 程序托管 ─────────────────────────────────────────────────

namespace {
/// 实例目录名里不能用的字符换成 _（Windows 的限制最严，统一按它来）。
std::string safeFolderName(const std::string& name) {
    std::string out;
    for (unsigned char c : name) out += (c < 0x20 || std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos) ? '_' : static_cast<char>(c);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(0, 1);
    return out;
}

bool isPlainFolderName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos && safeFolderName(name) == name;
}

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
}  // namespace

ProgramManager::ProgramManager(fs::path configFile, fs::path installRoot, Logger log, int maxPrograms,
                               bool fullAccess, std::shared_ptr<files::FileStore> store)
    : file_(std::move(configFile)), root_(std::move(installRoot)), log_(std::move(log)), maxPrograms_(maxPrograms),
      fullAccess_(fullAccess), store_(std::move(store)) {
    instances_ = root_ / "data" / "instances";
    logs_ = root_ / "data" / "instance-logs";
    std::error_code ec;
    fs::create_directories(instances_, ec);
    std::ifstream in(file_, std::ios::binary);
    const auto j = json::parse(in, nullptr, false);
    // 早期版本存的是数组；现在是 {programs, settings}
    const json items = j.is_array() ? j : j.is_object() ? j.value("programs", json::array()) : json::array();
    if (j.is_object() && j.contains("settings") && j["settings"].is_object()) {
        logSettings_.retentionDays = std::clamp(j["settings"].value("log_retention_days", 30), 0, 3650);
        logSettings_.maxMegabytes = std::clamp(j["settings"].value("log_max_mb", 100), 1, 102400);
    }
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        auto config = ProgramConfig::fromJson(item);
        if (config.id.empty()) config.id = randomId();
        programs_.push_back(makeRuntime(config));
    }
    migrateLegacyFolders();
}

std::shared_ptr<ProgramManager::Runtime> ProgramManager::makeRuntime(const ProgramConfig& config) {
    auto runtime = std::make_shared<Runtime>();
    runtime->config = config;
    runtime->log = std::make_shared<ProgramLog>(logs_ / fromUtf8(config.id), [this] { return logSettings(); });
    // 上次的输出（崩溃前、重启 CatOfficial 前）载回控制台，再接上落盘
    const auto previous = runtime->log->tail(64 * 1024);
    if (!previous.empty()) {
        runtime->output->append(previous);
        runtime->output->append("\r\n\x1b[90m[CatOfficial] —— 以上是上次运行的日志 ——\x1b[0m\r\n");
    }
    std::weak_ptr<ProgramLog> weakLog = runtime->log;
    runtime->output->setSink([weakLog](const char* data, std::size_t size) {
        if (auto log = weakLog.lock()) log->append(data, size);
    });
    return runtime;
}

/// 早期版本的实例目录相对于安装目录（如 dice-suhui）。现在统一放到 data/instances 下：
/// 目标还不存在、旧目录就在安装目录里时，整个挪过去（同一块盘上的改名，瞬间完成）。
void ProgramManager::migrateLegacyFolders() {
    static const std::set<std::string> reserved{"data", "app", "web", "updates", "uploads", ".catofficial"};
    bool changed = false;
    std::vector<std::shared_ptr<Runtime>> copy;
    {
        std::lock_guard lock(mu_);
        copy = programs_;
    }
    for (const auto& r : copy) {
        auto& cwd = r->config.cwd;
        if (cwd.empty()) {
            cwd = safeFolderName(r->config.name).empty() ? r->config.id : safeFolderName(r->config.name);
            changed = true;
            continue;
        }
        if (!isPlainFolderName(cwd) || reserved.count(lowerAscii(cwd))) continue;
        const auto target = instances_ / fromUtf8(cwd);
        const auto legacy = root_ / fromUtf8(cwd);
        std::error_code ec;
        if (fs::exists(target, ec) || !fs::is_directory(legacy, ec)) continue;
        fs::rename(legacy, target, ec);
        if (log_) {
            if (ec) log_(LogLevel::warn, "无法把托管程序「" + r->config.name + "」的目录 " + cwd + " 移到 data/instances 下，请手动移动");
            else log_(LogLevel::info, "已把托管程序「" + r->config.name + "」的目录 " + cwd + " 移到 data/instances/" + cwd);
        }
    }
    if (changed) persist();
}

ProgramManager::~ProgramManager() { stopAll(); }

std::shared_ptr<ProgramManager::Runtime> ProgramManager::find(const std::string& id) {
    std::lock_guard lock(mu_);
    for (const auto& p : programs_) if (p->config.id == id) return p;
    return nullptr;
}

void ProgramManager::persist() {
    json programs = json::array();
    json settings;
    {
        std::lock_guard lock(mu_);
        for (const auto& p : programs_) programs.push_back(p->config.toJson());
        settings = logSettings_.toJson();
    }
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    const auto temporary = fs::path(file_).concat(".tmp");
    {
        std::ofstream o(temporary, std::ios::binary | std::ios::trunc);
        o << json{{"programs", programs}, {"settings", settings}}.dump(2) << '\n';
    }
    fs::rename(temporary, file_, ec);
}

fs::path ProgramManager::workingDirectory(const ProgramConfig& config) const {
    const auto path = fromUtf8(config.cwd);
    if (path.is_absolute()) return path;
    return instances_ / (config.cwd.empty() ? fromUtf8(config.id) : path);
}

bool ProgramManager::normalizeFolder(ProgramConfig& config, std::string& error) {
    auto& cwd = config.cwd;
    const auto first = cwd.find_first_not_of(" \t");
    cwd = first == std::string::npos ? std::string() : cwd.substr(first, cwd.find_last_not_of(" \t") - first + 1);
    if (cwd.empty()) {
        // 没填：按名称起一个，和别的实例撞了就加数字
        std::string base = safeFolderName(config.name);
        if (base.empty()) base = config.id;
        cwd = base;
        for (int n = 2;; ++n) {
            bool taken = false;
            {
                std::lock_guard lock(mu_);
                for (const auto& p : programs_)
                    if (p->config.id != config.id && lowerAscii(p->config.cwd) == lowerAscii(cwd)) taken = true;
            }
            if (!taken) break;
            cwd = base + "-" + std::to_string(n);
        }
        return true;
    }
    if (fromUtf8(cwd).is_absolute()) {
        if (!fullAccess_) { error = "实例目录只能填 data/instances 下的文件夹名称；要放到其他位置，请用 --files-full-access 启动"; return false; }
        return true;
    }
    if (!isPlainFolderName(cwd)) {
        error = fullAccess_ ? "实例目录请填 data/instances 下的文件夹名称，或者完整的绝对路径"
                            : "实例目录只能填 data/instances 下的文件夹名称（不能包含 / \\ : 等字符）";
        return false;
    }
    std::lock_guard lock(mu_);
    for (const auto& p : programs_)
        if (p->config.id != config.id && lowerAscii(p->config.cwd) == lowerAscii(cwd)) { error = "实例目录已被「" + p->config.name + "」使用"; return false; }
    return true;
}

json ProgramManager::describe(const std::shared_ptr<Runtime>& r) {
    std::lock_guard lock(r->mu);
    json j = r->config.toJson();
    j["state"] = r->state;
    j["pid"] = r->pid;
    j["started_at"] = r->startedAt;
    if (r->hasExit) j["exit_code"] = r->exitCode;
    j["error"] = r->error;
    j["output_bytes"] = r->output->total();
    const auto dir = workingDirectory(r->config);
    j["resolved_cwd"] = utf8(dir);
    // 文件管理里打开它用的路径：只开放安装目录时相对安装目录；开放整台机器时是完整路径（Windows 写成 /C:/…）
    std::error_code ec;
    std::string filesPath;
    if (fullAccess_) {
        const auto generic = dir.generic_u8string();
        filesPath = std::string(generic.begin(), generic.end());
        if (filesPath.empty() || filesPath[0] != '/') filesPath = "/" + filesPath;
    } else {
        const auto relative = dir.lexically_relative(root_).generic_u8string();
        const std::string text(relative.begin(), relative.end());
        if (!text.empty() && text.rfind("..", 0) != 0) filesPath = "/" + text;
    }
    j["files_path"] = filesPath;
    j["folder_exists"] = fs::is_directory(dir, ec);
    return j;
}

json ProgramManager::list() {
    std::vector<std::shared_ptr<Runtime>> copy;
    {
        std::lock_guard lock(mu_);
        copy = programs_;
    }
    json out = json::array();
    for (const auto& r : copy) out.push_back(describe(r));
    return out;
}

json ProgramManager::save(const json& body, std::string& error) {
    auto config = ProgramConfig::fromJson(body);
    auto trim = [](std::string s) {
        const auto a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    };
    config.name = trim(config.name);
    config.command = trim(config.command);
    if (config.name.empty()) { error = "请填写实例名称"; return nullptr; }
    if (config.command.empty()) { error = "请填写启动命令"; return nullptr; }
    std::shared_ptr<Runtime> runtime = config.id.empty() ? nullptr : find(config.id);
    if (!config.id.empty() && !runtime) { error = "没有找到这个实例"; return nullptr; }
    if (!runtime) {
        std::lock_guard lock(mu_);
        if (programs_.size() >= static_cast<std::size_t>(maxPrograms_))
            { error = maxPrograms_ == 0 ? "不能添加托管程序（启动参数 --max-programs=0）" : "最多托管 " + std::to_string(maxPrograms_) + " 个程序（启动参数 --max-programs）"; return nullptr; }
    }
    if (!runtime) config.id = randomId();
    if (!normalizeFolder(config, error)) return nullptr;
    std::error_code ec;
    fs::create_directories(workingDirectory(config), ec);
    if (ec) { error = "无法创建实例目录"; return nullptr; }
    if (!runtime) {
        runtime = makeRuntime(config);
        std::lock_guard lock(mu_);
        programs_.push_back(runtime);
    } else {
        std::lock_guard lock(runtime->mu);
        runtime->config = config;   // 命令、目录等改动在下一次启动时生效
    }
    persist();
    return describe(runtime);
}

bool ProgramManager::remove(const std::string& id, bool removeFiles, std::string& error) {
    auto runtime = find(id);
    if (!runtime) { error = "没有找到这个实例"; return false; }
    fs::path dir;
    {
        std::lock_guard lock(runtime->mu);
        if (runtime->state == "running" || runtime->state == "stopping") { error = "请先停止程序再删除"; return false; }
        runtime->stopRequested = true;
        ++runtime->generation;   // 取消等待中的自动重启
        dir = workingDirectory(runtime->config);
    }
    if (removeFiles && store_) {
        // 实例目录与日志一起进回收站（保留 7 天，可还原）
        std::error_code ec;
        try {
            if (fs::exists(dir, ec)) store_->trashReal(dir, "面板");
            runtime->output->setSink({});
            runtime->log.reset();
            if (fs::exists(logs_ / fromUtf8(id), ec)) store_->trashReal(logs_ / fromUtf8(id), "面板");
        } catch (const files::FileError& e) {
            error = std::string("移入回收站失败：") + e.what();
            return false;
        }
    }
    {
        std::lock_guard lock(mu_);
        programs_.erase(std::remove(programs_.begin(), programs_.end(), runtime), programs_.end());
    }
    persist();
    return true;
}

bool ProgramManager::importArchive(const std::string& id, const fs::path& archive, const std::string& fileName,
                                   const std::string& encoding, std::string& error) {
    auto runtime = find(id);
    if (!runtime) { error = "没有找到这个实例"; return false; }
    fs::path dir;
    {
        std::lock_guard lock(runtime->mu);
        if (runtime->state == "running" || runtime->state == "stopping" || runtime->state == "restarting") { error = "请先停止程序再导入"; return false; }
        dir = workingDirectory(runtime->config);
    }
    const auto lower = lowerAscii(fileName);
    const bool zip = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0;
    const bool tar = (lower.size() > 7 && lower.compare(lower.size() - 7, 7, ".tar.gz") == 0) || (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".tgz") == 0);
    if (!zip && !tar) { error = "只支持 .zip、.tar.gz、.tgz 压缩包"; return false; }
    // 先解到同一块盘上的暂存目录，成功了再换上去：解到一半失败，原来的目录不受影响
    const auto staging = dir.parent_path() / fromUtf8(".import-" + id);
    std::error_code ec;
    fs::remove_all(staging, ec);
    try {
        if (zip) files::extractZipTo(archive, staging, encoding, true, 8ULL << 30);
        else if (!update::extractTarArchive(archive, staging, error)) { fs::remove_all(staging, ec); return false; }
    } catch (const files::FileError& e) {
        fs::remove_all(staging, ec);
        error = e.what();
        return false;
    }
    // 压缩包外面只套了一层文件夹（常见的「Dice/…」打包方式）时去掉这一层
    fs::path content = staging;
    {
        std::vector<fs::path> children;
        for (const auto& item : fs::directory_iterator(staging, ec)) children.push_back(item.path());
        if (children.size() == 1 && fs::is_directory(children[0], ec)) content = children[0];
    }
    try {
        if (fs::exists(dir, ec) && !fs::is_empty(dir, ec) && store_) store_->trashReal(dir, "面板");
        else fs::remove_all(dir, ec);
    } catch (const files::FileError& e) {
        fs::remove_all(staging, ec);
        error = std::string("旧的实例目录移入回收站失败：") + e.what();
        return false;
    }
    fs::create_directories(dir.parent_path(), ec);
    fs::rename(content, dir, ec);
    fs::remove_all(staging, ec);
    if (ec) { error = "替换实例目录失败：没有权限或文件正在使用"; return false; }
    if (store_) {
        files::OpRecord op;
        op.action = "instance-import";
        op.path = fileName;
        op.target = utf8(dir);
        op.actor = "面板";
        store_->record(op);
    }
#ifndef _WIN32
    markExecutables(dir);
#endif
    runtime->output->append("\r\n\x1b[36m[CatOfficial] 已导入压缩包 " + fileName + "，原来的实例目录已移入回收站\x1b[0m\r\n");
    if (log_) log_(LogLevel::info, "托管程序「" + runtime->config.name + "」已导入压缩包 " + fileName);
    return true;
}

json ProgramManager::logList(const std::string& id) {
    auto r = find(id);
    return r && r->log ? r->log->list() : json::array();
}

fs::path ProgramManager::logFile(const std::string& id, const std::string& name) {
    auto r = find(id);
    return r && r->log ? r->log->file(name) : fs::path();
}

ProgramLogSettings ProgramManager::logSettings() const {
    std::lock_guard lock(mu_);
    return logSettings_;
}

bool ProgramManager::saveLogSettings(const json& body, std::string& error) {
    const int days = body.value("log_retention_days", logSettings().retentionDays);
    const int megabytes = body.value("log_max_mb", logSettings().maxMegabytes);
    if (days < 0 || days > 3650) { error = "日志保存天数应在 0 到 3650 之间（0 表示不按天数删除）"; return false; }
    if (megabytes < 1 || megabytes > 102400) { error = "每个实例的日志上限应在 1 到 102400 MB 之间"; return false; }
    {
        std::lock_guard lock(mu_);
        logSettings_.retentionDays = days;
        logSettings_.maxMegabytes = megabytes;
    }
    persist();
    pruneLogs();
    return true;
}

void ProgramManager::pruneLogs() {
    std::vector<std::shared_ptr<Runtime>> copy;
    {
        std::lock_guard lock(mu_);
        copy = programs_;
    }
    for (const auto& r : copy) if (r->log) r->log->prune();
}

bool ProgramManager::launch(const std::shared_ptr<Runtime>& r, std::string& error) {
    PtyOptions options;
    ProgramConfig config;
    {
        std::lock_guard lock(r->mu);
        config = r->config;
    }
#ifdef _WIN32
    options.commandLine = widen(resolveProgramCommand(config.command, workingDirectory(config)));
#else
    ensureCommandExecutable(config.command, workingDirectory(config));
    options.argv = {defaultShell(), "-c", resolveProgramCommand(config.command, workingDirectory(config))};
#endif
    options.cwd = utf8(workingDirectory(config));
    options.env = config.env;
    options.cols = cols_;
    options.rows = rows_;
    auto pty = Pty::start(options, error);
    if (!pty) {
        std::lock_guard lock(r->mu);
        r->state = "crashed";
        r->error = error;
        r->output->append("\r\n\x1b[31m[CatOfficial] 启动失败：" + error + "\x1b[0m\r\n");
        return false;
    }
    if (r->pump.joinable()) r->pump.join();   // 上一次的读线程早已结束
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(r->mu);
        r->pty = std::move(pty);
        r->pid = r->pty->pid();
        r->state = "running";
        r->error.clear();
        r->hasExit = false;
        r->stopRequested = false;
        r->startedAt = nowRfc3339();
        generation = ++r->generation;
        r->output->append("\r\n\x1b[36m[CatOfficial] 已启动：" + config.command + "\x1b[0m\r\n");
    }
    std::weak_ptr<Runtime> weak = r;
    Pty* pty_ = r->pty.get();
    auto output = r->output;
    Runtime* raw = r.get();
    raw->exited = false;
    // 读线程只持有输出缓冲，不持有程序本身：程序被删除时不会在自己的线程里析构自己
    r->pump = std::thread([this, weak, pty_, output, raw, generation] {
        std::vector<char> chunk(8192);
        while (true) {
            const auto n = pty_->read(chunk.data(), chunk.size());
            if (n == 0) break;
            output->append(chunk.data(), n);
        }
        const int code = pty_->wait();
        raw->exited = true;
        drogon::app().getLoop()->queueInLoop([this, weak, code, generation] {
            auto r = weak.lock();
            if (!r) return;
            {
                std::lock_guard lock(r->mu);
                if (r->generation != generation) return;
            }
            onExit(r, code);
        });
    });
    if (log_) log_(LogLevel::info, "托管程序「" + config.name + "」已启动（PID " + std::to_string(r->pid) + "）");
    return true;
}

void ProgramManager::onExit(const std::shared_ptr<Runtime>& r, int code) {
    bool restart = false;
    int delay = 5;
    std::string name;
    {
        std::lock_guard lock(r->mu);
        name = r->config.name;
        r->exitCode = code;
        r->hasExit = true;
        r->pid = 0;
        r->output->append("\r\n\x1b[33m[CatOfficial] 程序已退出，退出码 " + std::to_string(code) + "\x1b[0m\r\n");
        if (r->stopRequested) {
            r->state = "stopped";
        } else if (r->config.autoRestart) {
            // 两分钟内连续崩溃五次就不再自动重启，免得刷屏刷满日志
            const auto now = std::chrono::steady_clock::now();
            while (!r->recentStarts.empty() && now - r->recentStarts.front() > std::chrono::minutes(2)) r->recentStarts.pop_front();
            if (r->recentStarts.size() >= 5) {
                r->state = "crashed";
                r->error = "两分钟内连续退出五次，已停止自动重启";
                r->output->append("\x1b[31m[CatOfficial] " + r->error + "\x1b[0m\r\n");
            } else {
                r->state = "restarting";
                restart = true;
                delay = r->config.restartDelaySeconds;
                r->output->append("\x1b[33m[CatOfficial] " + std::to_string(delay) + " 秒后自动重启\x1b[0m\r\n");
            }
        } else {
            r->state = "crashed";
        }
    }
    if (log_) log_(restart || code == 0 ? LogLevel::info : LogLevel::warn, "托管程序「" + name + "」已退出，退出码 " + std::to_string(code));
    if (!restart) return;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(r->mu);
        generation = r->generation;
    }
    std::weak_ptr<Runtime> weak = r;
    schedule(delay, [this, weak, generation] {
        auto r = weak.lock();
        if (!r) return;
        {
            std::lock_guard lock(r->mu);
            if (r->generation != generation || r->state != "restarting") return;
            r->recentStarts.push_back(std::chrono::steady_clock::now());
        }
        std::string error;
        launch(r, error);
    });
}

bool ProgramManager::start(const std::string& id, std::string& error) {
    auto r = find(id);
    if (!r) { error = "没有找到这个程序"; return false; }
    {
        std::lock_guard lock(r->mu);
        if (r->state == "running" || r->state == "stopping") { error = "程序已在运行"; return false; }
        r->recentStarts.clear();
        r->recentStarts.push_back(std::chrono::steady_clock::now());
    }
    return launch(r, error);
}

bool ProgramManager::stop(const std::string& id, bool force, std::string& error) {
    auto r = find(id);
    if (!r) { error = "没有找到这个程序"; return false; }
    std::uint64_t generation = 0;
    int timeout = 15;
    {
        std::lock_guard lock(r->mu);
        if (r->state == "restarting" || r->state == "crashed") {
            ++r->generation;   // 取消等待中的自动重启
            r->state = "stopped";
            return true;
        }
        if (r->state != "running" && r->state != "stopping") { error = "程序没有在运行"; return false; }
        r->stopRequested = true;
        generation = r->generation;
        timeout = r->config.stopTimeoutSeconds;
        if (force || r->state == "stopping") {
            r->output->append("\r\n\x1b[33m[CatOfficial] 强制结束\x1b[0m\r\n");
            r->pty->kill();
            return true;
        }
        r->state = "stopping";
        // 先按程序自己的方式退出（输入停止指令，或发送 Ctrl+C），给它保存数据的时间
        if (!r->config.stopCommand.empty()) r->pty->write(r->config.stopCommand + "\r");
        else r->pty->write("\x03");
    }
    std::weak_ptr<Runtime> weak = r;
    schedule(timeout, [weak, generation] {
        auto r = weak.lock();
        if (!r) return;
        std::lock_guard lock(r->mu);
        if (r->generation != generation || r->state != "stopping" || !r->pty) return;
        r->output->append("\r\n\x1b[33m[CatOfficial] 停止超时，强制结束\x1b[0m\r\n");
        r->pty->kill();
    });
    return true;
}

bool ProgramManager::restart(const std::string& id, std::string& error) {
    auto r = find(id);
    if (!r) { error = "没有找到这个程序"; return false; }
    bool running = false;
    {
        std::lock_guard lock(r->mu);
        running = r->state == "running" || r->state == "stopping";
    }
    if (!running) return start(id, error);
    if (!stop(id, false, error)) return false;
    // 等它真正退出后再拉起来（最多等停止超时再多五秒）
    std::weak_ptr<Runtime> weak = r;
    auto poll = std::make_shared<std::function<void(int)>>();
    *poll = [this, weak, poll, id](int left) {
        auto r = weak.lock();
        if (!r) return;
        bool stopped = false;
        {
            std::lock_guard lock(r->mu);
            stopped = r->state == "stopped" || r->state == "crashed";
        }
        if (stopped) { std::string error; start(id, error); return; }
        if (left > 0) schedule(0.5, [poll, left] { (*poll)(left - 1); });
    };
    int timeout = 15;
    {
        std::lock_guard lock(r->mu);
        timeout = r->config.stopTimeoutSeconds;
    }
    schedule(0.5, [poll, timeout] { (*poll)((timeout + 5) * 2); });
    return true;
}

bool ProgramManager::input(const std::string& id, const std::string& data) {
    auto r = find(id);
    if (!r) return false;
    std::lock_guard lock(r->mu);
    return r->pty && (r->state == "running" || r->state == "stopping") && r->pty->write(data);
}

void ProgramManager::resize(const std::string& id, int cols, int rows) {
    if (cols > 0 && rows > 0) { cols_ = cols; rows_ = rows; }
    auto r = find(id);
    if (!r) return;
    std::lock_guard lock(r->mu);
    if (r->pty && r->state == "running") r->pty->resize(cols, rows);
}

bool ProgramManager::read(const std::string& id, std::int64_t cursor, double wait, OutputBuffer::Reply reply) {
    auto r = find(id);
    if (!r) return false;
    r->output->read(cursor, wait, std::move(reply));
    return true;
}

void ProgramManager::clearOutput(const std::string& id) {
    auto r = find(id);
    if (!r) return;
    std::lock_guard lock(r->mu);
    r->output->append("\x1b[2J\x1b[3J\x1b[H");
}

void ProgramManager::autoStart() {
    std::vector<std::shared_ptr<Runtime>> copy;
    {
        std::lock_guard lock(mu_);
        copy = programs_;
    }
    for (const auto& r : copy) {
        bool wanted = false;
        {
            std::lock_guard lock(r->mu);
            wanted = r->config.autoStart;
        }
        if (wanted) { std::string error; start(r->config.id, error); }
    }
}

void ProgramManager::stopAll() {
    std::vector<std::shared_ptr<Runtime>> copy;
    {
        std::lock_guard lock(mu_);
        copy = programs_;
    }
    int longest = 0;
    for (const auto& r : copy) {
        std::lock_guard lock(r->mu);
        if (r->state == "restarting") { ++r->generation; r->state = "stopped"; }
        if (r->state != "running" || !r->pty) continue;
        r->stopRequested = true;
        r->state = "stopping";
        if (!r->config.stopCommand.empty()) r->pty->write(r->config.stopCommand + "\r");
        else r->pty->write("\x03");
        longest = std::max(longest, r->config.stopTimeoutSeconds);
    }
    // 事件循环可能已经停了，这里直接等：给每个程序各自的停止时间，到点强制结束
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(longest);
    for (const auto& r : copy) {
        while (std::chrono::steady_clock::now() < deadline) {
            if (r->exited) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::lock_guard lock(r->mu);
        if (r->pty) r->pty->kill();
    }
    for (const auto& r : copy) if (r->pump.joinable()) r->pump.join();
}

}  // namespace catofficial::app::terminal
