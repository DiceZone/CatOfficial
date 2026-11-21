#pragma once

// OneBot V11 的四类连接（照 NapCat 的划分）：
//   · http_server：HTTP API，/{action}
//   · ws_server：正向 WebSocket，/ 收事件并调用动作；/event 只收事件；/api 只调用动作
//   · http_client：HTTP POST 上报，响应体可做快速回复
//   · ws_client：反向 WebSocket（Universal），每个机器人一条连接，带 X-Self-ID
// 每个连接有自己的访问令牌与消息格式，并可限定服务哪些官方机器人。
// 另提供头像跳转 /avatar/user/{user_id} 与官方 Webhook 入口。

#include "hub.h"

#include <drogon/WebSocketClient.h>
#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace catofficial::app {

struct ParsedEndpoint { std::string origin, path; };
/// scheme://host[:port]/path?query → {scheme://host[:port], /path?query}
inline ParsedEndpoint splitUrl(const std::string& url) {
    const auto scheme = url.find("://");
    const auto slash = scheme == std::string::npos ? std::string::npos : url.find('/', scheme + 3);
    if (slash == std::string::npos) return {url, "/"};
    return {url.substr(0, slash), url.substr(slash)};
}

inline std::string hmacSha1Hex(const std::string& key, const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE]; unsigned int len = 0;
    HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &len);
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (unsigned int i = 0; i < len; ++i) { hex += kHex[out[i] >> 4]; hex += kHex[out[i] & 0xF]; }
    return hex;
}

/// 各连接的当前状态，供管理面板显示。
struct ConnectionStats {
    std::mutex mu;
    std::map<std::string, int> wsClients;                          // 连接 id → 在线客户端数
    std::map<std::string, nlohmann::json> reverse;                 // 连接 id → {机器人 id → {connected, since, error}}
    std::map<std::string, nlohmann::json> posts;                   // 连接 id → {ok, status, at, error}
    static ConnectionStats& get() { static ConnectionStats s; return s; }
    void clientDelta(const std::string& conn, int delta) { std::lock_guard lock(mu); wsClients[conn] += delta; }
    void setReverse(const std::string& conn, const std::string& bot, nlohmann::json state) { std::lock_guard lock(mu); reverse[conn][bot] = std::move(state); }
    void setPost(const std::string& conn, nlohmann::json state) { std::lock_guard lock(mu); posts[conn] = std::move(state); }
    nlohmann::json snapshot(const std::string& conn) {
        std::lock_guard lock(mu);
        nlohmann::json out = nlohmann::json::object();
        if (auto it = wsClients.find(conn); it != wsClients.end()) out["clients"] = it->second;
        if (auto it = reverse.find(conn); it != reverse.end()) out["reverse"] = it->second;
        if (auto it = posts.find(conn); it != posts.end()) out["last_post"] = it->second;
        return out;
    }
};

class Server {
public:
    using json = nlohmann::json;

    static std::shared_ptr<Hub>& hub() { static std::shared_ptr<Hub> h; return h; }
    /// 端口 → 监听在该端口的连接。
    static std::map<uint16_t, ConnectionConfig>& listeners() { static std::map<uint16_t, ConnectionConfig> l; return l; }
    /// 管理面板的端口与单层路径的静态文件（如 /favicon.svg），由 WebUi 设置。
    static uint16_t& panelPort() { static uint16_t p = 0; return p; }
    static std::function<drogon::HttpResponsePtr(const std::string&)>& panelFile() {
        static std::function<drogon::HttpResponsePtr(const std::string&)> f; return f;
    }

    static const ConnectionConfig* connectionAt(const drogon::HttpRequestPtr& req, const std::string& type) {
        auto it = listeners().find(req->getLocalAddr().toPort());
        return it != listeners().end() && it->second.type == type ? &it->second : nullptr;
    }

    /// 校验访问令牌：Authorization: Bearer xxx 或查询参数 access_token。0 通过，401 缺失，403 不符。
    static int authorize(const drogon::HttpRequestPtr& req, const std::string& token) {
        if (token.empty()) return 0;
        std::string given = req->getHeader("Authorization");
        for (const char* prefix : {"Bearer ", "Token "})
            if (given.rfind(prefix, 0) == 0) { given = given.substr(std::string(prefix).size()); break; }
        if (given.empty()) given = req->getParameter("access_token");
        if (given.empty()) return 401;
        return given == token ? 0 : 403;
    }

