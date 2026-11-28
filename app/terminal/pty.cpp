#include "pty.h"

#include "../paths.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#elif !defined(__ANDROID__)
#include <pty.h>
#endif
#endif

namespace catofficial::app::terminal {

#ifdef _WIN32

namespace {
// ConPTY 在 Windows 10 1809 才有。用 GetProcAddress 取，旧系统上给出明确的提示而不是起不来。
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, void**);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(void*, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(void*);
constexpr DWORD_PTR kAttributePseudoConsole = 0x00020016;   // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
};

const ConPtyApi& api() {
    static const ConPtyApi value = [] {
        ConPtyApi a;
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            a.create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(kernel, "CreatePseudoConsole"));
            a.resize = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(kernel, "ResizePseudoConsole"));
            a.close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel, "ClosePseudoConsole"));
        }
        return a;
    }();
    return value;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

/// 当前环境加上额外变量，按 CreateProcessW 要求排好（双零结尾）。
std::vector<wchar_t> environmentBlock(const std::vector<std::pair<std::string, std::string>>& extra) {
    std::vector<std::wstring> entries;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) entries.emplace_back(p);
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [key, value] : extra) {
        const auto name = widen(key);
        const auto prefix = name + L"=";
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::wstring& e) {
            return _wcsnicmp(e.c_str(), prefix.c_str(), prefix.size()) == 0;
        }), entries.end());
        entries.push_back(prefix + widen(value));
    }
    std::vector<wchar_t> out;
    for (const auto& e : entries) { out.insert(out.end(), e.begin(), e.end()); out.push_back(L'\0'); }
    out.push_back(L'\0');
    return out;
}
}  // namespace

std::unique_ptr<Pty> Pty::start(const PtyOptions& options, std::string& error) {
    const auto& conpty = api();
    if (!conpty.create || !conpty.close) { error = "网页终端需要 Windows 10 1809 或更新的系统"; return nullptr; }
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, nullptr, 0) || !CreatePipe(&outRead, &outWrite, nullptr, 0)) {
        error = "无法创建管道";
        return nullptr;
    }
    void* console = nullptr;
    const COORD size{static_cast<SHORT>(options.cols > 0 ? options.cols : 100), static_cast<SHORT>(options.rows > 0 ? options.rows : 30)};
    if (FAILED(conpty.create(size, inRead, outWrite, 0, &console))) {
        CloseHandle(inRead); CloseHandle(inWrite); CloseHandle(outRead); CloseHandle(outWrite);
        error = "无法创建伪终端";
        return nullptr;
    }
    // 伪控制台已经持有这两端
    CloseHandle(inRead);
    CloseHandle(outWrite);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuffer(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, kAttributePseudoConsole, console, sizeof(console), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    // 父进程的标准输入输出被重定向（比如后台运行）时，子进程会沿用它们而不是伪控制台：
    // 明确给空句柄，子进程才会用伪控制台。
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> command(options.commandLine.begin(), options.commandLine.end());
    command.push_back(L'\0');
    auto env = environmentBlock(options.env);
    const std::wstring cwd = widen(options.cwd);
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env.data(),
        cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!created) {
        const DWORD code = GetLastError();
        conpty.close(console);
        CloseHandle(inWrite); CloseHandle(outRead);
        error = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? "找不到要运行的程序或工作目录"
              : code == ERROR_DIRECTORY ? "工作目录不存在" : "无法启动程序（错误代码 " + std::to_string(code) + "）";
        return nullptr;
    }
    // 作业对象：关闭终端或停止程序时，连它派生的子进程一起结束
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::unique_ptr<Pty> pty(new Pty());
    pty->pid_ = pi.dwProcessId;
    pty->process_ = pi.hProcess;
    pty->job_ = job;
    pty->console_ = console;
    pty->input_ = inWrite;
    pty->output_ = outRead;
    // 进程退出后关闭伪控制台，输出管道才会读到结尾
    HANDLE process = pi.hProcess;
    pty->watcher_ = new std::thread([process, console] {
        WaitForSingleObject(process, INFINITE);
        api().close(console);
    });
    return pty;
}

Pty::~Pty() {
    if (process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) kill();
    if (watcher_) {
        auto* thread = static_cast<std::thread*>(watcher_);
        if (thread->joinable()) thread->join();
        delete thread;
    }
    if (input_) CloseHandle(input_);
    if (output_) CloseHandle(output_);
    if (job_) CloseHandle(job_);
    if (process_) CloseHandle(process_);
}

std::size_t Pty::read(char* buffer, std::size_t size) {
    DWORD read = 0;
    if (!ReadFile(output_, buffer, static_cast<DWORD>(size), &read, nullptr)) return 0;
    return read;
}

