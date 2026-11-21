#pragma once

// 中枢：管理所有机器人（官方与原 QQ）、OneBot 连接上的事件分发、动作路由与通知。
//   · 事件：官方机器人产生的 OneBot 事件，送到服务这个机器人的连接。
//   · 动作：按 self_id（参数或请求头）找到机器人；没给时用连接服务的第一个机器人。
//   · 配置变化时 apply()：增删改机器人、更新邮件与绑定设置，不需要重启。

#include "bind.h"
#include "config.h"
#include "legacy.h"
#include "logbuf.h"
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
#include <tuple>
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
        {
            std::lock_guard lock(mu_);
            bind_ = std::make_shared<BindService>(config_.bind, store_, mailer_, log_);
        }
        std::weak_ptr<Hub> weak = weak_from_this();
        LogBuffer::get().setErrorSink([weak](const std::string& message) { if (auto self = weak.lock()) self->reportError(message); });
    }

    /// 按当前配置启动全部机器人（事件循环开始后调用）。
    void start() { applyBots({}, config().bots); }

    void stop() {
        std::vector<std::shared_ptr<Relay>> relays;
        std::map<std::string, std::shared_ptr<LegacyClient>> legacies;
        { std::lock_guard lock(mu_); relays.swap(relays_); legacies.swap(legacies_); }
        for (auto& r : relays) r->stop();
        for (auto& [id, l] : legacies) l->stop();
    }

    /// 应用新配置中可以热加载的部分：机器人、邮件、绑定指令、日志级别、消息保留天数。
    void apply(const Config& next) {
        const auto previous = config();
        { std::lock_guard lock(mu_); config_ = next; }
        mailer_->setConfig(next.smtp);
        applyBots(previous.bots, next.bots);
        LogBuffer::get().setMinimum(next.logLevel);
    }

    Config config() const { std::lock_guard lock(mu_); return config_; }
    std::shared_ptr<Store> store() const { return store_; }
    std::shared_ptr<Mailer> mailer() const { return mailer_; }
    std::shared_ptr<BindService> bind() const { std::lock_guard lock(mu_); return bind_; }
    std::vector<std::shared_ptr<Relay>> relays() const { std::lock_guard lock(mu_); return relays_; }

    std::shared_ptr<Relay> relay(const std::string& botId) const {
        std::lock_guard lock(mu_);
        for (const auto& r : relays_) if (r->id() == botId) return r;
        return nullptr;
    }
    std::shared_ptr<LegacyClient> legacy(const std::string& botId) const {
        std::lock_guard lock(mu_);
        auto it = legacies_.find(botId);
        return it == legacies_.end() ? nullptr : it->second;
    }

    /// 连接服务的官方机器人（按配置顺序）。
    std::vector<std::shared_ptr<Relay>> relaysFor(const ConnectionConfig& conn) const {
        std::vector<std::shared_ptr<Relay>> out;
        for (const auto& r : relays()) if (conn.serves(r->id())) out.push_back(r);
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

    /// 连接的设置变了（服务的机器人、消息格式、是否上报自身消息）：更新已有订阅。
    /// keepBots 为 true 时保留订阅原有的机器人范围（反向 WS 一条连接只对应一个机器人）。
    void updateSinks(const ConnectionConfig& conn, bool keepBots) {
        std::lock_guard lock(sinkMu_);
        for (auto& [id, entry] : sinks_) {
            if (entry.connId != conn.id) continue;
            if (!keepBots) entry.bots = conn.bots;
            entry.reportSelf = conn.reportSelf;
            entry.asString = conn.messageFormat == "string";
        }
    }

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
        const int days = config().messageRetentionDays;
        if (days > 0) store_->pruneMessages(static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(days) * 86400);
    }

    uint64_t received() const { uint64_t n = 0; for (const auto& r : relays()) n += r->received(); return n; }
    uint64_t sent() const { uint64_t n = 0; for (const auto& r : relays()) n += r->sent(); return n; }

