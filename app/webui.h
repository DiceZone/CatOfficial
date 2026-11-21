#pragma once

// 管理面板的后端：静态前端文件与 /api/webui/* 接口。只在 webui.port 上响应，
// 与 OneBot 的端口分开。登录用配置里的密码，会话存在内存里（重启后需重新登录）。

#include "config.h"
#include "paths.h"
#include "relay.h"
#include "server.h"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace catofficial::app {

class WebUi {
public:
    using json = nlohmann::json;
    using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

    static std::atomic<bool>& restartRequested() { static std::atomic<bool> flag{false}; return flag; }

    static std::string randomHex(std::size_t bytes) {
        std::string raw(bytes, '\0');
        RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), static_cast<int>(bytes));
        static const char kHex[] = "0123456789abcdef";
        std::string out;
        for (unsigned char c : raw) { out += kHex[c >> 4]; out += kHex[c & 0xF]; }
        return out;
    }

    /// 读配置文件原文（保留未知字段），首次启动时生成面板密码并写回。返回是否新生成了密码。
    static bool ensurePassword(const std::filesystem::path& configPath, Config& config) {
        if (!config.webuiEnabled || !config.webuiPassword.empty()) return false;
        std::ifstream in(configPath, std::ios::binary);
        auto raw = json::parse(in, nullptr, false);
        if (!raw.is_object()) return false;
        config.webuiPassword = randomHex(6);
        raw["webui"]["password"] = config.webuiPassword;
        std::ofstream(configPath, std::ios::binary) << raw.dump(2) << "\n";
        return true;
    }

    static void setup(std::filesystem::path configPath, std::shared_ptr<Relay> relay) {
        auto& s = state();
        s.configPath = std::move(configPath);
        s.relay = std::move(relay);
        const auto& cfg = s.relay->config();
        if (!cfg.webuiEnabled) return;
        s.root = fromUtf8(cfg.webuiRoot);
        if (s.root.is_relative()) s.root = s.configPath.parent_path() / s.root;
        drogon::app().addListener(cfg.webuiHost, cfg.webuiPort);
        Server::panelPort() = cfg.webuiPort;
        Server::panelFile() = [](const std::string& file) { return staticFile(file); };

        auto& app = drogon::app();
        app.registerHandler("/", [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!onPanel(req)) { cb(notFound()); return; }
            cb(staticFile("index.html"));
        }, {drogon::Get});
        app.registerHandler("/assets/{file}", [](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string& file) {
            cb(onPanel(req) ? staticFile("assets/" + file) : notFound());
        }, {drogon::Get});
        app.registerHandler("/licenses/{file}", [](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string& file) {
            cb(onPanel(req) ? staticFile("licenses/" + file) : notFound());
        }, {drogon::Get});

        route("/api/webui/login", drogon::Post, false, [](const drogon::HttpRequestPtr& req, const json& body, Callback&& cb) { login(req, body, std::move(cb)); });
        route("/api/webui/logout", drogon::Post, false, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            { std::lock_guard lock(state().mu); state().sessions.erase(req->getCookie(kCookie)); }
            auto resp = jsonResponse(200, json{{"ok", true}});
            drogon::Cookie cookie(kCookie, "");
            cookie.setPath("/"); cookie.setMaxAge(0);
            resp->addCookie(cookie);
            cb(resp);
        });
        route("/api/webui/session", drogon::Get, false, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            cb(jsonResponse(200, json{{"authenticated", authenticated(req)}}));
        });
        route("/api/webui/status", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) { cb(jsonResponse(200, status())); });
        route("/api/webui/config", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) { cb(readConfig()); });
        route("/api/webui/config", drogon::Put, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) { cb(writeConfig(body)); });
        route("/api/webui/restart", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            restartRequested() = true;
            drogon::app().getLoop()->runAfter(0.5, [] { drogon::app().quit(); });
            cb(jsonResponse(200, json{{"ok", true}}));
        });
        route("/api/webui/bindings", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            json list = json::array();
            for (const auto& i : state().relay->store()->boundIdentities()) list.push_back(identityJson(i));
            cb(jsonResponse(200, list));
        });
        route("/api/webui/bindings", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            json result;
            state().relay->handle("set_identity_binding", body, [&](json r) { result = std::move(r); });
            const bool ok = result.value("status", std::string()) == "ok";
            cb(jsonResponse(ok ? 200 : 400, ok ? json{{"ok", true}} : json{{"error", result.value("message", std::string())}}));
        });
        route("/api/webui/bindings", drogon::Delete, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            const auto kind = body.value("type", std::string("user")) == "group" ? Kind::group : Kind::user;
            const auto openId = body.value("openid", std::string());
            const auto number = state().relay->store()->unbind(kind, openId);
            cb(number ? jsonResponse(200, json{{"ok", true}, {"number", number}}) : jsonResponse(404, json{{"error", "该 OpenID 没有绑定"}}));
        });
        route("/api/webui/identities", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            json list = json::array();
            for (const auto& i : state().relay->store()->searchIdentities(req->getParameter("q"), 50)) list.push_back(identityJson(i));
            cb(jsonResponse(200, list));
        });
        route("/api/webui/messages", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            int64_t before = 0; int limit = 50;
            try { if (!req->getParameter("before").empty()) before = std::stoll(req->getParameter("before")); } catch (...) {}
            try { if (!req->getParameter("limit").empty()) limit = std::clamp(std::stoi(req->getParameter("limit")), 1, 200); } catch (...) {}
            cb(jsonResponse(200, messages(limit, before)));
        });
    }

