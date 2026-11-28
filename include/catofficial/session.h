#pragma once

// 官方机器人的连接会话：AccessToken 获取与续期、Gateway 地址获取、WebSocket 连接、
// 鉴权（identify / resume）、心跳与断线重连。
//
// 会话只管连接本身。连上之后的事件（READY、RESUMED、业务事件）、断线与失败都通过
// Hooks 交给调用方；调用方据此维护自己的在线状态、名称缓存等。所有回调都在 drogon
// 事件循环或网络线程里触发，调用方不应在回调里阻塞。

#include "api.h"
#include "gateway_socket.h"
#include "log.h"
#include "protocol.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <nlohmann/json.hpp>
#include <trantor/net/EventLoop.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace catofficial {

class Session : public std::enable_shared_from_this<Session> {
public:
    using json = nlohmann::json;
    using LogLevel = catofficial::LogLevel;

    struct Options {
        std::string appId;
        std::string appSecret;
        bool subscribeGroupMemberEvents = false;
        std::string clientName = "CatOfficial";   // identify 里的 $os / $browser / $device
    };

    struct Hooks {
        /// 取得或续期 AccessToken 之后。client 是解析 api.bot.qq.com 后的 HTTPS 客户端，可复用。
        std::function<void(const drogon::HttpClientPtr& client)> token;
        /// Gateway READY（d 为原始数据，含 user / session_id）。心跳已由会话启动。
        std::function<void(const json& d)> ready;
        std::function<void()> resumed;
        /// 业务事件（op 0 且不是 READY / RESUMED）。
        std::function<void(const std::string& type, const json& d, const std::string& eventId)> dispatch;
        /// WebSocket 已断开；随后会自动安排重连（若未 stop）。
        std::function<void()> closed;
        /// 即将重连 Gateway。
        std::function<void()> reconnecting;
        /// 取 Token、取 Gateway 地址或连接失败；会话不再自动重试，由调用方决定。
        std::function<void(const std::string& error)> failed;
        Logger log;
    };

    Session(Options options, Hooks hooks) : options_(std::move(options)), hooks_(std::move(hooks)) {}

    /// 更换配置。AppID 变化时作废已有 Token。
    void configure(Options options) {
        std::lock_guard lock(mu_);
        if (options.appId != options_.appId) accessToken_.clear();
        options_ = std::move(options);
    }

    std::string accessToken() const { std::lock_guard lock(mu_); return accessToken_; }
    void clearAccessToken() { std::lock_guard lock(mu_); accessToken_.clear(); }

    /// 开始取 Token。成功后触发 hooks.token；连 Gateway 需另行调用 openGateway()。
    void start() {
        stopping_ = false;
        fetchAccessToken();
    }

    /// 停止计时器与 WebSocket；之后到达的回调全部丢弃。
    void stop() {
        stopping_ = true;
        if (heartbeatTimer_) { drogon::app().getLoop()->invalidateTimer(*heartbeatTimer_); heartbeatTimer_.reset(); }
        if (accessTokenTimer_) { drogon::app().getLoop()->invalidateTimer(*accessTokenTimer_); accessTokenTimer_.reset(); }
        if (gateway_) gateway_->stop();
        gateway_.reset();
    }

    bool stopping() const { return stopping_; }

    /// 取 Gateway 地址并连接。
    void openGateway() {
        auto self = shared_from_this();
        auto r = drogon::HttpRequest::newHttpRequest();
        r->setPath("/gateway/bot");
        r->addHeader("Host", api::kHost);
        r->addHeader("Authorization", "QQBot " + accessToken());
        api::httpsSend(api::kHost, r, [self](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
            try {
                if (rr != drogon::ReqResult::Ok || !resp)
                    throw std::runtime_error("获取 Gateway 地址失败（" + api::httpsFailure(rr) + "）");
                const auto body = resp->body();
                auto j = json::parse(body);
                if (resp->statusCode() >= 300 || !j.contains("url") || !j["url"].is_string()) {
                    self->log(LogLevel::warn, "QQOfficial Gateway response: HTTP " + std::to_string(static_cast<int>(resp->statusCode())) + " " + std::string(body));
                    throw std::runtime_error("获取 Gateway 地址失败（请查看后台日志中的 Gateway 响应）");
                }
                self->connectGateway(j["url"].get<std::string>());
            } catch (const std::exception& e) { self->fail(e.what()); }
        });
    }

private:
    void log(LogLevel level, const std::string& message) const { if (hooks_.log) hooks_.log(level, message); }
    void fail(const std::string& error) { if (hooks_.failed) hooks_.failed(error); }

