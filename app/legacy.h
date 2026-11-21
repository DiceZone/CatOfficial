#pragma once

// 普通 QQ 号上的 OneBot 实现（如 NapCat）。CatOfficial 以客户端身份连它的正向 WebSocket，
// 只读取群消息：官民绑定时，同一条 .bind 消息在这一侧带着真实 QQ 号与群号出现。

#include "config.h"

#include <catofficial/log.h>

#include <drogon/WebSocketClient.h>
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace catofficial::app {

struct LegacyMessage {
    std::string botId;
    int64_t groupId = 0;
    int64_t userId = 0;
    std::string role;    // owner / admin / member
    std::string text;    // 去掉 @ 与引用后的纯文本
};

class LegacyClient : public std::enable_shared_from_this<LegacyClient> {
public:
    using json = nlohmann::json;
    using Handler = std::function<void(const LegacyMessage&)>;

    LegacyClient(BotConfig config, Handler handler, Logger log)
        : config_(std::move(config)), handler_(std::move(handler)), log_(std::move(log)) {}

    void start() { stopping_ = false; connect(); }
    void stop() {
        stopping_ = true;
        if (client_) client_->stop();
    }

    json status() const {
        std::lock_guard lock(mu_);
        return json{{"online", connected_.load()}, {"self_id", selfId_}, {"nickname", nickname_}, {"error", error_}};
    }

    /// 消息段或字符串 → 纯文本（去掉 @、引用与其他非文字段）。
    static std::string plainText(const json& message) {
        if (message.is_string()) {
            std::string s = message.get<std::string>(), out;
            for (std::size_t i = 0; i < s.size();) {
                if (s.compare(i, 4, "[CQ:") == 0) {
                    const auto end = s.find(']', i);
                    if (end != std::string::npos) { i = end + 1; continue; }
                }
                out += s[i++];
            }
            return unescape(out);
        }
        std::string out;
        if (message.is_array())
            for (const auto& seg : message)
                if (seg.is_object() && seg.value("type", std::string()) == "text" && seg.contains("data"))
                    out += seg["data"].value("text", std::string());
        return out;
    }

private:
    static std::string unescape(std::string s) {
        for (const auto& [from, to] : {std::pair<std::string, std::string>{"&#91;", "["}, {"&#93;", "]"}, {"&#44;", ","}, {"&amp;", "&"}}) {
            for (std::size_t p = 0; (p = s.find(from, p)) != std::string::npos;) { s.replace(p, from.size(), to); p += to.size(); }
        }
        return s;
    }

    void say(LogLevel level, const std::string& message) const { if (log_) log_(level, "OneBot「" + config_.name + "」：" + message); }

    void connect() {
        if (stopping_) return;
        const auto scheme = config_.url.find("://");
        const auto slash = config_.url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        const std::string origin = slash == std::string::npos ? config_.url : config_.url.substr(0, slash);
        const std::string path = slash == std::string::npos ? "/" : config_.url.substr(slash);
        client_ = drogon::WebSocketClient::newWebSocketClient(origin);
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath(path);
        if (!config_.token.empty()) req->addHeader("Authorization", "Bearer " + config_.token);
        std::weak_ptr<LegacyClient> weak = weak_from_this();
        client_->setMessageHandler([weak](std::string&& message, const drogon::WebSocketClientPtr&, const drogon::WebSocketMessageType& type) {
            if (type != drogon::WebSocketMessageType::Text) return;
            if (auto self = weak.lock()) self->onMessage(message);
        });
        client_->setConnectionClosedHandler([weak](const drogon::WebSocketClientPtr&) {
            auto self = weak.lock(); if (!self) return;
            if (self->connected_.exchange(false)) self->say(LogLevel::warn, "连接已断开");
            self->retry("连接已断开");
        });
        client_->connectToServer(req, [weak](drogon::ReqResult result, const drogon::HttpResponsePtr& resp, const drogon::WebSocketClientPtr& client) {
            auto self = weak.lock(); if (!self) return;
            if (result != drogon::ReqResult::Ok) {
                self->retry(resp && resp->statusCode() == drogon::k401Unauthorized ? "访问令牌不正确" : "连接失败：" + drogon::to_string(result));
                return;
            }
            self->connected_ = true;
            { std::lock_guard lock(self->mu_); self->error_.clear(); }
            self->say(LogLevel::info, "已连接");
            if (auto conn = client->getConnection()) conn->send(json{{"action", "get_login_info"}, {"params", json::object()}, {"echo", "catofficial-login"}}.dump());
        });
    }

    void retry(const std::string& error) {
        { std::lock_guard lock(mu_); error_ = error; }
        if (stopping_) return;
        std::weak_ptr<LegacyClient> weak = weak_from_this();
        drogon::app().getLoop()->runAfter(5.0, [weak] { if (auto self = weak.lock()) self->connect(); });
    }

    void onMessage(const std::string& raw) {
        const auto j = json::parse(raw, nullptr, false);
        if (!j.is_object()) return;
        if (j.value("echo", std::string()) == "catofficial-login") {
            const auto data = j.value("data", json::object());
            std::lock_guard lock(mu_);
            if (data.contains("user_id")) selfId_ = data["user_id"].is_number() ? data["user_id"].get<int64_t>() : 0;
            nickname_ = data.value("nickname", std::string());
            return;
        }
        if (j.contains("self_id") && j["self_id"].is_number()) { std::lock_guard lock(mu_); selfId_ = j["self_id"].get<int64_t>(); }
        if (j.value("post_type", std::string()) != "message" || j.value("message_type", std::string()) != "group") return;
        LegacyMessage m;
        m.botId = config_.id;
        m.groupId = j.value("group_id", int64_t(0));
        m.userId = j.value("user_id", int64_t(0));
        m.role = j.contains("sender") && j["sender"].is_object() ? j["sender"].value("role", std::string("member")) : "member";
        m.text = plainText(j.contains("message") ? j["message"] : json(j.value("raw_message", std::string())));
        const auto first = m.text.find_first_not_of(" \t\r\n");
        m.text = first == std::string::npos ? std::string() : m.text.substr(first, m.text.find_last_not_of(" \t\r\n") - first + 1);
        if (m.groupId && m.userId && handler_) handler_(m);
    }

    BotConfig config_;
    Handler handler_;
    Logger log_;
    drogon::WebSocketClientPtr client_;
    std::atomic<bool> stopping_{false}, connected_{false};
    mutable std::mutex mu_;
    int64_t selfId_ = 0;
    std::string nickname_, error_;
};

}  // namespace catofficial::app
