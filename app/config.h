#pragma once

// 配置文件（JSON）。
//   bots：要接入的机器人。official 为 QQ 官方机器人；onebot 为普通 QQ 号上的 OneBot 实现
//         （如 NapCat），只用于官民绑定时认出真实 QQ 号与群号。
//   connections：OneBot 连接。写法沿用 NapCat 的四类：http_server / ws_server 由 CatOfficial
//         监听，http_client（HTTP POST 上报）/ ws_client（反向 WebSocket）由 CatOfficial 连出去。
//         每个连接可以指定服务哪些官方机器人，留空表示全部。

#include <nlohmann/json.hpp>

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace catofficial::app {

struct BotConfig {
    std::string id, name;
    bool enabled = true;
    std::string type = "official";    // official / onebot
    // official
    std::string appId, appSecret;
    bool webhook = false;              // false：WebSocket 方式；true：Webhook 方式（官方文档的两种事件接收方式）
    bool subscribeGroupMemberEvents = false;
    bool autoAckInteractions = true;
    // onebot：reverse 为反向 WebSocket（它连 CatOfficial 的 host:port），forward 为正向（CatOfficial 连它的 url）
    std::string connection = "reverse";
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    std::string url, token;
    bool official() const { return type == "official"; }
    bool reverse() const { return connection == "reverse"; }
    /// 重连才能生效的字段变了没有（名称、自动回应互动等可以就地更新）。
    bool sameConnection(const BotConfig& o) const {
        return type == o.type && enabled == o.enabled && appId == o.appId && appSecret == o.appSecret && webhook == o.webhook
            && subscribeGroupMemberEvents == o.subscribeGroupMemberEvents && connection == o.connection && host == o.host
            && port == o.port && url == o.url && token == o.token;
    }
};

struct ConnectionConfig {
    std::string id, name;
    bool enabled = true;
    std::string type;                  // http_server / ws_server / http_client / ws_client
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    std::string url;
    std::string token;                 // 服务端与反向 WS：访问令牌；HTTP POST：签名密钥
    std::string messageFormat = "array";
    bool reportSelf = false;           // 上报机器人自己发出的消息（message_sent）
    int heartbeatInterval = 30000;     // 毫秒，0 不发
    int reconnectInterval = 3000;      // 毫秒，反向 WS
    double timeout = 5.0;              // 秒，HTTP POST
    std::vector<std::string> bots;     // 服务的官方机器人 id，空 = 全部
    bool server() const { return type == "http_server" || type == "ws_server"; }
    bool serves(const std::string& botId) const {
        if (bots.empty()) return true;
        for (const auto& b : bots) if (b == botId) return true;
        return false;
    }
};

struct SmtpConfig {
    bool enabled = false;
    std::string host;
    uint16_t port = 465;
    std::string security = "ssl";      // ssl / starttls / none
    std::string username, password, from;
    std::vector<std::string> notifyEmails;   // 运行异常时通知的邮箱
    bool notifyErrors = true;
    bool usable() const { return enabled && !host.empty() && !from.empty(); }
};

struct BindConfig {
    bool enabled = true;
    std::string command = ".bind";
    int windowSeconds = 5;
    int groupConfirmUsers = 3;
    bool emailEnabled = true;
    std::string emailDomain = "qq.com";
    int codeTtlMinutes = 10;
};

/// 主人：某个官方机器人（为空表示全部）上的 OpenID。
struct MasterConfig {
    std::string botId;
    std::string openId;
};

struct Config {
    std::vector<BotConfig> bots;
    std::vector<ConnectionConfig> connections;
    std::vector<MasterConfig> masters;
    bool notifyMasters = true;         // 运行异常时也私聊通知主人

    // 官方 Webhook 入口：默认挂在管理面板的端口上；standalone 时单独监听 webhookPort。
    std::string webhookPath = "/webhook/qqbot";
    bool webhookStandalone = false;
    std::string webhookHost = "0.0.0.0";
    uint16_t webhookPort = 8443;

    SmtpConfig smtp;
    BindConfig bind;

    bool webuiEnabled = true;
    std::string webuiHost = "127.0.0.1";
    uint16_t webuiPort = 18082;
    std::string webuiPassword;          // 为空时首次启动自动生成并写回配置文件
    std::string webuiRoot = "web";      // 前端文件目录，相对配置文件所在目录

    std::string dataDir = "data";
    std::string logLevel = "info";
    int messageRetentionDays = 7;

    const BotConfig* bot(const std::string& id) const {
        for (const auto& b : bots) if (b.id == id) return &b;
        return nullptr;
    }
    bool isMaster(const std::string& botId, const std::string& openId) const {
        for (const auto& m : masters) if (m.openId == openId && (m.botId.empty() || m.botId == botId)) return true;
        return false;
    }
};

inline const char* kConfigTemplate = R"({
  "bots": [],
  "connections": [
    { "id": "http", "name": "HTTP API", "enabled": true, "type": "http_server", "host": "127.0.0.1", "port": 5700, "token": "", "message_format": "array", "report_self": false, "bots": [] },
    { "id": "ws", "name": "正向 WebSocket", "enabled": true, "type": "ws_server", "host": "127.0.0.1", "port": 6700, "token": "", "message_format": "array", "report_self": false, "heartbeat_interval": 30000, "bots": [] }
  ],
  "masters": [],
  "notify_masters": true,
  "webhook": { "path": "/webhook/qqbot", "standalone": false, "host": "0.0.0.0", "port": 8443 },
  "smtp": { "enabled": false, "host": "", "port": 465, "security": "ssl", "username": "", "password": "", "from": "", "notify_emails": [], "notify_errors": true },
  "bind": { "enabled": true, "command": ".bind", "window_seconds": 5, "group_confirm_users": 3, "email_enabled": true, "email_domain": "qq.com", "code_ttl_minutes": 10 },
  "webui": { "enabled": true, "host": "127.0.0.1", "port": 18082, "password": "", "root": "web" },
  "data_dir": "data",
  "log_level": "info",
  "message_retention_days": 7
}
)";