    void fetchAccessToken() {
        auto self = shared_from_this();
        std::string appId, secret;
        { std::lock_guard lock(mu_); appId = options_.appId; secret = options_.appSecret; }
        auto r = drogon::HttpRequest::newHttpRequest();
        r->setMethod(drogon::Post);
        r->setPath("/app/getAppAccessToken");
        r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        r->addHeader("Host", api::kHost);
        r->setBody(json{{"appId", appId}, {"clientSecret", secret}}.dump());
        api::httpsSend(api::kHost, r, [self](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
            try {
                if (self->stopping_) return;
                if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) throw std::runtime_error("获取 AccessToken 失败");
                auto j = json::parse(resp->body());
                { std::lock_guard lock(self->mu_); self->accessToken_ = j.at("access_token").get<std::string>(); }
                int expiresIn = 7200;
                if (j.contains("expires_in")) {
                    const auto& v = j["expires_in"];
                    if (v.is_number_integer() || v.is_number_unsigned()) expiresIn = v.get<int>();
                    else if (v.is_string()) try { expiresIn = std::stoi(v.get<std::string>()); } catch (...) {}
                }
                self->scheduleTokenRefresh(expiresIn);
                if (self->hooks_.token) self->hooks_.token({});
            } catch (const std::exception& e) { self->fail(e.what()); }
        });
    }

    void scheduleTokenRefresh(int expiresIn) {
        if (accessTokenTimer_) drogon::app().getLoop()->invalidateTimer(*accessTokenTimer_);
        const int refreshIn = expiresIn > 300 ? expiresIn - 300 : 60;
        auto self = shared_from_this();
        accessTokenTimer_ = drogon::app().getLoop()->runAfter(static_cast<double>(refreshIn), [self] {
            if (!self->stopping_) self->fetchAccessToken();
        });
    }

    void connectGateway(const std::string& url) {
        gatewayUrl_ = url;
        const std::string prefix = "wss://";
        if (url.rfind(prefix, 0) != 0) { fail("QQ Gateway 地址格式无效"); return; }
        const auto slash = url.find('/', prefix.size());
        const std::string authority = url.substr(prefix.size(), slash == std::string::npos ? std::string::npos : slash - prefix.size());
        const auto colon = authority.rfind(':');
        const std::string host = colon == std::string::npos ? authority : authority.substr(0, colon);
        uint16_t port = 443;
        try { if (colon != std::string::npos) port = static_cast<uint16_t>(std::stoi(authority.substr(colon + 1))); }
        catch (...) { fail("QQ Gateway 端口无效"); return; }
        const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
        auto self = shared_from_this();
        // QQ's Gateway CDN requires the hostname in TLS SNI.  This transport
        // resolves through the OS, avoiding c-ares VPN/CNAME failures without
        // sacrificing SNI by connecting to a bare IP address.
        if (gateway_) gateway_->stop();
        gateway_ = std::make_shared<GatewaySocket>(host, port, path,
            [self](std::string raw) { if (!self->stopping_) self->onGateway(raw); },
            [self](const std::string& error) { if (!self->stopping_) self->fail("连接 QQ Gateway 失败：" + error); },
            [self] {
                if (self->stopping_) return;
                if (self->hooks_.closed) self->hooks_.closed();
                self->scheduleGatewayReconnect();
            });
        gateway_->start();
    }

    void onGateway(const std::string& raw) {
        try {
            auto p = json::parse(raw);
            if (p.contains("s") && !p["s"].is_null()) seq_ = protocol::asInteger(p["s"]).value_or(seq_);
            const int op = protocol::integer(p, "op", -1);
            if (op == 10) { if (!sessionId_.empty() && seq_ >= 0) resume(); else identify(); return; }
            if (op == 7) { scheduleGatewayReconnect(); return; }
            if (op != 0) return;
            const auto t = p.value("t", std::string());
            const auto d = p.value("d", json::object());
            if (t == "READY") {
                sessionId_ = d.value("session_id", std::string());
                if (hooks_.ready) hooks_.ready(d);
                beginHeartbeat(protocol::integer(d, "heartbeat_interval", 45000));
            } else if (t == "RESUMED") {
                if (hooks_.resumed) hooks_.resumed();
            } else if (hooks_.dispatch) {
                hooks_.dispatch(t, d, p.value("id", std::string()));
            }
        } catch (const std::exception& e) { log(LogLevel::warn, std::string("QQOfficial parse: ") + e.what()); }
    }

    void identify() {
        bool members; std::string client;
        { std::lock_guard lock(mu_); members = options_.subscribeGroupMemberEvents; client = options_.clientName; }
        if (gateway_) gateway_->sendText(json{{"op", 2}, {"d", {
            {"token", "QQBot " + accessToken()}, {"intents", protocol::intents(members)},
            {"shard", json::array({0, 1})},
            {"properties", {{"$os", client}, {"$browser", client}, {"$device", client}}}}}}.dump());
    }
    void resume() {
        if (gateway_) gateway_->sendText(json{{"op", 6}, {"d", {
            {"token", "QQBot " + accessToken()}, {"session_id", sessionId_}, {"seq", seq_}}}}.dump());
    }
    void scheduleGatewayReconnect() {
        if (stopping_ || gatewayUrl_.empty()) return;
        if (hooks_.reconnecting) hooks_.reconnecting();
        auto self = shared_from_this();
        drogon::app().getLoop()->runAfter(5.0, [self] { if (!self->stopping_) self->connectGateway(self->gatewayUrl_); });
    }
    void beginHeartbeat(int ms) {
        if (heartbeatTimer_) drogon::app().getLoop()->invalidateTimer(*heartbeatTimer_);
        auto self = shared_from_this();
        const int intervalMs = ms > 0 ? ms : 1000;
        heartbeatTimer_ = drogon::app().getLoop()->runEvery(static_cast<double>(intervalMs) / 1000.0, [self] {
            if (self->gateway_) self->gateway_->sendText(json{{"op", 1}, {"d", self->seq_ >= 0 ? json(self->seq_) : json(nullptr)}}.dump());
        });
    }

    mutable std::mutex mu_;
    Options options_;
    Hooks hooks_;
    std::string accessToken_;
    std::atomic<bool> stopping_{false};
    std::string sessionId_, gatewayUrl_;
    int64_t seq_ = -1;
    std::shared_ptr<GatewaySocket> gateway_;
    std::optional<trantor::TimerId> heartbeatTimer_, accessTokenTimer_;
};

}  // namespace catofficial