private:
    static constexpr const char* kCookie = "catofficial_session";
    static constexpr std::time_t kSessionLifetime = 7 * 24 * 3600;

    struct State {
        std::filesystem::path configPath, root;
        std::shared_ptr<Relay> relay;
        std::mutex mu;
        std::map<std::string, std::time_t> sessions;                 // 令牌 → 到期时间
        std::map<std::string, std::deque<std::time_t>> failures;     // IP → 最近失败时间
    };
    static State& state() { static State s; return s; }

    using Handler = std::function<void(const drogon::HttpRequestPtr&, const json&, Callback&&)>;

    static bool onPanel(const drogon::HttpRequestPtr& req) {
        const auto& cfg = state().relay->config();
        return cfg.webuiEnabled && req->getLocalAddr().toPort() == cfg.webuiPort;
    }

    static drogon::HttpResponsePtr notFound() {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k404NotFound);
        return resp;
    }

    static drogon::HttpResponsePtr jsonResponse(int status, const json& body) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(body.dump());
        resp->addHeader("Cache-Control", "no-store");
        return resp;
    }

    static drogon::HttpResponsePtr staticFile(const std::string& relative) {
        if (relative.find("..") != std::string::npos || relative.find('\\') != std::string::npos) return notFound();
        const auto path = state().root / fromUtf8(relative);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            if (relative == "index.html") {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k404NotFound);
                resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                resp->setBody("找不到管理面板的前端文件（" + utf8(state().root) + "）。请确认 web 目录与程序放在一起，或在配置的 webui.root 指定位置。");
                return resp;
            }
            return notFound();
        }
        auto resp = drogon::HttpResponse::newFileResponse(utf8(path));
        // 资源文件名带内容哈希，可以长期缓存；入口页每次都取最新。
        resp->addHeader("Cache-Control", relative == "index.html" ? "no-cache" : "public, max-age=31536000, immutable");
        return resp;
    }

    static bool authenticated(const drogon::HttpRequestPtr& req) {
        const auto token = req->getCookie(kCookie);
        if (token.empty()) return false;
        std::lock_guard lock(state().mu);
        auto it = state().sessions.find(token);
        if (it == state().sessions.end()) return false;
        if (it->second < std::time(nullptr)) { state().sessions.erase(it); return false; }
        return true;
    }

    static void route(const std::string& path, drogon::HttpMethod method, bool needAuth, Handler handler) {
        drogon::app().registerHandler(path, [handler, needAuth](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!onPanel(req)) { cb(notFound()); return; }
            if (needAuth && !authenticated(req)) { cb(jsonResponse(401, json{{"error", "请先登录"}})); return; }
            json body = json::object();
            if (!req->getBody().empty()) {
                body = json::parse(req->getBody(), nullptr, false);
                if (body.is_discarded()) { cb(jsonResponse(400, json{{"error", "请求体不是有效的 JSON"}})); return; }
            }
            try { handler(req, body, std::move(cb)); }
            catch (const std::exception& e) { cb(jsonResponse(500, json{{"error", e.what()}})); }
        }, {method});
    }

    static void login(const drogon::HttpRequestPtr& req, const json& body, Callback&& cb) {
        auto& s = state();
        const auto ip = req->getPeerAddr().toIp();
        const auto now = std::time(nullptr);
        {
            std::lock_guard lock(s.mu);
            auto& list = s.failures[ip];
            while (!list.empty() && now - list.front() > 300) list.pop_front();
            if (list.size() >= 5) { cb(jsonResponse(429, json{{"error", "密码错误次数过多，请 5 分钟后再试"}})); return; }
        }
        const auto password = body.value("password", std::string());
        if (password.empty() || password != s.relay->config().webuiPassword) {
            { std::lock_guard lock(s.mu); s.failures[ip].push_back(now); }
            cb(jsonResponse(401, json{{"error", "密码不正确"}}));
            return;
        }
        const auto token = randomHex(24);
        {
            std::lock_guard lock(s.mu);
            s.failures.erase(ip);
            for (auto it = s.sessions.begin(); it != s.sessions.end();) it = it->second < now ? s.sessions.erase(it) : std::next(it);
            s.sessions[token] = now + kSessionLifetime;
        }
        auto resp = jsonResponse(200, json{{"ok", true}});
        drogon::Cookie cookie(kCookie, token);
        cookie.setPath("/");
        cookie.setHttpOnly(true);
        cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
        cookie.setMaxAge(static_cast<int>(kSessionLifetime));
        resp->addCookie(cookie);
        cb(resp);
    }

    static json status() {
        auto& s = state();
        auto out = s.relay->overview();
        const auto& cfg = s.relay->config();
        auto& stats = ConnectionStats::get();
        json listeners = json::array();
        for (const auto& l : cfg.listeners) listeners.push_back({{"type", l.type}, {"host", l.host}, {"port", l.port}});
        json posts = json::array(), reverse = json::array();
        {
            std::lock_guard lock(stats.mu);
            for (const auto& p : cfg.httpPost) {
                json item{{"url", p.url}};
                if (auto it = stats.posts.find(p.url); it != stats.posts.end()) item["last"] = it->second;
                posts.push_back(item);
            }
            for (const auto& r : cfg.wsReverse) {
                json item{{"url", r.url}, {"connected", false}};
                if (auto it = stats.reverse.find(r.url); it != stats.reverse.end()) item.update(it->second);
                reverse.push_back(item);
            }
        }
        out["connections"] = {{"listeners", listeners}, {"ws_clients", stats.wsClients.load()}, {"http_post", posts}, {"ws_reverse", reverse}};
        const auto store = s.relay->store();
        out["counts"] = {{"users", store->count("identities WHERE kind='user' AND open_id NOT LIKE 'bot:%'")},
                         {"groups", store->count("identities WHERE kind='group'")},
                         {"bound", store->count("identities WHERE bound=1")},
                         {"messages", store->count("messages")}};
        out["webhook"] = cfg.webhook ? json{{"host", cfg.webhookHost}, {"port", cfg.webhookPort}, {"path", cfg.webhookPath}} : json(nullptr);
        return out;
    }

    static json identityJson(const Store::Identity& i) {
        return json{{"type", kindName(i.kind)}, {"openid", i.openId}, {"number", i.number}, {"bound", i.bound},
                    {"direct", i.direct}, {"name", i.name}};
    }

    static json messages(int limit, int64_t before) {
        auto& s = state();
        const auto store = s.relay->store();
        json list = json::array();
        for (const auto& m : store->recentAll(limit, before)) {
            const bool direct = m.scene == static_cast<int>(event::Scene::direct);
            json item{{"id", m.id}, {"time", m.time}, {"self", m.self}, {"scene", direct ? "private" : "group"},
                      {"content", onebot::toCq(m.message)}, {"text", onebot::plainText(m.message)}};
            if (direct) {
                item["peer"] = store->number(Kind::user, m.peer);
                item["peer_name"] = store->name(Kind::user, m.peer);
            } else {
                item["peer"] = store->number(Kind::group, m.peer);
                item["peer_name"] = store->name(Kind::group, m.peer);
            }
            if (!m.self && !m.sender.empty()) {
                item["sender"] = store->number(Kind::user, m.sender);
                item["sender_name"] = store->name(Kind::user, m.sender);
            }
            list.push_back(item);
        }
        return list;
    }

    static json fileConfig() {
        std::ifstream in(state().configPath, std::ios::binary);
        auto raw = json::parse(in, nullptr, false);
        return raw.is_object() ? raw : json::object();
    }

    static drogon::HttpResponsePtr readConfig() {
        auto raw = fileConfig();
        // 面板不回传密钥原文：AppSecret 留空表示不修改；面板密码另用 new_password 修改。
        const bool hasSecret = raw.contains("bot") && raw["bot"].is_object() && !raw["bot"].value("app_secret", std::string()).empty();
        if (raw.contains("bot") && raw["bot"].is_object()) raw["bot"]["app_secret"] = "";
        if (raw.contains("webui") && raw["webui"].is_object()) raw["webui"].erase("password");
        return jsonResponse(200, json{{"config", raw}, {"app_secret_set", hasSecret}});
    }

    static drogon::HttpResponsePtr writeConfig(const json& body) {
        if (!body.contains("config") || !body["config"].is_object()) return jsonResponse(400, json{{"error", "缺少 config"}});
        auto next = body["config"];
        const auto current = fileConfig();
        auto keep = [&](const char* section, const char* key) {
            if (!next.contains(section) || !next[section].is_object()) next[section] = json::object();
            const auto value = next[section].value(key, std::string());
            if (value.empty() && current.contains(section) && current[section].is_object() && current[section].contains(key))
                next[section][key] = current[section][key];
        };
        keep("bot", "app_secret");
        keep("webui", "password");
        const auto newPassword = body.value("new_password", std::string());
        if (!newPassword.empty()) {
            if (newPassword.size() < 6) return jsonResponse(400, json{{"error", "面板密码至少 6 位"}});
            next["webui"]["password"] = newPassword;
        }
        try { (void)parseConfig(next); }
        catch (const std::exception& e) { return jsonResponse(400, json{{"error", e.what()}}); }
        const auto path = state().configPath;
        const auto temp = path.string() + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return jsonResponse(500, json{{"error", "无法写入配置文件"}});
            out << next.dump(2) << "\n";
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec) return jsonResponse(500, json{{"error", "保存配置失败：" + ec.message()}});
        return jsonResponse(200, json{{"ok", true}, {"restart_required", true}});
    }
};

}  // namespace catofficial::app
