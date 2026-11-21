#pragma once

// OneBot V11 的四类连接（照 NapCat 的划分）：
//   · http_server：HTTP API，/{action}
//   · ws_server：正向 WebSocket，/ 收事件并调用动作；/event 只收事件；/api 只调用动作
//   · http_client：HTTP POST 上报，响应体可做快速回复
//   · ws_client：反向 WebSocket（Universal），每个机器人一条连接，带 X-Self-ID
// 两类服务端用 IXWebSocket 监听：每个连接一个实例，可以随时启停，改端口也不用重启程序。
// drogon 只负责管理面板、头像跳转与官方 Webhook 入口。

#include "hub.h"

#include <drogon/WebSocketClient.h>
#include <drogon/drogon.h>
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

/// URL 百分号解码（+ 视为空格）。
inline std::string urlDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}

/// "/path?a=1&b=2" → path 与参数表
inline std::pair<std::string, std::map<std::string, std::string>> splitUri(const std::string& uri) {
    std::map<std::string, std::string> params;
    const auto q = uri.find('?');
    const std::string path = uri.substr(0, q);
    if (q != std::string::npos) {
        std::size_t p = q + 1;
        while (p <= uri.size()) {
            const auto amp = uri.find('&', p);
            const auto pair = uri.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
            const auto eq = pair.find('=');
            if (!pair.empty()) params[urlDecode(pair.substr(0, eq))] = eq == std::string::npos ? std::string() : urlDecode(pair.substr(eq + 1));
            if (amp == std::string::npos) break;
            p = amp + 1;
        }
    }
    return {path, params};
}

/// 访问令牌：Authorization: Bearer xxx 或参数 access_token。0 通过，401 缺失，403 不符。
inline int checkToken(const std::string& token, std::string header, const std::map<std::string, std::string>& params) {
    if (token.empty()) return 0;
    for (const char* prefix : {"Bearer ", "Token "})
        if (header.rfind(prefix, 0) == 0) { header = header.substr(std::string(prefix).size()); break; }
    if (header.empty()) if (auto it = params.find("access_token"); it != params.end()) header = it->second;
    if (header.empty()) return 401;
    return header == token ? 0 : 403;
}

inline int64_t parseSelfId(const std::string& value) {
    return !value.empty() && value.size() <= 18 && value.find_first_not_of("0123456789") == std::string::npos ? std::stoll(value) : 0;
}

/// 各连接的当前状态，供管理面板显示。
struct ConnectionStats {
    std::mutex mu;
    std::map<std::string, int> wsClients;                          // 连接 id → 在线客户端数
    std::map<std::string, nlohmann::json> reverse;                 // 连接 id → {机器人 id → {connected, since, error}}
    std::map<std::string, nlohmann::json> posts;                   // 连接 id → {ok, status, at, error}
    std::map<std::string, std::string> listenErrors;               // 连接 id → 监听失败原因
    static ConnectionStats& get() { static ConnectionStats s; return s; }
    void clientDelta(const std::string& conn, int delta) { std::lock_guard lock(mu); wsClients[conn] += delta; }
    void setReverse(const std::string& conn, const std::string& bot, nlohmann::json state) { std::lock_guard lock(mu); reverse[conn][bot] = std::move(state); }
    void setPost(const std::string& conn, nlohmann::json state) { std::lock_guard lock(mu); posts[conn] = std::move(state); }
    void setListenError(const std::string& conn, const std::string& error) { std::lock_guard lock(mu); if (error.empty()) listenErrors.erase(conn); else listenErrors[conn] = error; }
    void forget(const std::string& conn) { std::lock_guard lock(mu); wsClients.erase(conn); reverse.erase(conn); posts.erase(conn); listenErrors.erase(conn); }
    nlohmann::json snapshot(const std::string& conn) {
        std::lock_guard lock(mu);
        nlohmann::json out = nlohmann::json::object();
        if (auto it = wsClients.find(conn); it != wsClients.end()) out["clients"] = it->second;
        if (auto it = reverse.find(conn); it != reverse.end()) out["reverse"] = it->second;
        if (auto it = posts.find(conn); it != posts.end()) out["last_post"] = it->second;
        if (auto it = listenErrors.find(conn); it != listenErrors.end()) out["error"] = it->second;
        return out;
    }
};