bool Pty::write(const std::string& data) {
    DWORD written = 0;
    return WriteFile(input_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
}

void Pty::resize(int cols, int rows) {
    if (api().resize && console_ && cols > 0 && rows > 0) api().resize(console_, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
}

void Pty::kill() {
    if (job_) TerminateJobObject(job_, 1);
    else if (process_) TerminateProcess(process_, 1);
}

int Pty::wait() {
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    return static_cast<int>(code);
}

std::string defaultShell() {
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    std::error_code ec;
    if (length && length < MAX_PATH) {
        const std::filesystem::path powershell = std::filesystem::path(std::wstring(system, length)) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
        if (std::filesystem::exists(powershell, ec)) return utf8(powershell);
        return utf8(std::filesystem::path(std::wstring(system, length)) / L"cmd.exe");
    }
    return "cmd.exe";
}

#else

std::unique_ptr<Pty> Pty::start(const PtyOptions& options, std::string& error) {
    if (options.argv.empty()) { error = "没有要运行的命令"; return nullptr; }
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(options.cols > 0 ? options.cols : 100);
    ws.ws_row = static_cast<unsigned short>(options.rows > 0 ? options.rows : 30);
    int errorPipe[2];
    if (::pipe(errorPipe) != 0) { error = "无法创建管道"; return nullptr; }
    ::fcntl(errorPipe[1], F_SETFD, FD_CLOEXEC);
    auto runChild = [&] {
        ::close(errorPipe[0]);
        // 子进程自成一个会话：结束时杀整个进程组就能连子进程一起带走
        if (!options.cwd.empty() && ::chdir(options.cwd.c_str()) != 0) {
            const int code = 1;
            (void)!::write(errorPipe[1], &code, sizeof(code));
            ::_exit(127);
        }
        ::setenv("TERM", "xterm-256color", 1);
        for (const auto& [key, value] : options.env) ::setenv(key.c_str(), value.c_str(), 1);
        std::vector<char*> argv;
        for (const auto& arg : options.argv) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        const int code = 2;
        (void)!::write(errorPipe[1], &code, sizeof(code));
        ::_exit(127);
    };

    int master = -1;
    pid_t pid = -1;
#ifdef __ANDROID__
    // bionic 没有 libutil/forkpty。用 posix_openpt，和 Termux 同一路。
    master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0) {
        (void)::ioctl(master, TIOCSWINSZ, &ws);
        char slaveName[64]{};
        if (::ptsname_r(master, slaveName, sizeof(slaveName)) == 0) {
            pid = ::fork();
            if (pid == 0) {
                ::setsid();
                const int slave = ::open(slaveName, O_RDWR);
                if (slave < 0) ::_exit(127);
                (void)::ioctl(slave, TIOCSCTTY, nullptr);
                ::dup2(slave, STDIN_FILENO);
                ::dup2(slave, STDOUT_FILENO);
                ::dup2(slave, STDERR_FILENO);
                if (slave > STDERR_FILENO) ::close(slave);
                ::close(master);
                runChild();
            }
        }
    }
    if (pid < 0 && master >= 0) {
        ::close(master);
        master = -1;
    }
#else
    pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid == 0) runChild();
#endif
    if (pid < 0) { ::close(errorPipe[0]); ::close(errorPipe[1]); if (master >= 0) ::close(master); error = "无法创建伪终端"; return nullptr; }
    ::close(errorPipe[1]);
    int code = 0;
    const auto got = ::read(errorPipe[0], &code, sizeof(code));
    ::close(errorPipe[0]);
    if (got == sizeof(code)) {
        ::close(master);
        ::waitpid(pid, nullptr, 0);
        error = code == 1 ? "工作目录不存在" : "找不到要运行的程序";
        return nullptr;
    }
    std::unique_ptr<Pty> pty(new Pty());
    pty->pid_ = pid;
    pty->master_ = master;
    return pty;
}

Pty::~Pty() {
    if (pid_ > 0 && ::waitpid(static_cast<pid_t>(pid_), nullptr, WNOHANG) == 0) {
        kill();
        ::waitpid(static_cast<pid_t>(pid_), nullptr, 0);
    }
    if (master_ >= 0) ::close(master_);
}

std::size_t Pty::read(char* buffer, std::size_t size) {
    while (true) {
        const auto n = ::read(master_, buffer, size);
        if (n > 0) return static_cast<std::size_t>(n);
        if (n < 0 && errno == EINTR) continue;
        return 0;   // EIO：终端另一端都关了
    }
}

bool Pty::write(const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto n = ::write(master_, data.data() + offset, data.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

void Pty::resize(int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ::ioctl(master_, TIOCSWINSZ, &ws);
}

void Pty::kill() {
    if (pid_ > 0) {
        ::kill(-static_cast<pid_t>(pid_), SIGKILL);
        ::kill(static_cast<pid_t>(pid_), SIGKILL);
    }
}

int Pty::wait() {
    int status = 0;
    while (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0 && errno == EINTR) {}
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    pid_ = 0;
    return code;
}

std::string defaultShell() {
    std::error_code ec;
#ifdef __ANDROID__
    if (std::filesystem::exists("/system/bin/sh", ec)) return "/system/bin/sh";
    return "/system/bin/sh";
#else
    if (const char* shell = std::getenv("SHELL"); shell && *shell && std::filesystem::exists(shell, ec)) return shell;
    if (std::filesystem::exists("/bin/bash", ec)) return "/bin/bash";
    return "/bin/sh";
#endif
}

#endif

}  // namespace catofficial::app::terminal
