#pragma once

// 一个官方机器人的中继：官方事件 → OneBot 事件，OneBot 动作 → 官方接口。
// 官方能力以外的动作照 OneBot 惯例返回 retcode 1404，不去模拟官方做不到的事。
// 事件经 Hooks::emit 交给 Hub，由 Hub 分发到服务这个机器人的连接。

#include "config.h"
#include "onebot.h"
#include "paths.h"
#include "store.h"

#include <catofficial/bot.h>

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace catofficial::app {

inline constexpr const char* kVersion = "0.1.0";

class Relay : public std::enable_shared_from_this<Relay> {
public:
    using json = nlohmann::json;
    using Done = std::function<void(json response)>;
    using Scene = event::Scene;

    struct Hooks {
        std::function<void(const json& event)> emit;
        /// 返回 true 表示消息已被处理（如 .bind 指令），不再推给 OneBot 应用。
        std::function<bool(Relay& relay, const event::MessageEvent& e)> intercept;
        std::function<void(bool online, const std::string& reason)> state;
    };

    Relay(BotConfig config, std::shared_ptr<Store> store, Logger log, Hooks hooks = {})
        : config_(std::move(config)), store_(std::move(store)), log_(std::move(log)), hooks_(std::move(hooks)) {}

    /// 建立 Bot（需在 drogon 事件循环启动后调用 start）。
    void init() {
        std::weak_ptr<Relay> weak = weak_from_this();
        Bot::Options options;
        options.appId = config_.appId;
        options.appSecret = config_.appSecret;
        options.webhook = config_.webhook;
        options.subscribeGroupMemberEvents = config_.subscribeGroupMemberEvents;
        Bot::Handlers handlers;
        handlers.message = [weak](const event::MessageEvent& e) { if (auto self = weak.lock()) self->onMessage(e); };
        handlers.notice = [weak](const event::Notice& n) { if (auto self = weak.lock()) self->onNotice(n); };
        handlers.online = [weak] {
            auto self = weak.lock(); if (!self) return;
            self->say(LogLevel::info, "已上线：" + self->bot_->username() + "（self_id=" + std::to_string(self->selfId()) + "）");
            if (self->hooks_.state) self->hooks_.state(true, {});
        };
        handlers.offline = [weak](const std::string& reason) {
            auto self = weak.lock(); if (!self) return;
            { std::lock_guard lock(self->errorMu_); self->lastError_ = reason; }
            self->say(LogLevel::warn, "离线：" + reason);
            if (self->hooks_.state) self->hooks_.state(false, reason);
        };
        handlers.log = [weak](LogLevel level, const std::string& message) { if (auto self = weak.lock()) self->say(level, message); };
        bot_ = Bot::create(std::move(options), std::move(handlers));
    }

    bool configured() const { return !config_.appId.empty() && !config_.appSecret.empty(); }
    void start() {
        if (!configured()) { say(LogLevel::warn, "尚未填写 AppID 与 AppSecret，请在管理面板或配置文件中填写后重启"); return; }
        bot_->start();
    }
    void stop() { bot_->stop(); }
    std::shared_ptr<Bot> bot() const { return bot_; }
    std::shared_ptr<Store> store() const { return store_; }
    const BotConfig& config() const { return config_; }
    const std::string& id() const { return config_.id; }

    /// 不需要重连的设置（名称、自动回应互动）就地更新。
    void update(const BotConfig& next) {
        std::lock_guard lock(nameMu_);
        name_ = next.name;
        autoAck_ = next.autoAckInteractions;
    }
    /// 显示名：用户起的名字；没起时用机器人昵称；再没有用 AppID。
    std::string displayName() const {
        { std::lock_guard lock(nameMu_); if (!name_.empty()) return name_; }
        const auto nick = bot_ ? bot_->username() : std::string();
        return nick.empty() ? config_.appId : nick;
    }
    uint64_t received() const { return received_.load(); }
    uint64_t sent() const { return sent_.load(); }

    /// 管理面板用的运行状态。
    json overview() {
        std::string error;
        { std::lock_guard lock(errorMu_); error = lastError_; }
        return json{{"configured", configured()}, {"online", bot_->online()}, {"self_id", selfId()}, {"nickname", bot_->username()}, {"display_name", displayName()},
                    {"avatar", bot_->avatar()}, {"app_id", config_.appId}, {"mode", config_.webhook ? "webhook" : "websocket"},
                    {"received", received_.load()}, {"sent", sent_.load()}, {"error", bot_->online() ? std::string() : error}};
    }

    /// 主动私聊（运行异常通知主人用）。
    void sendPrivateText(const std::string& openId, const std::string& text) {
        bot_->sendText(Bot::Target{Scene::direct, openId}, text, std::string(), [weak = weak_from_this()](json r) {
            auto self = weak.lock();
            if (self && !r.value("ok", false)) self->say(LogLevel::warn, "通知主人失败：" + r.value("message", std::string()));
        });
    }

    /// 被动回复一条收到的消息（.bind 等内部指令用）。
    void replyText(const event::MessageEvent& e, const std::string& text) {
        const auto scene = e.scene;
        Bot::Target target{scene, e.targetId};
        bot_->sendText(target, text, e.id == e.eventId ? std::string() : e.id, [weak = weak_from_this()](json r) {
            auto self = weak.lock();
            if (self && !r.value("ok", false)) self->say(LogLevel::warn, "回复失败：" + r.value("message", std::string()));
        });
    }
    std::string displayName(Kind kind, const std::string& openId) { return store_->name(kind, openId); }

    /// OneBot self_id：机器人的真实 QQ 号；平台没给时分配一个固定号码。
    int64_t selfId() {
        const auto uin = bot_->robotUin();
        if (!uin.empty() && uin.find_first_not_of("0123456789") == std::string::npos && uin.size() <= 18) return std::stoll(uin);
        const auto id = bot_->selfId();
        return id.empty() ? 0 : store_->number(Kind::user, "bot:" + id);
    }

    json lifecycle(const std::string& subType) {
        return json{{"time", std::time(nullptr)}, {"self_id", selfId()}, {"post_type", "meta_event"},
                    {"meta_event_type", "lifecycle"}, {"sub_type", subType}};
    }
    json heartbeat(int interval) {
        return json{{"time", std::time(nullptr)}, {"self_id", selfId()}, {"post_type", "meta_event"},
                    {"meta_event_type", "heartbeat"}, {"status", status()}, {"interval", interval}};
    }
    void emit(const json& event) { if (hooks_.emit) hooks_.emit(event); }

    /// 头像地址。官方只按 AppID + OpenID 提供用户头像。
    std::string userAvatar(int64_t number, int size = 640) {
        const auto openId = store_->openId(Kind::user, number);
        if (openId.empty() || openId.rfind("bot:", 0) == 0) return {};
        return "https://q.qlogo.cn/qqapp/" + config_.appId + "/" + openId + "/" + std::to_string(size);
    }

    static json ok(json data = nullptr) { return json{{"status", "ok"}, {"retcode", 0}, {"data", std::move(data)}, {"message", ""}, {"wording", ""}}; }
    static json failed(int retcode, const std::string& message) {
        return json{{"status", "failed"}, {"retcode", retcode}, {"data", nullptr}, {"message", message}, {"wording", message}};
    }

    /// 执行一个 OneBot 动作。done 收到不含 echo 的响应。
    void handle(const std::string& rawAction, const json& rawParams, Done done) {
        const json params = rawParams.is_object() ? rawParams : json::object();
        std::string action = rawAction;
        // _async 与 _rate_limited 后缀：照常执行。
        for (const char* suffix : {"_async", "_rate_limited"}) {
            const std::string s = suffix;
            if (action.size() > s.size() && action.compare(action.size() - s.size(), s.size(), s) == 0) action.resize(action.size() - s.size());
        }
        try {
            if (action == "send_private_msg") return sendTo(Kind::user, params.value("user_id", json()), params, std::move(done));
            if (action == "send_group_msg") return sendTo(Kind::group, params.value("group_id", json()), params, std::move(done));
            if (action == "send_msg") {
                const auto type = params.value("message_type", std::string());
                const bool group = type == "group" || (type.empty() && params.contains("group_id"));
                return sendTo(group ? Kind::group : Kind::user, params.value(group ? "group_id" : "user_id", json()), params, std::move(done));
            }
            if (action == "delete_msg") return deleteMessage(params, std::move(done));
            if (action == "get_msg") return getMessage(params, std::move(done));
            if (action == "get_login_info") return done(ok({{"user_id", selfId()}, {"nickname", bot_->username()}}));
            if (action == "get_stranger_info") return done(strangerInfo(params));
            if (action == "get_friend_list") return done(friendList());
            if (action == "get_group_info") return done(groupInfo(params));
            if (action == "get_group_list") return done(groupList());
            if (action == "get_group_member_info") return done(memberInfo(params));
            if (action == "get_group_member_list") return done(memberList(params));
            if (action == "set_group_kick") return groupKick(params, std::move(done));
            if (action == "set_group_ban") return groupBan(params, std::move(done));
            if (action == "set_group_add_request") return groupRequest(params, std::move(done));
            if (action == "get_status") return done(ok(status()));
            if (action == "get_version_info")
                return done(ok({{"app_name", "CatOfficial"}, {"app_version", kVersion}, {"protocol_version", "v11"}}));
            if (action == "can_send_image" || action == "can_send_record") return done(ok({{"yes", true}}));
            if (action == "set_identity_binding") return done(bindIdentity(params));
            if (action == "get_official_id") return done(officialId(params));
            if (action == "call_official_api") return callOfficial(params, std::move(done));
            done(failed(1404, "QQ 官方机器人不支持此动作：" + rawAction));
        } catch (const std::exception& e) {
            done(failed(1400, std::string("参数无效：") + e.what()));
        }
    }

    /// HTTP POST 上报的快速操作（目前只支持 reply）。
    void quickOperation(const json& event, const json& op) {
        if (!op.is_object() || event.value("post_type", std::string()) != "message" || !op.contains("reply")) return;
        const bool group = event.value("message_type", std::string()) == "group";
        json message = onebot::normalize(op["reply"], op.value("auto_escape", false));
        if (message.empty()) return;
        json segments = json::array();
        segments.push_back(json{{"type", "reply"}, {"data", {{"id", std::to_string(event.value("message_id", int64_t(0)))}}}});
        if (group && op.value("at_sender", true)) {
            segments.push_back(json{{"type", "at"}, {"data", {{"qq", std::to_string(event.value("user_id", int64_t(0)))}}}});
            segments.push_back(onebot::text(" "));
        }
        for (auto& seg : message) segments.push_back(seg);
        json params{{"message", segments}};
        sendTo(group ? Kind::group : Kind::user, event.value(group ? "group_id" : "user_id", json()), params, [this](json r) {
            if (r.value("status", std::string()) != "ok") say(LogLevel::warn, "快速回复失败：" + r.value("message", std::string()));
        });
    }

    webhook::Reply acceptWebhook(const std::string& timestamp, const std::string& signature, const std::string& raw) {
        return bot_->acceptWebhook(timestamp, signature, raw);
    }

private:
    struct Peer { Scene scene = Scene::none; std::string id; };

    void say(LogLevel level, const std::string& message) const { if (log_) log_(level, "官方机器人「" + displayName() + "」：" + message); }

    json status() const { return json{{"online", bot_->online()}, {"good", bot_->online()}}; }

    // ── 收到的事件 ──────────────────────────────────────────

    onebot::Resolver resolver() {
        onebot::Resolver r;
        r.user = [this](const std::string& openId) { return store_->number(Kind::user, openId); };
        r.quoted = [this](const event::MessageEvent& e) -> std::optional<int64_t> {
            if (auto m = store_->messageByRefIdx(e.quotedIndex)) return m->id;
            if (auto m = store_->messageByOfficialId(e.quotedMessageId)) return m->id;
            if (e.quotedIndex.empty() || !e.quoted.is_object() || e.quoted.empty()) return std::nullopt;
            const std::string peer = e.scene == Scene::channel ? "channel:" + e.targetId : e.targetId;
            // 同一条消息在不同时刻给出的引用索引并不相同（发送响应的 ref_idx 与之后被引用时的
            // ref_msg_idx 只有后半段一致），所以对不上时按内容在同一会话最近的消息里找。
            const auto quotedText = protocol::text(e.quoted, "content");
            if (!quotedText.empty()) {
                for (const auto& m : store_->recentMessages(peer, 200)) {
                    if (m.officialId == e.id || onebot::plainText(m.message) != quotedText) continue;
                    store_->setRefIdx(m.id, e.quotedIndex);
                    return m.id;
                }
            }
            // 没见过的消息（CatOfficial 启动前的、或已清理的）：用事件带来的内容补一条索引，
            // 让 reply 段与 get_msg 照样能用。发送者未知。
            Store::Message m;
            m.refIdx = e.quotedIndex;
            m.scene = static_cast<int>(e.scene);
            m.peer = peer;
            m.time = e.time;
            m.message = json::array();
            if (!quotedText.empty()) m.message.push_back(onebot::text(quotedText));
            if (e.quoted.contains("attachments") && e.quoted["attachments"].is_array())
                for (const auto& a : e.quoted["attachments"]) { auto seg = onebot::attachmentSegment(a); if (!seg.is_null()) m.message.push_back(seg); }
            return store_->rememberMessage(m);
        };
        return r;
    }

    void onMessage(const event::MessageEvent& e) {
        ++received_;
        say(LogLevel::debug, "官方原始消息 " + e.type + "：" + e.data.dump());
        const bool channel = e.scene == Scene::channel;
        const bool group = e.scene == Scene::group || channel;
        const std::string peerOpenId = channel ? "channel:" + e.targetId : e.targetId;
        const auto userId = store_->number(Kind::user, e.senderId);
        const auto username = protocol::text(e.author, "username");
        store_->rememberName(Kind::user, e.senderId, username);
        if (group) {
            store_->rememberMember(peerOpenId, e.senderId, e.role);
            if (!e.groupName.empty()) store_->rememberName(Kind::group, peerOpenId, e.groupName);
        } else {
            store_->markDirect(e.senderId);
        }
        // 被 @ 的其他成员也记下名字，get_group_member_info 用得上。
        for (const auto& m : e.mentions) if (!m.self && !m.id.empty()) {
            store_->rememberName(Kind::user, m.id, protocol::text(m.raw, "username"));
            if (group) store_->rememberMember(peerOpenId, m.id, "");
        }
        const auto self = selfId();
        auto segments = onebot::incomingSegments(e, self, resolver());
        Store::Message record;
        record.officialId = e.id;
        record.refIdx = e.messageIndex;
        record.scene = static_cast<int>(channel ? Scene::channel : e.scene);
        record.peer = peerOpenId;
        record.sender = e.senderId;
        record.time = e.time;
        record.message = segments;
        const auto messageId = store_->rememberMessage(record);
        if (hooks_.intercept && hooks_.intercept(*this, e)) return;
        const auto raw = onebot::toCq(segments);
        json sender{{"user_id", userId}, {"nickname", nickname(e.senderId)}, {"sex", "unknown"}, {"age", 0}};
        const auto avatar = userAvatar(userId);
        if (!avatar.empty()) sender["avatar"] = avatar;
        json event{{"time", e.time}, {"self_id", self}, {"post_type", "message"}, {"message_id", messageId},
                   {"user_id", userId}, {"message", segments},
                   {"raw_message", raw}, {"font", 0}};
        if (group) {
            const auto groupId = store_->number(Kind::group, peerOpenId);
            sender["card"] = ""; sender["role"] = e.role.empty() ? "member" : e.role; sender["title"] = "";
            event["message_type"] = "group"; event["sub_type"] = "normal";
            event["group_id"] = groupId; event["anonymous"] = nullptr;
            say(LogLevel::info, "收 群 " + std::to_string(groupId) + " " + nickname(e.senderId) + "(" + std::to_string(userId) + ")：" + onebot::plainText(segments));
        } else {
            event["message_type"] = "private"; event["sub_type"] = "friend"; event["target_id"] = self;
            say(LogLevel::info, "收 私聊 " + nickname(e.senderId) + "(" + std::to_string(userId) + ")：" + onebot::plainText(segments));
        }
        event["sender"] = sender;
        emit(event);
    }

    std::string nickname(const std::string& openId) {
        auto name = store_->name(Kind::user, openId);
        return name;
    }

    json noticeBase(const event::Notice& n) {
        return json{{"time", n.time}, {"self_id", selfId()}, {"post_type", "notice"}};
    }

    void onNotice(const event::Notice& n) {
        const auto self = selfId();
        const auto group = n.groupId.empty() ? 0 : store_->number(Kind::group, n.groupId);
        auto user = [&](const std::string& openId) -> int64_t { return openId.empty() ? 0 : store_->number(Kind::user, openId); };
        if (!n.groupId.empty() && !n.groupName.empty()) store_->rememberName(Kind::group, n.groupId, n.groupName);
        if (!n.userId.empty() && n.userId != bot_->selfId()) store_->rememberName(Kind::user, n.userId, n.username);

        json ev = noticeBase(n);
        const auto& t = n.type;
        if (t == "GROUP_ADD_ROBOT" || t == "GROUP_DEL_ROBOT") {
            ev["notice_type"] = t == "GROUP_ADD_ROBOT" ? "group_increase" : "group_decrease";
            ev["sub_type"] = t == "GROUP_ADD_ROBOT" ? "invite" : "kick_me";
            ev["group_id"] = group; ev["user_id"] = self; ev["operator_id"] = user(n.operatorId);
            if (t == "GROUP_DEL_ROBOT") store_->forgetGroupMembers(n.groupId);
        } else if (t == "GROUP_MEMBER_ADD" || t == "GROUP_MEMBER_REMOVE") {
            ev["notice_type"] = t == "GROUP_MEMBER_ADD" ? "group_increase" : "group_decrease";
            ev["sub_type"] = t == "GROUP_MEMBER_ADD" ? "approve" : "leave";
            ev["group_id"] = group; ev["user_id"] = user(n.userId); ev["operator_id"] = user(n.operatorId.empty() ? n.userId : n.operatorId);
            if (t == "GROUP_MEMBER_ADD") store_->rememberMember(n.groupId, n.userId, "member");
            else store_->forgetMember(n.groupId, n.userId);
        } else if (t == "FRIEND_ADD") {
            store_->markDirect(n.userId);
            ev["notice_type"] = "friend_add"; ev["user_id"] = user(n.userId);
        } else if (t == "GROUP_JOIN_REQUEST") {
            ev = json{{"time", n.time}, {"self_id", self}, {"post_type", "request"}, {"request_type", "group"},
                      {"sub_type", "add"}, {"group_id", group}, {"user_id", user(n.userId)}, {"comment", n.comment},
                      {"flag", n.groupId + "|" + n.userId + "|" + n.joinRequestId}};
            if (!n.operatorId.empty()) ev["invitor_id"] = user(n.operatorId);
        } else {
            // 没有 OneBot 对应的官方事件（主动消息开关、好友删除、互动、订阅状态、频道事件）
            // 以扩展通知送出，原始数据放在 data。
            ev["notice_type"] = "qq_official";
            ev["sub_type"] = lower(t);
            if (group) ev["group_id"] = group;
            if (!n.userId.empty()) ev["user_id"] = user(n.userId);
            if (!n.operatorId.empty()) ev["operator_id"] = user(n.operatorId);
            ev["data"] = n.data;
            if (t == "INTERACTION_CREATE") {
                ev["interaction_id"] = n.interactionId;
                ev["interaction_type"] = n.interactionType;
                ev["button_data"] = protocol::text(protocol::object(n.interactionData, "resolved"), "button_data");
                bool ack;
                { std::lock_guard lock(nameMu_); ack = autoAck_; }
                if (ack && !n.interactionId.empty())
                    bot_->call(drogon::Put, "/interactions/" + n.interactionId, json{{"code", 0}}, [](json) {});
            }
        }
        emit(ev);
    }

    static std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // ── 发送 ────────────────────────────────────────────────

    std::optional<Peer> peer(Kind kind, const json& idValue) {
        const auto n = onebot::number(idValue);
        if (!n) return std::nullopt;
        const auto openId = store_->openId(kind, *n);
        if (openId.empty()) return std::nullopt;
        if (kind == Kind::group && openId.rfind("channel:", 0) == 0) return Peer{Scene::channel, openId.substr(8)};
        return Peer{kind == Kind::group ? Scene::group : Scene::direct, openId};
    }

    onebot::OutResolver outResolver() {
        onebot::OutResolver r;
        r.user = [this](int64_t n) { return store_->openId(Kind::user, n); };
        return r;
    }

    struct SendJob {
        Bot::Target target;
        std::string peerOpenId;
        std::string replyTo;          // 被动回复用的官方消息 ID
        json reference;               // message_reference
        onebot::OutPlan plan;
        json segments;
        bool mix = false;
        std::size_t next = 0;
        int64_t firstId = 0;
        Done done;
    };

    void sendTo(Kind kind, const json& idValue, const json& params, Done done) {
        const auto p = peer(kind, idValue);
        if (!p) { done(failed(1400, kind == Kind::group ? "未知的 group_id（机器人还没在这个群收到过消息）" : "未知的 user_id（机器人还没收到过这个用户的消息）")); return; }
        auto segments = onebot::normalize(params.value("message", json()), params.value("auto_escape", false));
        auto job = std::make_shared<SendJob>();
        job->target = Bot::Target{p->scene, p->id};
        job->peerOpenId = p->scene == Scene::channel ? "channel:" + p->id : p->id;
        job->plan = onebot::plan(segments, p->scene != Scene::direct, outResolver());
        job->segments = segments;
        job->done = std::move(done);
        resolveReply(*job);
        if (!job->plan.skipped.empty()) {
            std::string list;
            for (const auto& s : job->plan.skipped) list += (list.empty() ? "" : "、") + s;
            say(LogLevel::debug, "官方不支持的消息段已略过：" + list);
        }
        if (job->plan.empty()) { job->done(failed(1400, "消息为空，或只包含官方不支持的消息段")); return; }
        job->mix = job->plan.markdown.is_null() && job->plan.media.size() == 1
            && job->plan.media[0].fileType == media::kImage && !job->plan.text.empty();
        const auto where = (p->scene == Scene::direct ? "私聊 " : "群 ") + onebot::dataText(idValue);
        say(LogLevel::info, "发 " + where + "：" + onebot::plainText(segments));
        if (!job->mix && (!job->plan.text.empty() || !job->plan.markdown.is_null())) {
            json body;
            if (!job->plan.markdown.is_null()) body = json{{"markdown", job->plan.markdown}, {"msg_type", 2}};
            else body = json{{"content", job->plan.text}, {"msg_type", 0}};
            if (!job->plan.keyboard.is_null()) body["keyboard"] = job->plan.keyboard;
            if (!job->reference.is_null()) body["message_reference"] = job->reference;
            auto self = shared_from_this();
            bot_->send(job->target, body, job->replyTo, [self, job](json result) {
                if (!result.value("ok", false)) { job->done(failed(100, result.value("message", std::string("发送失败")))); return; }
                self->recordSent(*job, result);
                self->sendNextMedia(job);
            });
            return;
        }
        sendNextMedia(job);
    }

    /// reply 段：以引用形式展示（message_reference 填被引用消息的 REFIDX），并且如果被引用的
    /// 是对方发来、仍在被动回复期限内的消息，就直接回复它。
    void resolveReply(SendJob& job) {
        if (!job.plan.replyId) return;
        const auto m = store_->messageById(job.plan.replyId);
        if (!m) return;
        if (!m->refIdx.empty()) job.reference = json{{"message_id", m->refIdx}};
        const bool direct = job.target.scene == Scene::direct;
        const auto window = direct ? Bot::kDirectReplyWindow : Bot::kGroupReplyWindow;
        if (!m->self && !m->officialId.empty() && m->peer == job.peerOpenId && std::time(nullptr) - m->time < window)
            job.replyTo = m->officialId;
    }

    void recordSent(SendJob& job, const json& result) {
        ++sent_;
        say(LogLevel::debug, "官方发送响应：" + result.value("data", json::object()).dump());
        Store::Message m;
        const auto data = protocol::object(result, "data");
        m.officialId = protocol::text(data, "id");
        m.refIdx = protocol::text(protocol::object(data, "ext_info"), "ref_idx");
        m.scene = static_cast<int>(job.target.scene);
        m.peer = job.peerOpenId;
        m.time = std::time(nullptr);
        m.self = true;
        m.message = onebot::withoutInlineData(job.segments);
        const auto id = store_->rememberMessage(m);
        if (!job.firstId) job.firstId = id;
        // 机器人自己发出的消息，供开启了「上报自身消息」的连接使用。
        const bool group = job.target.scene != Scene::direct;
        const auto self = selfId();
        json sent{{"time", m.time}, {"self_id", self}, {"post_type", "message_sent"}, {"message_id", id}, {"user_id", self},
                  {"message", job.segments}, {"raw_message", onebot::toCq(job.segments)}, {"font", 0},
                  {"sender", {{"user_id", self}, {"nickname", bot_->username()}}}};
        if (group) { sent["message_type"] = "group"; sent["sub_type"] = "normal"; sent["group_id"] = store_->number(Kind::group, job.peerOpenId); }
        else { sent["message_type"] = "private"; sent["sub_type"] = "friend"; sent["target_id"] = store_->number(Kind::user, job.peerOpenId); }
        emit(sent);
    }

    void finish(const std::shared_ptr<SendJob>& job) {
        job->done(ok({{"message_id", job->firstId}}));
    }

    void sendNextMedia(const std::shared_ptr<SendJob>& job) {
        if (job->next >= job->plan.media.size()) { finish(job); return; }
        const auto item = job->plan.media[job->next++];
        auto self = shared_from_this();
        auto uploaded = [self, job, item](media::Upload upload) {
            if (upload.fileInfo.empty()) {
                const auto reason = upload.reason.empty() ? upload.error : upload.reason;
                job->done(failed(100, "上传" + std::string(item.fileType == media::kImage ? "图片" : "媒体") + "失败：" + reason));
                return;
            }
            const auto caption = job->mix ? job->plan.text : std::string();
            self->bot_->sendMedia(job->target, upload.fileInfo, caption, job->replyTo, job->reference, [self, job](json result) {
                if (!result.value("ok", false)) { job->done(failed(100, result.value("message", std::string("发送失败")))); return; }
                self->recordSent(*job, result);
                self->sendNextMedia(job);
            });
        };
        if (job->target.scene == Scene::channel) { job->done(failed(100, "频道不支持富媒体")); return; }
        if (item.source == onebot::OutMedia::Source::url) { bot_->uploadUrl(job->target, item.fileType, item.ref, std::move(uploaded)); return; }
        std::string bytes, name = item.name;
        if (item.source == onebot::OutMedia::Source::base64) bytes = drogon::utils::base64Decode(item.ref);
        else {
            const auto path = fromUtf8(item.ref);
            std::ifstream in(path, std::ios::binary);
            if (!in) { job->done(failed(100, "读取本地文件失败：" + item.ref)); return; }
            std::ostringstream ss; ss << in.rdbuf(); bytes = ss.str();
            if (name.empty()) name = utf8(path.filename());
        }
        if (bytes.empty()) { job->done(failed(100, "媒体内容为空")); return; }
        if (bytes.size() > media::kHardLimitBytes) { job->done(failed(100, "媒体超过官方 200MB 上限")); return; }
        if (name.empty()) name = item.fileType == media::kImage ? "image.png" : item.fileType == media::kVideo ? "video.mp4" : item.fileType == media::kVoice ? "voice.silk" : "file.bin";
        bot_->uploadBytes(job->target, item.fileType, name, std::move(bytes), std::move(uploaded));
    }

    // ── 其他动作 ────────────────────────────────────────────

    void deleteMessage(const json& params, Done done) {
        const auto id = onebot::number(params.value("message_id", json()));
        auto m = id ? store_->messageById(*id) : std::nullopt;
        if (!m || m->officialId.empty()) { done(failed(1400, "未知的 message_id")); return; }
        if (m->scene == static_cast<int>(Scene::channel)) { done(failed(1404, "频道消息暂不支持撤回")); return; }
        const std::string base = m->scene == static_cast<int>(Scene::direct) ? "/v2/users/" + m->peer : "/v2/groups/" + m->peer;
        bot_->call(drogon::Delete, base + "/messages/" + m->officialId, json::object(), [done](json result) {
            done(result.value("ok", false) ? ok() : failed(100, result.value("message", std::string("撤回失败"))));
        });
    }

    void getMessage(const json& params, Done done) {
        const auto id = onebot::number(params.value("message_id", json()));
        auto m = id ? store_->messageById(*id) : std::nullopt;
        if (!m) { done(failed(1400, "未知的 message_id")); return; }
        const bool group = m->scene != static_cast<int>(Scene::direct);
        const auto sender = m->self ? selfId() : store_->number(Kind::user, m->sender);
        json data{{"time", m->time}, {"message_type", group ? "group" : "private"}, {"message_id", m->id}, {"real_id", m->id},
                  {"sender", {{"user_id", sender}, {"nickname", m->self ? bot_->username() : nickname(m->sender)}}},
                  {"message", m->message},
                  {"raw_message", onebot::toCq(m->message)}};
        if (group) data["group_id"] = store_->number(Kind::group, m->peer);
        done(ok(data));
    }

    json strangerInfo(const json& params) {
        const auto n = onebot::number(params.value("user_id", json()));
        if (!n) return failed(1400, "缺少 user_id");
        const auto openId = store_->openId(Kind::user, *n);
        if (openId.empty()) return failed(1400, "未知的 user_id");
        json data{{"user_id", *n}, {"nickname", nickname(openId)}, {"sex", "unknown"}, {"age", 0}};
        const auto avatar = userAvatar(*n);
        if (!avatar.empty()) data["avatar"] = avatar;
        return ok(data);
    }

    json friendList() {
        json list = json::array();
        for (const auto& openId : store_->directUsers())
            list.push_back({{"user_id", store_->number(Kind::user, openId)}, {"nickname", nickname(openId)}, {"remark", ""}});
        return ok(list);
    }

    json groupJson(const std::string& openId) {
        return json{{"group_id", store_->number(Kind::group, openId)}, {"group_name", store_->name(Kind::group, openId)},
                    {"member_count", static_cast<int64_t>(store_->members(openId).size())}, {"max_member_count", 0}};
    }
    json groupInfo(const json& params) {
        const auto n = onebot::number(params.value("group_id", json()));
        const auto openId = n ? store_->openId(Kind::group, *n) : std::string();
        if (openId.empty()) return failed(1400, "未知的 group_id");
        return ok(groupJson(openId));
    }
    json groupList() {
        json list = json::array();
        for (const auto& openId : store_->groups()) list.push_back(groupJson(openId));
        return ok(list);
    }

    json memberJson(const std::string& groupOpenId, const Store::Member& m) {
        const auto userId = store_->number(Kind::user, m.userOpenId);
        json data{{"group_id", store_->number(Kind::group, groupOpenId)}, {"user_id", userId},
                  {"nickname", nickname(m.userOpenId)}, {"card", m.card}, {"sex", "unknown"}, {"age", 0},
                  {"area", ""}, {"join_time", 0}, {"last_sent_time", m.lastSeen}, {"level", ""},
                  {"role", m.role.empty() ? "member" : m.role}, {"unfriendly", false}, {"title", ""},
                  {"title_expire_time", 0}, {"card_changeable", false}};
        const auto avatar = userAvatar(userId);
        if (!avatar.empty()) data["avatar"] = avatar;
        return data;
    }
    json memberInfo(const json& params) {
        const auto g = onebot::number(params.value("group_id", json()));
        const auto u = onebot::number(params.value("user_id", json()));
        const auto groupOpenId = g ? store_->openId(Kind::group, *g) : std::string();
        const auto userOpenId = u ? store_->openId(Kind::user, *u) : std::string();
        if (groupOpenId.empty() || userOpenId.empty()) return failed(1400, "未知的 group_id 或 user_id");
        auto m = store_->member(groupOpenId, userOpenId);
        if (!m) return failed(100, "机器人还没在这个群见过该成员");
        return ok(memberJson(groupOpenId, *m));
    }
    json memberList(const json& params) {
        const auto g = onebot::number(params.value("group_id", json()));
        const auto groupOpenId = g ? store_->openId(Kind::group, *g) : std::string();
        if (groupOpenId.empty()) return failed(1400, "未知的 group_id");
        json list = json::array();
        for (const auto& m : store_->members(groupOpenId)) list.push_back(memberJson(groupOpenId, m));
        return ok(list);
    }

    void groupKick(const json& params, Done done) {
        const auto g = peer(Kind::group, params.value("group_id", json()));
        const auto u = peer(Kind::user, params.value("user_id", json()));
        if (!g || !u || g->scene != Scene::group) { done(failed(1400, "未知的 group_id 或 user_id")); return; }
        bot_->call(drogon::Post, "/v2/groups/" + g->id + "/batch_remove_members", json{{"member_openids", json::array({u->id})}},
            [done](json r) { done(r.value("ok", false) ? ok() : failed(100, r.value("message", std::string("移出成员失败")))); });
    }

    static std::string rfc3339After(int64_t seconds) {
        const std::time_t at = std::time(nullptr) + seconds;
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &at);
#else
        gmtime_r(&at, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }
    void groupBan(const json& params, Done done) {
        const auto g = peer(Kind::group, params.value("group_id", json()));
        const auto u = peer(Kind::user, params.value("user_id", json()));
        if (!g || !u || g->scene != Scene::group) { done(failed(1400, "未知的 group_id 或 user_id")); return; }
        const auto duration = onebot::number(params.value("duration", json(1800))).value_or(1800);
        const bool remove = duration <= 0;
        json body{{"members", json::array({json{{"op", remove ? "del" : "add"}, {"member_openid", u->id},
                                                {"mute_expire_at", remove ? std::string() : rfc3339After(duration)}}})}};
        bot_->call(drogon::Post, "/v2/groups/" + g->id + "/restrict_chat_setting", body,
            [done](json r) { done(r.value("ok", false) ? ok() : failed(100, r.value("message", std::string("禁言失败")))); });
    }

    void groupRequest(const json& params, Done done) {
        const auto flag = params.value("flag", std::string());
        std::vector<std::string> parts;
        for (std::size_t p = 0;;) {
            const auto bar = flag.find('|', p);
            parts.push_back(flag.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
            if (bar == std::string::npos) break;
            p = bar + 1;
        }
        if (parts.size() != 3 || parts[0].empty() || parts[1].empty()) { done(failed(1400, "flag 无效")); return; }
        const bool approve = params.value("approve", true);
        json body{{"op", approve ? "approve" : "decline"}, {"join_request_id", parts[2]}};
        const auto reason = params.value("reason", std::string());
        if (!approve && !reason.empty()) body["reject_reason"] = reason;
        bot_->call(drogon::Post, "/v2/groups/" + parts[0] + "/approval_join_request/" + parts[1], body,
            [done](json r) { done(r.value("ok", false) ? ok() : failed(100, r.value("message", std::string("审批失败")))); });
    }

    /// 扩展动作：把当前号码（或 OpenID）绑定到真实 QQ 号 / 群号。
    json bindIdentity(const json& params) {
        const auto kind = params.value("type", std::string("user")) == "group" ? Kind::group : Kind::user;
        std::string openId = protocol::text(params, "openid");
        if (openId.empty()) {
            const auto current = onebot::number(params.value(kind == Kind::group ? "group_id" : "user_id", json()));
            if (current) openId = store_->openId(kind, *current);
        }
        const auto qq = onebot::number(params.value("qq", json()));
        if (openId.empty() || !qq) return failed(1400, "需要 openid 或现有的 user_id / group_id，以及 qq");
        if (!store_->bind(kind, openId, *qq)) return failed(100, "号码已对应另一个 OpenID");
        return ok({{"openid", openId}, {kind == Kind::group ? "group_id" : "user_id", *qq}});
    }

    /// 扩展动作：查数字 ID 对应的 OpenID。
    json officialId(const json& params) {
        const bool group = params.contains("group_id");
        const auto n = onebot::number(params.value(group ? "group_id" : "user_id", json()));
        const auto openId = n ? store_->openId(group ? Kind::group : Kind::user, *n) : std::string();
        if (openId.empty()) return failed(1400, "未知的号码");
        return ok({{"openid", openId}, {"bound", store_->bound(group ? Kind::group : Kind::user, openId)}});
    }

    /// 扩展动作：直接调用开放平台接口，供需要官方专有能力的应用使用。
    void callOfficial(const json& params, Done done) {
        const auto method = params.value("method", std::string("GET"));
        const auto path = params.value("path", std::string());
        if (path.empty() || path[0] != '/') { done(failed(1400, "path 必须以 / 开头")); return; }
        const auto m = method == "POST" ? drogon::Post : method == "PUT" ? drogon::Put : method == "PATCH" ? drogon::Patch
            : method == "DELETE" ? drogon::Delete : drogon::Get;
        bot_->call(m, path, params.value("body", json::object()), [done](json r) {
            done(r.value("ok", false) ? ok(r["data"]) : failed(100, r.value("message", std::string("调用失败"))));
        });
    }

    BotConfig config_;
    std::shared_ptr<Store> store_;
    Logger log_;
    Hooks hooks_;
    mutable std::mutex nameMu_;
    std::string name_ = config_.name;
    bool autoAck_ = config_.autoAckInteractions;
    std::shared_ptr<Bot> bot_;
    std::mutex errorMu_;
    std::string lastError_;
    std::atomic<uint64_t> received_{0}, sent_{0};

};

}  // namespace catofficial::app