class Server;

/// 解析 {action, params, echo} 并执行，响应原样带回 echo。
inline void dispatchAction(const std::shared_ptr<Hub>& hub, const ConnectionConfig& conn, int64_t selfHint, const std::string& message,
                           std::function<void(const std::string&)> reply) {
    using json = nlohmann::json;
    const auto request = json::parse(message, nullptr, false);
    if (!request.is_object()) { reply(Relay::failed(1400, "请求不是 JSON 对象").dump()); return; }
    const json echo = request.contains("echo") ? request["echo"] : json();
    hub->handle(conn, selfHint, request.value("action", std::string()), request.value("params", json::object()),
        [reply, echo](json response) {
            if (!echo.is_null()) response["echo"] = echo;
            reply(response.dump());
        });
}

// ── 服务端连接（IXWebSocket）────────────────────────────────

class Listener {
public:
    using json = nlohmann::json;
    Listener(std::shared_ptr<Hub> hub, ConnectionConfig conn) : hub_(std::move(hub)), conn_(std::move(conn)) {}
    virtual ~Listener() = default;
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    /// 令牌、消息格式、服务的机器人等可以就地更新；地址与端口变了要换新实例。
    void update(const ConnectionConfig& conn) { std::lock_guard lock(mu_); conn_ = conn; }
    ConnectionConfig conn() const { std::lock_guard lock(mu_); return conn_; }
    bool sameAddress(const ConnectionConfig& conn) const { auto c = this->conn(); return c.type == conn.type && c.host == conn.host && c.port == conn.port; }

protected:
    std::shared_ptr<Hub> hub_;
    mutable std::mutex mu_;
    ConnectionConfig conn_;
};

class WsListener : public Listener, public std::enable_shared_from_this<WsListener> {
public:
    using Listener::Listener;
    ~WsListener() override { stop(); }

    bool start(std::string& error) override {
        const auto c = conn();
        server_ = std::make_unique<ix::WebSocketServer>(c.port, c.host, ix::SocketServer::kDefaultTcpBacklog, 256);
        server_->disablePerMessageDeflate();
        std::weak_ptr<WsListener> weak = weak_from_this();
        server_->setOnConnectionCallback([weak](std::weak_ptr<ix::WebSocket> socket, std::shared_ptr<ix::ConnectionState>) {
            auto ws = socket.lock(); auto self = weak.lock();
            if (!ws || !self) return;
            auto state = std::make_shared<Client>();
            ws->setOnMessageCallback([weak, socket, state](const ix::WebSocketMessagePtr& msg) {
                if (auto self = weak.lock()) self->onMessage(socket, state, msg);
            });
        });
        const auto result = server_->listen();
        if (!result.first) { error = result.second; server_.reset(); return false; }
        server_->start();
        return true;
    }

    void stop() override {
        if (!server_) return;
        server_->stop();
        server_.reset();
    }

private:
    struct Client { bool open = false; bool api = false; bool events = false; int sink = 0; int64_t selfHint = 0; };

