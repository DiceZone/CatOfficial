#pragma once

// 一个官方机器人的完整生命周期：取 Token → 机器人资料 → 分享链接（真实 QQ 号）→
// WebSocket 或 Webhook 收事件 → 解析成中性事件 → 发送时自动补全被动回复。
//
// Bot 只做协议层的事，不决定上层如何呈现：消息与通知以 event::MessageEvent /
// event::Notice 交给 Handlers，发送接受官方消息体（content / markdown / media …）。

#include "api.h"
#include "event.h"
#include "log.h"
#include "media.h"
#include "reply_state.h"
#include "session.h"
#include "webhook.h"

#include <drogon/HttpClient.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace catofficial {

class Bot : public std::enable_shared_from_this<Bot> {
public:
    using json = nlohmann::json;
    using Scene = event::Scene;

    struct Options {
        std::string appId;
        std::string appSecret;
        bool webhook = false;                    // true：事件经 Webhook 送达，不连 WebSocket
        bool subscribeGroupMemberEvents = false;
        std::string clientName = "CatOfficial";
    };

    struct Handlers {
        std::function<void(const event::MessageEvent&)> message;
        std::function<void(const event::Notice&)> notice;
        std::function<void()> online;
        std::function<void(const std::string& reason)> offline;
        Logger log;
    };

    struct Target {
        Scene scene = Scene::none;
        std::string id;   // 单聊为用户 OpenID，群为 group_openid，频道为 channel_id
    };

    /// 被动回复窗口（官方文档，2026-11-17 更新）：单聊消息 60 分钟内可回 4 次；群消息 60 分钟内
    /// 可回 5 次（机器人开发者是群主或管理员时为 50 次，这里无从得知，按 5 次算）。
    /// 群消息的 msg_id 也可用来私聊回复发送者（须是机器人好友），与群回复共用次数。
    static constexpr std::time_t kDirectReplyWindow = 60 * 60;
    static constexpr int kDirectReplyLimit = 4;
    static constexpr std::time_t kGroupReplyWindow = 60 * 60;
    static constexpr int kGroupReplyLimit = 5;

    static std::shared_ptr<Bot> create(Options options, Handlers handlers) {
        return std::shared_ptr<Bot>(new Bot(std::move(options), std::move(handlers)));
    }

    void start() {
        stopping_ = false;
        session()->start();
    }
    void stop() {
        stopping_ = true;
        online_ = false;
        if (auto s = currentSession()) s->stop();
    }

    bool online() const { return online_; }
    const Options& options() const { return options_; }
    std::string accessToken() const { auto s = currentSession(); return s ? s->accessToken() : std::string(); }

    /// READY 给出的机器人用户 ID（用于识别 @本机器人）。
    std::string selfId() const { std::lock_guard lock(mu_); return selfId_; }
    std::string username() const { std::lock_guard lock(mu_); return username_; }
    std::string avatar() const { std::lock_guard lock(mu_); return avatar_; }
    /// 分享链接里的机器人真实 QQ 号；平台没给时为空。
    std::string robotUin() const { std::lock_guard lock(mu_); return robotUin_; }

    /// 调用任意开放平台接口，结果见 api::checkedResponse。
    void call(drogon::HttpMethod method, const std::string& path, const json& body, api::Callback cb, double timeout = 0.0) {
        if (accessToken().empty()) { cb(api::result(false, 0, "尚未取得 AccessToken")); return; }
        api::call(method, path, body, accessToken(), std::move(cb), timeout, logger(""));
    }

    webhook::Reply acceptWebhook(const std::string& timestamp, const std::string& signature, const std::string& raw) {
        auto weak = weak_from_this();
        auto reply = webhook::handle(options_.appSecret, timestamp, signature, raw,
            [weak](const std::string& type, const json& d, const std::string& eventId) {
                if (auto self = weak.lock()) self->dispatch(type, d, eventId);
            });
        if (!reply.error.empty()) say(LogLevel::warn, reply.error);
        return reply;
    }

    static std::string messagesPath(const Target& target) {
        if (target.scene == Scene::direct) return "/v2/users/" + target.id + "/messages";
        if (target.scene == Scene::group) return "/v2/groups/" + target.id + "/messages";
        return "/channels/" + target.id + "/messages";
    }

