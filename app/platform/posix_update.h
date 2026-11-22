#pragma once
// 移植自 Dice!Next：POSIX 上的更新包路径校验与小工具（只依赖头文件 JSON）。
// 管理程序只在 core 没有运行时改动这些路径。
#ifndef _WIN32
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace catofficial::app::update::posix {
namespace fs = std::filesystem;
/// core 以这个退出码结束 = 请管理程序重启它（顺便安装已下载的更新）。
inline constexpr int kRestartExitCode = 42;

inline fs::path executablePath() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return fs::canonical(buffer.data());
#else
    std::vector<char> buffer(65536);
    const auto size = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    return size > 0 && static_cast<size_t>(size) < buffer.size()
        ? fs::path(std::string(buffer.data(), size)) : fs::path();
#endif
}

inline bool relativePathSafe(const fs::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& part : path) {
        const auto value = part.string();
        if (value == ".." || value == ".") return false;
        for (unsigned char ch : value) if (ch < 32 || ch == 127 || ch == '\\') return false;
    }
    return true;
}

inline bool pathExists(const fs::path& path) {
    return fs::symlink_status(path).type() != fs::file_type::not_found;
}

// 创建、替换、恢复文件时绝不跟随本地的符号链接。
inline void safeParents(const fs::path& root, const fs::path& relative, bool create) {
    if (!relativePathSafe(relative)) throw std::runtime_error("unsafe update path");
    auto current = root;
    for (const auto& part : relative.parent_path()) {
        current /= part;
        if (!pathExists(current)) {
            if (create) fs::create_directory(current);
        } else if (fs::symlink_status(current).type() != fs::file_type::directory) {
            throw std::runtime_error("update parent is not a real directory: " + current.string());
        }
    }
}

inline void writeJson(const fs::path& path, const nlohmann::json& value) {
    const auto temporary = path.string() + ".tmp";
    const std::string text = value.dump(2) + '\n';
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) throw std::runtime_error("cannot write update journal");
    size_t offset = 0;
    while (offset < text.size()) {
        const auto written = ::write(fd, text.data() + offset, text.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ::close(fd); throw std::runtime_error("cannot persist update journal"); }
        offset += static_cast<size_t>(written);
    }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) throw std::runtime_error("cannot sync update journal");
    fs::rename(temporary, path);
    const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) { ::fsync(directory); ::close(directory); }
}

inline nlohmann::json readJson(const fs::path& path) {
    if (fs::symlink_status(path).type() != fs::file_type::regular || fs::file_size(path) > 1024 * 1024)
        throw std::runtime_error("invalid update metadata");
    std::ifstream input(path);
    return nlohmann::json::parse(input);
}

/// 是否由同目录的管理程序 catofficial 拉起（只有这样才能自动安装更新）。
inline bool managedLaunch(const fs::path& root) {
    const char* managed = std::getenv("CATOFFICIAL_MANAGED");
    const char* parent = std::getenv("CATOFFICIAL_MANAGER_PID");
    if (!managed || std::string(managed) != "1" || !parent) return false;
    try {
        if (std::stoll(parent) != ::getppid()) return false;
        return fs::symlink_status(root / "catofficial").type() == fs::file_type::regular &&
            fs::symlink_status(root / "catofficial-core").type() == fs::file_type::regular &&
            ::access((root / "catofficial").c_str(), X_OK) == 0 &&
            ::access((root / "catofficial-core").c_str(), X_OK) == 0;
    } catch (...) { return false; }
}

/// 更新包可以替换的顶层路径；config.json 与 data/ 永远不碰。
inline const std::vector<std::string>& packagePaths() {
    static const std::vector<std::string> paths{
        "catofficial", "catofficial-core", "web", "update-mirrors.json", "LICENSE", "README.md", "使用说明.txt"};
    return paths;
}

inline void validatePackage(const fs::path& stage) {
    for (const auto* file : {"catofficial", "catofficial-core", "web/index.html"}) {
        safeParents(stage, file, false);
        if (fs::symlink_status(stage / file).type() != fs::file_type::regular)
            throw std::runtime_error(std::string("incomplete POSIX package: ") + file);
    }
    for (const auto* file : {"catofficial", "catofficial-core"})
        if (::access((stage / file).c_str(), X_OK) != 0)
            throw std::runtime_error(std::string("package executable is not executable: ") + file);
    for (const auto& entry : fs::recursive_directory_iterator(stage)) {
        const auto type = entry.symlink_status().type();
        if (type != fs::file_type::directory && type != fs::file_type::regular)
            throw std::runtime_error("unsupported special file in update package");
    }
}

}  // namespace catofficial::app::update::posix
#endif
