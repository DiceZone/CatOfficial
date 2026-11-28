#pragma once

// 管理面板的后端：静态前端文件与 /api/webui/* 接口。只在 webui.port 上响应，
// 与 OneBot 的端口分开。登录用配置里的密码；会话保存在数据目录，重启后仍然有效。
// 机器人、连接与设置的修改写入配置文件，重启后生效（面板会提示）。

#include "config.h"
#include "platform/runtime.h"
#include "hub.h"
#include "logbuf.h"
#include "panel.h"
#include "paths.h"
#include "server.h"
#include "platform/tray_win.h"
#include "update/update_service.h"
#include "files/file_store.h"
#include "terminal/terminal.h"

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
#include <thread>

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

    /// 首次启动时生成面板密码并写回配置文件。返回是否新生成了密码；@p saved 表示是否写进了文件。
    static bool ensurePassword(const std::filesystem::path& configPath, Config& config, bool& saved) {
        saved = true;
#ifdef __ANDROID__
        // 内嵌本机 UI 不生成面板密码。其他平台出包仍走下面的首次生成逻辑。
        (void)configPath;
        config.webuiMustChangePassword = false;
        return false;
#endif
        if (!config.webuiEnabled || !config.webuiPassword.empty()) return false;
        json raw;
        {   // 读完就关闭：Windows 上文件还开着时无法用改名的方式覆盖它，密码会写不进去。
            std::ifstream in(configPath, std::ios::binary);
            raw = json::parse(in, nullptr, false);
        }
        if (!raw.is_object()) return false;
        config.webuiPassword = randomHex(6);
        raw["webui"]["password"] = config.webuiPassword;
        raw["webui"]["must_change_password"] = true;
        config.webuiMustChangePassword = true;
        saved = writeJson(configPath, migrateConfig(raw));
        return true;
    }

    /// 在线更新服务（main 创建；退出前置空）。
    static void setUpdater(std::shared_ptr<update::UpdateService> updater) {
        std::lock_guard lock(updaterMutex());
        updaterSlot() = std::move(updater);
    }

    /// 文件管理、网页终端与程序托管（main 创建）。
    struct Tools {
        std::shared_ptr<files::FileStore> files;
        std::shared_ptr<terminal::SessionManager> sessions;
        std::shared_ptr<terminal::ProgramManager> programs;
    };
    static void setTools(Tools tools) { toolsSlot() = std::move(tools); }

    /// 启动时改写配置文件里的面板端口（端口被占用时顺延）。
    static bool saveWebuiPort(const std::filesystem::path& configPath, uint16_t port) {
        json raw;
        { std::ifstream in(configPath, std::ios::binary); raw = json::parse(in, nullptr, false); }
        if (!raw.is_object()) return false;
        raw["webui"]["port"] = port;
        return writeJson(configPath, raw);
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
        for (const char* dir : {"assets", "platform-icons", "os"}) {
            const std::string prefix = dir;
            app.registerHandler("/" + prefix + "/{file}", [prefix](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string& file) {
                cb(onPanel(req) ? staticFile(prefix + "/" + file) : notFound());
            }, {drogon::Get});
        }

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
            const bool ok = authenticated(req);
            const auto& tools = toolsSlot();
            cb(jsonResponse(200, json{{"authenticated", ok}, {"must_change_password", ok && forcePasswordChange(req)},
                                      {"passwordless", localAppSession(req)},
                                      {"os", runtimeOs()},
                                      {"features", json{{"files", tools.files != nullptr}, {"terminal", tools.sessions != nullptr}, {"programs", tools.programs != nullptr}}}}));
        });
        // 在线更新（与 Dice!Next 的 /api/system/update 相同的状态与动作）。
        route("/api/webui/update", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            auto updater = currentUpdater();
            cb(updater ? jsonResponse(200, updater->status()) : jsonResponse(503, json{{"error", "在线更新不可用"}}));
        });
        route("/api/webui/update", drogon::Put, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto updater = currentUpdater();
            if (!updater) { cb(jsonResponse(503, json{{"error", "在线更新不可用"}})); return; }
            std::string error;
            if (!updater->updateSettings(body, error)) { cb(jsonResponse(400, json{{"error", error}})); return; }
            cb(jsonResponse(200, updater->status()));
        });
        route("/api/webui/update/{action}", drogon::Post, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            auto updater = currentUpdater();
            if (!updater) { cb(jsonResponse(503, json{{"error", "在线更新不可用"}})); return; }
            const auto action = param(req);
            std::string error;
            bool ok = false;
            if (action == "check") ok = updater->requestCheck(true, error);
            else if (action == "download") ok = updater->requestDownload(error);
            else if (action == "install") ok = updater->requestInstall(error);
            else if (action == "cancel") ok = updater->requestCancelDownload(error);
            else { cb(jsonResponse(404, json{{"error", "未知的操作"}})); return; }
            if (!ok) { cb(jsonResponse(409, json{{"error", error}})); return; }
            cb(jsonResponse(200, updater->status()));
        });
        // 首次登录强制修改自动生成的密码。
        route("/api/webui/password", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            const auto password = body.value("password", std::string());
            if (password.size() < 6) { cb(jsonResponse(400, json{{"error", "面板密码至少 6 位"}})); return; }
            if (password == state().hub->config().webuiPassword) { cb(jsonResponse(400, json{{"error", "新密码不能与当前密码相同"}})); return; }
            auto config = fileConfig();
            config["webui"]["password"] = password;
            config["webui"].erase("must_change_password");
            cb(commit(config));
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
                const auto saved = saveItem("bots", body.value("bot_id", std::string()), bot);
                if (saved->statusCode() != drogon::k200OK) { cb(saved); return; }
                const auto result = json::parse(saved->body(), nullptr, false);
                const auto botId = result.value("id", std::string());
                rememberBinder(botId, r.data.value("userOpenId", std::string()));
                cb(jsonResponse(200, json{{"status", "completed"}, {"bot_id", botId}, {"app_id", bot["app_id"]}}));
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
        // 消息记录
        route("/api/webui/messages", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            panel::messages(*state().hub, params(req), respond(std::move(cb)));
        });
        route("/api/webui/message-filters", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            panel::messageFilters(*state().hub, respond(std::move(cb)));
        });
        route("/api/webui/image/{name}", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            const auto path = ImageCache::get().pathOf(param(req));
            if (path.empty()) { cb(notFound()); return; }
            auto resp = drogon::HttpResponse::newFileResponse(utf8(path));
            resp->addHeader("Cache-Control", "private, max-age=86400");
            cb(resp);
        });
        route("/api/webui/image-cache", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            const auto [files, bytes] = ImageCache::get().usage();
            cb(jsonResponse(200, json{{"files", files}, {"bytes", bytes}, {"days", ImageCache::get().days()}}));
        });
        route("/api/webui/image-cache/clear", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            const int days = ImageCache::get().days();
            ImageCache::get().setDays(0);
            const int removed = ImageCache::get().prune();
            ImageCache::get().setDays(days);
            cb(jsonResponse(200, json{{"removed", removed}}));
        });

        // 在线聊天
        route("/api/webui/chat/conversations", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            panel::chatConversations(*state().hub, params(req), respond(std::move(cb)));
        });
        route("/api/webui/chat/messages", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            panel::chatMessages(*state().hub, params(req), respond(std::move(cb)));
        });
        route("/api/webui/chat/peer", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            panel::chatPeer(*state().hub, params(req), respond(std::move(cb)));
        });
        route("/api/webui/chat/manage", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            panel::chatManage(*state().hub, body, respond(std::move(cb)));
        });
        route("/api/webui/chat/send", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            panel::chatSend(*state().hub, body, respond(std::move(cb)));
        });

        // 自定义回复
        route("/api/webui/replies", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            cb(jsonResponse(200, panel::replyList(*state().hub)));
        });
        route("/api/webui/replies", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            panel::replySave(*state().hub, "", body, respond(std::move(cb)));
        });
        route("/api/webui/replies/{id}", drogon::Put, true, [](const drogon::HttpRequestPtr& req, const json& body, Callback&& cb) {
            panel::replySave(*state().hub, param(req), body, respond(std::move(cb)));
        });
        route("/api/webui/replies/{id}", drogon::Delete, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            auto service = state().hub->replies();
            try { if (service) service->remove(param(req)); cb(jsonResponse(200, json{{"ok", true}})); }
            catch (const std::exception& e) { cb(jsonResponse(400, json{{"error", e.what()}})); }
        });
        route("/api/webui/replies/{id}/reset", drogon::Post, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            auto service = state().hub->replies();
            try { cb(jsonResponse(200, service ? json(service->reset(param(req))) : json())); }
            catch (const std::exception& e) { cb(jsonResponse(400, json{{"error", e.what()}})); }
        });
        route("/api/webui/replies-test", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            panel::replyTest(*state().hub, body, respond(std::move(cb)));
        });

        // 官方图床
        route("/api/webui/materials", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            panel::materialList(*state().hub, params(req), respond(std::move(cb)));
        });
        route("/api/webui/materials", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            panel::materialUpload(*state().hub, body, respond(std::move(cb)));
        });
        route("/api/webui/materials/{id}", drogon::Delete, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            panel::materialDelete(*state().hub, req->getParameter("bot"), param(req), respond(std::move(cb)));
        });

        // 服务器性能
        route("/api/webui/sysinfo", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            cb(jsonResponse(200, panel::sysinfoJson(panel::intParam(params(req), "since", 0))));
        });

        registerTools();
    }

