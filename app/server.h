#pragma once

// OneBot V11 的四种连接方式：
//   · 正向 WebSocket（/、/api、/event）与 HTTP API（/{action}）—— CatOfficial 当服务端；
//   · HTTP POST 上报与反向 WebSocket（Universal）—— CatOfficial 主动连出去。
// 另提供头像跳转 /avatar/user/{user_id} 与（Webhook 模式下的）官方回调入口。

#include "relay.h"

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

/// 各连接方式的当前状态，供管理面板显示。
struct ConnectionStats {
    std::atomic<int> wsClients{0};
    std::mutex mu;
    std::map<std::string, nlohmann::json> reverse;   // url → {connected, since, error}
    std::map<std::string, nlohmann::json> posts;     // url → {ok, status, at}
    static ConnectionStats& get() { static ConnectionStats s; return s; }
    void setReverse(const std::string& url, nlohmann::json state) { std::lock_guard lock(mu); reverse[url] = std::move(state); }
    void setPost(const std::string& url, nlohmann::json state) { std::lock_guard lock(mu); posts[url] = std::move(state); }
};

class Server {
public:
    static std::shared_ptr<Relay>& relay() { static std::shared_ptr<Relay> r; return r; }
    static std::set<uint16_t>& httpPorts() { static std::set<uint16_t> p; return p; }
    static std::set<uint16_t>& wsPorts() { static std::set<uint16_t> p; return p; }
    /// 管理面板的端口与单层路径的静态文件（如 /favicon.svg），由 WebUi 设置。
    static uint16_t& panelPort() { static uint16_t p = 0; return p; }
    static std::function<drogon::HttpResponsePtr(const std::string&)>& panelFile() {
        static std::function<drogon::HttpResponsePtr(const std::string&)> f; return f;
    }

    /// 校验 access_token：Authorization: Bearer xxx 或查询参数 access_token。0 通过，401 缺失，403 不符。
    static int authorize(const drogon::HttpRequestPtr& req) {
        const auto& token = relay()->config().accessToken;
        if (token.empty()) return 0;
        std::string given = req->getHeader("Authorization");
        for (const char* prefix : {"Bearer ", "Token "})
            if (given.rfind(prefix, 0) == 0) { given = given.substr(std::string(prefix).size()); break; }
        if (given.empty()) given = req->getParameter("access_token");
        if (given.empty()) return 401;
        return given == token ? 0 : 403;
    }