    void onMessage(const std::weak_ptr<ix::WebSocket>& socket, const std::shared_ptr<Client>& client, const ix::WebSocketMessagePtr& msg) {
        auto ws = socket.lock();
        if (!ws) return;
        if (msg->type == ix::WebSocketMessageType::Open) {
            const auto c = conn();
            const auto [path, params] = splitUri(msg->openInfo.uri);
            auto header = [&](const char* name) { auto it = msg->openInfo.headers.find(name); return it == msg->openInfo.headers.end() ? std::string() : it->second; };
            if (checkToken(c.token, header("Authorization"), params) != 0) { ws->close(1008, "unauthorized"); return; }
            client->open = true;
            client->api = path != "/event";
            client->events = path != "/api";
            client->selfHint = parseSelfId(header("X-Self-ID"));
            if (auto it = params.find("self_id"); !client->selfHint && it != params.end()) client->selfHint = parseSelfId(it->second);
            ConnectionStats::get().clientDelta(c.id, 1);
            if (client->events) {
                client->sink = hub_->addSink(c, c.bots, [socket](const json& event) { if (auto w = socket.lock()) w->sendText(event.dump()); });
                for (const auto& r : hub_->relaysFor(c)) ws->sendText(r->lifecycle("connect").dump());
            }
        } else if (msg->type == ix::WebSocketMessageType::Message) {
            if (!client->open || !client->api) return;
            dispatchAction(hub_, conn(), client->selfHint, msg->str, [socket](const std::string& response) { if (auto w = socket.lock()) w->sendText(response); });
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            if (!client->open) return;
            client->open = false;
            if (client->sink) hub_->removeSink(client->sink);
            ConnectionStats::get().clientDelta(conn().id, -1);
        }
    }

    std::unique_ptr<ix::WebSocketServer> server_;
};

class HttpListener : public Listener, public std::enable_shared_from_this<HttpListener> {
public:
    using Listener::Listener;
    ~HttpListener() override { stop(); }

    bool start(std::string& error) override {
        const auto c = conn();
        server_ = std::make_unique<ix::HttpServer>(c.port, c.host, ix::SocketServer::kDefaultTcpBacklog, 256);
        std::weak_ptr<HttpListener> weak = weak_from_this();
        server_->setOnConnectionCallback([weak](ix::HttpRequestPtr request, std::shared_ptr<ix::ConnectionState>) -> ix::HttpResponsePtr {
            auto self = weak.lock();
            if (!self) return std::make_shared<ix::HttpResponse>(503, "Service Unavailable", ix::HttpErrorCode::Ok, ix::WebSocketHttpHeaders{}, "");
            return self->handle(*request);
        });
        const auto result = server_->listen();
        if (!result.first) { error = result.second; server_.reset(); return false; }
        server_->start();
        return true;
    }

    void stop() override {
        if (!server_) return;
        server_->stop();
        server_.reset();
    }

private:
    static ix::HttpResponsePtr respond(int status, const json& body) {
        ix::WebSocketHttpHeaders headers;
        headers["Content-Type"] = "application/json; charset=utf-8";
        return std::make_shared<ix::HttpResponse>(status, status == 200 ? "OK" : "Error", ix::HttpErrorCode::Ok, headers, body.dump());
    }

    ix::HttpResponsePtr handle(const ix::HttpRequest& request) {
        const auto c = conn();
        const auto [path, query] = splitUri(request.uri);
        auto header = [&](const char* name) { auto it = request.headers.find(name); return it == request.headers.end() ? std::string() : it->second; };

        // 头像跳转：/avatar/user/{user_id}?s=640
        if (request.method == "GET" && path.rfind("/avatar/user/", 0) == 0) {
            const auto n = parseSelfId(path.substr(13));
            int size = 640;
            if (auto it = query.find("s"); it != query.end()) size = static_cast<int>(parseSelfId(it->second));
            if (size != 40 && size != 100 && size != 140 && size != 640) size = 640;
            std::string url;
            if (n) for (const auto& r : hub_->relays()) { url = r->userAvatar(n, size); if (!url.empty()) break; }
            if (url.empty()) return std::make_shared<ix::HttpResponse>(404, "Not Found", ix::HttpErrorCode::Ok, ix::WebSocketHttpHeaders{}, "");
            ix::WebSocketHttpHeaders headers;
            headers["Location"] = url;
            return std::make_shared<ix::HttpResponse>(302, "Found", ix::HttpErrorCode::Ok, headers, "");
        }

        if (const int code = checkToken(c.token, header("Authorization"), query))
            return respond(code, Relay::failed(code, code == 401 ? "缺少 access_token" : "access_token 不正确"));
        if (path.size() < 2 || path.find('/', 1) != std::string::npos) return respond(404, Relay::failed(1404, "未知的路径"));
        const auto action = path.substr(1);

        json params = json::object();
        const auto type = header("Content-Type");
        if (request.method == "POST" && !request.body.empty()) {
            if (type.find("application/x-www-form-urlencoded") != std::string::npos) {
                for (const auto& [k, v] : splitUri("?" + request.body).second) params[k] = v;
            } else {
                params = json::parse(request.body, nullptr, false);
                if (!params.is_object()) return respond(400, Relay::failed(1400, "请求体不是 JSON 对象"));
            }
        }
        for (const auto& [key, value] : query) {
            if (key == "access_token" || params.contains(key)) continue;
            // 查询串与表单里只有字符串；布尔参数按 OneBot 惯例转换。
            if (value == "true" || value == "false") params[key] = value == "true";
            else params[key] = value;
        }
        // IXWebSocket 的 HTTP 回调是同步的：每个连接有自己的线程，这里等动作完成再返回。
        auto promise = std::make_shared<std::promise<json>>();
        auto future = promise->get_future();
        hub_->handle(c, parseSelfId(header("X-Self-ID")), action, params, [promise](json response) {
            try { promise->set_value(std::move(response)); } catch (...) {}
        });
        if (future.wait_for(std::chrono::seconds(90)) != std::future_status::ready) return respond(504, Relay::failed(100, "动作超时"));
        const auto response = future.get();
        return respond(response.value("retcode", 0) == 1404 ? 404 : 200, response);
    }

