#pragma once
// 在线更新（移植自 Dice!Next 的 UpdateService）：检查 GitHub Release 的 update-manifest.json、
// 竞速选择 GitHub 或加速镜像、下载并校验 SHA-256、解压到 updates/pending，
// 再由启动器（Windows 的 catofficial.exe / POSIX 的 catofficial）在 core 退出后替换文件。
// 配置文件 config.json 与 data/ 不在更新包里，升级不会碰到。

#include <catofficial/log.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace catofficial::app::update {

struct ReleaseAsset {
    std::string os;
    std::string arch;
    std::string name;
    std::string sha256;
    std::uint64_t size = 0;
};

struct ReleaseManifest {
    int schema = 0;
    std::string repository;
    std::string tag;
    std::string version;
    int build = 0;
    bool prerelease = false;
    std::string publishedAt;
    std::string releaseUrl;
    std::string releaseNotes;
    std::vector<ReleaseAsset> assets;
};

inline constexpr const char* kRepository = "DiceZone/CatOfficial";

bool parseReleaseManifest(const std::string& text, ReleaseManifest& manifest, std::string& error);
int compareRelease(const std::string& leftVersion, int leftBuild, const std::string& rightVersion, int rightBuild);
const ReleaseAsset* selectAsset(const ReleaseManifest& manifest, const std::string& os, const std::string& arch);
bool archiveEntrySafe(const std::string& entry);
std::vector<std::string> missingWindowsPackageComponents(const std::filesystem::path& packageRoot);
std::string buildMirroredUrl(const std::string& originalUrl, const std::string& mirror);
std::string currentOs();
std::string currentArch();
std::string versionString();
int buildNumber();
bool isPrerelease();
std::string releaseTag();
/// 更新说明太长时截断（通知与面板里只放前面一段）。
std::string releaseNotesPreview(const std::string& notes);
/// 服务器所在时区相对 UTC 的分钟数（定时安装按服务器时区计算）。
int timezoneOffsetMinutes();

struct ContainerDetectionInput {
    std::string catofficialMarker;
    std::string dotnetMarker;
    std::string standardMarker;
    std::string systemdMarker;
    std::string kubernetesServiceHost;
    std::string windowsSandboxMount;
    bool dockerEnvFile = false;
    bool containerEnvFile = false;
    std::string cgroup;
    std::string mountInfo;
};

struct ContainerEnvironment {
    bool detected = false;
    std::string type;
    std::string evidence;
};

ContainerEnvironment detectContainerEnvironment(const ContainerDetectionInput& input);
ContainerEnvironment detectContainerEnvironment();

struct DownloadPolicy {
    std::chrono::milliseconds idleTimeout{60000};
    std::chrono::milliseconds attemptTimeout{20 * 60 * 1000};
    std::chrono::milliseconds pollInterval{250};
};

class UpdateService {
public:
    using Json = nlohmann::json;
    using NotifyCallback = std::function<void(const std::string& event, const std::string& message)>;
    using CancellationCheck = std::function<bool()>;

    /// @p settingsFile 保存更新设置（data/update.json）；@p restart 在需要安装时被调用，
    /// 负责让启动器接手（Windows 拉起 catofficial.exe --after，POSIX 以退出码 42 结束）。
    UpdateService(std::filesystem::path settingsFile, Logger log, std::function<void()> restart,
                  NotifyCallback notify = {}, ContainerEnvironment container = detectContainerEnvironment());
    ~UpdateService();

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    Json status() const;
    bool updateSettings(const Json& values, std::string& error);
    bool requestCheck(bool force, std::string& error);
    bool requestDownload(std::string& error);
    bool requestInstall(std::string& error);
    bool requestCancelDownload(std::string& error);
    /// 每分钟调用一次：处理上次安装结果、到点的定时安装与定期检查。
    void tick();
    /// 当前启动方式是否支持自动安装（由启动器拉起、不在容器里）。
    bool installSupported() const;

private:
    enum class Job { none, check, download, install };

    struct Settings {
        bool autoCheck = true;
        int intervalHours = 6;
        std::string action = "notify";
        bool scheduledInstall = false;
        std::string installTime = "04:00";
        std::string source = "auto";
        std::string customMirror;
    };

    struct Source {
        std::string prefix;
        std::string label;
        long long latencyMs = 0;
    };

    struct ProbeResult {
        bool ok = false;
        Source source;
        ReleaseManifest manifest;
        std::string error;
    };

    Json readSettingsFile() const;
    bool writeSettingsFile(const Json& value) const;
    Settings settings() const;
    bool downloadSupported() const;
    std::string containerUpdateError() const;
    bool isBusyLocked() const;
    bool queueJobLocked(Job job, const std::string& phase, std::string& error);
    void workerLoop();
    void doCheck(bool force);
    void doDownload();
    void doInstall();
    void processInstallResult();
    bool reconcilePendingLocked(const Settings& current, std::string& error, bool arm = false);
    std::int64_t now() const;
    void emitNotification(const std::string& event, const std::string& message) const;
    void say(LogLevel level, const std::string& message) const { if (log_) log_(level, message); }

    std::vector<Source> configuredSources(const Settings& settings) const;
    ProbeResult probeManifest(const Source& source, const CancellationCheck& cancelled = {}) const;
    std::vector<ProbeResult> raceManifestSources(const std::vector<Source>& sources) const;
    bool downloadAsset(const ReleaseManifest& manifest, const ReleaseAsset& asset,
                       const std::vector<Source>& sources, std::filesystem::path& archive,
                       std::string& usedSource, std::string& error);
    bool prepareStage(const std::filesystem::path& archive, const ReleaseManifest& manifest, std::string& error);

public:
    static bool fetchToFile(const std::string& url, const std::filesystem::path& output,
                            std::uint64_t maxBytes, int timeoutSeconds, std::string& error,
                            const CancellationCheck& cancelled = {});
    static bool sha256File(const std::filesystem::path& file, std::string& digest, std::string& error,
                           const CancellationCheck& cancelled = {});
    static std::string sourceLabel(const std::string& prefix);

private:
    std::filesystem::path settingsFile_;
    Logger log_;
    std::function<void()> restart_;
    NotifyCallback notify_;
    ContainerEnvironment container_;
    DownloadPolicy downloadPolicy_;

    mutable std::mutex settingsMutex_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;

    Job job_ = Job::none;
    bool forceCheck_ = false;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> downloadCancelled_{false};
    bool downloadActive_ = false;

    std::string phase_ = "idle";
    std::string error_;
    std::string activeSource_;
    std::uint64_t downloadedBytes_ = 0;
    std::uint64_t totalBytes_ = 0;
    std::int64_t checkedAt_ = 0;
    bool hasLatest_ = false;
    bool updateAvailable_ = false;
    bool automaticDownload_ = false;
    bool installResultProcessed_ = false;
    bool scheduledInstallJob_ = false;
    std::int64_t scheduledInstallAt_ = 0;
    std::string pendingTag_;
    ReleaseManifest latest_;
    std::vector<Source> sourceOrder_;
    std::int64_t sourceCacheUntil_ = 0;
    std::thread worker_;
};

}  // namespace catofficial::app::update