    /// 发送一条消息。body 是官方消息体（content、msg_type、markdown、media、keyboard…）。
    /// replyTo 指定被动回复的消息 ID；为空时自动选用该会话里仍可回复的最近一条消息（私聊时
    /// 也可以是对方最近在群里发的消息），没有时用 5 分钟内的事件 ID，都没有就作为主动消息发送。
    void send(const Target& target, json body, const std::string& replyTo, api::Callback cb) {
        if (target.id.empty() || target.scene == Scene::none) { cb(api::result(false, 0, "发送目标无效")); return; }
        if (!body.contains("msg_type")) body["msg_type"] = body.contains("markdown") ? 2 : body.contains("media") ? 7 : 0;
        std::string msgId = replyTo;
        bool viaGroup = false;
        if (msgId.empty()) msgId = latestReplyable(target, viaGroup);
        else countReply(msgId);
        if (viaGroup) {
            // 用群消息私聊回复要求对方是机器人好友；不是好友会被拒，这时改按主动消息再发一次。
            auto retry = body;
            auto self = shared_from_this();
            body["msg_id"] = msgId;
            body["msg_seq"] = passive_.nextSeq(msgId);
            call(drogon::Post, messagesPath(target), body, [self, target, retry, cb = std::move(cb)](json result) mutable {
                if (result.value("ok", false)) { cb(std::move(result)); return; }
                self->call(drogon::Post, messagesPath(target), retry, std::move(cb));
            });
            return;
        }
        if (!msgId.empty()) {
            body["msg_id"] = msgId;
            if (target.scene != Scene::channel) body["msg_seq"] = passive_.nextSeq(msgId);
        } else if (target.scene != Scene::channel) {
            const auto eventId = passive_.takeEvent(conversationKey(target));
            if (!eventId.empty()) body["event_id"] = eventId;
        }
        call(drogon::Post, messagesPath(target), body, std::move(cb));
    }

    void sendText(const Target& target, const std::string& text, const std::string& replyTo, api::Callback cb) {
        send(target, json{{"content", text}, {"msg_type", 0}}, replyTo, std::move(cb));
    }

    /// 发送已上传的富媒体。caption 只有图片能与之同条显示；空 content 官方会拒，纯空格会
    /// 显示成「0」，所以没有文字时用零宽空格占位。
    void sendMedia(const Target& target, const std::string& fileInfo, const std::string& caption,
                   const std::string& replyTo, const json& reference, api::Callback cb) {
        json body{{"content", caption.empty() ? std::string("\xE2\x80\x8B") : caption}, {"msg_type", 7},
                  {"media", {{"file_info", fileInfo}}}};
        if (!reference.is_null() && !reference.empty()) body["message_reference"] = reference;
        send(target, std::move(body), replyTo, std::move(cb));
    }

    void uploadUrl(const Target& target, int fileType, const std::string& url, media::Done done) {
        if (target.scene != Scene::direct && target.scene != Scene::group) { done(media::Upload{{}, "频道不支持富媒体上传", {}}); return; }
        media::uploadUrl(tokenSource(), target.scene == Scene::direct, target.id, fileType, url, std::move(done), logger(""));
    }
    void uploadBytes(const Target& target, int fileType, const std::string& fileName, std::string data, media::Done done) {
        if (target.scene != Scene::direct && target.scene != Scene::group) { done(media::Upload{{}, "频道不支持富媒体上传", {}}); return; }
        media::uploadBytes(tokenSource(), target.scene == Scene::direct, target.id, fileType, fileName, std::move(data), std::move(done), logger(""));
    }

    /// 测试与回放用：直接注入一个原始事件。
    void dispatch(const std::string& type, const json& d, const std::string& eventId) {
        if (!d.is_object()) return;
        const auto self = selfId();
        if (auto message = event::parseMessage(type, d, eventId, self, options_.appId)) {
            if (!message->dedupKey.empty() && !dedup_.acceptOnce(message->dedupKey)) return;
            rememberMessage(*message);
            if (handlers_.message) handlers_.message(*message);
            return;
        }
        auto notice = event::parseNotice(type, d, eventId, self, options_.appId);
        if (!notice.dedupKey.empty() && !dedup_.acceptOnce(notice.dedupKey)) return;
        if (!notice.replyTarget.empty()) passive_.rememberEvent(conversationKey({notice.replyScene, notice.replyTarget}), eventId);
        if (handlers_.notice) handlers_.notice(notice);
    }

