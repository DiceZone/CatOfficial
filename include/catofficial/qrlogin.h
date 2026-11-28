#pragma once

// 扫码登录官方机器人：在 q.qq.com 建一个绑定任务，用户用手机 QQ 扫码确认后，轮询取回
// AppID 与加密的 AppSecret，用本地生成的 AES 密钥解开。密钥只留在本进程，调用方只拿到
// 会话 ID 与二维码地址。

#include "api.h"

#include <drogon/HttpClient.h>
#include <drogon/utils/Utilities.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace catofficial::qrlogin {
using json = nlohmann::json;

struct Result {
    bool ok = false;
    json data;          // begin：{sessionId, url}；poll：{status: pending|expired|completed, appId?, appSecret?}
    std::string error;
};
using Callback = std::function<void(Result)>;

namespace detail {
struct Session { std::string taskId, key; std::time_t createdAt = 0; };
inline std::mutex& mutex() { static std::mutex m; return m; }
inline std::map<std::string, Session>& sessions() { static std::map<std::string, Session> s; return s; }

inline std::string randomHex(std::size_t bytes) {
    std::string raw(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(raw.data()), static_cast<int>(bytes)) != 1) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : raw) { out += kHex[b >> 4]; out += kHex[b & 0x0f]; }
    return out;
}

/// AES-256-GCM：12 字节 IV + 密文 + 16 字节 tag，整体 base64。
inline std::string decrypt(const std::string& cipher64, const std::string& key) {
    const auto raw = drogon::utils::base64Decode(cipher64);
    if (raw.size() < 29 || key.size() != 32) return {};
    const auto* iv = reinterpret_cast<const unsigned char*>(raw.data());
    const auto* tag = reinterpret_cast<const unsigned char*>(raw.data() + raw.size() - 16);
    const auto* enc = reinterpret_cast<const unsigned char*>(raw.data() + 12);
    const int encLen = static_cast<int>(raw.size()) - 28;
    int len = 0, total = 0;
    std::string out(encLen, '\0');
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    if (!c) return {};
    bool ok = EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
        && EVP_DecryptInit_ex(c, nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.data()), iv) == 1
        && EVP_DecryptUpdate(c, reinterpret_cast<unsigned char*>(out.data()), &len, enc, encLen) == 1;
    total = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, const_cast<unsigned char*>(tag)) == 1
        && EVP_DecryptFinal_ex(c, reinterpret_cast<unsigned char*>(out.data()) + total, &len) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) return {};
    out.resize(total + len);
    return out;
}

inline drogon::HttpRequestPtr request(const std::string& path, const json& body, const std::string& userAgent) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath(path);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->addHeader("Accept", "application/json");
    req->addHeader("User-Agent", userAgent);
    req->addHeader("Host", "q.qq.com");
    req->setBody(body.dump());
    return req;
}
}  // namespace detail

/// 开始扫码。source 会出现在二维码页面的来源参数里。
inline void begin(const std::string& source, Callback cb) {
    std::string key(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), 32) != 1) { cb({false, {}, "无法生成扫码密钥"}); return; }
    // 先建好请求再把 key 移进回调：函数实参的求值顺序不确定，写在同一个调用里 key 可能已被移走。
    auto req = detail::request("/lite/create_bind_task", json{{"key", drogon::utils::base64Encode(key)}}, source + "-QQBot/1.0");
    api::httpsSend("q.qq.com", req,
        [key = std::move(key), source, cb = std::move(cb)](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) mutable {
            try {
                if (r != drogon::ReqResult::Ok || !resp)
                    throw std::runtime_error("创建扫码任务失败（" + api::httpsFailure(r) + "）");
                if (resp->statusCode() >= 300)
                    throw std::runtime_error("创建扫码任务失败（QQ 服务返回 HTTP " + std::to_string(static_cast<int>(resp->statusCode())) + "）");
                const auto j = json::parse(resp->body());
                if (j.value("retcode", -1) != 0) throw std::runtime_error(j.value("msg", std::string("创建扫码任务失败")));
                const auto task = j.at("data").at("task_id").get<std::string>();
                const auto sid = detail::randomHex(18);
                if (sid.empty()) throw std::runtime_error("无法生成扫码会话");
                {
                    std::lock_guard lock(detail::mutex());
                    const auto now = std::time(nullptr);
                    for (auto it = detail::sessions().begin(); it != detail::sessions().end();)
                        it = now - it->second.createdAt > 600 ? detail::sessions().erase(it) : std::next(it);
                    detail::sessions()[sid] = {task, std::move(key), now};
                }
                cb({true, {{"sessionId", sid}, {"url", "https://q.qq.com/qqbot/openclaw/connect.html?task_id=" + task + "&source=" + source + "&_wv=2"}}, {}});
            } catch (const std::exception& e) { cb({false, {}, e.what()}); }
        });
}

/// 查询扫码结果。完成时 data 里带 appId 与解密后的 appSecret，会话随即作废。
inline void poll(const std::string& sid, const std::string& source, Callback cb) {
    detail::Session session;
    {
        std::lock_guard lock(detail::mutex());
        auto it = detail::sessions().find(sid);
        if (it == detail::sessions().end()) { cb({false, {}, "扫码会话不存在或已过期"}); return; }
        session = it->second;
    }
    auto req = detail::request("/lite/poll_bind_result", json{{"task_id", session.taskId}}, source + "-QQBot/1.0");
    api::httpsSend("q.qq.com", req,
        [sid, session = std::move(session), cb = std::move(cb)](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) mutable {
            try {
                if (r != drogon::ReqResult::Ok || !resp)
                    throw std::runtime_error("查询扫码状态失败（" + api::httpsFailure(r) + "）");
                if (resp->statusCode() >= 300)
                    throw std::runtime_error("查询扫码状态失败（QQ 服务返回 HTTP " + std::to_string(static_cast<int>(resp->statusCode())) + "）");
                const auto j = json::parse(resp->body());
                if (j.value("retcode", -1) != 0) throw std::runtime_error(j.value("msg", std::string("查询扫码状态失败")));
                const auto d = j.value("data", json::object());
                const int status = d.value("status", 0);
                if (status == 3) {
                    { std::lock_guard lock(detail::mutex()); detail::sessions().erase(sid); }
                    cb({true, {{"status", "expired"}}, {}});
                    return;
                }
                if (status != 2) { cb({true, {{"status", "pending"}}, {}}); return; }
                const auto secret = detail::decrypt(d.value("bot_encrypt_secret", std::string()), session.key);
                if (secret.empty()) throw std::runtime_error("扫码凭据解密失败");
                { std::lock_guard lock(detail::mutex()); detail::sessions().erase(sid); }
                cb({true, {{"status", "completed"}, {"appId", d.value("bot_appid", std::string())}, {"appSecret", secret}}, {}});
            } catch (const std::exception& e) { cb({false, {}, e.what()}); }
        });
}

}  // namespace catofficial::qrlogin
