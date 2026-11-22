#pragma once
// 文件管理（移植自 BDC 公寓守护进程的 filestore / trash / jobs）。
//
// # 范围
// 默认只开放 CatOfficial 的安装目录（config.json、data、web 所在的那一层），路径越不出去：
// 符号链接与 Windows 的目录联接也算，检查是在解析真实路径之后做的。启动时加参数
// --files-full-access 才开放整台机器（Windows 上根目录列出各个盘）。
//
// # 回收站与操作记录（与 BDC 相同）
// 删除一律先进回收站，留 7 天后才真正删掉；覆盖（移动、复制、上传撞名）时被盖掉的那一份也进回收站。
// 每次改动记一行：什么时候、从哪个 IP、做了什么、动了哪个路径。
// 都放在安装目录的 .catofficial 里（回收站 .catofficial/trash/<id>/，记录 .catofficial/file-ops.jsonl），
// 文件管理里看不见、动不了。开放整台机器时也放在这里，跨盘删除会先复制再删除。

#include "../paths.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace catofficial::app::files {

namespace fs = std::filesystem;
using json = nlohmann::json;

/// 给面板看的错误：只说相对路径怎么了，不带服务器上的绝对路径。
struct FileError : std::runtime_error {
    int status;
    explicit FileError(const std::string& message, int status = 400) : std::runtime_error(message), status(status) {}
};

inline constexpr std::uint64_t kMaxReadBytes = 64ULL << 20;          // 在线编辑 / 预览读取上限
inline constexpr std::uint64_t kMaxStreamUploadBytes = 2ULL << 30;   // 串流上传上限
inline constexpr std::size_t kMaxEntries = 2000;                     // 一次最多列出多少条
inline constexpr std::uint64_t kMaxExtractBytes = 512ULL << 20;      // 解压后的总体积上限
inline constexpr std::int64_t kTrashRetentionSeconds = 7 * 24 * 3600;
inline constexpr const char* kReservedDir = ".catofficial";

struct Entry { std::string name; bool isDir = false; std::uint64_t size = 0; std::string modified; };

struct TrashEntry {
    std::string id, path;
    bool isDir = false;
    std::int64_t size = 0;   // 条目太多没量完时为 -1
    std::string deletedAt, expiresAt, deletedBy;
    json toJson() const;
};

struct OpRecord {
    std::string time, action, path, target, actor, ip, trashId;
    int count = 0;
    std::int64_t bytes = 0;
    bool permanent = false;
    json toJson() const;
};

using Progress = std::function<void(std::int64_t delta, const std::string& name)>;

/// 一次后台文件操作（压缩、解压）。
class Job {
public:
    Job(std::string id, std::string kind) : id_(std::move(id)), kind_(std::move(kind)), started_(std::chrono::steady_clock::now()) {}
    void advance(std::int64_t delta, const std::string& name) {
        done_ += delta;
        if (!name.empty()) { std::lock_guard lock(mu_); current_ = name; }
    }
    void setTotal(std::int64_t total) { total_ = total; }
    void finish(const std::string& error);
    json snapshot() const;
    const std::string& id() const { return id_; }
    bool expired() const;

private:
    std::string id_, kind_;
    std::atomic<std::int64_t> done_{0}, total_{0};
    mutable std::mutex mu_;
    std::string current_, error_, status_ = "running";
    std::chrono::steady_clock::time_point started_, finished_{};
};

class FileStore {
public:
    /// @p installRoot 安装目录（回收站与记录总放在这里）；@p fullAccess 是否开放整台机器。
    FileStore(fs::path installRoot, bool fullAccess);

    bool fullAccess() const { return full_; }
    json info() const;

    // ── 面板的文件操作（与 BDC 的 action 一一对应）──
    std::vector<Entry> list(const std::string& path) const;
    std::string read(const std::string& path) const;
    std::uint64_t write(const std::string& path, const std::string& content);
    /// 上传：边收边写临时文件，收完才改名；同名旧文件进回收站。
    std::pair<std::uint64_t, std::optional<TrashEntry>> upload(const std::string& path, const fs::path& source, const std::string& by);
    std::optional<TrashEntry> discard(const std::string& path, bool recursive, bool permanent, const std::string& by);
    void mkdir(const std::string& path);
    std::optional<TrashEntry> rename(const std::string& from, const std::string& to, bool overwrite, const std::string& by);
    std::optional<TrashEntry> copy(const std::string& from, const std::string& to, bool overwrite, const std::string& by);
    bool exists(const std::string& path) const;
    std::int64_t countItems(const std::vector<std::string>& items) const;
    std::int64_t countEntries(const std::string& archive) const;
    std::vector<std::string> peekConflicts(const std::string& archive, const std::string& target, const std::string& encoding, std::size_t limit) const;
    void compress(const std::vector<std::string>& items, const std::string& target, bool overwrite, const Progress& progress);
    void extract(const std::string& archive, const std::string& target, const std::string& encoding, bool overwrite, const Progress& progress);
    /// 下载用：解析出真实路径（只允许普通文件），返回 {真实路径, 文件名}。
    std::pair<fs::path, std::string> fileForDownload(const std::string& path) const;

    // ── 回收站 ──
    std::vector<TrashEntry> trashList() const;
    TrashEntry trashRestore(const std::string& id, const std::string& by, bool overwrite);
    std::vector<TrashEntry> trashDelete(const std::vector<std::string>& ids);
    std::vector<TrashEntry> trashEmpty();
    int purgeExpiredTrash();

    // ── 操作记录 ──
    void record(OpRecord record);
    std::vector<OpRecord> readOps(std::size_t limit) const;

    // ── 后台任务 ──
    std::shared_ptr<Job> startJob(const std::string& kind, std::function<void(Job&)> work);
    std::shared_ptr<Job> job(const std::string& id);

    /// 虚拟路径 → 真实路径（越界抛 FileError）。也供终端设置工作目录使用。
    fs::path resolve(const std::string& path, bool allowRoot = true) const;

private:
    /// 规范化面板给的路径：以 / 分隔、去掉多余的 /、拒绝 . 与 ..。返回各段。
    static std::vector<std::string> segments(const std::string& path);
    static std::string joinSegments(const std::vector<std::string>& parts);
    std::string virtualPath(const fs::path& real) const;
    bool isReserved(const fs::path& real) const;
    void ensureInside(const fs::path& real) const;
    void guard(const fs::path& real) const;
    TrashEntry moveToTrash(const fs::path& real, const std::string& by);
    TrashEntry readTrashEntry(const std::string& id) const;
    static void copyRecursive(const fs::path& source, const fs::path& target);
    static std::int64_t measure(const fs::path& real);
    void walk(const fs::path& real, const std::function<void(const fs::path&, bool isDir)>& visit) const;

    fs::path install_;
    fs::path base_;     // 安装目录模式：安装目录的真实路径；整台机器模式（POSIX）：/
    bool full_;
    fs::path trashRoot_, opsLog_, opsLogOld_;
    mutable std::mutex opsMu_;
    std::mutex jobsMu_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
};

/// 面板操作的入口：body 是 {action, path, target, ...}，返回响应 JSON；出错抛 FileError。
json handleAction(FileStore& store, const json& body, const std::string& actor, const std::string& ip);

}  // namespace catofficial::app::files
