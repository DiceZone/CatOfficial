#pragma once

// QQ 机器人开放平台 HTTP 接口的通用部分：地址解析、查询串、统一结果格式与错误判定。
// 结果统一为 {ok, httpStatus, message, data[, error_code]}，调用方不必再分辨各接口
// 把错误码放在 code 还是 err_code。

#include "log.h"
#include "protocol.h"

#include <drogon/HttpClient.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#include "https.h"

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

/// 连到已解析 IP 的 Drogon HTTPS 客户端（无 SNI）。QQ 出站请用 httpsSend。
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

using Callback = std::function<void(json result)>;

inline const char* methodName(drogon::HttpMethod method) {
    switch (method) {
        case drogon::Get: return "GET";
        case drogon::Post: return "POST";
        case drogon::Patch: return "PATCH";
        case drogon::Delete: return "DELETE";
        default: return "HTTP";
    }
}

/// 调用开放平台接口。GET 的 body 平铺为查询串，其余方法作为 JSON 请求体。
/// 回调收到 checkedResponse() 的结果；请求没有完成时 ok=false、httpStatus=0。
/// 平台的「无权限」错误记为 debug（不少接口只对内邀或白名单应用开放，普通应用每次
/// 调用都会稳定返回，这是平台权限而非故障），其余失败记为 warn。
inline void call(drogon::HttpMethod method, const std::string& path,
                 const json& body, const std::string& accessToken, Callback cb,
                 double timeoutSeconds = 0.0, Logger log = {}) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(method);
    if (method == drogon::Get) {
        // 查询串已逐段编码，关掉 drogon 的整体编码，否则 ? 与 & 会被转义。
        const std::string query = queryString(body);
        request->setPathEncode(false);
        request->setPath(query.empty() ? path : path + "?" + query);
    } else {
        request->setPath(path);
        if (!body.is_null() && !body.empty()) request->setBody(body.dump());
    }
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->addHeader("Host", kHost);
    request->addHeader("Authorization", "QQBot " + accessToken);
    httpsSend(kHost, request, [method, path, cb = std::move(cb), log = std::move(log)](
        drogon::ReqResult outcome, const drogon::HttpResponsePtr& response) mutable {
        if (outcome != drogon::ReqResult::Ok || !response) {
            const std::string message = "QQ 官方接口请求未完成: " + path + "（" + httpsFailure(outcome) + "）";
            if (log) log(LogLevel::warn, message);
            cb(result(false, 0, message));
            return;
        }
        const int status = static_cast<int>(response->statusCode());
        const auto raw = response->body();
        json data = raw.empty() ? json::object() : json::parse(raw, nullptr, false);
        auto checked = checkedResponse(status, data);
        if (!checked.value("ok", false) && log) {
            const auto code = checked.value("error_code", int64_t(0));
            const auto message = checked.value("message", std::string());
            const auto detail = "HTTP " + std::to_string(status) + "，错误码 " + std::to_string(code) + "，" + message;
            if (isPermissionDenied(status, code, message))
                log(LogLevel::debug, std::string(methodName(method)) + " " + path + " 无接口访问权限：" + detail);
            else
                log(LogLevel::warn, std::string(methodName(method)) + " " + path + " 失败：" + detail);
        }
        cb(std::move(checked));
    }, timeoutSeconds);
}

struct BotProfile {
    std::string id, username, avatar, shareUrl;
};

/// GET /users/@me。请求失败时 profile 与 error 都为空；应答无法解析时 error 为原因。
inline void fetchProfile(const drogon::HttpClientPtr& client, const std::string& accessToken,
                         std::function<void(std::optional<BotProfile> profile, std::string error)> done) {
    (void)client;
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setPath("/users/@me");
    request->addHeader("Host", kHost);
    request->addHeader("Authorization", "QQBot " + accessToken);
    httpsSend(kHost, request, [done = std::move(done)](drogon::ReqResult outcome, const drogon::HttpResponsePtr& response) {
        if (outcome != drogon::ReqResult::Ok || !response || response->statusCode() >= 300) { done(std::nullopt, {}); return; }
        BotProfile profile;
        try {
            const auto j = json::parse(response->body());
            profile.id = protocol::text(j, "id");
            if (profile.id.empty()) {
                if (const auto id = protocol::asInteger(j.contains("id") ? j["id"] : json()); id) profile.id = std::to_string(*id);
            }
            profile.username = protocol::text(j, "username");
            profile.avatar = protocol::text(j, "avatar");
            profile.shareUrl = protocol::text(j, "share_url");
        } catch (const std::exception& e) { done(std::nullopt, e.what()); return; }
        done(std::move(profile), {});
    });
}

/// POST /v2/generate_url_link，生成机器人分享链接。请求失败时 url 与 error 都为空；
/// 平台拒绝或应答无效时 error 为原因。
inline void generateShareUrl(const drogon::HttpClientPtr& client, const std::string& accessToken,
                             const std::string& callbackData,
                             std::function<void(std::string url, std::string error)> done) {
    (void)client;
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setPath("/v2/generate_url_link");
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->addHeader("Host", kHost);
    request->addHeader("Authorization", "QQBot " + accessToken);
    request->setBody(json{{"callback_data", callbackData}}.dump());
    httpsSend(kHost, request, [done = std::move(done)](drogon::ReqResult outcome, const drogon::HttpResponsePtr& response) {
        if (outcome != drogon::ReqResult::Ok || !response || response->statusCode() >= 300) { done({}, {}); return; }
        std::string url;
        try {
            const auto j = json::parse(response->body());
            const auto checked = checkedResponse(static_cast<int>(response->statusCode()), j);
            if (!checked.value("ok", false)) throw std::runtime_error(checked.value("message", std::string("分享链接接口未成功")));
            url = protocol::shareUrl(j);
            if (url.empty()) throw std::runtime_error("响应缺少有效的 data.url");
        } catch (const std::exception& e) { done({}, e.what()); return; }
        done(std::move(url), {});
    });
}

/// 分享链接里的机器人 QQ 号（robot_uin）；没有时为空。
inline std::string robotUin(const std::string& shareUrl) { return queryValue(shareUrl, "robot_uin"); }

/// 普通 QQ 头像（填真实 QQ 号）。机器人资料里的 avatar / qqapp+BotID 经常 404，这条和官民绑定后的用户/群头像同一套 CDN。
inline std::string qqPortrait(const std::string& uin, int size = 100) {
    if (uin.empty() || uin.find_first_not_of("0123456789") != std::string::npos) return {};
    if (size != 40 && size != 100 && size != 140 && size != 640) size = 100;
    return "https://q.qlogo.cn/g?b=qq&nk=" + uin + "&s=" + std::to_string(size);
}

}  // namespace catofficial::api
