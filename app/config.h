#pragma once

// 配置文件（JSON）。连接方式的写法沿用 NapCat / go-cqhttp 的习惯：
// http / ws 是 CatOfficial 开的服务端，http_post / ws_reverse 是 CatOfficial 主动连出去。

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace catofficial::app {

struct Listener {
    std::string type;   // "http" 或 "ws"
    std::string host = "127.0.0.1";
    uint16_t port = 0;
};

struct HttpPost {
    std::string url;
    std::string secret;
    double timeout = 5.0;
};

struct ReverseWs {
    std::string url;
    int reconnectInterval = 3000;   // 毫秒
};

struct Config {
    std::string appId;
    std::string appSecret;
    bool webhook = false;
    std::string webhookHost = "0.0.0.0";
    uint16_t webhookPort = 0;
    std::string webhookPath = "/webhook/qqbot";
    bool subscribeGroupMemberEvents = false;
    bool autoAckInteractions = true;

    std::string dataDir = "data";
    std::string logLevel = "info";
    int messageRetentionDays = 7;

    std::string accessToken;
    std::string messageFormat = "array";   // array / string
    int heartbeatInterval = 30000;          // 毫秒；0 表示不发心跳
    std::vector<Listener> listeners;
    std::vector<HttpPost> httpPost;
    std::vector<ReverseWs> wsReverse;

    // 管理面板
    bool webuiEnabled = true;
    std::string webuiHost = "127.0.0.1";
    uint16_t webuiPort = 18082;
    std::string webuiPassword;          // 为空时首次启动自动生成并写回配置文件
    std::string webuiRoot = "web";      // 前端文件目录，相对配置文件所在目录
};

inline const char* kConfigTemplate = R"({
  "bot": {
    "app_id": "",
    "app_secret": "",
    "mode": "gateway",
    "subscribe_group_member_events": false,
    "auto_ack_interactions": true
  },
  "webhook": {
    "host": "0.0.0.0",
    "port": 8443,
    "path": "/webhook/qqbot"
  },
  "onebot": {
    "access_token": "",
    "message_format": "array",
    "heartbeat_interval": 30000,
    "servers": [
      { "type": "http", "host": "127.0.0.1", "port": 5700 },
      { "type": "ws", "host": "127.0.0.1", "port": 6700 }
    ],
    "http_post": [],
    "ws_reverse": []
  },
  "webui": {
    "enabled": true,
    "host": "127.0.0.1",
    "port": 18082,
    "password": "",
    "root": "web"
  },
  "data_dir": "data",
  "log_level": "info",
  "message_retention_days": 7
}
)";

inline Config parseConfig(const nlohmann::json& j) {
    using json = nlohmann::json;
    if (!j.is_object()) throw std::runtime_error("配置文件必须是 JSON 对象");
    Config c;
    const auto bot = j.value("bot", json::object());
    c.appId = bot.value("app_id", std::string());
    c.appSecret = bot.value("app_secret", std::string());
    c.webhook = bot.value("mode", std::string("gateway")) == "webhook";
    c.subscribeGroupMemberEvents = bot.value("subscribe_group_member_events", false);
    c.autoAckInteractions = bot.value("auto_ack_interactions", true);

    const auto webhook = j.value("webhook", json::object());
    c.webhookHost = webhook.value("host", c.webhookHost);
    c.webhookPort = webhook.value("port", static_cast<uint16_t>(0));
    c.webhookPath = webhook.value("path", c.webhookPath);
    if (c.webhook && c.webhookPort == 0) throw std::runtime_error("mode 为 webhook 时需要 webhook.port");
    if (c.webhookPath.empty() || c.webhookPath[0] != '/' || c.webhookPath.find('/', 1) == std::string::npos)
        throw std::runtime_error("webhook.path 须以 / 开头且至少两段（如 /webhook/qqbot），以免与 HTTP 动作路径冲突");

    const auto ob = j.value("onebot", json::object());
    c.accessToken = ob.value("access_token", std::string());
    c.messageFormat = ob.value("message_format", c.messageFormat);
    if (c.messageFormat != "array" && c.messageFormat != "string") throw std::runtime_error("onebot.message_format 只能是 array 或 string");
    c.heartbeatInterval = ob.value("heartbeat_interval", c.heartbeatInterval);
    for (const auto& s : ob.value("servers", json::array())) {
        Listener l;
        l.type = s.value("type", std::string());
        l.host = s.value("host", l.host);
        l.port = s.value("port", static_cast<uint16_t>(0));
        if ((l.type != "http" && l.type != "ws") || l.port == 0) throw std::runtime_error("onebot.servers 每项需要 type（http/ws）与 port");
        c.listeners.push_back(l);
    }
    for (const auto& p : ob.value("http_post", json::array())) {
        HttpPost h;
        h.url = p.value("url", std::string());
        h.secret = p.value("secret", std::string());
        h.timeout = p.value("timeout", h.timeout);
        if (h.url.rfind("http://", 0) != 0 && h.url.rfind("https://", 0) != 0) throw std::runtime_error("onebot.http_post 的 url 须以 http:// 或 https:// 开头");
        c.httpPost.push_back(h);
    }
    for (const auto& r : ob.value("ws_reverse", json::array())) {
        ReverseWs w;
        w.url = r.value("url", std::string());
        w.reconnectInterval = r.value("reconnect_interval", w.reconnectInterval);
        if (w.url.rfind("ws://", 0) != 0 && w.url.rfind("wss://", 0) != 0) throw std::runtime_error("onebot.ws_reverse 的 url 须以 ws:// 或 wss:// 开头");
        c.wsReverse.push_back(w);
    }
    c.dataDir = j.value("data_dir", c.dataDir);
    c.logLevel = j.value("log_level", c.logLevel);
    c.messageRetentionDays = j.value("message_retention_days", c.messageRetentionDays);
    const auto webui = j.value("webui", json::object());
    c.webuiEnabled = webui.value("enabled", c.webuiEnabled);
    c.webuiHost = webui.value("host", c.webuiHost);
    c.webuiPort = webui.value("port", c.webuiPort);
    c.webuiPassword = webui.value("password", std::string());
    c.webuiRoot = webui.value("root", c.webuiRoot);
    if (c.webuiEnabled && c.webuiPort == 0) throw std::runtime_error("webui.port 不能为 0");
    for (const auto& l : c.listeners)
        if (c.webuiEnabled && l.port == c.webuiPort) throw std::runtime_error("webui.port 不能与 OneBot 服务端口相同");
    return c;
}

}  // namespace catofficial::app
