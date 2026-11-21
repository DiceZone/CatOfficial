#pragma once

// 中枢：管理所有机器人（官方与原 QQ）、OneBot 连接上的事件分发、动作路由与通知。
//   · 事件：官方机器人产生的 OneBot 事件，送到服务这个机器人的连接。
//   · 动作：按 self_id（参数或请求头）找到机器人；没给时用连接服务的第一个机器人。

#include "bind.h"
#include "config.h"
#include "legacy.h"
#include "mailer.h"
#include "relay.h"
#include "store.h"

#include <catofficial/webhook.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace catofficial::app {

class Hub : public std::enable_shared_from_this<Hub> {
public:
    using json = nlohmann::json;
    using Sink = std::function<void(const json& event)>;

    Hub(Config config, std::shared_ptr<Store> store, Logger log)
        : config_(std::move(config)), store_(std::move(store)), log_(std::move(log)) {}

    void init() {
        mailer_ = std::make_shared<Mailer>(config_.smtp, log_);
        bind_ = std::make_shared<BindService>(config_.bind, store_, mailer_, log_);
        std::weak_ptr<Hub> weak = weak_from_this();
        bool legacy = false;
        for (const auto& b : config_.bots) {
            if (!b.enabled) continue;
            if (b.official()) {
                Relay::Hooks hooks;
                const auto id = b.id;
                hooks.emit = [weak, id](const json& event) { if (auto self = weak.lock()) self->emit(id, event); };
                hooks.intercept = [weak](Relay& relay, const event::MessageEvent& e) {
                    auto self = weak.lock(); if (!self) return false;
                    std::weak_ptr<Relay> r = relay.weak_from_this();
                    return self->bind_->onOfficial(e, [r, e](const std::string& text) { if (auto relay = r.lock()) relay->replyText(e, text); });
                };
                hooks.state = [weak, id, name = b.name](bool online, const std::string& reason) {
                    auto self = weak.lock(); if (!self) return;
                    if (!self->mailer_->config().notifyOffline) return;
                    if (online) self->mailer_->notifyOwner("online:" + id, "官方机器人「" + name + "」已恢复在线", "官方机器人「" + name + "」已重新上线。", 0);
                    else self->mailer_->notifyOwner("offline:" + id, "官方机器人「" + name + "」离线", "官方机器人「" + name + "」离线：" + reason + "\n\nCatOfficial 会自动重连；长时间未恢复请检查网络与机器人配置。");
                };
                auto relay = std::make_shared<Relay>(b, store_, log_, std::move(hooks));
                relay->init();
                relays_.push_back(relay);
                if (b.webhook) webhooks_.bind(b.appId, b.id, relay);
            } else {
                legacy = true;
                auto client = std::make_shared<LegacyClient>(b, [weak](const LegacyMessage& m) {
                    if (auto self = weak.lock()) self->bind_->onLegacy(m);
                }, log_);
                legacies_[b.id] = client;
            }
        }
        bind_->setLegacyAvailable(legacy);
    }

    void start() {
        for (auto& r : relays_) r->start();
        for (auto& [id, l] : legacies_) l->start();
    }
    void stop() {
        for (auto& r : relays_) r->stop();
        for (auto& [id, l] : legacies_) l->stop();
    }

    const Config& config() const { return config_; }
    std::shared_ptr<Store> store() const { return store_; }
    std::shared_ptr<Mailer> mailer() const { return mailer_; }
    const std::vector<std::shared_ptr<Relay>>& relays() const { return relays_; }

    std::shared_ptr<Relay> relay(const std::string& botId) const {
        for (const auto& r : relays_) if (r->id() == botId) return r;
        return nullptr;
    }
    std::shared_ptr<LegacyClient> legacy(const std::string& botId) const {
        auto it = legacies_.find(botId);
        return it == legacies_.end() ? nullptr : it->second;
    }

    /// 连接服务的官方机器人（按配置顺序）。
    std::vector<std::shared_ptr<Relay>> relaysFor(const ConnectionConfig& conn) const {
        std::vector<std::shared_ptr<Relay>> out;
        for (const auto& r : relays_) if (conn.serves(r->id())) out.push_back(r);
        return out;
    }

    // ── 事件分发 ────────────────────────────────────────────

    int addSink(const ConnectionConfig& conn, std::vector<std::string> bots, Sink sink) {
        std::lock_guard lock(sinkMu_);
        const int id = ++nextSink_;
        sinks_[id] = Entry{conn.id, std::move(bots), conn.reportSelf, conn.messageFormat == "string", std::move(sink)};
        return id;
    }
    void removeSink(int id) { std::lock_guard lock(sinkMu_); sinks_.erase(id); }

    /// 发往某个连接的全部订阅者（心跳用）。
    void emitTo(const std::string& connId, const std::string& botId, const json& event) { dispatch(&connId, botId, event); }
    void emit(const std::string& botId, const json& event) { dispatch(nullptr, botId, event); }

