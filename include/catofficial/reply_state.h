#pragma once

// 官方机器人的被动回复与事件去重状态。
//
// 被动回复：回复某条消息时带上它的 msg_id 与逐条递增的 msg_seq；没有消息可回时，
// 可以用 5 分钟内收到的事件 ID（入群、加好友、开启主动消息、互动按钮等）代替。
// 去重：Gateway 重连与 Webhook 重投都可能把同一事件送达多次。

#include <cstddef>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace catofficial {

class PassiveReplies {
public:
    static constexpr std::time_t kEventLifetimeSeconds = 300;

    /// 记下某个会话（调用方自定的键，例如「类型:目标」）最近一次可用于被动回复的事件 ID。
    void rememberEvent(const std::string& key, const std::string& eventId) {
        if (key.empty() || eventId.empty()) return;
        std::lock_guard lock(mu_);
        events_[key] = {eventId, std::time(nullptr)};
    }
    /// 取出并作废该会话的事件 ID；超过 5 分钟或不存在时返回空。
    std::string takeEvent(const std::string& key) {
        std::lock_guard lock(mu_);
        auto it = events_.find(key);
        if (it == events_.end() || std::time(nullptr) - it->second.second > kEventLifetimeSeconds) return {};
        auto id = it->second.first;
        events_.erase(it);
        return id;
    }
    /// 回复同一条消息的下一个 msg_seq，从 1 开始。
    int nextSeq(const std::string& messageId) {
        if (messageId.empty()) return 0;
        std::lock_guard lock(mu_);
        auto& seq = seqs_[messageId];
        if (seq <= 0) seq = 1;
        return seq++;
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, int> seqs_;
    std::unordered_map<std::string, std::pair<std::string, std::time_t>> events_;
};

class EventDeduplicator {
public:
    static constexpr std::time_t kWindowSeconds = 600;
    static constexpr std::size_t kCapacity = 16384;

    /// 第一次见到 key 返回 true；10 分钟内再次出现返回 false。
    bool acceptOnce(const std::string& key) {
        std::lock_guard lock(mu_);
        const auto now = std::time(nullptr);
        if (now - sweptAt_ >= 30) {
            for (auto it = seen_.begin(); it != seen_.end(); )
                if (now - it->second > kWindowSeconds) it = seen_.erase(it); else ++it;
            sweptAt_ = now;
        }
        if (seen_.count(key)) return false;
        if (seen_.size() >= kCapacity) {
            auto oldest = seen_.begin();
            for (auto it = seen_.begin(); it != seen_.end(); ++it) if (it->second < oldest->second) oldest = it;
            seen_.erase(oldest);
        }
        seen_[key] = now;
        return true;
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, std::time_t> seen_;
    std::time_t sweptAt_ = 0;
};

}  // namespace catofficial