private:
    struct Entry {
        std::string connId;
        std::vector<std::string> bots;   // 空 = 全部
        bool reportSelf = false;
        bool asString = false;
        Sink sink;
    };

    void say(LogLevel level, const std::string& message) const { if (log_) log_(level, message); }

    /// 对比前后两份机器人配置：停掉删除、停用或改过的，启动新增与改过的。
    void applyBots(const std::vector<BotConfig>& before, const std::vector<BotConfig>& after) {
        auto find = [](const std::vector<BotConfig>& list, const std::string& id) -> const BotConfig* {
            for (const auto& b : list) if (b.id == id) return &b;
            return nullptr;
        };
        std::vector<std::shared_ptr<Relay>> stopRelays;
        std::vector<std::shared_ptr<LegacyClient>> stopLegacies;
        std::vector<BotConfig> startList;
        {
            std::lock_guard lock(mu_);
            for (auto it = relays_.begin(); it != relays_.end();) {
                const auto* next = find(after, (*it)->id());
                const auto* prev = find(before, (*it)->id());
                if (!next || !next->enabled || !next->official() || (prev && !prev->sameConnection(*next))) {
                    stopRelays.push_back(*it);
                    it = relays_.erase(it);
                } else {
                    (*it)->update(*next);   // 名称等就地更新，不重连
                    ++it;
                }
            }
            for (auto it = legacies_.begin(); it != legacies_.end();) {
                const auto* next = find(after, it->first);
                const auto* prev = find(before, it->first);
                if (!next || !next->enabled || next->official() || (prev && !prev->sameConnection(*next))) {
                    stopLegacies.push_back(it->second);
                    it = legacies_.erase(it);
                } else {
                    it->second->update(*next);
                    ++it;
                }
            }
            for (const auto& b : after) {
                if (!b.enabled) continue;
                const bool running = b.official()
                    ? std::any_of(relays_.begin(), relays_.end(), [&](const auto& r) { return r->id() == b.id; })
                    : legacies_.count(b.id) > 0;
                if (!running) startList.push_back(b);
            }
        }
        for (auto& r : stopRelays) {
            webhooks_.unbind(r->config().appId, r->id());
            r->stop();
            say(LogLevel::info, "已停止官方机器人「" + r->displayName() + "」");
        }
        for (auto& l : stopLegacies) l->stop();
        std::weak_ptr<Hub> weak = weak_from_this();
        for (const auto& b : startList) {
            if (b.official()) {
                auto relay = std::make_shared<Relay>(b, store_, log_, hooksFor(b));
                relay->init();
                { std::lock_guard lock(mu_); relays_.push_back(relay); }
                if (b.webhook) webhooks_.bind(b.appId, b.id, relay);
                relay->start();
            } else {
                auto client = std::make_shared<LegacyClient>(b, [weak](const LegacyMessage& m) {
                    if (auto self = weak.lock()) self->bind()->onLegacy(m);
                }, log_);
                { std::lock_guard lock(mu_); legacies_[b.id] = client; }
                client->start();
            }
        }
        // 绑定服务的设置与「有没有原 QQ 机器人」都可能变了，换一个新的服务实例。
        const auto cfg = config();
        bool legacy = false;
        for (const auto& b : cfg.bots) if (b.enabled && !b.official()) legacy = true;
        auto service = std::make_shared<BindService>(cfg.bind, store_, mailer_, log_);
        service->setLegacyAvailable(legacy);
        service->setMasterCheck([weak](const std::string& botId, const std::string& openId) {
            auto self = weak.lock();
            return self && self->config().isMaster(botId, openId);
        });
        { std::lock_guard lock(mu_); bind_ = service; }
    }

    Relay::Hooks hooksFor(const BotConfig& b) {
        std::weak_ptr<Hub> weak = weak_from_this();
        Relay::Hooks hooks;
        const auto id = b.id;
        hooks.emit = [weak, id](const json& event) { if (auto self = weak.lock()) self->emit(id, event); };
        hooks.intercept = [weak](Relay& relay, const event::MessageEvent& e) {
            auto self = weak.lock(); if (!self) return false;
            std::weak_ptr<Relay> r = relay.weak_from_this();
            return self->bind()->onOfficial(relay.id(), e, [r, e](const std::string& text) { if (auto relay = r.lock()) relay->replyText(e, text); });
        };
        return hooks;
    }

    // ── 运行异常通知 ────────────────────────────────────────
    // 错误级别的日志先攒起来，最多每 10 分钟汇总一次：发到通知邮箱，并私聊主人。

    void reportError(const std::string& message) {
        std::lock_guard lock(errMu_);
        if (pendingErrors_.size() < 50) pendingErrors_.push_back(message);
        if (flushScheduled_) return;
        flushScheduled_ = true;
        const auto now = std::time(nullptr);
        const double delay = std::max<double>(60.0, static_cast<double>(lastFlush_ + 600 - now));
        std::weak_ptr<Hub> weak = weak_from_this();
        drogon::app().getLoop()->runAfter(delay, [weak] { if (auto self = weak.lock()) self->flushErrors(); });
    }

    void flushErrors() {
        std::vector<std::string> errors;
        {
            std::lock_guard lock(errMu_);
            errors.swap(pendingErrors_);
            flushScheduled_ = false;
            lastFlush_ = std::time(nullptr);
        }
        if (errors.empty()) return;
        const auto cfg = config();
        std::string body = "CatOfficial 最近出现了 " + std::to_string(errors.size()) + " 条错误：\n\n";
        for (const auto& e : errors) body += "· " + e + "\n";
        body += "\n详情可在管理面板的「运行日志」中查看。";
        if (cfg.smtp.notifyErrors) mailer_->notifyAll("CatOfficial 运行异常", body);
        if (!cfg.notifyMasters) return;
        std::string brief = "CatOfficial 运行异常（" + std::to_string(errors.size()) + " 条）：";
        for (std::size_t i = 0; i < errors.size() && i < 3; ++i) brief += "\n· " + errors[i];
        if (errors.size() > 3) brief += "\n……详见管理面板的运行日志";
        const auto running = relays();
        for (const auto& m : cfg.masters) {
            for (const auto& r : running) {
                if (!m.botId.empty() && m.botId != r->id()) continue;
                if (!r->bot()->online()) continue;
                r->sendPrivateText(m.openId, brief);
                break;   // 「全部机器人」的主人只由第一个在线的机器人通知一次
            }
        }
    }

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

    std::mutex errMu_;
    std::vector<std::string> pendingErrors_;
    bool flushScheduled_ = false;
    std::time_t lastFlush_ = 0;
    mutable std::mutex mu_;
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