    /// 字符串格式的连接：把 message 换成 CQ 码。
    static json formatted(const json& value, bool asString) {
        if (!asString) return value;
        json out = value;
        auto convert = [](json& obj) {
            if (obj.is_object() && obj.contains("message") && obj["message"].is_array()) obj["message"] = onebot::toCq(obj["message"]);
        };
        convert(out);
        if (out.contains("data")) convert(out["data"]);
        return out;
    }

    // ── 动作路由 ────────────────────────────────────────────

    void handle(const ConnectionConfig& conn, int64_t selfIdHint, const std::string& action, const json& params, Relay::Done done) {
        auto candidates = relaysFor(conn);
        int64_t wanted = selfIdHint;
        if (params.is_object() && params.contains("self_id")) if (const auto n = onebot::number(params["self_id"])) wanted = *n;
        std::shared_ptr<Relay> target;
        for (const auto& r : candidates) if (wanted && r->selfId() == wanted) { target = r; break; }
        if (!target && !wanted && !candidates.empty()) target = candidates.front();
        if (!target) { done(Relay::failed(1400, wanted ? "这个连接没有服务 self_id 为 " + std::to_string(wanted) + " 的机器人" : "这个连接还没有可用的官方机器人")); return; }
        const bool asString = conn.messageFormat == "string";
        target->handle(action, params, [done, asString](json response) { done(formatted(response, asString)); });
    }

    webhook::Reply acceptWebhook(const std::string& appId, const std::string& timestamp, const std::string& signature, const std::string& raw) {
        auto relay = webhooks_.find(appId, [](const Relay&) { return true; });
        if (!relay) return {404, {}, {}};
        return relay->acceptWebhook(timestamp, signature, raw);
    }

    /// 读取官民绑定文件：[{"type":"user"|"group","openid":"…","qq":123}]。
    void loadBindings(const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        const auto list = json::parse(in, nullptr, false);
        if (!list.is_array()) { say(LogLevel::warn, "官民绑定文件不是 JSON 数组，已忽略"); return; }
        int applied = 0;
        for (const auto& item : list) {
            if (!item.is_object()) continue;
            const auto kind = item.value("type", std::string("user")) == "group" ? Kind::group : Kind::user;
            const auto openId = protocol::text(item, "openid");
            const auto qq = onebot::number(item.value("qq", json()));
            if (openId.empty() || !qq) continue;
            if (store_->bind(kind, openId, *qq)) ++applied;
            else say(LogLevel::warn, "官民绑定冲突：号码 " + std::to_string(*qq) + " 已对应另一个 OpenID");
        }
        say(LogLevel::info, "已载入官民绑定 " + std::to_string(applied) + " 条");
    }

    void prune() {
        if (config_.messageRetentionDays > 0)
            store_->pruneMessages(static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(config_.messageRetentionDays) * 86400);
    }

    uint64_t received() const { uint64_t n = 0; for (const auto& r : relays_) n += r->received(); return n; }
    uint64_t sent() const { uint64_t n = 0; for (const auto& r : relays_) n += r->sent(); return n; }

private:
    struct Entry {
        std::string connId;
        std::vector<std::string> bots;   // 空 = 全部
        bool reportSelf = false;
        bool asString = false;
        Sink sink;
    };

    void say(LogLevel level, const std::string& message) const { if (log_) log_(level, message); }

    void dispatch(const std::string* connId, const std::string& botId, const json& event) {
        const bool selfMessage = event.value("post_type", std::string()) == "message_sent";
        std::vector<std::pair<Sink, bool>> targets;
        {
            std::lock_guard lock(sinkMu_);
            for (auto& [id, entry] : sinks_) {
                if (connId && entry.connId != *connId) continue;
                if (!entry.bots.empty() && std::find(entry.bots.begin(), entry.bots.end(), botId) == entry.bots.end()) continue;
                if (selfMessage && !entry.reportSelf) continue;
                targets.emplace_back(entry.sink, entry.asString);
            }
        }
        for (auto& [sink, asString] : targets) {
            try { sink(formatted(event, asString)); } catch (const std::exception& e) { say(LogLevel::warn, std::string("推送事件失败：") + e.what()); }
        }
    }

    Config config_;
    std::shared_ptr<Store> store_;
    Logger log_;
    std::shared_ptr<Mailer> mailer_;
    std::shared_ptr<BindService> bind_;
    std::vector<std::shared_ptr<Relay>> relays_;
    std::map<std::string, std::shared_ptr<LegacyClient>> legacies_;
    webhook::Registry<Relay> webhooks_;
    std::mutex sinkMu_;
    std::map<int, Entry> sinks_;
    int nextSink_ = 0;
};

}  // namespace catofficial::app