/// 旧格式（单个 bot + onebot 段）转成新格式；已是新格式时原样返回。
inline nlohmann::json migrateConfig(nlohmann::json j) {
    using json = nlohmann::json;
    if (!j.is_object() || j.contains("bots") || !j.contains("bot")) return j;
    const auto bot = j.value("bot", json::object());
    json bots = json::array();
    if (!bot.value("app_id", std::string()).empty())
        bots.push_back(json{{"id", "bot1"}, {"name", "官方机器人"}, {"enabled", true}, {"type", "official"},
            {"app_id", bot.value("app_id", std::string())}, {"app_secret", bot.value("app_secret", std::string())},
            {"mode", bot.value("mode", std::string("websocket")) == "webhook" ? "webhook" : "websocket"},
            {"subscribe_group_member_events", bot.value("subscribe_group_member_events", false)},
            {"auto_ack_interactions", bot.value("auto_ack_interactions", true)}});
    const auto ob = j.value("onebot", json::object());
    const auto token = ob.value("access_token", std::string());
    const auto format = ob.value("message_format", std::string("array"));
    const int heartbeat = ob.value("heartbeat_interval", 30000);
    json connections = json::array();
    int n = 0;
    for (const auto& s : ob.value("servers", json::array())) {
        const std::string type = s.value("type", std::string()) == "http" ? "http_server" : "ws_server";
        connections.push_back(json{{"id", "conn" + std::to_string(++n)}, {"name", type == "http_server" ? "HTTP API" : "正向 WebSocket"},
            {"enabled", true}, {"type", type}, {"host", s.value("host", std::string("127.0.0.1"))}, {"port", s.value("port", 0)},
            {"token", token}, {"message_format", format}, {"heartbeat_interval", heartbeat}, {"bots", json::array()}});
    }
    for (const auto& p : ob.value("http_post", json::array()))
        connections.push_back(json{{"id", "conn" + std::to_string(++n)}, {"name", "HTTP POST"}, {"enabled", true}, {"type", "http_client"},
            {"url", p.value("url", std::string())}, {"token", p.value("secret", std::string())}, {"message_format", format},
            {"timeout", p.value("timeout", 5.0)}, {"bots", json::array()}});
    for (const auto& r : ob.value("ws_reverse", json::array()))
        connections.push_back(json{{"id", "conn" + std::to_string(++n)}, {"name", "反向 WebSocket"}, {"enabled", true}, {"type", "ws_client"},
            {"url", r.value("url", std::string())}, {"token", token}, {"message_format", format}, {"heartbeat_interval", heartbeat},
            {"reconnect_interval", r.value("reconnect_interval", 3000)}, {"bots", json::array()}});
    // 旧版 webhook 单独监听端口；新格式默认挂在面板端口上，旧配置用了 webhook 时保持独立端口。
    if (j.contains("webhook") && j["webhook"].is_object()) j["webhook"]["standalone"] = bot.value("mode", std::string()) == "webhook";
    j.erase("bot");
    j.erase("onebot");
    j["bots"] = bots;
    j["connections"] = connections;
    return j;
}