    /// 让测试在没有网络的情况下设定机器人身份。
    void setIdentityForTest(std::string selfId, std::string robotUin) {
        std::lock_guard lock(mu_);
        selfId_ = std::move(selfId); robotUin_ = std::move(robotUin);
    }

private:
    Bot(Options options, Handlers handlers) : options_(std::move(options)), handlers_(std::move(handlers)) {}

    static std::string conversationKey(const Target& target) {
        return std::to_string(static_cast<int>(target.scene)) + ":" + target.id;
    }

    struct Replyable { std::string messageId; std::time_t at = 0; bool group = false; };
    struct Usage { int used = 0; std::time_t at = 0; };

    void rememberMessage(const event::MessageEvent& message) {
        if (message.scene != Scene::direct && message.scene != Scene::group) return;
        if (message.id.empty() || message.id == message.eventId) return;
        const auto now = std::time(nullptr);
        const Replyable r{message.id, now, message.scene == Scene::group};
        std::lock_guard lock(replyMu_);
        latest_[conversationKey({message.scene, message.targetId})] = r;
        if (r.group && !message.senderId.empty()) groupSenders_[message.senderId] = r;
        uses_.try_emplace(message.id, Usage{0, now});
        if (now - usesSweptAt_ >= 600) {
            for (auto it = uses_.begin(); it != uses_.end(); ) if (now - it->second.at > kGroupReplyWindow) it = uses_.erase(it); else ++it;
            for (auto it = groupSenders_.begin(); it != groupSenders_.end(); ) if (now - it->second.at > kGroupReplyWindow) it = groupSenders_.erase(it); else ++it;
            usesSweptAt_ = now;
        }
    }
    void countReply(const std::string& messageId) {
        std::lock_guard lock(replyMu_);
        auto it = uses_.find(messageId);
        if (it != uses_.end()) ++it->second.used;
    }
    /// 选一条还能被动回复的消息并记一次使用；viaGroup 表示私聊时借用了对方的群消息。
    std::string latestReplyable(const Target& target, bool& viaGroup) {
        viaGroup = false;
        if (target.scene != Scene::direct && target.scene != Scene::group) return {};
        std::lock_guard lock(replyMu_);
        const auto now = std::time(nullptr);
        auto usable = [&](const Replyable& r) {
            const auto window = r.group ? kGroupReplyWindow : kDirectReplyWindow;
            const auto limit = r.group ? kGroupReplyLimit : kDirectReplyLimit;
            if (now - r.at > window) return false;
            auto& u = uses_[r.messageId];
            if (u.at == 0) u.at = r.at;
            if (u.used >= limit) return false;
            ++u.used;
            return true;
        };
        if (auto it = latest_.find(conversationKey(target)); it != latest_.end() && usable(it->second)) return it->second.messageId;
        if (target.scene == Scene::direct)
            if (auto it = groupSenders_.find(target.id); it != groupSenders_.end() && usable(it->second)) {
                viaGroup = true;
                return it->second.messageId;
            }
        return {};
    }

    void say(LogLevel level, const std::string& message) const { if (handlers_.log) handlers_.log(level, message); }
    Logger logger(const std::string& prefix) const {
        auto log = handlers_.log;
        return [log, prefix](LogLevel level, const std::string& message) { if (log) log(level, prefix + message); };
    }
    media::TokenSource tokenSource() {
        return [weak = weak_from_this()] { auto self = weak.lock(); return self ? self->accessToken() : std::string(); };
    }

