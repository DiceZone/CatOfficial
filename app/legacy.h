#pragma once

// 普通 QQ 号上的 OneBot 实现（如 NapCat）。只读取群消息：官民绑定时，同一条 .bind 消息在
// 这一侧带着真实 QQ 号与群号出现。两种连接方式：
//   · 反向（默认）：CatOfficial 监听 host:port，在 NapCat 里添加 WebSocket 客户端连过来；
//   · 正向：CatOfficial 连 NapCat 的 WebSocket 服务端。

#include "config.h"

#include <catofficial/log.h>

#include <drogon/WebSocketClient.h>
#include <drogon/drogon.h>
#include <ixwebsocket/IXWebSocketServer.h>
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
    ~LegacyClient() { stop(); }

    void start() {
        stopping_ = false;
        if (config_.reverse()) listen(); else connect();
    }
    void stop() {
        stopping_ = true;
        if (client_) client_->stop();
        if (server_) { server_->stop(); server_.reset(); }
    }

    const BotConfig& config() const { return config_; }
    /// 只改名称等无需重连的字段。
    void update(const BotConfig& config) { std::lock_guard lock(mu_); config_.name = config.name; }

    json status() const {
        std::lock_guard lock(mu_);
        return json{{"online", connected_.load()}, {"self_id", selfId_}, {"nickname", nickname_}, {"error", connected_ ? std::string() : error_},
                    {"listening", listening_.load()}};
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

    std::string label() const { std::lock_guard lock(mu_); return config_.name.empty() ? config_.id : config_.name; }
    void say(LogLevel level, const std::string& message) const { if (log_) log_(level, "OneBot 机器人「" + label() + "」：" + message); }
    void setError(const std::string& e) { std::lock_guard lock(mu_); error_ = e; }

    // ── 反向：CatOfficial 监听 ─────────────────────────────

    void listen() {
        server_ = std::make_unique<ix::WebSocketServer>(config_.port, config_.host, ix::SocketServer::kDefaultTcpBacklog, 8);
        server_->disablePerMessageDeflate();
        std::weak_ptr<LegacyClient> weak = weak_from_this();
        server_->setOnConnectionCallback([weak](std::weak_ptr<ix::WebSocket> socket, std::shared_ptr<ix::ConnectionState>) {
            auto ws = socket.lock(); if (!ws) return;
            auto authorized = std::make_shared<bool>(false);
            ws->setOnMessageCallback([weak, socket, authorized](const ix::WebSocketMessagePtr& msg) {
                auto self = weak.lock(); auto ws = socket.lock();
                if (!self || !ws) return;
                if (msg->type == ix::WebSocketMessageType::Open) {
                    auto header = [&](const char* n) { auto it = msg->openInfo.headers.find(n); return it == msg->openInfo.headers.end() ? std::string() : it->second; };
                    std::string given = header("Authorization");
                    for (const char* prefix : {"Bearer ", "Token "}) if (given.rfind(prefix, 0) == 0) { given = given.substr(std::string(prefix).size()); break; }
                    if (given.empty()) {
                        const auto& uri = msg->openInfo.uri;
                        const auto p = uri.find("access_token=");
                        if (p != std::string::npos) given = uri.substr(p + 13, uri.find('&', p) == std::string::npos ? std::string::npos : uri.find('&', p) - p - 13);
                    }
                    if (!self->config_.token.empty() && given != self->config_.token) {
                        self->say(LogLevel::warn, "拒绝了令牌不正确的连接");
                        ws->close(1008, "unauthorized");
                        return;
                    }
                    *authorized = true;
                    if (const auto id = header("X-Self-ID"); !id.empty() && id.find_first_not_of("0123456789") == std::string::npos && id.size() <= 18) {
                        std::lock_guard lock(self->mu_); self->selfId_ = std::stoll(id);
                    }
                    self->connected_ = true;
                    self->setError({});
                    self->say(LogLevel::info, "已连入");
                    ws->sendText(json{{"action", "get_login_info"}, {"params", json::object()}, {"echo", "catofficial-login"}}.dump());
                } else if (msg->type == ix::WebSocketMessageType::Message) {
                    if (*authorized) self->onMessage(msg->str);
                } else if (msg->type == ix::WebSocketMessageType::Close) {
                    if (*authorized && self->connected_.exchange(false)) { self->say(LogLevel::warn, "连接已断开"); self->setError("连接已断开"); }
                }
            });
        });
        const auto result = server_->listen();
        if (!result.first) {
            listening_ = false;
            setError("无法监听 " + config_.host + ":" + std::to_string(config_.port) + "：" + result.second);
            say(LogLevel::error, "无法监听 " + config_.host + ":" + std::to_string(config_.port) + "：" + result.second);
            server_.reset();
            return;
        }
        server_->start();
        listening_ = true;
        setError("等待连入");
        say(LogLevel::info, "等待连入 ws://" + config_.host + ":" + std::to_string(config_.port) + "/");
    }

    // ── 正向：CatOfficial 连出去 ───────────────────────────

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
            auto self = weak.lock(); if (!self || self->stopping_) return;
            if (self->connected_.exchange(false)) self->say(LogLevel::warn, "连接已断开");
            self->retry("连接已断开");
        });
        client_->connectToServer(req, [weak](drogon::ReqResult result, const drogon::HttpResponsePtr& resp, const drogon::WebSocketClientPtr& client) {
            auto self = weak.lock(); if (!self) return;
            if (self->stopping_) { client->stop(); return; }
            if (result != drogon::ReqResult::Ok) {
                self->retry(resp && resp->statusCode() == drogon::k401Unauthorized ? "访问令牌不正确" : "连接失败：" + drogon::to_string(result));
                return;
            }
            self->connected_ = true;
            self->setError({});
            self->say(LogLevel::info, "已连接");
            if (auto conn = client->getConnection()) conn->send(json{{"action", "get_login_info"}, {"params", json::object()}, {"echo", "catofficial-login"}}.dump());
        });
    }

    void retry(const std::string& error) {
        setError(error);
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
            if (data.contains("user_id") && data["user_id"].is_number()) selfId_ = data["user_id"].get<int64_t>();
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
    std::unique_ptr<ix::WebSocketServer> server_;
    std::atomic<bool> stopping_{false}, connected_{false}, listening_{false};
    mutable std::mutex mu_;
    int64_t selfId_ = 0;
    std::string nickname_, error_;
};

}  // namespace catofficial::app