    std::unique_ptr<ix::HttpServer> server_;
};

// ── 反向 WebSocket ──────────────────────────────────────────

/// 反向 WebSocket：一个机器人一条连接。
class ReverseClient : public std::enable_shared_from_this<ReverseClient> {
public:
    ReverseClient(std::shared_ptr<Hub> hub, ConnectionConfig config, std::shared_ptr<Relay> relay)
        : hub_(std::move(hub)), config_(std::move(config)), relay_(std::move(relay)) {}

    void connect() {
        if (stopped_) return;
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
            dispatchAction(self->hub_, self->scoped(), self->relay_->selfId(), message, [weak](const std::string& response) {
                if (auto c = weak.lock()) if (auto conn = c->getConnection(); conn && conn->connected()) conn->send(response);
            });
        });
        client_->setConnectionClosedHandler([self](const drogon::WebSocketClientPtr&) {
            if (self->stopped_) return;
            self->state(false, "连接已断开");
            self->retry();
        });
        client_->connectToServer(req, [self](drogon::ReqResult result, const drogon::HttpResponsePtr&, const drogon::WebSocketClientPtr& client) {
            if (self->stopped_) { client->stop(); return; }
            if (result != drogon::ReqResult::Ok) { self->state(false, drogon::to_string(result)); self->retry(); return; }
            self->state(true, {});
            std::weak_ptr<drogon::WebSocketClient> weak = client;
            self->sink_ = self->hub_->addSink(self->config_, {self->relay_->id()}, [weak](const nlohmann::json& event) {
                if (auto c = weak.lock()) if (auto conn = c->getConnection(); conn && conn->connected()) conn->send(event.dump());
            });
            if (auto conn = client->getConnection()) conn->send(self->relay_->lifecycle("connect").dump());
        });
    }

    void stop() {
        stopped_ = true;
        if (sink_) { hub_->removeSink(sink_); sink_ = 0; }
        if (client_) client_->stop();
    }

    const std::shared_ptr<Relay>& relay() const { return relay_; }

private:
    ConnectionConfig scoped() const { auto c = config_; c.bots = {relay_->id()}; return c; }
    void state(bool connected, const std::string& error) {
        ConnectionStats::get().setReverse(config_.id, relay_->id(),
            nlohmann::json{{"connected", connected}, {"error", error}, {"since", std::time(nullptr)}});
    }
    void retry() {
        if (sink_) { hub_->removeSink(sink_); sink_ = 0; }
        if (stopped_) return;
        std::weak_ptr<ReverseClient> weak = shared_from_this();
        drogon::app().getLoop()->runAfter(std::max(config_.reconnectInterval, 500) / 1000.0, [weak] { if (auto self = weak.lock()) self->connect(); });
    }

    std::shared_ptr<Hub> hub_;
    ConnectionConfig config_;
    std::shared_ptr<Relay> relay_;
    drogon::WebSocketClientPtr client_;
    std::atomic<bool> stopped_{false};
    int sink_ = 0;
};

