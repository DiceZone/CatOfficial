#pragma once

// 管理面板的后端：静态前端文件与 /api/webui/* 接口。只在 webui.port 上响应，
// 与 OneBot 的端口分开。登录用配置里的密码；会话保存在数据目录，重启后仍然有效。
// 机器人、连接与设置的修改写入配置文件，重启后生效（面板会提示）。

#include "config.h"
#include "hub.h"
#include "logbuf.h"
#include "paths.h"
#include "server.h"

#include <catofficial/qrlogin.h>

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <ctime>
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

    /// 首次启动时生成面板密码并写回配置文件。返回是否新生成了密码。
    static bool ensurePassword(const std::filesystem::path& configPath, Config& config) {
        if (!config.webuiEnabled || !config.webuiPassword.empty()) return false;
        std::ifstream in(configPath, std::ios::binary);
        auto raw = json::parse(in, nullptr, false);
        if (!raw.is_object()) return false;
        config.webuiPassword = randomHex(6);
        raw["webui"]["password"] = config.webuiPassword;
        writeJson(configPath, migrateConfig(raw));
        return true;
    }

    static void setup(std::filesystem::path configPath, std::filesystem::path dataDir, std::shared_ptr<Hub> hub) {
        auto& s = state();
        s.startup = hub->config();
        s.configPath = std::move(configPath);
        s.sessionFile = dataDir / "webui-sessions.json";
        s.hub = std::move(hub);
        const auto& cfg = s.hub->config();
        if (!cfg.webuiEnabled) return;
        s.root = fromUtf8(cfg.webuiRoot);
        if (s.root.is_relative()) s.root = s.configPath.parent_path() / s.root;
        loadSessions();
        drogon::app().addListener(cfg.webuiHost, cfg.webuiPort);
        Server::panelPort() = cfg.webuiPort;
        Server::panelFile() = [](const std::string& file) { return staticFile(file); };

        auto& app = drogon::app();
        app.registerHandler("/", [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            cb(onPanel(req) ? staticFile("index.html") : notFound());
        }, {drogon::Get});
        app.registerHandler("/assets/{file}", [](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string& file) {
            cb(onPanel(req) ? staticFile("assets/" + file) : notFound());
        }, {drogon::Get});

        route("/api/webui/login", drogon::Post, false, [](const drogon::HttpRequestPtr& req, const json& body, Callback&& cb) { login(req, body, std::move(cb)); });
        route("/api/webui/logout", drogon::Post, false, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            { std::lock_guard lock(state().mu); state().sessions.erase(req->getCookie(kCookie)); }
            saveSessions();
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
        route("/api/webui/restart", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            restartRequested() = true;
            drogon::app().getLoop()->runAfter(0.5, [] { drogon::app().quit(); });
            cb(jsonResponse(200, json{{"ok", true}}));
        });

        // 机器人（登录管理）
        route("/api/webui/bots", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) { cb(jsonResponse(200, botList())); });
        route("/api/webui/bots", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) { cb(saveItem("bots", "", body)); });
        route("/api/webui/bots/{id}", drogon::Put, true, [](const drogon::HttpRequestPtr& req, const json& body, Callback&& cb) { cb(saveItem("bots", param(req), body)); });
        route("/api/webui/bots/{id}", drogon::Delete, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) { cb(removeItem("bots", param(req))); });

        // OneBot 连接（连接管理）
        route("/api/webui/connections", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) { cb(jsonResponse(200, connectionList())); });
        route("/api/webui/connections", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) { cb(saveItem("connections", "", body)); });
        route("/api/webui/connections/{id}", drogon::Put, true, [](const drogon::HttpRequestPtr& req, const json& body, Callback&& cb) { cb(saveItem("connections", param(req), body)); });
        route("/api/webui/connections/{id}", drogon::Delete, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) { cb(removeItem("connections", param(req))); });

        // 设置
        route("/api/webui/settings", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) { cb(readSettings()); });
        route("/api/webui/settings", drogon::Put, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) { cb(writeSettings(body)); });
        route("/api/webui/smtp/test", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) { smtpTest(body, std::move(cb)); });

        // 扫码登录官方机器人
        route("/api/webui/qr/start", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            qrlogin::begin("CatOfficial", [cb = std::move(cb)](qrlogin::Result r) {
                cb(r.ok ? jsonResponse(200, json{{"session_id", r.data.value("sessionId", std::string())}, {"url", r.data.value("url", std::string())}})
                        : jsonResponse(502, json{{"error", r.error}}));
            });
        });
        route("/api/webui/qr/{id}/poll", drogon::Post, true, [](const drogon::HttpRequestPtr& req, const json& body, Callback&& cb) {
            qrlogin::poll(param(req), "CatOfficial", [body, cb = std::move(cb)](qrlogin::Result r) {
                if (!r.ok) { cb(jsonResponse(502, json{{"error", r.error}})); return; }
                const auto status = r.data.value("status", std::string("pending"));
                if (status != "completed") { cb(jsonResponse(200, json{{"status", status}})); return; }
                // 凭据不回传浏览器：直接写进配置。
                json bot = body.value("bot", json::object());
                bot["type"] = "official";
                bot["app_id"] = r.data.value("appId", std::string());
                bot["app_secret"] = r.data.value("appSecret", std::string());
                if (bot.value("name", std::string()).empty()) bot["name"] = "官方机器人 " + bot["app_id"].get<std::string>();
                const auto saved = saveItem("bots", body.value("bot_id", std::string()), bot);
                if (saved->statusCode() != drogon::k200OK) { cb(saved); return; }
                const auto result = json::parse(saved->body(), nullptr, false);
                cb(jsonResponse(200, json{{"status", "completed"}, {"bot_id", result.value("id", std::string())}, {"app_id", bot["app_id"]}}));
            });
        });

        // 日志
        route("/api/webui/logs", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            uint64_t after = 0; std::size_t limit = 500;
            try { if (!req->getParameter("after").empty()) after = std::stoull(req->getParameter("after")); } catch (...) {}
            try { if (!req->getParameter("limit").empty()) limit = std::clamp<std::size_t>(std::stoul(req->getParameter("limit")), 1, 3000); } catch (...) {}
            cb(jsonResponse(200, LogBuffer::get().since(after, limit)));
        });

        // 官民绑定
        route("/api/webui/bindings", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            const auto store = state().hub->store();
            json list = json::array();
            for (const auto& i : store->boundIdentities()) list.push_back(identityJson(i));
            json pending = json::array();
            for (const auto& [groupOpen, realGroup, votes] : store->pendingGroups())
                pending.push_back({{"openid", groupOpen}, {"number", store->number(Kind::group, groupOpen)}, {"name", store->name(Kind::group, groupOpen)},
                                   {"real_group", realGroup}, {"votes", votes}});
            cb(jsonResponse(200, json{{"bound", list}, {"pending_groups", pending}, {"required_votes", state().hub->config().bind.groupConfirmUsers}}));
        });
        route("/api/webui/bindings", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            const auto kind = body.value("type", std::string("user")) == "group" ? Kind::group : Kind::user;
            const auto store = state().hub->store();
            std::string openId = body.value("openid", std::string());
            if (openId.empty()) {
                const auto current = onebot::number(body.value(kind == Kind::group ? "group_id" : "user_id", json()));
                if (current) openId = store->openId(kind, *current);
            }
            const auto qq = onebot::number(body.value("qq", json()));
            if (openId.empty() || !qq) { cb(jsonResponse(400, json{{"error", "需要 OpenID 或已知的号码，以及真实号码"}})); return; }
            if (!store->bind(kind, openId, *qq)) { cb(jsonResponse(409, json{{"error", "这个真实号码已关联到另一个 OpenID"}})); return; }
            if (kind == Kind::group) store->clearGroupVotes(openId);
            cb(jsonResponse(200, json{{"ok", true}}));
        });
        route("/api/webui/bindings", drogon::Delete, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            const auto kind = body.value("type", std::string("user")) == "group" ? Kind::group : Kind::user;
            const auto number = state().hub->store()->unbind(kind, body.value("openid", std::string()));
            cb(number ? jsonResponse(200, json{{"ok", true}, {"number", number}}) : jsonResponse(404, json{{"error", "该 OpenID 没有绑定"}}));
        });
        route("/api/webui/identities", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            json list = json::array();
            for (const auto& i : state().hub->store()->searchIdentities(req->getParameter("q"), 50)) list.push_back(identityJson(i));
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
        std::filesystem::path configPath, root, sessionFile;
        std::shared_ptr<Hub> hub;
        std::mutex mu;
        std::map<std::string, std::time_t> sessions;                 // 令牌 → 到期时间
        std::map<std::string, std::deque<std::time_t>> failures;     // IP → 最近失败时间
        std::atomic<bool> pendingRestart{false};
        Config startup;                                              // 启动时的配置，用来判断改动是否需要重启
        std::time_t startedAt = std::time(nullptr);
    };
    static State& state() { static State s; return s; }

    using Handler = std::function<void(const drogon::HttpRequestPtr&, const json&, Callback&&)>;

    static std::string param(const drogon::HttpRequestPtr& req) {
        const auto& path = req->path();
        const auto parts = drogon::utils::splitString(path, "/");
        // /api/webui/<kind>/<id>[/poll]
        return parts.size() >= 4 ? parts[3] : std::string();
    }

    static bool onPanel(const drogon::HttpRequestPtr& req) {
        const auto& cfg = state().hub->config();
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
        resp->addHeader("Cache-Control", relative == "index.html" ? "no-cache" : "public, max-age=31536000, immutable");
        return resp;
    }

    // ── 会话 ───────────────────────────────────────────────

    static void loadSessions() {
        std::ifstream in(state().sessionFile, std::ios::binary);
        const auto j = json::parse(in, nullptr, false);
        if (!j.is_object()) return;
        const auto now = std::time(nullptr);
        std::lock_guard lock(state().mu);
        for (const auto& [token, expires] : j.items())
            if (expires.is_number_integer() && expires.get<int64_t>() > now) state().sessions[token] = expires.get<int64_t>();
    }
    static void saveSessions() {
        json j = json::object();
        { std::lock_guard lock(state().mu); for (const auto& [token, expires] : state().sessions) j[token] = expires; }
        writeJson(state().sessionFile, j);
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

    static void serve(const Handler& handler, bool needAuth, const drogon::HttpRequestPtr& req, Callback&& cb) {
        if (!onPanel(req)) { cb(notFound()); return; }
        if (needAuth && !authenticated(req)) { cb(jsonResponse(401, json{{"error", "请先登录"}})); return; }
        json body = json::object();
        if (!req->getBody().empty()) {
            body = json::parse(req->getBody(), nullptr, false);
            if (body.is_discarded()) { cb(jsonResponse(400, json{{"error", "请求体不是有效的 JSON"}})); return; }
        }
        try { handler(req, body, std::move(cb)); }
        catch (const std::exception& e) { cb(jsonResponse(500, json{{"error", e.what()}})); }
    }

    static void route(const std::string& path, drogon::HttpMethod method, bool needAuth, Handler handler) {
        // 带 {id} 的路径，drogon 要求处理函数参数与占位符一一对应；id 由 param() 从路径里取。
        if (path.find('{') != std::string::npos)
            drogon::app().registerHandler(path, [handler, needAuth](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string&) {
                serve(handler, needAuth, req, std::move(cb));
            }, {method});
        else
            drogon::app().registerHandler(path, [handler, needAuth](const drogon::HttpRequestPtr& req, Callback&& cb) {
                serve(handler, needAuth, req, std::move(cb));
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
        if (password.empty() || password != s.hub->config().webuiPassword) {
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
        saveSessions();
        auto resp = jsonResponse(200, json{{"ok", true}});
        drogon::Cookie cookie(kCookie, token);
        cookie.setPath("/");
        cookie.setHttpOnly(true);
        cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
        cookie.setMaxAge(static_cast<int>(kSessionLifetime));
        resp->addCookie(cookie);
        cb(resp);
    }

    // ── 配置文件 ───────────────────────────────────────────

    static void writeJson(const std::filesystem::path& path, const json& value) {
        const auto temp = utf8(path) + ".tmp";
        { std::ofstream out(fromUtf8(temp), std::ios::binary | std::ios::trunc); out << value.dump(2) << "\n"; }
        std::error_code ec;
        std::filesystem::rename(fromUtf8(temp), path, ec);
    }

    static json fileConfig() {
        std::ifstream in(state().configPath, std::ios::binary);
        auto raw = json::parse(in, nullptr, false);
        return migrateConfig(raw.is_object() ? raw : json::object());
    }

    /// 校验后写回配置文件并立即应用。只有面板地址、Webhook 入口与数据目录要重启才生效。
    static drogon::HttpResponsePtr commit(const json& next, json extra = json::object()) {
        Config parsed;
        try { parsed = parseConfig(next); }
        catch (const std::exception& e) { return jsonResponse(400, json{{"error", e.what()}}); }
        writeJson(state().configPath, next);
        state().hub->apply(parsed);
        Server::applyConnections(parsed.connections);
        const auto& s = state().startup;
        const bool restart = parsed.webuiEnabled != s.webuiEnabled || parsed.webuiHost != s.webuiHost || parsed.webuiPort != s.webuiPort
            || parsed.webuiRoot != s.webuiRoot || parsed.webhookHost != s.webhookHost || parsed.webhookPort != s.webhookPort
            || parsed.webhookPath != s.webhookPath || parsed.dataDir != s.dataDir;
        state().pendingRestart = restart;
        extra["ok"] = true;
        extra["restart_required"] = restart;
        return jsonResponse(200, extra);
    }

    /// 新增或修改 bots / connections 里的一项。密钥字段留空表示不修改。
    static drogon::HttpResponsePtr saveItem(const std::string& section, const std::string& id, json item) {
        if (!item.is_object()) return jsonResponse(400, json{{"error", "请求体必须是对象"}});
        auto config = fileConfig();
        if (!config.contains(section) || !config[section].is_array()) config[section] = json::array();
        auto& list = config[section];
        auto it = std::find_if(list.begin(), list.end(), [&](const json& x) { return x.value("id", std::string()) == id; });
        if (!id.empty() && it == list.end()) return jsonResponse(404, json{{"error", "没有找到这一项"}});
        for (const char* secret : {"app_secret", "smtp_password"}) {
            if (item.value(secret, std::string()).empty()) {
                if (it != list.end() && it->contains(secret)) item[secret] = (*it)[secret]; else item.erase(secret);
            }
        }
        if (section == "bots" && item.value("type", std::string()) == "onebot" && item.value("token", std::string()).empty()
            && !item.value("clear_token", false) && it != list.end() && it->contains("token"))
            item["token"] = (*it)["token"];
        item.erase("clear_token");
        item.erase("runtime");
        item.erase("app_secret_set");
        item.erase("token_set");
        if (id.empty()) {
            item["id"] = (section == "bots" ? "bot" : "conn") + randomHex(3);
            list.push_back(item);
        } else {
            item["id"] = id;
            *it = item;
        }
        return commit(config, json{{"id", item["id"]}});
    }

    static drogon::HttpResponsePtr removeItem(const std::string& section, const std::string& id) {
        auto config = fileConfig();
        auto& list = config[section];
        if (!list.is_array()) return jsonResponse(404, json{{"error", "没有找到这一项"}});
        const auto before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const json& x) { return x.value("id", std::string()) == id; }), list.end());
        if (list.size() == before) return jsonResponse(404, json{{"error", "没有找到这一项"}});
        if (section == "bots" && config.contains("connections"))
            for (auto& conn : config["connections"])
                if (conn.contains("bots") && conn["bots"].is_array())
                    conn["bots"].erase(std::remove(conn["bots"].begin(), conn["bots"].end(), json(id)), conn["bots"].end());
        return commit(config);
    }

    // ── 列表与状态 ─────────────────────────────────────────

    static json botList() {
        const auto& hub = state().hub;
        json list = json::array();
        for (auto bot : fileConfig().value("bots", json::array())) {
            const auto id = bot.value("id", std::string());
            bot["app_secret_set"] = !bot.value("app_secret", std::string()).empty();
            bot["token_set"] = !bot.value("token", std::string()).empty();
            bot.erase("app_secret");
            bot.erase("token");
            json runtime{{"running", false}};
            if (auto r = hub->relay(id)) { runtime = r->overview(); runtime["running"] = true; }
            else if (auto l = hub->legacy(id)) { runtime = l->status(); runtime["running"] = true; }
            bot["runtime"] = runtime;
            list.push_back(bot);
        }
        return list;
    }

    static json connectionList() {
        json list = json::array();
        for (auto conn : fileConfig().value("connections", json::array())) {
            const auto id = conn.value("id", std::string());
            auto runtime = ConnectionStats::get().snapshot(id);
            runtime["listening"] = Server::listening(id);
            conn["runtime"] = runtime;
            list.push_back(conn);
        }
        return list;
    }

    static json status() {
        auto& s = state();
        const auto& hub = s.hub;
        json bots = json::array();
        for (const auto& b : hub->config().bots) {
            json item{{"id", b.id}, {"name", b.name}, {"type", b.type}, {"enabled", b.enabled}};
            if (auto r = hub->relay(b.id)) item["runtime"] = r->overview();
            else if (auto l = hub->legacy(b.id)) item["runtime"] = l->status();
            bots.push_back(item);
        }
        json connections = json::array();
        for (const auto& c : hub->config().connections) {
            json item{{"id", c.id}, {"name", c.name}, {"type", c.type}, {"enabled", c.enabled}, {"host", c.host}, {"port", c.port}, {"url", c.url}};
            item["runtime"] = ConnectionStats::get().snapshot(c.id);
            connections.push_back(item);
        }
        const auto store = hub->store();
        return json{{"version", kVersion}, {"started_at", s.startedAt}, {"pending_restart", s.pendingRestart.load()},
                    {"received", hub->received()}, {"sent", hub->sent()}, {"bots", bots}, {"connections", connections},
                    {"counts", {{"users", store->count("identities WHERE kind='user' AND open_id NOT LIKE 'bot:%'")},
                                {"groups", store->count("identities WHERE kind='group'")},
                                {"bound", store->count("identities WHERE bound=1")},
                                {"messages", store->count("messages")}}}};
    }

    static drogon::HttpResponsePtr readSettings() {
        auto config = fileConfig();
        json smtp = config.value("smtp", json::object());
        const bool passwordSet = !smtp.value("password", std::string()).empty();
        smtp["password"] = "";
        json webui = config.value("webui", json::object());
        webui.erase("password");
        return jsonResponse(200, json{
            {"webhook", config.value("webhook", json::object())}, {"smtp", smtp}, {"smtp_password_set", passwordSet},
            {"bind", config.value("bind", json::object())}, {"webui", webui},
            {"data_dir", config.value("data_dir", std::string("data"))}, {"log_level", config.value("log_level", std::string("info"))},
            {"message_retention_days", config.value("message_retention_days", 7)}});
    }

    static drogon::HttpResponsePtr writeSettings(const json& body) {
        auto config = fileConfig();
        for (const char* section : {"webhook", "smtp", "bind"})
            if (body.contains(section) && body[section].is_object()) config[section] = body[section];
        if (body.contains("smtp") && body["smtp"].value("password", std::string()).empty()) {
            const auto current = fileConfig().value("smtp", json::object());
            config["smtp"]["password"] = current.value("password", std::string());
        }
        if (body.contains("webui") && body["webui"].is_object()) {
            const auto password = config.value("webui", json::object()).value("password", std::string());
            config["webui"] = body["webui"];
            config["webui"]["password"] = password;
        }
        const auto newPassword = body.value("new_password", std::string());
        if (!newPassword.empty()) {
            if (newPassword.size() < 6) return jsonResponse(400, json{{"error", "面板密码至少 6 位"}});
            config["webui"]["password"] = newPassword;
        }
        for (const char* key : {"data_dir", "log_level", "message_retention_days"}) if (body.contains(key)) config[key] = body[key];
        return commit(config);
    }

    static void smtpTest(const json& body, Callback&& cb) {
        // 用面板上尚未保存的参数也能测试：没填的字段沿用已保存的配置。
        auto saved = fileConfig().value("smtp", json::object());
        auto smtp = body.value("smtp", json::object());
        for (auto& [key, value] : saved.items()) if (!smtp.contains(key) || (key == "password" && smtp[key].get<std::string>().empty())) smtp[key] = value;
        smtp["enabled"] = true;
        Config parsed;
        try { parsed = parseConfig(json{{"smtp", smtp}}); }
        catch (const std::exception& e) { cb(jsonResponse(400, json{{"error", e.what()}})); return; }
        const auto to = body.value("to", parsed.smtp.ownerEmail);
        if (to.empty()) { cb(jsonResponse(400, json{{"error", "请填写收件地址或主人邮箱"}})); return; }
        Mailer mailer(parsed.smtp, nullptr);
        if (!mailer.usable()) { cb(jsonResponse(400, json{{"error", "请填写 SMTP 服务器与发件地址"}})); return; }
        auto loop = drogon::app().getLoop();
        auto shared = std::make_shared<Callback>(std::move(cb));
        mailer.send(to, "CatOfficial 测试邮件", "这是一封测试邮件。收到它说明 CatOfficial 的 SMTP 配置可以正常发信。",
            [loop, shared](bool ok, const std::string& error) {
                loop->queueInLoop([shared, ok, error] {
                    (*shared)(ok ? jsonResponse(200, json{{"ok", true}}) : jsonResponse(502, json{{"error", error}}));
                });
            });
    }

    static json identityJson(const Store::Identity& i) {
        return json{{"type", kindName(i.kind)}, {"openid", i.openId}, {"number", i.number}, {"bound", i.bound},
                    {"direct", i.direct}, {"name", i.name}};
    }

    static json messages(int limit, int64_t before) {
        const auto store = state().hub->store();
        json list = json::array();
        for (const auto& m : store->recentAll(limit, before)) {
            const bool direct = m.scene == static_cast<int>(event::Scene::direct);
            json item{{"id", m.id}, {"time", m.time}, {"self", m.self}, {"scene", direct ? "private" : "group"},
                      {"content", onebot::toCq(m.message)}, {"text", onebot::plainText(m.message)}};
            const auto kind = direct ? Kind::user : Kind::group;
            item["peer"] = store->number(kind, m.peer);
            item["peer_name"] = store->name(kind, m.peer);
            if (!m.self && !m.sender.empty()) {
                item["sender"] = store->number(Kind::user, m.sender);
                item["sender_name"] = store->name(Kind::user, m.sender);
            }
            list.push_back(item);
        }
        return list;
    }
};

}  // namespace catofficial::app