inline Config parseConfig(const nlohmann::json& input) {
    using json = nlohmann::json;
    const auto j = migrateConfig(input);
    if (!j.is_object()) throw std::runtime_error("配置文件必须是 JSON 对象");
    Config c;
    std::set<std::string> ids;
    for (const auto& b : j.value("bots", json::array())) {
        BotConfig bot;
        bot.id = b.value("id", std::string());
        bot.name = b.value("name", std::string());
        bot.enabled = b.value("enabled", true);
        bot.type = b.value("type", std::string("official"));
        bot.appId = b.value("app_id", std::string());
        bot.appSecret = b.value("app_secret", std::string());
        bot.webhook = b.value("mode", std::string("websocket")) == "webhook";   // websocket（旧值 gateway）/ webhook
        bot.subscribeGroupMemberEvents = b.value("subscribe_group_member_events", false);
        bot.autoAckInteractions = b.value("auto_ack_interactions", true);
        bot.url = b.value("url", std::string());
        bot.token = b.value("token", std::string());
        // 旧配置只有 url（正向）；新配置默认反向
        bot.connection = b.value("connection", std::string(b.contains("url") && !b.contains("port") ? "forward" : "reverse"));
        bot.host = b.value("host", bot.host);
        bot.port = b.value("port", static_cast<uint16_t>(0));
        if (bot.id.empty() || !ids.insert(bot.id).second) throw std::runtime_error("bots 里的 id 不能为空或重复");
        if (bot.type != "official" && bot.type != "onebot") throw std::runtime_error("机器人类型只能是 official 或 onebot");
        const auto label = "OneBot 机器人「" + (bot.name.empty() ? bot.id : bot.name) + "」";
        if (bot.type == "onebot") {
            if (bot.connection != "reverse" && bot.connection != "forward") throw std::runtime_error(label + "的连接方式只能是 reverse 或 forward");
            if (!bot.reverse() && bot.url.rfind("ws://", 0) != 0 && bot.url.rfind("wss://", 0) != 0)
                throw std::runtime_error(label + "的地址须以 ws:// 或 wss:// 开头");
            if (bot.reverse() && bot.port == 0) throw std::runtime_error(label + "需要监听端口");
        }
        c.bots.push_back(bot);
    }

    for (const auto& m : j.value("masters", json::array())) {
        MasterConfig master{m.value("bot_id", std::string()), m.value("openid", std::string())};
        if (!master.openId.empty()) c.masters.push_back(master);
    }
    c.notifyMasters = j.value("notify_masters", true);

    const auto webhook = j.value("webhook", json::object());
    c.webhookHost = webhook.value("host", c.webhookHost);
    c.webhookPort = webhook.value("port", c.webhookPort);
    c.webhookPath = webhook.value("path", c.webhookPath);
    // 旧格式没有 standalone：端口不为 0 视为独立端口
    c.webhookStandalone = webhook.value("standalone", webhook.contains("port") && !webhook.contains("standalone") && c.webhookPort != 0);
    if (c.webhookPath.empty() || c.webhookPath[0] != '/' || c.webhookPath.find('/', 1) == std::string::npos)
        throw std::runtime_error("webhook.path 须以 / 开头且至少两段（如 /webhook/qqbot），以免与 HTTP 动作路径冲突");

    std::set<std::string> connIds;
    std::set<uint16_t> ports;
    for (const auto& s : j.value("connections", json::array())) {
        ConnectionConfig conn;
        conn.id = s.value("id", std::string());
        conn.name = s.value("name", std::string());
        conn.enabled = s.value("enabled", true);
        conn.type = s.value("type", std::string());
        conn.host = s.value("host", conn.host);
        conn.port = s.value("port", static_cast<uint16_t>(0));
        conn.url = s.value("url", std::string());
        conn.token = s.value("token", std::string());
        conn.messageFormat = s.value("message_format", conn.messageFormat);
        conn.reportSelf = s.value("report_self", false);
        conn.heartbeatInterval = s.value("heartbeat_interval", conn.heartbeatInterval);
        conn.reconnectInterval = s.value("reconnect_interval", conn.reconnectInterval);
        conn.timeout = s.value("timeout", conn.timeout);
        for (const auto& b : s.value("bots", json::array())) if (b.is_string()) conn.bots.push_back(b.get<std::string>());
        const auto label = "连接「" + (conn.name.empty() ? conn.id : conn.name) + "」";
        if (conn.id.empty() || !connIds.insert(conn.id).second) throw std::runtime_error("connections 里的 id 不能为空或重复");
        if (conn.type != "http_server" && conn.type != "ws_server" && conn.type != "http_client" && conn.type != "ws_client")
            throw std::runtime_error(label + "的类型无效");
        if (conn.messageFormat != "array" && conn.messageFormat != "string") throw std::runtime_error(label + "的消息格式只能是 array 或 string");
        if (conn.server()) {
            if (conn.port == 0) throw std::runtime_error(label + "需要端口");
            if (conn.enabled && !ports.insert(conn.port).second) throw std::runtime_error(label + "的端口与其他连接重复");
        } else if (conn.type == "http_client" && conn.url.rfind("http://", 0) != 0 && conn.url.rfind("https://", 0) != 0) {
            throw std::runtime_error(label + "的地址须以 http:// 或 https:// 开头");
        } else if (conn.type == "ws_client" && conn.url.rfind("ws://", 0) != 0 && conn.url.rfind("wss://", 0) != 0) {
            throw std::runtime_error(label + "的地址须以 ws:// 或 wss:// 开头");
        }
        c.connections.push_back(conn);
    }

    const auto smtp = j.value("smtp", json::object());
    c.smtp.enabled = smtp.value("enabled", false);
    c.smtp.host = smtp.value("host", std::string());
    c.smtp.port = smtp.value("port", static_cast<uint16_t>(465));
    c.smtp.security = smtp.value("security", c.smtp.security);
    c.smtp.username = smtp.value("username", std::string());
    c.smtp.password = smtp.value("password", std::string());
    c.smtp.from = smtp.value("from", std::string());
    for (const auto& e : smtp.value("notify_emails", json::array())) if (e.is_string() && !e.get<std::string>().empty()) c.smtp.notifyEmails.push_back(e.get<std::string>());
    if (const auto owner = smtp.value("owner_email", std::string()); !owner.empty() && c.smtp.notifyEmails.empty()) c.smtp.notifyEmails.push_back(owner);   // 旧字段
    c.smtp.notifyErrors = smtp.value("notify_errors", smtp.value("notify_offline", true));
    if (c.smtp.security != "ssl" && c.smtp.security != "starttls" && c.smtp.security != "none")
        throw std::runtime_error("smtp.security 只能是 ssl、starttls 或 none");

    const auto bind = j.value("bind", json::object());
    c.bind.enabled = bind.value("enabled", c.bind.enabled);
    c.bind.command = bind.value("command", c.bind.command);
    c.bind.windowSeconds = bind.value("window_seconds", c.bind.windowSeconds);
    c.bind.groupConfirmUsers = bind.value("group_confirm_users", c.bind.groupConfirmUsers);
    c.bind.emailEnabled = bind.value("email_enabled", c.bind.emailEnabled);
    c.bind.emailDomain = bind.value("email_domain", c.bind.emailDomain);
    c.bind.codeTtlMinutes = bind.value("code_ttl_minutes", c.bind.codeTtlMinutes);
    if (c.bind.command.empty()) throw std::runtime_error("bind.command 不能为空");
    if (c.bind.windowSeconds < 1 || c.bind.windowSeconds > 60) throw std::runtime_error("bind.window_seconds 须在 1 到 60 之间");
    if (c.bind.groupConfirmUsers < 1) throw std::runtime_error("bind.group_confirm_users 至少为 1");

    const auto webui = j.value("webui", json::object());
    c.webuiEnabled = webui.value("enabled", c.webuiEnabled);
    c.webuiHost = webui.value("host", c.webuiHost);
    c.webuiPort = webui.value("port", c.webuiPort);
    c.webuiPassword = webui.value("password", std::string());
    c.webuiRoot = webui.value("root", c.webuiRoot);
    if (c.webuiEnabled && c.webuiPort == 0) throw std::runtime_error("webui.port 不能为 0");
    for (const auto& b : c.bots)
        if (b.enabled && b.type == "onebot" && b.reverse() && !ports.insert(b.port).second)
            throw std::runtime_error("OneBot 机器人「" + (b.name.empty() ? b.id : b.name) + "」的端口与其他连接重复");
    if (c.webuiEnabled && ports.count(c.webuiPort)) throw std::runtime_error("面板端口不能与 OneBot 连接或机器人的端口相同");
    if (c.webhookStandalone) {
        if (c.webhookPort == 0) throw std::runtime_error("Webhook 独立端口不能为 0");
        if (ports.count(c.webhookPort) || (c.webuiEnabled && c.webhookPort == c.webuiPort)) throw std::runtime_error("Webhook 独立端口与其他端口重复");
    }

    c.dataDir = j.value("data_dir", c.dataDir);
    c.logLevel = j.value("log_level", c.logLevel);
    c.messageRetentionDays = j.value("message_retention_days", c.messageRetentionDays);
    return c;
}

}  // namespace catofficial::app
