#pragma once

// 图片缓存：收发消息里的图片下载到 data/cache/images，供管理面板的消息记录与在线聊天显示
// （官方给的图片地址会过期）。文件名是地址的 SHA-1；超过保留天数的按修改时间删除。

#include "http.h"
#include "paths.h"

#include <catofficial/log.h>

#include <openssl/sha.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace catofficial::app {

class ImageCache {
public:
    static constexpr std::size_t kMaxBytes = 20u << 20;

    static ImageCache& get() { static ImageCache cache; return cache; }

    void start(const std::filesystem::path& dir, Logger log) {
        std::lock_guard lock(mu_);
        if (worker_.joinable()) return;
        dir_ = dir;
        log_ = std::move(log);
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        worker_ = std::thread([this] { run(); });
    }
    void stop() {
        { std::lock_guard lock(mu_); stopping_ = true; }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    ~ImageCache() { stop(); }

    void setDays(int days) { std::lock_guard lock(mu_); days_ = days; }
    int days() const { std::lock_guard lock(mu_); return days_; }

    /// 排队下载；已缓存或已在队列里的跳过。保留天数为 0 时不缓存。
    void enqueue(const std::vector<std::string>& urls) {
        {
            std::lock_guard lock(mu_);
            if (days_ <= 0 || dir_.empty()) return;
            for (const auto& url : urls) {
                if (queued_.count(url) || queue_.size() >= 500) continue;
                if (!findLocked(key(url)).empty()) continue;
                queued_.insert(url);
                queue_.push_back(url);
            }
        }
        wake_.notify_one();
    }

    /// 已缓存时返回文件名（不含目录），否则为空。
    std::string cached(const std::string& url) {
        std::lock_guard lock(mu_);
        return findLocked(key(url));
    }
    /// 文件名对应的路径；名字不合法或不存在时为空。
    std::filesystem::path pathOf(const std::string& name) {
        if (name.size() < 41 || name.size() > 46 || name.find_first_of("/\\:") != std::string::npos || name.find("..") != std::string::npos) return {};
        std::lock_guard lock(mu_);
        auto path = dir_ / name;
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) ? path : std::filesystem::path();
    }

    /// 删除过期文件，返回删除数量。
    int prune() {
        std::filesystem::path dir;
        int days;
        { std::lock_guard lock(mu_); dir = dir_; days = days_; }
        if (dir.empty()) return 0;
        const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24) * (days <= 0 ? 0 : days);
        int removed = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::error_code e2;
            if (!entry.is_regular_file(e2)) continue;
            if (days > 0 && entry.last_write_time(e2) >= cutoff) continue;
            if (std::filesystem::remove(entry.path(), e2)) ++removed;
        }
        return removed;
    }

    /// 缓存目录的文件数与总大小。
    std::pair<int, uintmax_t> usage() {
        std::filesystem::path dir;
        { std::lock_guard lock(mu_); dir = dir_; }
        int files = 0; uintmax_t bytes = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::error_code e2;
            if (!entry.is_regular_file(e2)) continue;
            ++files; bytes += entry.file_size(e2);
        }
        return {files, bytes};
    }

    static std::string key(const std::string& url) {
        unsigned char digest[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const unsigned char*>(url.data()), url.size(), digest);
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (unsigned char c : digest) { out += hex[c >> 4]; out += hex[c & 15]; }
        return out;
    }

private:
    ImageCache() = default;

    std::string findLocked(const std::string& k) const {
        if (dir_.empty()) return {};
        std::error_code ec;
        for (const char* ext : {".jpg", ".png", ".gif", ".webp", ".bmp", ".img"})
            if (std::filesystem::exists(dir_ / (k + ext), ec)) return k + ext;
        return {};
    }

    static const char* extension(const std::string& type, const std::string& body) {
        auto starts = [&](const char* magic, std::size_t n) { return body.size() >= n && body.compare(0, n, magic, n) == 0; };
        if (starts("\x89PNG", 4)) return ".png";
        if (starts("\xFF\xD8\xFF", 3)) return ".jpg";
        if (starts("GIF8", 4)) return ".gif";
        if (body.size() >= 12 && body.compare(0, 4, "RIFF") == 0 && body.compare(8, 4, "WEBP") == 0) return ".webp";
        if (starts("BM", 2)) return ".bmp";
        if (type.rfind("image/", 0) == 0) return ".img";
        return nullptr;
    }

    void run() {
        for (;;) {
            std::string url;
            {
                std::unique_lock lock(mu_);
                wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                url = queue_.front();
                queue_.pop_front();
            }
            const auto response = http::get(url, kMaxBytes);
            std::filesystem::path dir;
            Logger log;
            { std::lock_guard lock(mu_); queued_.erase(url); dir = dir_; log = log_; }
            const char* ext = response.ok() ? extension(response.contentType, response.body) : nullptr;
            if (!ext) {
                if (log) log(LogLevel::debug, "图片缓存失败：" + (response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error));
                continue;
            }
            const auto path = dir / (key(url) + ext);
            const auto temp = path.string() + ".part";
            {
                std::ofstream out(temp, std::ios::binary);
                out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
            }
            std::error_code ec;
            std::filesystem::rename(temp, path, ec);
            if (ec) std::filesystem::remove(temp, ec);
        }
    }

    mutable std::mutex mu_;
    std::condition_variable wake_;
    std::thread worker_;
    std::filesystem::path dir_;
    Logger log_;
    std::deque<std::string> queue_;
    std::unordered_set<std::string> queued_;
    int days_ = 7;
    bool stopping_ = false;
};

}  // namespace catofficial::app