    static int64_t selfIdHint(const drogon::HttpRequestPtr& req) {
        for (const auto& value : {req->getHeader("X-Self-ID"), req->getParameter("self_id")})
            if (!value.empty() && value.find_first_not_of("0123456789") == std::string::npos && value.size() <= 18) return std::stoll(value);
        return 0;
    }

    static void setup(std::shared_ptr<Hub> h) {
        hub() = std::move(h);
        const auto& cfg = hub()->config();
        for (const auto& conn : cfg.connections) {
            if (!conn.enabled || !conn.server()) continue;
            listeners()[conn.port] = conn;
            drogon::app().addListener(conn.host, conn.port);
        }
        if (cfg.webhookPort) drogon::app().addListener(cfg.webhookHost, cfg.webhookPort);

        drogon::app().registerHandler("/{action}",
            [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& action) {
                handleHttp(req, std::move(callback), action);
            }, {drogon::Get, drogon::Post});

        drogon::app().registerHandler("/avatar/user/{user_id}",
            [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& userId) {
                const auto n = onebot::number(json(userId));
                int size = 640;
                try { if (!req->getParameter("s").empty()) size = std::stoi(req->getParameter("s")); } catch (...) {}
                if (size != 40 && size != 100 && size != 140 && size != 640) size = 640;
                std::string url;
                if (n) for (const auto& r : hub()->relays()) { url = r->userAvatar(*n, size); if (!url.empty()) break; }
                if (url.empty()) { auto resp = drogon::HttpResponse::newHttpResponse(); resp->setStatusCode(drogon::k404NotFound); callback(resp); return; }
                callback(drogon::HttpResponse::newRedirectionResponse(url, drogon::k302Found));
            }, {drogon::Get});

        if (cfg.webhookPort) {
            drogon::app().registerHandler(cfg.webhookPath,
                [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    if (req->getLocalAddr().toPort() != hub()->config().webhookPort) { resp->setStatusCode(drogon::k404NotFound); callback(resp); return; }
                    const auto reply = hub()->acceptWebhook(req->getHeader("X-Bot-Appid"), req->getHeader("X-Signature-Timestamp"),
                        req->getHeader("X-Signature-Ed25519"), std::string(req->getBody()));
                    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(reply.status));
                    if (!reply.body.empty()) { resp->setContentTypeCode(drogon::CT_APPLICATION_JSON); resp->setBody(reply.body); }
                    callback(resp);
                }, {drogon::Post});
        }
    }

    /// 机器人启动后：HTTP POST、反向 WebSocket、心跳、消息索引清理。
    static void startClients();

private:
    static json requestParams(const drogon::HttpRequestPtr& req) {
        json params = json::object();
        if (req->method() == drogon::Post && req->getContentType() == drogon::CT_APPLICATION_JSON) {
            params = json::parse(req->getBody(), nullptr, false);
            if (!params.is_object()) return json();
        }
        for (const auto& [key, value] : req->getParameters()) {
            if (key == "access_token" || params.contains(key)) continue;
            // 查询串与表单里只有字符串；布尔参数按 OneBot 惯例转换。
            if (value == "true" || value == "false") params[key] = value == "true";
            else params[key] = value;
        }
        return params;
    }

    static void handleHttp(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& action) {
        if (panelFile() && panelPort() && req->getLocalAddr().toPort() == panelPort()) { callback(panelFile()(action)); return; }
        auto respond = [callback](int status, const json& body) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(body.dump());
            callback(resp);
        };
        const auto* conn = connectionAt(req, "http_server");
        if (!conn) { respond(404, Relay::failed(1404, "此端口未开启 HTTP API")); return; }
        if (const int code = authorize(req, conn->token)) { respond(code, Relay::failed(code, code == 401 ? "缺少 access_token" : "access_token 不正确")); return; }
        const auto params = requestParams(req);
        if (params.is_null()) { respond(400, Relay::failed(1400, "请求体不是 JSON 对象")); return; }
        hub()->handle(*conn, selfIdHint(req), action, params, [respond](json response) {
            respond(response.value("retcode", 0) == 1404 ? 404 : 200, response);
        });
    }
};

class OneBotSocket : public drogon::WebSocketController<OneBotSocket> {
public:
    using json = nlohmann::json;
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/");
    WS_PATH_ADD("/api");
    WS_PATH_ADD("/event");
    WS_PATH_LIST_END

