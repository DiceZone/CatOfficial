#pragma once

// 运行日志：除了打到控制台，还在内存里保留最近的若干条，供管理面板的「运行日志」页查看。

#include <catofficial/log.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace catofficial::app {

class LogBuffer {
public:
    static constexpr std::size_t kCapacity = 3000;

    static LogBuffer& get() { static LogBuffer b; return b; }

    /// 全局日志入口（主程序设置），没有持有 Logger 的模块可以经这里写日志。
    void setLogger(Logger logger) { std::lock_guard lock(mu_); logger_ = std::move(logger); }
    void log(LogLevel level, const std::string& message) {
        Logger logger;
        { std::lock_guard lock(mu_); logger = logger_; }
        if (logger) logger(level, message);
    }

    /// 最低记录级别（可热更新）：0 debug，1 info，2 warn，3 error。
    void setMinimum(const std::string& name) { minimum_ = name == "debug" ? 0 : name == "warn" ? 2 : name == "error" ? 3 : 1; }
    int minimum() const { return minimum_.load(); }

    void push(LogLevel level, const std::string& message) {
        std::lock_guard lock(mu_);
        entries_.push_back(Entry{++seq_, std::time(nullptr), level, message});
        while (entries_.size() > kCapacity) entries_.pop_front();
    }

    /// seq 大于 after 的条目，最多 limit 条（取最新的）。
    nlohmann::json since(uint64_t after, std::size_t limit) {
        std::lock_guard lock(mu_);
        nlohmann::json list = nlohmann::json::array();
        std::size_t start = 0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < entries_.size(); ++i) if (entries_[i].seq > after) ++count;
        if (count > limit) start = entries_.size() - limit;
        for (std::size_t i = start; i < entries_.size(); ++i) {
            const auto& e = entries_[i];
            if (e.seq <= after) continue;
            list.push_back({{"seq", e.seq}, {"time", e.time}, {"level", name(e.level)}, {"message", e.message}});
        }
        return list;
    }

    static const char* name(LogLevel level) {
        switch (level) {
            case LogLevel::debug: return "debug";
            case LogLevel::info: return "info";
            case LogLevel::warn: return "warn";
            default: return "error";
        }
    }

private:
    struct Entry { uint64_t seq; std::time_t time; LogLevel level; std::string message; };
    std::mutex mu_;
    Logger logger_;
    std::atomic<int> minimum_{1};
    std::deque<Entry> entries_;
    uint64_t seq_ = 0;
};

}  // namespace catofficial::app