private:
    static Tools& toolsSlot() { static Tools t; return t; }

    /// 面板监听的不是本机时，文件管理与终端页顶部要提示风险。
    static bool panelExposed() {
        const auto& host = state().startup.webuiHost;
        return host != "127.0.0.1" && host != "localhost" && host != "::1";
    }

    /// 不经 serve() 的接口（上传的请求体不是 JSON）用它做同样的登录检查。
    static bool guardRaw(const drogon::HttpRequestPtr& req, Callback& cb) {
        if (!onPanel(req)) { cb(notFound()); return false; }
        if (!authenticated(req)) { cb(jsonResponse(401, json{{"error", "请先登录"}})); return false; }
        if (forcePasswordChange(req)) { cb(jsonResponse(403, json{{"error", "请先修改面板密码"}, {"must_change_password", true}})); return false; }
        return true;
    }

    /// 文件管理、网页终端、程序托管的接口（前端照搬 BDC 的 InstanceFiles / NodeTerminal）。
    static void registerTools() {
        using files::FileError;
        // ── 文件管理 ──
        route("/api/webui/files/info", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            auto store = toolsSlot().files;
            if (!store) { cb(jsonResponse(503, json{{"error", "文件管理已在启动参数中关闭（--disable-files）"}})); return; }
            auto info = store->info();
            info["exposed"] = panelExposed();
            cb(jsonResponse(200, info));
        });
        route("/api/webui/files", drogon::Post, true, [](const drogon::HttpRequestPtr& req, const json& body, Callback&& cb) {
            auto store = toolsSlot().files;
            if (!store) { cb(jsonResponse(503, json{{"error", "文件管理已在启动参数中关闭（--disable-files）"}})); return; }
            auto shared = std::make_shared<Callback>(std::move(cb));
            const auto ip = req->getPeerAddr().toIp();
            // 列目录、复制等可能碰到慢盘，放到工作线程，别卡住面板的事件循环
            std::thread([store, body, ip, shared] {
                drogon::HttpResponsePtr resp;
                try {
                    resp = jsonResponse(200, files::handleAction(*store, body, "面板", ip));
                } catch (const FileError& e) {
                    json error{{"error", e.what()}};
                    if (e.status == 409) error["conflict"] = true;
                    resp = jsonResponse(e.status, error);
                } catch (const std::exception& e) {
                    resp = jsonResponse(500, json{{"error", std::string("操作失败：") + e.what()}});
                }
                (*shared)(resp);
            }).detach();
        });
        drogon::app().registerHandler("/api/webui/files/jobs/{id}", [](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string& id) {
            if (!guardRaw(req, cb)) return;
            auto store = toolsSlot().files;
            auto job = store ? store->job(id) : nullptr;
            cb(job ? jsonResponse(200, job->snapshot()) : jsonResponse(404, json{{"error", "任务不存在或已过期"}}));
        }, {drogon::Get});
        // 下载 / 预览：?path=...&inline=1
        drogon::app().registerHandler("/api/webui/files/download", [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!guardRaw(req, cb)) return;
            auto store = toolsSlot().files;
            if (!store) { cb(jsonResponse(503, json{{"error", "文件管理已在启动参数中关闭（--disable-files）"}})); return; }
            try {
                const auto [real, name] = store->fileForDownload(req->getParameter("path"));
                const bool inlineView = req->getParameter("inline") == "1";
                auto resp = drogon::HttpResponse::newFileResponse(utf8(real));
                // 中文文件名按 RFC 5987 写成 filename*=UTF-8''…，直接塞原始字节浏览器会显示成乱码
                std::string ascii;
                for (unsigned char c : name) ascii += (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? static_cast<char>(c) : '_';
                resp->addHeader("Content-Disposition", std::string(inlineView ? "inline" : "attachment") + "; filename=\"" + ascii +
                    "\"; filename*=UTF-8''" + drogon::utils::urlEncodeComponent(name));
                resp->addHeader("Cache-Control", "no-store");
                cb(resp);
            } catch (const FileError& e) {
                cb(jsonResponse(e.status, json{{"error", e.what()}}));
            }
        }, {drogon::Get});
        // 上传：请求体就是文件内容，?path= 指定目标
        drogon::app().registerHandler("/api/webui/files/upload", [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!guardRaw(req, cb)) return;
            auto store = toolsSlot().files;
            if (!store) { cb(jsonResponse(503, json{{"error", "文件管理已在启动参数中关闭（--disable-files）"}})); return; }
            const auto path = req->getParameter("path");
            const auto ip = req->getPeerAddr().toIp();
            // 先把请求体落到临时文件，再交给文件管理（它负责写临时文件、改名、旧文件进回收站）
            std::error_code ec;
            const auto staging = std::filesystem::temp_directory_path(ec) / ("catofficial-upload-" + randomHex(8));
            {
                std::ofstream out(staging, std::ios::binary | std::ios::trunc);
                const auto body = req->getBody();
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
                if (!out) { std::filesystem::remove(staging, ec); cb(jsonResponse(500, json{{"error", "暂存上传内容失败，磁盘可能已满"}})); return; }
            }
            auto shared = std::make_shared<Callback>(std::move(cb));
            std::thread([store, path, ip, staging, shared] {
                drogon::HttpResponsePtr resp;
                try {
                    const auto [written, replaced] = store->upload(path, staging, "面板");
                    files::OpRecord record;
                    record.action = "upload";
                    record.path = path;
                    record.bytes = static_cast<std::int64_t>(written);
                    record.actor = "面板";
                    record.ip = ip;
                    if (replaced) record.trashId = replaced->id;
                    store->record(record);
                    resp = jsonResponse(200, json{{"written", written}, {"path", path}});
                } catch (const FileError& e) {
                    resp = jsonResponse(e.status, json{{"error", e.what()}});
                } catch (const std::exception& e) {
                    resp = jsonResponse(500, json{{"error", std::string("上传失败：") + e.what()}});
                }
                std::error_code ignored;
                std::filesystem::remove(staging, ignored);
                (*shared)(resp);
            }).detach();
        }, {drogon::Post});

        // ── 网页终端 ──
        route("/api/webui/terminal/sessions", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            auto sessions = toolsSlot().sessions;
            if (!sessions) { cb(jsonResponse(503, json{{"error", "网页终端已在启动参数中关闭（--disable-terminal）"}})); return; }
            cb(jsonResponse(200, json{{"sessions", sessions->list()}, {"shell", terminal::defaultShell()}, {"exposed", panelExposed()}}));
        });
        route("/api/webui/terminal", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto sessions = toolsSlot().sessions;
            if (!sessions) { cb(jsonResponse(503, json{{"error", "网页终端已在启动参数中关闭（--disable-terminal）"}})); return; }
            std::string error;
            auto session = sessions->open(body.value("label", std::string()), body.value("cols", 100), body.value("rows", 30), error);
            if (!session) { cb(jsonResponse(400, json{{"error", error}})); return; }
            cb(jsonResponse(200, json{{"id", session->id}, {"shell", session->shell}, {"opened", session->opened}}));
        });
        route("/api/webui/terminal/read", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto sessions = toolsSlot().sessions;
            auto session = sessions ? sessions->get(body.value("session_id", std::string())) : nullptr;
            if (!session) { cb(jsonResponse(404, json{{"error", "终端会话不存在"}})); return; }
            auto shared = std::make_shared<Callback>(std::move(cb));
            session->output->read(body.value("cursor", std::int64_t(0)), 25.0, [shared](const std::string& data, std::int64_t cursor, bool alive) {
                (*shared)(jsonResponse(200, json{{"data", drogon::utils::base64Encode(data)}, {"cursor", cursor}, {"alive", alive}}));
            });
        });
        route("/api/webui/terminal/input", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto sessions = toolsSlot().sessions;
            auto session = sessions ? sessions->get(body.value("session_id", std::string())) : nullptr;
            if (!session) { cb(jsonResponse(404, json{{"error", "终端会话不存在"}})); return; }
            session->pty->write(drogon::utils::base64Decode(body.value("data", std::string())));
            cb(jsonResponse(200, json{{"ok", true}}));
        });
        route("/api/webui/terminal/resize", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto sessions = toolsSlot().sessions;
            auto session = sessions ? sessions->get(body.value("session_id", std::string())) : nullptr;
            if (!session) { cb(jsonResponse(404, json{{"error", "终端会话不存在"}})); return; }
            session->pty->resize(body.value("cols", 100), body.value("rows", 30));
            cb(jsonResponse(200, json{{"ok", true}}));
        });
        route("/api/webui/terminal/close", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto sessions = toolsSlot().sessions;
            const bool closed = sessions && sessions->close(body.value("session_id", std::string()));
            cb(closed ? jsonResponse(200, json{{"ok", true}}) : jsonResponse(404, json{{"error", "终端会话不存在"}}));
        });

        // ── 程序托管 ──
        route("/api/webui/programs", drogon::Get, true, [](const drogon::HttpRequestPtr&, const json&, Callback&& cb) {
            auto programs = toolsSlot().programs;
            if (!programs) { cb(jsonResponse(503, json{{"error", "程序托管已在启动参数中关闭（--disable-programs）"}})); return; }
            cb(jsonResponse(200, json{{"programs", programs->list()}, {"exposed", panelExposed()}, {"max_programs", programs->maxPrograms()},
                                      {"full_access", programs->fullAccess()}, {"files_enabled", toolsSlot().files != nullptr},
                                      {"os", runtimeOs()},
                                      {"settings", programs->logSettings().toJson()}}));
        });
        route("/api/webui/programs", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto programs = toolsSlot().programs;
            if (!programs) { cb(jsonResponse(503, json{{"error", "程序托管已在启动参数中关闭（--disable-programs）"}})); return; }
            std::string error;
            auto saved = programs->save(body, error);
            cb(saved.is_null() ? jsonResponse(400, json{{"error", error}}) : jsonResponse(200, saved));
        });
        route("/api/webui/programs/action", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto programs = toolsSlot().programs;
            if (!programs) { cb(jsonResponse(503, json{{"error", "程序托管已在启动参数中关闭（--disable-programs）"}})); return; }
            const auto id = body.value("id", std::string());
            const auto action = body.value("action", std::string());
            std::string error;
            bool ok = false;
            if (action == "start") ok = programs->start(id, error);
            else if (action == "stop") ok = programs->stop(id, false, error);
            else if (action == "kill") ok = programs->stop(id, true, error);
            else if (action == "restart") ok = programs->restart(id, error);
            else if (action == "delete") ok = programs->remove(id, body.value("remove_files", false), error);
            else if (action == "clear") { programs->clearOutput(id); ok = true; }
            else error = "未知的操作";
            cb(ok ? jsonResponse(200, json{{"ok", true}, {"programs", programs->list()}}) : jsonResponse(400, json{{"error", error}}));
        });
        route("/api/webui/programs/settings", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto programs = toolsSlot().programs;
            if (!programs) { cb(jsonResponse(503, json{{"error", "程序托管已在启动参数中关闭（--disable-programs）"}})); return; }
            std::string error;
            cb(programs->saveLogSettings(body, error) ? jsonResponse(200, programs->logSettings().toJson()) : jsonResponse(400, json{{"error", error}}));
        });
        // 实例的历史日志：列表与下载（不依赖文件管理是否开启）
        route("/api/webui/programs/logs", drogon::Get, true, [](const drogon::HttpRequestPtr& req, const json&, Callback&& cb) {
            auto programs = toolsSlot().programs;
            if (!programs) { cb(jsonResponse(503, json{{"error", "程序托管已在启动参数中关闭（--disable-programs）"}})); return; }
            cb(jsonResponse(200, json{{"logs", programs->logList(req->getParameter("id"))}, {"settings", programs->logSettings().toJson()}}));
        });
        drogon::app().registerHandler("/api/webui/programs/log", [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!guardRaw(req, cb)) return;
            auto programs = toolsSlot().programs;
            const auto path = programs ? programs->logFile(req->getParameter("id"), req->getParameter("name")) : std::filesystem::path();
            if (path.empty()) { cb(jsonResponse(404, json{{"error", "日志不存在或已被清理"}})); return; }
            auto resp = drogon::HttpResponse::newFileResponse(utf8(path));
            const auto name = req->getParameter("name");
            resp->addHeader("Content-Disposition", std::string(req->getParameter("inline") == "1" ? "inline" : "attachment") + "; filename=\"" + name + "\"");
            resp->addHeader("Cache-Control", "no-store");
            cb(resp);
        }, {drogon::Get});
        // 导入压缩包作为实例目录：请求体就是压缩包，?id=&name=&encoding=
        drogon::app().registerHandler("/api/webui/programs/import", [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!guardRaw(req, cb)) return;
            auto programs = toolsSlot().programs;
            if (!programs) { cb(jsonResponse(503, json{{"error", "程序托管已在启动参数中关闭（--disable-programs）"}})); return; }
            const auto id = req->getParameter("id");
            const auto name = req->getParameter("name");
            const auto encoding = req->getParameter("encoding");
            std::error_code ec;
            const auto staging = std::filesystem::temp_directory_path(ec) / ("catofficial-import-" + randomHex(8));
            {
                std::ofstream out(staging, std::ios::binary | std::ios::trunc);
                const auto body = req->getBody();
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
                if (!out) { std::filesystem::remove(staging, ec); cb(jsonResponse(500, json{{"error", "暂存压缩包失败，磁盘可能已满"}})); return; }
            }
            auto shared = std::make_shared<Callback>(std::move(cb));
            std::thread([programs, id, name, encoding, staging, shared] {
                std::string error;
                const bool ok = programs->importArchive(id, staging, name, encoding.empty() ? "utf8" : encoding, error);
                std::error_code ignored;
                std::filesystem::remove(staging, ignored);
                (*shared)(ok ? jsonResponse(200, json{{"ok", true}, {"programs", programs->list()}}) : jsonResponse(400, json{{"error", error}}));
            }).detach();
        }, {drogon::Post});
        route("/api/webui/programs/read", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto programs = toolsSlot().programs;
            if (!programs) { cb(jsonResponse(503, json{{"error", "程序托管已在启动参数中关闭（--disable-programs）"}})); return; }
            auto shared = std::make_shared<Callback>(std::move(cb));
            const bool found = programs->read(body.value("id", std::string()), body.value("cursor", std::int64_t(0)), 25.0,
                [shared](const std::string& data, std::int64_t cursor, bool) {
                    (*shared)(jsonResponse(200, json{{"data", drogon::utils::base64Encode(data)}, {"cursor", cursor}, {"alive", true}}));
                });
            if (!found) (*shared)(jsonResponse(404, json{{"error", "没有找到这个程序"}}));
        });
        route("/api/webui/programs/input", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            auto programs = toolsSlot().programs;
            const bool ok = programs && programs->input(body.value("id", std::string()), drogon::utils::base64Decode(body.value("data", std::string())));
            cb(ok ? jsonResponse(200, json{{"ok", true}}) : jsonResponse(409, json{{"error", "程序没有在运行"}}));
        });
        route("/api/webui/programs/resize", drogon::Post, true, [](const drogon::HttpRequestPtr&, const json& body, Callback&& cb) {
            if (auto programs = toolsSlot().programs) programs->resize(body.value("id", std::string()), body.value("cols", 120), body.value("rows", 32));
            cb(jsonResponse(200, json{{"ok", true}}));
        });
    }

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
    static std::mutex& updaterMutex() { static std::mutex m; return m; }
    static std::shared_ptr<update::UpdateService>& updaterSlot() { static std::shared_ptr<update::UpdateService> u; return u; }
    static std::shared_ptr<update::UpdateService> currentUpdater() { std::lock_guard lock(updaterMutex()); return updaterSlot(); }

    using Handler = std::function<void(const drogon::HttpRequestPtr&, const json&, Callback&&)>;

    static panel::Params params(const drogon::HttpRequestPtr& req) {
        return [req](const std::string& name) { return req->getParameter(name); };
    }
    /// 把 panel 的结果写回请求。回调可能在别的线程触发，统一交回事件循环。
    static panel::Respond respond(Callback&& cb) {
        auto shared = std::make_shared<Callback>(std::move(cb));
        return [shared](int status, json body) {
            drogon::app().getLoop()->queueInLoop([shared, status, body = std::move(body)] { (*shared)(jsonResponse(status, body)); });
        };
    }

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

    static bool loopbackPeer(const drogon::HttpRequestPtr& req) {
        const auto ip = req->getPeerAddr().toIp();
        return ip == "127.0.0.1" || ip == "::1" || ip == ":ffff:127.0.0.1" || ip.rfind("127.", 0) == 0;
    }

    /// 内嵌本机 UI（当前是 Android 应用 WebView）从 loopback 进面板时免登录。
    /// Windows / Linux / macOS 出包不走这里，对外监听时也仍要密码。
    static bool localAppSession(const drogon::HttpRequestPtr& req) {
#ifdef __ANDROID__
        return loopbackPeer(req);
#else
        (void)req;
        return false;
#endif
    }

    static bool forcePasswordChange(const drogon::HttpRequestPtr& req) {
        return !localAppSession(req) && state().hub->config().webuiMustChangePassword;
    }

    static bool authenticated(const drogon::HttpRequestPtr& req) {
        if (localAppSession(req)) return true;
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
        if (needAuth && forcePasswordChange(req) && req->path() != "/api/webui/password") {
            cb(jsonResponse(403, json{{"error", "请先修改面板密码"}, {"must_change_password", true}}));
            return;
        }
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

    /// 先写临时文件再改名替换，写一半断电也不会留下损坏的文件。失败返回 false。
    static bool writeJson(const std::filesystem::path& path, const json& value) {
        const auto temp = utf8(path) + ".tmp";
        { std::ofstream out(fromUtf8(temp), std::ios::binary | std::ios::trunc); out << value.dump(2) << "\n"; }
        std::error_code ec;
        std::filesystem::rename(fromUtf8(temp), path, ec);
        if (!ec) return true;
        std::filesystem::remove(fromUtf8(temp), ec);
        return false;
    }

    static json fileConfig() {
        std::ifstream in(state().configPath, std::ios::binary);
        auto raw = json::parse(in, nullptr, false);
        auto config = migrateConfig(raw.is_object() ? raw : json::object());
        // 早先版本自动起的名字（「官方机器人」「官方机器人 <AppID>」）不算用户命名，改为空，界面显示机器人昵称。
        if (config.contains("bots") && config["bots"].is_array())
            for (auto& b : config["bots"]) {
                const auto name = b.value("name", std::string());
                const std::string auto1 = "\xE5\xAE\x98\xE6\x96\xB9\xE6\x9C\xBA\xE5\x99\xA8\xE4\xBA\xBA";   // 官方机器人
                if (name == auto1 || (name.rfind(auto1 + " ", 0) == 0 && name.substr(auto1.size() + 1) == b.value("app_id", std::string()))) b["name"] = "";
            }
        return config;
    }

    /// 校验后写回配置文件并立即应用。只有面板地址、Webhook 入口与数据目录要重启才生效。
    static drogon::HttpResponsePtr commit(const json& next, json extra = json::object()) {
        Config parsed;
        try { parsed = parseConfig(next); }
        catch (const std::exception& e) { return jsonResponse(400, json{{"error", e.what()}}); }
        if (!writeJson(state().configPath, next)) return jsonResponse(500, json{{"error", "无法写入配置文件，请确认程序目录可写且文件没有被其他程序占用"}});
        state().hub->apply(parsed);
        Server::applyConnections(parsed.connections);
        updateSystemTrayText(parsed.trayText);   // 托盘文字保存后立即生效
        const auto& s = state().startup;
        const bool restart = parsed.webuiEnabled != s.webuiEnabled || parsed.webuiHost != s.webuiHost || parsed.webuiPort != s.webuiPort
            || parsed.webuiRoot != s.webuiRoot || parsed.webhookStandalone != s.webhookStandalone || parsed.webhookPath != s.webhookPath
            || (parsed.webhookStandalone && (parsed.webhookHost != s.webhookHost || parsed.webhookPort != s.webhookPort))
            ;
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

    /// 把扫码用户记成该官方机器人的主人，上线后私聊提醒返回面板。
    static void rememberBinder(const std::string& botId, const std::string& openId) {
        if (botId.empty()) return;
        if (openId.empty()) {
            state().hub->log(LogLevel::warn, "扫码绑定未返回用户 OpenID，无法私聊提醒返回面板");
            return;
        }
        auto config = fileConfig();
        if (!config.contains("masters") || !config["masters"].is_array()) config["masters"] = json::array();
        bool known = false;
        for (const auto& m : config["masters"])
            if (m.value("bot_id", std::string()) == botId && m.value("openid", std::string()) == openId) { known = true; break; }
        if (!known) {
            config["masters"].push_back({{"bot_id", botId}, {"openid", openId}});
            commit(config);
        }
        state().hub->greetBinder(botId, openId);
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
            {"masters", config.value("masters", json::array())}, {"notify_masters", config.value("notify_masters", true)},
            {"log_level", config.value("log_level", std::string("info"))},
            {"message_retention_days", config.value("message_retention_days", 365)}, {"image_cache_days", config.value("image_cache_days", 7)},
            {"tray", json{{"text", config.value("tray", json::object()).value("text", std::string())},
                          {"start_hidden", config.value("tray", json::object()).value("start_hidden", true)}}},
            {"tray_supported", tray_settings::supported}, {"tray_port", state().startup.webuiPort}});
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
            if (const bool must = fileConfig().value("webui", json::object()).value("must_change_password", false)) config["webui"]["must_change_password"] = must;
        }
        const auto newPassword = body.value("new_password", std::string());
        if (!newPassword.empty()) {
            if (newPassword.size() < 6) return jsonResponse(400, json{{"error", "面板密码至少 6 位"}});
            config["webui"]["password"] = newPassword;
            config["webui"].erase("must_change_password");
        }
        if (body.contains("masters") && body["masters"].is_array())
            for (const auto& m : body["masters"])
                if (m.is_object() && m.value("bot_id", std::string()).empty() && !m.value("openid", std::string()).empty())
                    return jsonResponse(400, json{{"error", "每个主人都要指定官方机器人：同一个用户在不同官方机器人下的 OpenID 不同"}});
        config.erase("data_dir");
        if (config.contains("bind") && config["bind"].is_object()) config["bind"].erase("email_domain");
        for (const char* key : {"log_level", "message_retention_days", "image_cache_days", "masters", "notify_masters"}) if (body.contains(key)) config[key] = body[key];
        if (body.contains("tray") && body["tray"].is_object()) {
            const auto& tray = body["tray"];
            if (tray.contains("text") && !tray["text"].is_string()) return jsonResponse(400, json{{"error", "托盘文字必须是字符串"}});
            config["tray"] = json{{"text", tray.value("text", std::string())}, {"start_hidden", tray.value("start_hidden", true)}};
        }
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
        const auto to = parsed.smtp.notifyEmails.empty() ? std::string() : parsed.smtp.notifyEmails.front();
        if (to.empty()) { cb(jsonResponse(400, json{{"error", "请先填写至少一个通知邮箱"}})); return; }
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
};

}  // namespace catofficial::app
