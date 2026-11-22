// CatOfficial 的 POSIX 管理程序（对应 Dice!Next 的 dice-next）：前台运行，systemd / launchd 看管的是它。
// 拉起同目录的 catofficial-core 并转发 SIGTERM / SIGINT；core 以退出码 42 结束表示「请重启」，
// 这时若 updates/pending 里有已授权的更新包，先替换文件（失败自动回滚），再重新拉起 core。
// 不经过 shell，也不依赖第三方动态库。
#include "../platform/posix_update.h"
#include "../platform/update_schedule.h"

#include <chrono>
#include <csignal>
#include <ctime>
#include <iostream>
#include <sys/file.h>
#include <sys/wait.h>

namespace fs = std::filesystem;
namespace update = catofficial::app::update;
namespace posix = catofficial::app::update::posix;

namespace {
volatile sig_atomic_t childPid = 0;
volatile sig_atomic_t stopping = 0;

void signalHandler(int signal) {
    stopping = signal;
    if (childPid > 0) ::kill(childPid, signal);
}

bool containerRuntime() {
    for (const auto* name : {"CATOFFICIAL_CONTAINER", "DOTNET_RUNNING_IN_CONTAINER", "container", "KUBERNETES_SERVICE_HOST"}) {
        const char* value = std::getenv(name);
        if (value && *value && std::string(value) != "0" && std::string(value) != "false") return true;
    }
    return fs::exists("/.dockerenv") || fs::exists("/run/.containerenv");
}

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm);
    return buffer;
}

void writeResult(const fs::path& root, const nlohmann::json& metadata, bool success, const std::string& message) {
    try {
        fs::create_directories(root / "updates");
        posix::writeJson(root / "updates/last-result.json",
            {{"schema", 1}, {"success", success}, {"metadata", metadata}, {"message", message}});
    } catch (...) {}
}

/// 用更新包替换程序文件：每一项先把旧的挪进 updates/rollbacks/<时间>，出错时按相反顺序还原。
bool applyUpdate(const fs::path& root, const fs::path& stage, std::string& error) {
    posix::validatePackage(stage);
    const fs::path rollback = root / "updates/rollbacks" / timestamp();
    struct Move { fs::path destination, backup; bool hadExisting; };
    std::vector<Move> moves;
    std::error_code ec;
    for (const auto& item : posix::packagePaths()) {
        const fs::path source = stage / item;
        if (!posix::pathExists(source)) continue;
        posix::safeParents(root, item, true);
        const fs::path destination = root / item;
        const fs::path backup = rollback / item;
        Move move{destination, backup, posix::pathExists(destination)};
        if (move.hadExisting) {
            fs::create_directories(backup.parent_path(), ec);
            if (!ec) fs::rename(destination, backup, ec);
        }
        if (!ec) fs::rename(source, destination, ec);
        if (ec) {
            error = "替换 " + item + " 失败：" + ec.message();
            if (move.hadExisting && !posix::pathExists(destination) && posix::pathExists(backup)) fs::rename(backup, destination, ec);
            for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
                std::error_code restore;
                fs::remove_all(it->destination, restore);
                if (it->hadExisting) fs::rename(it->backup, it->destination, restore);
            }
            return false;
        }
        moves.push_back(move);
    }
    fs::remove_all(stage, ec);
    std::cout << "CatOfficial update applied. Rollback: " << rollback.string() << '\n';
    return true;
}

pid_t launch(const fs::path& root, const std::vector<std::string>& args, int& execError) {
    execError = 0;
    const auto core = (root / "catofficial-core").string();
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(core.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    int pipeFds[2];
    if (::pipe(pipeFds) != 0) { execError = errno; return -1; }
    ::fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC);
    const pid_t child = ::fork();
    if (child < 0) { execError = errno; ::close(pipeFds[0]); ::close(pipeFds[1]); return -1; }
    if (child == 0) {
        ::close(pipeFds[0]);
        ::setenv("CATOFFICIAL_MANAGED", "1", 1);
        ::setenv("CATOFFICIAL_MANAGER_PID", std::to_string(::getppid()).c_str(), 1);
        ::execv(core.c_str(), argv.data());
        const int error = errno;
        (void)!::write(pipeFds[1], &error, sizeof(error));
        ::_exit(127);
    }
    ::close(pipeFds[1]);
    int error = 0;
    if (::read(pipeFds[0], &error, sizeof(error)) == sizeof(error)) execError = error;
    ::close(pipeFds[0]);
    childPid = child;
    return child;
}

int waitFor(pid_t child) {
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    childPid = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}
}  // namespace

int main(int argc, char* argv[]) {
    try {
        const auto root = posix::executablePath().parent_path();
        if (root.empty()) throw std::runtime_error("cannot locate CatOfficial installation");
        fs::current_path(root);
        fs::create_directories(root / "updates");
        const int lock = ::open((root / "updates/manager.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (lock < 0 || ::flock(lock, LOCK_EX | LOCK_NB) != 0) {
            std::cerr << "CatOfficial is already running, or the installation is not writable.\n";
            return 1;
        }
        std::signal(SIGTERM, signalHandler);
        std::signal(SIGINT, signalHandler);
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        while (!stopping) {
            const auto stage = root / "updates/pending";
            if (!containerRuntime() && fs::is_directory(stage) && update::pendingUpdateMayApply(stage)) {
                nlohmann::json metadata = nlohmann::json::object();
                try { metadata = posix::readJson(stage / "update.json"); } catch (...) {}
                std::string error;
                bool applied = false;
                try { applied = applyUpdate(root, stage, error); }
                catch (const std::exception& ex) { error = ex.what(); }
                writeResult(root, metadata, applied, applied ? "更新文件已成功应用" : error);
                if (applied) {
                    // 管理程序自己也可能被替换：重新执行新版本，再由它拉起 core。
                    std::vector<char*> self;
                    const auto path = (root / "catofficial").string();
                    self.push_back(const_cast<char*>(path.c_str()));
                    for (auto& arg : args) self.push_back(arg.data());
                    self.push_back(nullptr);
                    ::flock(lock, LOCK_UN);
                    ::close(lock);
                    ::execv(path.c_str(), self.data());
                    throw std::runtime_error("cannot restart the updated manager");
                }
                std::cerr << "CatOfficial update failed: " << error << '\n';
                std::error_code ec;
                fs::rename(stage, root / "updates" / ("failed-" + timestamp()), ec);
            }
            int execError = 0;
            const pid_t child = launch(root, args, execError);
            if (execError != 0) {
                if (child > 0) waitFor(child);
                throw std::runtime_error(std::string("cannot start core: ") + std::strerror(execError));
            }
            const int status = waitFor(child);
            if (stopping || status != posix::kRestartExitCode) return status;
        }
        return 128 + stopping;
    } catch (const std::exception& ex) {
        std::cerr << "CatOfficial manager: " << ex.what() << '\n';
        if (childPid > 0) { ::kill(childPid, SIGTERM); waitFor(childPid); }
        return 1;
    }
}