    std::shared_ptr<Session> currentSession() const { std::lock_guard lock(sessionMu_); return session_; }
    std::shared_ptr<Session> session() {
        std::lock_guard lock(sessionMu_);
        if (session_) return session_;
        std::weak_ptr<Bot> weak = weak_from_this();
        Session::Hooks hooks;
        hooks.token = [weak](const drogon::HttpClientPtr& client) {
            auto self = weak.lock(); if (!self || self->stopping_) return;
            if (!self->online_) self->fetchProfile(client);
        };
        hooks.ready = [weak](const json& d) {
            auto self = weak.lock(); if (!self) return;
            const auto user = protocol::object(d, "user");
            {
                std::lock_guard lock(self->mu_);
                const auto id = protocol::text(user, "id");
                if (!id.empty()) self->selfId_ = id;
                const auto name = protocol::text(user, "username");
                if (!name.empty()) self->username_ = name;
            }
            self->goOnline();
        };
        hooks.resumed = [weak] { if (auto self = weak.lock()) self->goOnline(); };
        hooks.dispatch = [weak](const std::string& type, const json& d, const std::string& eventId) {
            if (auto self = weak.lock()) self->dispatch(type, d, eventId);
        };
        hooks.closed = [weak] { if (auto self = weak.lock()) self->goOffline("WebSocket 连接已断开"); };
        hooks.reconnecting = [weak] { if (auto self = weak.lock()) self->say(LogLevel::info, "正在重连 WebSocket"); };
        hooks.failed = [weak](const std::string& error) {
            if (auto self = weak.lock()) { self->say(LogLevel::error, error); self->goOffline(error); }
        };
        hooks.log = handlers_.log;
        Session::Options so;
        so.appId = options_.appId; so.appSecret = options_.appSecret;
        so.subscribeGroupMemberEvents = options_.subscribeGroupMemberEvents;
        so.clientName = options_.clientName;
        session_ = std::make_shared<Session>(std::move(so), std::move(hooks));
        return session_;
    }

    void fetchProfile(const drogon::HttpClientPtr& client) {
        auto weak = weak_from_this();
        api::fetchProfile(client, accessToken(), [weak, client](std::optional<api::BotProfile> profile, std::string error) {
            auto self = weak.lock(); if (!self || self->stopping_) return;
            if (profile) {
                std::lock_guard lock(self->mu_);
                if (!profile->id.empty()) self->selfId_ = profile->id;
                self->username_ = profile->username;
                self->avatar_ = profile->avatar;
                const auto uin = api::robotUin(profile->shareUrl);
                if (!uin.empty()) self->robotUin_ = uin;
            } else {
                self->say(LogLevel::warn, error.empty() ? "获取机器人资料失败，将尝试生成分享链接" : "解析机器人资料失败：" + error);
            }
            if (self->robotUin().empty()) self->fetchShareUrl(client);
            else self->connect();
        });
    }
    void fetchShareUrl(const drogon::HttpClientPtr& client) {
        auto weak = weak_from_this();
        api::generateShareUrl(client, accessToken(), "catofficial", [weak](std::string url, std::string error) {
            auto self = weak.lock(); if (!self || self->stopping_) return;
            const auto uin = api::robotUin(url);
            if (!uin.empty()) { std::lock_guard lock(self->mu_); self->robotUin_ = uin; }
            else self->say(LogLevel::warn, error.empty() ? "生成机器人分享链接失败，机器人 QQ 号未知" : "解析机器人分享链接失败：" + error);
            self->connect();
        });
    }
    void connect() {
        if (stopping_ || online_) return;
        if (options_.webhook) { goOnline(); return; }
        session()->openGateway();
    }
    void goOnline() {
        if (online_.exchange(true)) return;
        if (handlers_.online) handlers_.online();
    }
    void goOffline(const std::string& reason) {
        if (!online_.exchange(false)) return;
        if (handlers_.offline) handlers_.offline(reason);
    }

    Options options_;
    Handlers handlers_;
    mutable std::mutex mu_, sessionMu_, replyMu_;
    std::shared_ptr<Session> session_;
    std::string selfId_, username_, avatar_, robotUin_;
    std::atomic<bool> stopping_{false}, online_{false};
    PassiveReplies passive_;
    EventDeduplicator dedup_;
    std::unordered_map<std::string, Replyable> latest_;
    std::unordered_map<std::string, Replyable> groupSenders_;   // 用户 OpenID → 他最近的群消息
    std::unordered_map<std::string, Usage> uses_;               // 消息 ID → 已被动回复次数
    std::time_t usesSweptAt_ = 0;
};

}  // namespace catofficial
