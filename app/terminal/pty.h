#pragma once
// 伪终端里跑一个进程：Windows 用 ConPTY（Windows 10 1809 起），POSIX 用 forkpty。
// 终端页的 shell 与「程序托管」里的程序都用它，网页上的 xterm 看到的就是真正的控制台输出。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace catofficial::app::terminal {

struct PtyOptions {
#ifdef _WIN32
    std::wstring commandLine;          // 原样交给 CreateProcessW
#else
    std::vector<std::string> argv;     // execvp
#endif
    std::string cwd;                   // UTF-8，空表示当前目录
    std::vector<std::pair<std::string, std::string>> env;   // 额外的环境变量
    int cols = 100;
    int rows = 30;
};

class Pty {
public:
    /// 启动失败返回 nullptr，原因写进 @p error。
    static std::unique_ptr<Pty> start(const PtyOptions& options, std::string& error);
    ~Pty();

    /// 阻塞读输出；进程结束且输出读完后返回 0。只在一个线程里调用。
    std::size_t read(char* buffer, std::size_t size);
    bool write(const std::string& data);
    void resize(int cols, int rows);
    /// 结束进程及其子进程（Windows 用作业对象，POSIX 杀整个进程组）。
    void kill();
    /// 等进程退出，返回退出码。read 返回 0 之后调用。
    int wait();
    std::int64_t pid() const { return pid_; }

private:
    Pty() = default;
    std::int64_t pid_ = 0;
#ifdef _WIN32
    void* process_ = nullptr;
    void* job_ = nullptr;
    void* console_ = nullptr;     // HPCON
    void* input_ = nullptr;       // 写给控制台
    void* output_ = nullptr;      // 从控制台读
    void* watcher_ = nullptr;     // 进程退出后关闭伪控制台，好让 read 读到结尾
#else
    int master_ = -1;
#endif
};

/// 默认的交互式 shell（Windows：PowerShell，没有时 cmd；POSIX：$SHELL / bash / sh）。
std::string defaultShell();

}  // namespace catofficial::app::terminal