    struct Context { ConnectionConfig conn; bool api = false; bool events = false; int sink = 0; int64_t selfHint = 0; };

    void handleNewConnection(const drogon::HttpRequestPtr& req, const drogon::WebSocketConnectionPtr& conn) override {
        const auto* config = Server::connectionAt(req, "ws_server");
        if (!config || Server::authorize(req, config->token) != 0) {
            conn->shutdown(drogon::CloseCode::kViolation, "unauthorized");
            return;
        }
        auto hub = Server::hub();
        auto ctx = std::make_shared<Context>();
        ctx->conn = *config;
        ctx->selfHint = Server::selfIdHint(req);
        const auto path = req->path();
        ctx->api = path != "/event";
        ctx->events = path != "/api";
        ConnectionStats::get().clientDelta(config->id, 1);
        if (ctx->events) {
            std::weak_ptr<drogon::WebSocketConnection> weak = conn;
            ctx->sink = hub->addSink(*config, config->bots, [weak](const json& event) { if (auto c = weak.lock()) if (c->connected()) c->send(event.dump()); });
            for (const auto& r : hub->relaysFor(*config)) conn->send(r->lifecycle("connect").dump());
        }
        conn->setContext(ctx);
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message, const drogon::WebSocketMessageType& type) override {
        if (type != drogon::WebSocketMessageType::Text) return;
        auto ctx = conn->getContext<Context>();
        if (!ctx || !ctx->api) return;
        dispatchAction(ctx->conn, ctx->selfHint, message, [conn](const std::string& response) { if (conn->connected()) conn->send(response); });
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
        auto ctx = conn->getContext<Context>();
        if (!ctx) return;
        if (ctx->sink) Server::hub()->removeSink(ctx->sink);
        ConnectionStats::get().clientDelta(ctx->conn.id, -1);
        conn->clearContext();
    }

    /// 解析 {action, params, echo} 并执行，响应原样带回 echo。
    static void dispatchAction(const ConnectionConfig& conn, int64_t selfHint, const std::string& message, std::function<void(const std::string&)> reply) {
        const auto request = json::parse(message, nullptr, false);
        if (!request.is_object()) { reply(Relay::failed(1400, "请求不是 JSON 对象").dump()); return; }
        const json echo = request.contains("echo") ? request["echo"] : json();
        Server::hub()->handle(conn, selfHint, request.value("action", std::string()), request.value("params", json::object()),
            [reply, echo](json response) {
                if (!echo.is_null()) response["echo"] = echo;
                reply(response.dump());
            });
    }
};

/// 反向 WebSocket：一个机器人一条连接。
class ReverseClient : public std::enable_shared_from_this<ReverseClient> {
public:
    ReverseClient(ConnectionConfig config, std::shared_ptr<Relay> relay) : config_(std::move(config)), relay_(std::move(relay)) {}

    void connect() {
        auto self = shared_from_this();
        // 还不知道 self_id（机器人尚未上线）时稍后再连，应用端靠 X-Self-ID 区分账号。
        const auto selfId = relay_->selfId();
        if (selfId == 0) { state(false, "等待机器人上线"); retry(); return; }
        const auto endpoint = splitUrl(config_.url);
        client_ = drogon::WebSocketClient::newWebSocketClient(endpoint.origin);
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath(endpoint.path);
        req->addHeader("X-Self-ID", std::to_string(selfId));
        req->addHeader("X-Client-Role", "Universal");
        req->addHeader("User-Agent", std::string("CatOfficial/") + kVersion);
        if (!config_.token.empty()) req->addHeader("Authorization", "Bearer " + config_.token);
        client_->setMessageHandler([self](std::string&& message, const drogon::WebSocketClientPtr& client, const drogon::WebSocketMessageType& type) {
            if (type != drogon::WebSocketMessageType::Text) return;
            std::weak_ptr<drogon::WebSocketClient> weak = client;
            OneBotSocket::dispatchAction(self->scoped(), self->relay_->selfId(), message, [weak](const std::string& response) {
                if (auto c = weak.lock()) if (auto conn = c->getConnection(); conn && conn->connected()) conn->send(response);
            });
        });
        client_->setConnectionClosedHandler([self](const drogon::WebSocketClientPtr&) {
            self->state(false, "连接已断开");
            self->retry();
        });
        client_->connectToServer(req, [self](drogon::ReqResult result, const drogon::HttpResponsePtr&, const drogon::WebSocketClientPtr& client) {
            if (result != drogon::ReqResult::Ok) { self->state(false, drogon::to_string(result)); self->retry(); return; }
            self->state(true, {});
            auto hub = Server::hub();
            std::weak_ptr<drogon::WebSocketClient> weak = client;
            self->sink_ = hub->addSink(self->config_, {self->relay_->id()}, [weak](const nlohmann::json& event) {
                if (auto c = weak.lock()) if (auto conn = c->getConnection(); conn && conn->connected()) conn->send(event.dump());
            });
            if (auto conn = client->getConnection()) conn->send(self->relay_->lifecycle("connect").dump());
        });
    }

private:
    ConnectionConfig scoped() const { auto c = config_; c.bots = {relay_->id()}; return c; }
    void state(bool connected, const std::string& error) {
        ConnectionStats::get().setReverse(config_.id, relay_->id(),
            nlohmann::json{{"connected", connected}, {"error", error}, {"since", std::time(nullptr)}});
    }
    void retry() {
        if (sink_) { Server::hub()->removeSink(sink_); sink_ = 0; }
        auto self = shared_from_this();
        drogon::app().getLoop()->runAfter(std::max(config_.reconnectInterval, 500) / 1000.0, [self] { self->connect(); });
    }