    static void setup(std::shared_ptr<Relay> r) {
        relay() = std::move(r);
        const auto& cfg = relay()->config();
        std::set<std::pair<std::string, uint16_t>> bound;
        for (const auto& l : cfg.listeners) {
            (l.type == "http" ? httpPorts() : wsPorts()).insert(l.port);
            if (bound.insert({l.host, l.port}).second) drogon::app().addListener(l.host, l.port);
        }
        if (cfg.webhook && bound.insert({cfg.webhookHost, cfg.webhookPort}).second)
            drogon::app().addListener(cfg.webhookHost, cfg.webhookPort);

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
                const auto url = n ? relay()->userAvatar(*n, size) : std::string();
                if (url.empty()) { auto resp = drogon::HttpResponse::newHttpResponse(); resp->setStatusCode(drogon::k404NotFound); callback(resp); return; }
                callback(drogon::HttpResponse::newRedirectionResponse(url, drogon::k302Found));
            }, {drogon::Get});

        if (cfg.webhook) {
            drogon::app().registerHandler(cfg.webhookPath,
                [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    if (req->getLocalAddr().toPort() != relay()->config().webhookPort) { resp->setStatusCode(drogon::k404NotFound); callback(resp); return; }
                    const auto appId = req->getHeader("X-Bot-Appid");
                    if (!appId.empty() && appId != relay()->config().appId) { resp->setStatusCode(drogon::k404NotFound); callback(resp); return; }
                    const auto reply = relay()->acceptWebhook(req->getHeader("X-Signature-Timestamp"),
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
    using json = nlohmann::json;

    static json requestParams(const drogon::HttpRequestPtr& req) {
        json params = json::object();
        const auto type = req->getContentType();
        if (req->method() == drogon::Post && type == drogon::CT_APPLICATION_JSON) {
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
        auto respond = [callback](int status, const json& body) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(body.dump());
            callback(resp);
        };
        if (panelFile() && panelPort() && req->getLocalAddr().toPort() == panelPort()) { callback(panelFile()(action)); return; }
        if (!httpPorts().count(req->getLocalAddr().toPort())) { respond(404, Relay::failed(1404, "此端口未开启 HTTP API")); return; }
        if (const int code = authorize(req)) { respond(code, Relay::failed(code, code == 401 ? "缺少 access_token" : "access_token 不正确")); return; }
        const auto params = requestParams(req);
        if (params.is_null()) { respond(400, Relay::failed(1400, "请求体不是 JSON 对象")); return; }
        relay()->handle(action, params, [respond](json response) {
            const bool unsupported = response.value("retcode", 0) == 1404;
            respond(unsupported ? 404 : 200, response);
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

    struct Context { bool api = false; bool events = false; int sink = 0; bool counted = false; };

    void handleNewConnection(const drogon::HttpRequestPtr& req, const drogon::WebSocketConnectionPtr& conn) override {
        auto relay = Server::relay();
        if (!Server::wsPorts().count(req->getLocalAddr().toPort()) || Server::authorize(req) != 0) {
            conn->shutdown(drogon::CloseCode::kViolation, "unauthorized");
            return;
        }
        auto ctx = std::make_shared<Context>();
        ctx->counted = true;
        ++ConnectionStats::get().wsClients;
        const auto path = req->path();
        ctx->api = path != "/event";
        ctx->events = path != "/api";
        if (ctx->events) {
            std::weak_ptr<drogon::WebSocketConnection> weak = conn;
            ctx->sink = relay->addSink([weak](const json& event) { if (auto c = weak.lock()) if (c->connected()) c->send(event.dump()); });
            conn->send(relay->lifecycle("connect").dump());
        }
        conn->setContext(ctx);
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message, const drogon::WebSocketMessageType& type) override {
        if (type != drogon::WebSocketMessageType::Text) return;
        auto ctx = conn->getContext<Context>();
        if (!ctx || !ctx->api) return;
        dispatchAction(message, [conn](const std::string& response) { if (conn->connected()) conn->send(response); });
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
        auto ctx = conn->getContext<Context>();
        if (!ctx) return;
        if (ctx->sink) Server::relay()->removeSink(ctx->sink);
        if (ctx->counted) { ctx->counted = false; --ConnectionStats::get().wsClients; }
    }

    /// 解析 {action, params, echo} 并执行，响应原样带回 echo。
    static void dispatchAction(const std::string& message, std::function<void(const std::string&)> reply) {
        using json = nlohmann::json;
        const auto request = json::parse(message, nullptr, false);
        if (!request.is_object()) { reply(Relay::failed(1400, "请求不是 JSON 对象").dump()); return; }
        const json echo = request.contains("echo") ? request["echo"] : json();
        Server::relay()->handle(request.value("action", std::string()), request.value("params", json::object()),
            [reply, echo](json response) {
                if (!echo.is_null()) response["echo"] = echo;
                reply(response.dump());
            });
    }
};

class ReverseClient : public std::enable_shared_from_this<ReverseClient> {
public:
    explicit ReverseClient(ReverseWs config) : config_(std::move(config)) {}

    void connect() {
        auto relay = Server::relay();
        auto self = shared_from_this();
        // 还不知道 self_id（机器人尚未上线）时稍后再连，应用端靠 X-Self-ID 区分账号。
        if (relay->selfId() == 0) {
            ConnectionStats::get().setReverse(config_.url, json{{"connected", false}, {"error", "等待机器人上线"}, {"since", std::time(nullptr)}});
            retry();
            return;
        }
        const auto endpoint = splitUrl(config_.url);
        client_ = drogon::WebSocketClient::newWebSocketClient(endpoint.origin);
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath(endpoint.path);
        req->addHeader("X-Self-ID", std::to_string(relay->selfId()));
        req->addHeader("X-Client-Role", "Universal");
        req->addHeader("User-Agent", std::string("CatOfficial/") + kVersion);
        if (!relay->config().accessToken.empty()) req->addHeader("Authorization", "Bearer " + relay->config().accessToken);
        client_->setMessageHandler([self](std::string&& message, const drogon::WebSocketClientPtr& client, const drogon::WebSocketMessageType& type) {
            if (type != drogon::WebSocketMessageType::Text) return;
            std::weak_ptr<drogon::WebSocketClient> weak = client;
            OneBotSocket::dispatchAction(message, [weak](const std::string& response) {
                if (auto c = weak.lock()) if (auto conn = c->getConnection(); conn && conn->connected()) conn->send(response);
            });
        });
        client_->setConnectionClosedHandler([self](const drogon::WebSocketClientPtr&) {
            ConnectionStats::get().setReverse(self->config_.url, json{{"connected", false}, {"error", "连接已断开"}, {"since", std::time(nullptr)}});
            self->retry();
        });
        client_->connectToServer(req, [self](drogon::ReqResult result, const drogon::HttpResponsePtr&, const drogon::WebSocketClientPtr& client) {
            if (result != drogon::ReqResult::Ok) {
                ConnectionStats::get().setReverse(self->config_.url, json{{"connected", false}, {"error", drogon::to_string(result)}, {"since", std::time(nullptr)}});
                self->retry();
                return;
            }
            ConnectionStats::get().setReverse(self->config_.url, json{{"connected", true}, {"since", std::time(nullptr)}});
            auto relay = Server::relay();
            std::weak_ptr<drogon::WebSocketClient> weak = client;
            self->sink_ = relay->addSink([weak](const json& event) {
                if (auto c = weak.lock()) if (auto conn = c->getConnection(); conn && conn->connected()) conn->send(event.dump());
            });
            if (auto conn = client->getConnection()) conn->send(relay->lifecycle("connect").dump());
        });
    }

private:
    using json = nlohmann::json;
    void detach() { if (sink_) { Server::relay()->removeSink(sink_); sink_ = 0; } }
    void retry() {
        detach();
        auto self = shared_from_this();
        drogon::app().getLoop()->runAfter(std::max(config_.reconnectInterval, 500) / 1000.0, [self] { self->connect(); });
    }

    ReverseWs config_;
    drogon::WebSocketClientPtr client_;
    int sink_ = 0;
};

inline void Server::startClients() {
    auto r = relay();
    const auto& cfg = r->config();
    for (const auto& post : cfg.httpPost) {
        const auto endpoint = splitUrl(post.url);
        auto client = drogon::HttpClient::newHttpClient(endpoint.origin);
        std::weak_ptr<Relay> weak = r;
        r->addSink([client, endpoint, post, weak](const json& event) {
            auto relay = weak.lock(); if (!relay) return;
            auto req = drogon::HttpRequest::newHttpRequest();
            req->setMethod(drogon::Post);
            req->setPath(endpoint.path);
            req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            const auto body = event.dump();
            req->setBody(body);
            req->addHeader("X-Self-ID", std::to_string(event.value("self_id", int64_t(0))));
            req->addHeader("User-Agent", std::string("CatOfficial/") + kVersion);
            if (!post.secret.empty()) req->addHeader("X-Signature", "sha1=" + hmacSha1Hex(post.secret, body));
            const auto url = post.url;
            client->sendRequest(req, [weak, event, url](drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
                const bool ok = result == drogon::ReqResult::Ok && resp && resp->statusCode() >= 200 && resp->statusCode() < 300;
                ConnectionStats::get().setPost(url, json{{"ok", ok}, {"at", std::time(nullptr)},
                    {"status", resp ? static_cast<int>(resp->statusCode()) : 0},
                    {"error", result == drogon::ReqResult::Ok ? std::string() : drogon::to_string(result)}});
                auto relay = weak.lock();
                if (!relay || result != drogon::ReqResult::Ok || !resp || resp->statusCode() != drogon::k200OK) return;
                const auto op = json::parse(resp->body(), nullptr, false);
                if (op.is_object() && !op.empty()) relay->quickOperation(event, op);
            }, post.timeout);
        });
    }
    for (const auto& ws : cfg.wsReverse) std::make_shared<ReverseClient>(ws)->connect();
    if (cfg.heartbeatInterval > 0) {
        std::weak_ptr<Relay> weak = r;
        drogon::app().getLoop()->runEvery(cfg.heartbeatInterval / 1000.0, [weak] { if (auto relay = weak.lock()) relay->emit(relay->heartbeat()); });
    }
    std::weak_ptr<Relay> weak = r;
    r->prune();
    drogon::app().getLoop()->runEvery(3600.0, [weak] { if (auto relay = weak.lock()) relay->prune(); });
}

}  // namespace catofficial::app
