#pragma once

// QQ 机器人开放平台 HTTP 接口的通用部分：地址解析、查询串、统一结果格式与错误判定。
// 结果统一为 {ok, httpStatus, message, data[, error_code]}，调用方不必再分辨各接口
// 把错误码放在 code 还是 err_code。

#include "protocol.h"

#include <drogon/HttpClient.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace catofficial::api {
using json = nlohmann::json;

inline constexpr const char* kHost = "api.bot.qq.com";

/// 经操作系统解析 IPv4。drogon 的 c-ares 解析器在部分 VPN/CNAME 配置下会失败。
inline std::string resolveIpv4(const std::string& host) {
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || !results) return {};
    char text[INET_ADDRSTRLEN]{};
    auto* addr = reinterpret_cast<sockaddr_in*>(results->ai_addr);
    const char* result = inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text));
    freeaddrinfo(results);
    return result ? std::string(result) : std::string();
}

/// 连到已解析 IP 的 HTTPS 客户端；请求须自行带 Host 头。解析失败返回空。
inline drogon::HttpClientPtr httpsClient(const std::string& host) {
    const auto ip = resolveIpv4(host);
    return ip.empty() ? nullptr : drogon::HttpClient::newHttpClient(ip, 443, true);
}

inline std::string urlEncode(const std::string& value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += kHex[c >> 4]; out += kHex[c & 0x0F]; }
    }
    return out;
}

/// 平铺 JSON 对象 → 查询串。官方 GET 接口的分页游标等参数走 query，读不到请求体。
inline std::string queryString(const json& params) {
    if (!params.is_object()) return {};
    std::string out;
    for (const auto& item : params.items()) {
        const json& value = item.value();
        std::string text;
        if (value.is_string()) text = value.get<std::string>();
        else if (value.is_boolean()) text = value.get<bool>() ? "true" : "false";
        else if (value.is_number()) text = value.dump();
        else continue;   // 对象/数组无法出现在查询串里
        if (!out.empty()) out += "&";
        out += urlEncode(item.key()) + "=" + urlEncode(text);
    }
    return out;
}

/// URL 查询串里某个参数的原始值（不解码）。分享链接用它取 robot_uin。
inline std::string queryValue(const std::string& url, const std::string& name) {
    const std::string key = name + "=";
    auto p = url.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    auto e = url.find('&', p);
    return url.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

inline json result(bool ok, int status, const std::string& message = {}, json data = json::object()) {
    return json{{"ok", ok}, {"httpStatus", status}, {"message", message}, {"data", std::move(data)}};
}

/// 平台返回的「无权限」类错误：未开通 / 内邀 / 白名单接口。只按平台给出的错误码
/// 与文案识别，不把网络、限流、参数错误当成权限问题。
inline bool isPermissionDenied(int status, int64_t code, const std::string& message) {
    // 40012010：应用无接口访问权限；11253/11254：成员等接口未授权。
    if (code == 40012010 || code == 11253 || code == 11254) return true;
    if (status == 401 || status == 429 || status >= 500) return false;
    for (const char* marker : {"无接口访问权限", "无权限", "没有权限", "权限不足", "未授权", "未开通", "no permission", "permission denied"})
        if (message.find(marker) != std::string::npos) return true;
    return false;
}

/// 把一次 HTTP 应答归一成 result()，并补上 error_code（code / err_code 中第一个非零值）。
inline json checkedResponse(int status, const json& data) {
    int64_t code = 0;
    if (data.is_object()) {
        for (const char* field : {"err_code", "code"}) {
            if (!data.contains(field)) continue;
            const auto current = protocol::asInteger(data[field]);
            if (!current) { code = -1; break; }
            if (*current != 0) { code = *current; break; }
        }
    }
    const bool ok = status >= 200 && status < 300 && code == 0 && !data.is_discarded();
    auto message = ok ? std::string() : protocol::text(data, "message");
    if (!ok && message.empty()) message = "QQ 官方接口未成功（HTTP " + std::to_string(status) + "，错误码 " + std::to_string(code) + "）";
    auto out = result(ok, status, message, data.is_discarded() ? json::object() : data);
    out["error_code"] = code;
    return out;
}

}  // namespace catofficial::api