    ConnectionConfig config_;
    std::shared_ptr<Relay> relay_;
    drogon::WebSocketClientPtr client_;
    int sink_ = 0;
};

inline void Server::startClients() {
    auto h = hub();
    const auto& cfg = h->config();
    std::weak_ptr<Hub> weak = h;
    for (const auto& conn : cfg.connections) {
        if (!conn.enabled) continue;
        if (conn.type == "http_client") {
            const auto endpoint = splitUrl(conn.url);
            auto client = drogon::HttpClient::newHttpClient(endpoint.origin);
            h->addSink(conn, conn.bots, [client, endpoint, conn, weak](const json& event) {
                auto req = drogon::HttpRequest::newHttpRequest();
                req->setMethod(drogon::Post);
                req->setPath(endpoint.path);
                req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                const auto body = event.dump();
                req->setBody(body);
                req->addHeader("X-Self-ID", std::to_string(event.value("self_id", int64_t(0))));
                req->addHeader("User-Agent", std::string("CatOfficial/") + kVersion);
                if (!conn.token.empty()) req->addHeader("X-Signature", "sha1=" + hmacSha1Hex(conn.token, body));
                client->sendRequest(req, [weak, event, id = conn.id](drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
                    const bool ok = result == drogon::ReqResult::Ok && resp && resp->statusCode() >= 200 && resp->statusCode() < 300;
                    ConnectionStats::get().setPost(id, json{{"ok", ok}, {"at", std::time(nullptr)},
                        {"status", resp ? static_cast<int>(resp->statusCode()) : 0},
                        {"error", result == drogon::ReqResult::Ok ? std::string() : drogon::to_string(result)}});
                    auto hub = weak.lock();
                    if (!hub || !ok || resp->statusCode() != drogon::k200OK) return;
                    const auto op = json::parse(resp->body(), nullptr, false);
                    if (!op.is_object() || op.empty()) return;
                    const auto selfId = event.value("self_id", int64_t(0));
                    for (const auto& r : hub->relays()) if (r->selfId() == selfId) { r->quickOperation(event, op); break; }
                }, conn.timeout);
            });
        } else if (conn.type == "ws_client") {
            for (const auto& r : h->relaysFor(conn)) std::make_shared<ReverseClient>(conn, r)->connect();
        }
        if (conn.heartbeatInterval > 0 && (conn.type == "ws_server" || conn.type == "ws_client")) {
            drogon::app().getLoop()->runEvery(conn.heartbeatInterval / 1000.0, [weak, conn] {
                auto hub = weak.lock(); if (!hub) return;
                for (const auto& r : hub->relaysFor(conn)) hub->emitTo(conn.id, r->id(), r->heartbeat(conn.heartbeatInterval));
            });
        }
    }
    h->prune();
    drogon::app().getLoop()->runEvery(3600.0, [weak] { if (auto hub = weak.lock()) hub->prune(); });
}

}  // namespace catofficial::app