// ── 连接管理 ────────────────────────────────────────────────

inline bool sameConnection(const ConnectionConfig& a, const ConnectionConfig& b) {
    return std::tie(a.id, a.name, a.enabled, a.type, a.host, a.port, a.url, a.token, a.messageFormat, a.reportSelf,
                    a.heartbeatInterval, a.reconnectInterval, a.timeout, a.bots)
        == std::tie(b.id, b.name, b.enabled, b.type, b.host, b.port, b.url, b.token, b.messageFormat, b.reportSelf,
                    b.heartbeatInterval, b.reconnectInterval, b.timeout, b.bots);
}

class Server {
public:
    using json = nlohmann::json;

    static std::shared_ptr<Hub>& hub() { static std::shared_ptr<Hub> h; return h; }
    /// 管理面板的端口与单层路径的静态文件（如 /favicon.svg），由 WebUi 设置。
    static uint16_t& panelPort() { static uint16_t p = 0; return p; }
    static std::function<drogon::HttpResponsePtr(const std::string&)>& panelFile() {
        static std::function<drogon::HttpResponsePtr(const std::string&)> f; return f;
    }

    /// drogon 部分：官方 Webhook 入口与面板的单层静态文件。端口在启动时确定。
    static void setup(std::shared_ptr<Hub> h) {
        hub() = std::move(h);
        const auto cfg = hub()->config();
        if (cfg.webhookPort) drogon::app().addListener(cfg.webhookHost, cfg.webhookPort);
        drogon::app().registerHandler("/{file}",
            [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& file) {
                if (panelFile() && panelPort() && req->getLocalAddr().toPort() == panelPort()) { callback(panelFile()(file)); return; }
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k404NotFound);
                callback(resp);
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

    /// 启动时调用：按配置建立全部连接，并定时清理消息索引。
    static void startClients() {
        applyConnections(hub()->config().connections);
        std::weak_ptr<Hub> weak = hub();
        hub()->prune();
        drogon::app().getLoop()->runEvery(3600.0, [weak] { if (auto h = weak.lock()) h->prune(); });
    }

    /// 让运行中的连接与配置一致：新增的建立、删除或停用的关闭、改过的重建或就地更新。
    /// 机器人变了时，反向 WebSocket 会按新的机器人列表重建。
    static void applyConnections(const std::vector<ConnectionConfig>& wanted) {
        std::lock_guard lock(mu());
        auto h = hub();
        auto& rts = runtimes();
        // 先关掉不再需要的
        for (auto it = rts.begin(); it != rts.end();) {
            const auto found = std::find_if(wanted.begin(), wanted.end(), [&](const ConnectionConfig& c) { return c.id == it->first; });
            if (found == wanted.end() || !found->enabled || !reusable(it->second, *found)) {
                teardown(it->second);
                ConnectionStats::get().forget(it->first);
                it = rts.erase(it);
            } else ++it;
        }
        for (const auto& conn : wanted) {
            if (!conn.enabled) continue;
            auto it = rts.find(conn.id);
            if (it != rts.end()) {
                // 地址没变的服务端：就地更新令牌、格式与机器人范围
                if (it->second.listener) it->second.listener->update(conn);
                if (!sameConnection(it->second.conn, conn)) {
                    h->updateSinks(conn, conn.type == "ws_client");
                    it->second.conn = conn;
                    restartHeartbeat(it->second);
                }
                continue;
            }
            Runtime rt;
            rt.conn = conn;
            if (conn.server()) {
                std::shared_ptr<Listener> listener;
                if (conn.type == "ws_server") listener = std::make_shared<WsListener>(h, conn);
                else listener = std::make_shared<HttpListener>(h, conn);
                std::string error;
                if (listener->start(error)) {
                    rt.listener = listener;
                    ConnectionStats::get().setListenError(conn.id, {});
                    say(LogLevel::info, "OneBot 连接「" + conn.name + "」开始监听 " + conn.host + ":" + std::to_string(conn.port));
                } else {
                    ConnectionStats::get().setListenError(conn.id, error);
                    say(LogLevel::error, "OneBot 连接「" + conn.name + "」无法监听 " + conn.host + ":" + std::to_string(conn.port) + "：" + error);
                }
            } else if (conn.type == "http_client") {
                rt.sink = addPostSink(h, conn);
            } else if (conn.type == "ws_client") {
                for (const auto& r : h->relaysFor(conn)) {
                    auto client = std::make_shared<ReverseClient>(h, conn, r);
                    client->connect();
                    rt.reverse.push_back(client);
                }
            }
            restartHeartbeat(rt);
            rts[conn.id] = std::move(rt);
        }
    }

    /// 面板显示用：服务端连接是否在监听。
    static bool listening(const std::string& connId) {
        std::lock_guard lock(mu());
        auto it = runtimes().find(connId);
        return it != runtimes().end() && it->second.listener != nullptr;
    }

private:
    struct Runtime {
        ConnectionConfig conn;
        std::shared_ptr<Listener> listener;
        int sink = 0;
        std::vector<std::shared_ptr<ReverseClient>> reverse;
        std::optional<trantor::TimerId> heartbeat;
    };
    static std::mutex& mu() { static std::mutex m; return m; }
    static std::map<std::string, Runtime>& runtimes() { static std::map<std::string, Runtime> r; return r; }

    static void say(LogLevel level, const std::string& message) { LogBuffer::get().log(level, message); }

    /// 已有的运行实例能不能沿用到新配置。
    static bool reusable(const Runtime& rt, const ConnectionConfig& next) {
        if (rt.conn.type != next.type) return false;
        if (next.server()) return rt.listener && rt.listener->sameAddress(next);
        if (next.type == "http_client") return sameConnection(rt.conn, next);
        // 反向 WS：配置相同且服务的机器人实例没变（机器人被修改会换新实例）
        if (!sameConnection(rt.conn, next)) return false;
        const auto relays = hub()->relaysFor(next);
        if (relays.size() != rt.reverse.size()) return false;
        for (std::size_t i = 0; i < relays.size(); ++i) if (relays[i] != rt.reverse[i]->relay()) return false;
        return true;
    }

    static void teardown(Runtime& rt) {
        if (rt.listener) rt.listener->stop();
        if (rt.sink) hub()->removeSink(rt.sink);
        for (auto& c : rt.reverse) c->stop();
        if (rt.heartbeat) drogon::app().getLoop()->invalidateTimer(*rt.heartbeat);
        rt.heartbeat.reset();
    }

    static void restartHeartbeat(Runtime& rt) {
        if (rt.heartbeat) { drogon::app().getLoop()->invalidateTimer(*rt.heartbeat); rt.heartbeat.reset(); }
        const auto conn = rt.conn;
        if (conn.heartbeatInterval <= 0 || (conn.type != "ws_server" && conn.type != "ws_client")) return;
        std::weak_ptr<Hub> weak = hub();
        rt.heartbeat = drogon::app().getLoop()->runEvery(conn.heartbeatInterval / 1000.0, [weak, conn] {
            auto h = weak.lock(); if (!h) return;
            for (const auto& r : h->relaysFor(conn)) h->emitTo(conn.id, r->id(), r->heartbeat(conn.heartbeatInterval));
        });
    }

    static int addPostSink(const std::shared_ptr<Hub>& h, const ConnectionConfig& conn) {
        const auto endpoint = splitUrl(conn.url);
        auto client = drogon::HttpClient::newHttpClient(endpoint.origin);
        std::weak_ptr<Hub> weak = h;
        return h->addSink(conn, conn.bots, [client, endpoint, conn, weak](const json& event) {
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
    }
};

}  // namespace catofficial::app
