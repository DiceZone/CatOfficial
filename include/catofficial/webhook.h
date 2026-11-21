#pragma once

// QQ 机器人 API v2 Webhook 的 Ed25519 签名。
// 种子算法与官方文档一致：把 Bot Secret 反复拼接，直到不少于 32 字节，再取前 32 字节。
// https://bot.q.qq.com/wiki/develop/api-v2/dev-prepare/interface-framework/sign.html
// https://bot.q.qq.com/wiki/develop/api-v2/dev-prepare/event-emit/webhook.html

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace catofficial::webhook {

inline bool isWebhook(std::string mode) {
    for (char& ch : mode) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return mode == "webhook";
}

inline std::string seedFromSecret(const std::string& secret) {
    std::string seed = secret;
    while (seed.size() < 32) {
        if (seed.empty()) return {};
        seed += seed;
    }
    seed.resize(32);
    return seed;
}

inline std::string toHex(const unsigned char* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(size * 2, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kDigits[data[i] >> 4];
        out[i * 2 + 1] = kDigits[data[i] & 0x0f];
    }
    return out;
}

inline bool fromHex(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nibble = [](unsigned char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(static_cast<unsigned char>(hex[i]));
        const int lo = nibble(static_cast<unsigned char>(hex[i + 1]));
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

inline EVP_PKEY* privateKey(const std::string& secret) {
    const std::string seed = seedFromSecret(secret);
    if (seed.size() != 32) return nullptr;
    return EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(seed.data()), seed.size());
}

inline std::string sign(const std::string& secret, const std::string& message) {
    EVP_PKEY* key = privateKey(secret);
    EVP_MD_CTX* ctx = key ? EVP_MD_CTX_new() : nullptr;
    std::array<unsigned char, 64> signature{};
    std::size_t size = signature.size();
    const bool ok = key && ctx
        && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestSign(ctx, signature.data(), &size,
            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1
        && size == signature.size();
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok ? toHex(signature.data(), signature.size()) : std::string();
}

// 校验 X-Signature-Timestamp + 原始 HTTP body。签名必须是 128 位十六进制，
// 且最后一字节的高三位为 0（与官方 Go 示例的规范签名检查一致）。
inline bool verify(const std::string& secret, const std::string& timestamp,
                   const std::string& body, const std::string& signatureHex) {
    if (secret.empty() || timestamp.empty() || signatureHex.size() != 128) return false;
    std::string signature;
    if (!fromHex(signatureHex, signature) || signature.size() != 64) return false;
    if ((static_cast<unsigned char>(signature.back()) & 224) != 0) return false;
    EVP_PKEY* secretKey = privateKey(secret);
    std::array<unsigned char, 32> rawPublic{};
    std::size_t publicSize = rawPublic.size();
    EVP_PKEY* key = secretKey && EVP_PKEY_get_raw_public_key(secretKey, rawPublic.data(), &publicSize) == 1
        ? EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, rawPublic.data(), publicSize) : nullptr;
    EVP_PKEY_free(secretKey);
    EVP_MD_CTX* ctx = key ? EVP_MD_CTX_new() : nullptr;
    const std::string message = timestamp + body;
    const bool ok = key && ctx
        && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

inline std::string validationSignature(const std::string& secret, const std::string& eventTs,
                                       const std::string& plainToken) {
    return sign(secret, eventTs + plainToken);
}

struct Reply {
    int status = 401;
    std::string body;
    /// 需要记日志的原因（签名不符、事件处理抛出异常）；正常请求为空。
    std::string error;
};

using Dispatch = std::function<void(const std::string& type, const nlohmann::json& d, const std::string& eventId)>;

/// 处理一次 Webhook 回调：X-Signature-Timestamp、X-Signature-Ed25519 与原始 body。
/// 回调地址验证（op 13）在官方示例里不带签名，允许放行；其余事件必须验过 Ed25519。
/// 带了签名头的请求，包括地址验证，签名不对一律拒绝。op 0 的事件交给 dispatch。
inline Reply handle(const std::string& secret, const std::string& timestamp, const std::string& signature,
                    const std::string& raw, const Dispatch& dispatch) {
    using json = nlohmann::json;
    json payload = json::parse(raw, nullptr, false);
    if (!payload.is_object()) return {400, {}, {}};
    const int op = payload.value("op", -1);
    const bool signedRequest = !timestamp.empty() || !signature.empty();
    if (signedRequest) {
        if (!verify(secret, timestamp, raw, signature)) return {401, {}, "Webhook 签名校验失败"};
    } else if (op != 13) {
        return {401, {}, {}};
    }
    if (op == 13) {
        const json data = payload.value("d", json::object());
        if (!data.is_object()) return {400, {}, {}};
        const auto token = data.value("plain_token", std::string());
        const auto eventTs = data.value("event_ts", std::string());
        if (token.empty() || eventTs.empty()) return {400, {}, {}};
        const auto signedToken = validationSignature(secret, eventTs, token);
        if (signedToken.empty()) return {500, {}, {}};
        return {200, json{{"plain_token", token}, {"signature", signedToken}}.dump(), {}};
    }
    if (op == 0 && dispatch) {
        try {
            dispatch(payload.value("t", std::string()),
                payload.contains("d") && payload["d"].is_object() ? payload["d"] : json::object(),
                payload.value("id", std::string()));
        } catch (const std::exception& e) {
            return {500, {}, std::string("Webhook 事件处理失败：") + e.what()};
        }
    }
    return {200, json{{"op", 12}}.dump(), {}};
}

/// 同一个回调地址可能服务多个机器人，按请求头 X-Bot-Appid 找到接收者。
/// 一个 AppID 同一时间只交给一个接收者；ownerId 用来避免旧接收者解绑时误删新接收者。
template <class T>
class Registry {
public:
    /// 绑定；若该 AppID 原先属于另一个接收者，返回 true。
    bool bind(const std::string& appId, const std::string& ownerId, std::weak_ptr<T> owner) {
        if (appId.empty()) return false;
        std::lock_guard lock(mu_);
        auto existing = slots_.find(appId);
        const bool replaced = existing != slots_.end() && existing->second.ownerId != ownerId;
        slots_[appId] = Slot{ownerId, std::move(owner)};
        return replaced;
    }
    void unbind(const std::string& appId, const std::string& ownerId) {
        if (appId.empty()) return;
        std::lock_guard lock(mu_);
        auto it = slots_.find(appId);
        if (it != slots_.end() && it->second.ownerId == ownerId) slots_.erase(it);
    }
    /// 查找接收者；已销毁或 stillReceiving 返回 false 的条目顺带清除。
    template <class Pred>
    std::shared_ptr<T> find(const std::string& appId, Pred stillReceiving) {
        std::lock_guard lock(mu_);
        auto it = slots_.find(appId);
        if (it == slots_.end()) return nullptr;
        auto owner = it->second.owner.lock();
        if (!owner || !stillReceiving(*owner)) { slots_.erase(it); return nullptr; }
        return owner;
    }

private:
    struct Slot { std::string ownerId; std::weak_ptr<T> owner; };
    std::mutex mu_;
    std::unordered_map<std::string, Slot> slots_;
};

}  // namespace catofficial::webhook
