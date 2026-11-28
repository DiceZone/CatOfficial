#pragma once

// 富媒体上传，换取发送消息用的 file_info。两条路：
//   · 公网 URL 直传：POST /v2/{users|groups}/{id}/files {file_type, url} → file_info；
//   · 本地文件分片：upload_prepare → 顺序 PUT 预签名分片 → upload_part_finish → /files 合并。
// 官方硬上限：任何类型超过 200MB 都会在最后一步报 850031，调用方应先检查大小。

#include "api.h"
#include "log.h"

#include <drogon/HttpClient.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace catofficial::media {
using json = nlohmann::json;

inline constexpr std::size_t kHardLimitBytes = 200ull * 1024 * 1024;
inline constexpr std::size_t kMd5PrefixBytes = 10002432;   // 官方要求前 10,002,432 字节的 MD5

/// 文件类型：1 图片、2 视频、3 语音、4 文件。
enum FileType { kImage = 1, kVideo = 2, kVoice = 3, kFile = 4 };

inline std::string hexDigest(const std::string& data, const EVP_MD* md) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    unsigned char buf[EVP_MAX_MD_SIZE]; unsigned int len = 0;
    const bool ok = EVP_DigestInit_ex(ctx, md, nullptr) == 1
        && EVP_DigestUpdate(ctx, data.data(), data.size()) == 1
        && EVP_DigestFinal_ex(ctx, buf, &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string out; out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) { out += kHex[buf[i] >> 4]; out += kHex[buf[i] & 0xF]; }
    return out;
}

struct ParsedUrl { std::string scheme, host, path, query; uint16_t port = 443; };
inline bool parseUrl(const std::string& url, ParsedUrl& out) {
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    out.scheme = url.substr(0, schemeEnd);
    if (out.scheme != "http" && out.scheme != "https") return false;
    const std::size_t hostStart = schemeEnd + 3;
    const std::size_t pathStart = url.find('/', hostStart);
    std::string authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos) { out.host = authority; out.port = out.scheme == "https" ? 443 : 80; }
    else {
        out.host = authority.substr(0, colon);
        try { out.port = static_cast<uint16_t>(std::stoi(authority.substr(colon + 1))); }
        catch (...) { return false; }
    }
    if (out.host.empty()) return false;
    const std::string rest = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    const std::size_t q = rest.find('?');
    out.path = q == std::string::npos ? rest : rest.substr(0, q);
    out.query = q == std::string::npos ? std::string() : rest.substr(q + 1);
    return true;
}

inline drogon::HttpClientPtr clientFor(const std::string& host, uint16_t port, bool tls) {
    const auto ip = api::resolveIpv4(host);
    return ip.empty() ? nullptr : drogon::HttpClient::newHttpClient(ip, port, tls);
}

/// 兼容两种响应形态：{code,message,data:{...}} 或直接 {...}。
inline const json* dataObject(const json& j) {
    if (!j.is_object()) return nullptr;
    if (j.contains("data") && j["data"].is_object()) return &j["data"];
    return &j;
}
inline std::string field(const json& j, const char* key) {
    const json* d = dataObject(j);
    return d ? d->value(key, std::string()) : std::string();
}
/// 部分数字字段以字符串返回（如 "1048576"），数字与字符串都接受。
inline std::size_t jsonSize(const json& v, std::size_t fallback) {
    if (v.is_number_unsigned()) return v.get<std::size_t>();
    if (v.is_number_integer()) { const long long x = v.get<long long>(); return x > 0 ? static_cast<std::size_t>(x) : 0; }
    if (v.is_string()) { try { return static_cast<std::size_t>(std::stoull(v.get_ref<const std::string&>())); } catch (...) {} }
    return fallback;
}
inline int jsonInt(const json& v, int fallback) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number_unsigned()) { const auto x = v.get<unsigned long long>(); return x <= static_cast<unsigned long long>(INT_MAX) ? static_cast<int>(x) : fallback; }
    if (v.is_string()) { try { return std::stoi(v.get_ref<const std::string&>()); } catch (...) {} }
    return fallback;
}

inline std::string basePath(bool direct, const std::string& target) {
    return (direct ? "/v2/users/" : "/v2/groups/") + target;
}

struct Upload {
    std::string fileInfo;  // 成功时非空
    std::string reason;    // 失败的简短原因；为空表示上传因异常中止
    std::string error;     // 平台或网络失败时的状态说明
};
using Done = std::function<void(Upload)>;
using TokenSource = std::function<std::string()>;

namespace detail {

struct Context {
    TokenSource token;
    Logger log;
    Done done;
    bool direct = false;
    std::string target;
    void say(LogLevel level, const std::string& message) const { if (log) log(level, message); }
    void fail(std::string reason, std::string error = {}) const { done(Upload{{}, std::move(reason), std::move(error)}); }
};
using ContextPtr = std::shared_ptr<const Context>;

/// drogon 事件循环不会捕获回调异常，任何 std::exception 都会直接 terminate（表现为闪退且
/// 无日志）。所有异步回调统一经此兜底。
template <typename Fn>
void guarded(const ContextPtr& ctx, const char* step, Fn&& fn) {
    try { fn(); }
    catch (const std::exception& e) {
        ctx->say(LogLevel::error, std::string(step) + " 抛出异常: " + e.what());
        ctx->fail({}, std::string("QQ 官方富媒体异常: ") + e.what());
    } catch (...) {
        ctx->say(LogLevel::error, std::string(step) + " 抛出未知异常");
        ctx->fail({}, "QQ 官方富媒体未知异常");
    }
}

inline std::string httpFailure(const drogon::HttpResponsePtr& resp) {
    return "HTTP " + std::to_string(resp ? static_cast<int>(resp->statusCode()) : 0) + " "
        + (resp ? std::string(resp->body()) : std::string("网络请求未完成"));
}

inline drogon::HttpRequestPtr apiPost(const ContextPtr& ctx, const std::string& path, const json& body) {
    auto r = drogon::HttpRequest::newHttpRequest();
    r->setMethod(drogon::Post); r->setPath(path);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->addHeader("Host", api::kHost);
    r->addHeader("Authorization", "QQBot " + ctx->token());
    r->setBody(body.dump());
    return r;
}

/// POST /files，换 file_info。
inline void requestFileInfo(const ContextPtr& ctx, const json& body, const char* step,
                            const char* failedError, const char* failedReason,
                            const char* missingReason, const char* successLog) {
    const std::string filesPath = basePath(ctx->direct, ctx->target) + "/files";
    api::httpsSend(api::kHost, apiPost(ctx, filesPath, body), [ctx, filesPath, step, failedError, failedReason, missingReason, successLog](
        drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
        guarded(ctx, step, [&] {
            if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                ctx->say(LogLevel::warn, "POST " + filesPath + " failed: " + httpFailure(resp));
                ctx->fail(failedReason, failedError);
                return;
            }
            const auto j = json::parse(resp->body(), nullptr, false);
            std::string fileInfo = field(j, "file_info");
            if (fileInfo.empty()) {
                ctx->say(LogLevel::warn, "/files 未返回 file_info: " + std::string(resp->body()));
                ctx->fail(missingReason);
                return;
            }
            ctx->say(LogLevel::info, successLog);
            ctx->done(Upload{std::move(fileInfo), {}, {}});
        });
    });
}

struct Part { int index = 0; std::string presignedUrl; std::size_t offset = 0; std::size_t size = 0; };

struct Chunked {
    std::shared_ptr<const std::string> data;
    std::string uploadId, fileName;
    int fileType = kFile;
    std::vector<Part> parts;
};
using ChunkedPtr = std::shared_ptr<const Chunked>;

inline void finalize(const ContextPtr& ctx, const ChunkedPtr& job) {
    requestFileInfo(ctx, json{{"file_type", job->fileType}, {"file_name", job->fileName},
                              {"upload_id", job->uploadId}, {"srv_send_msg", false}},
        "分片合并回调", "QQ 官方分片合并失败", "分片合并失败", "分片合并未返回 file_info",
        "分片合并成功，获得 file_info");
}

inline void uploadPart(const ContextPtr& ctx, const ChunkedPtr& job, std::size_t idx) {
    if (idx >= job->parts.size()) { finalize(ctx, job); return; }
    ctx->say(LogLevel::info, "开始上传分片 " + std::to_string(idx + 1) + "/" + std::to_string(job->parts.size())
        + " (index=" + std::to_string(job->parts[idx].index) + ")");
    guarded(ctx, "uploadPart 入口", [&] {
        const Part& p = job->parts[idx];
        ParsedUrl pu;
        if (!parseUrl(p.presignedUrl, pu)) { ctx->say(LogLevel::warn, "分片预签名地址无效，上传中止"); ctx->fail("分片预签名地址无效"); return; }
        auto r = drogon::HttpRequest::newHttpRequest();
        r->setMethod(drogon::Put);
        r->setPathEncode(false);
        r->setPath(pu.query.empty() ? pu.path : pu.path + "?" + pu.query);
        r->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);
        r->addHeader("Host", pu.host);
        r->setBody(job->data->substr(p.offset, p.size));
        auto afterPut = [ctx, job, idx](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
            guarded(ctx, "PUT 分片回调", [&] {
                const Part& p = job->parts[idx];
                if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                    ctx->say(LogLevel::warn, "PUT 分片 " + std::to_string(p.index) + " 失败: " + httpFailure(resp));
                    ctx->fail("分片上传失败", "QQ 官方分片上传失败");
                    return;
                }
                ctx->say(LogLevel::info, "PUT 分片 " + std::to_string(p.index) + " 成功 (" + std::to_string(p.size) + " bytes)，确认分片");
                const std::string partMd5 = hexDigest(job->data->substr(p.offset, p.size), EVP_md5());
                if (partMd5.empty()) { ctx->say(LogLevel::warn, "分片摘要计算失败，上传中止"); ctx->fail("分片摘要计算失败"); return; }
                const std::string finishPath = basePath(ctx->direct, ctx->target) + "/upload_part_finish";
                const auto body = json{{"upload_id", job->uploadId}, {"part_index", p.index},
                                       {"block_size", std::to_string(p.size)}, {"md5", partMd5}};
                api::httpsSend(api::kHost, apiPost(ctx, finishPath, body), [ctx, job, idx, finishPath](drogon::ReqResult rr2, const drogon::HttpResponsePtr& resp2) {
                    guarded(ctx, "upload_part_finish 回调", [&] {
                        if (rr2 != drogon::ReqResult::Ok || !resp2 || resp2->statusCode() >= 300) {
                            ctx->say(LogLevel::warn, "POST " + finishPath + " failed: " + httpFailure(resp2));
                            ctx->fail("分片确认失败", "QQ 官方分片确认失败");
                            return;
                        }
                        ctx->say(LogLevel::info, "分片 " + std::to_string(job->parts[idx].index) + " 确认完成");
                        uploadPart(ctx, job, idx + 1);
                    });
                });
            });
        };
        if (pu.scheme == "https") {
            api::httpsSend(pu.host, r, afterPut, 60.0, pu.port);
        } else {
            auto c = clientFor(pu.host, pu.port, false);
            if (!c) { const auto error = "无法解析分片上传域名 " + pu.host; ctx->fail(error, error); return; }
            c->sendRequest(r, [c, afterPut](drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) { afterPut(rr, resp); });
        }
    });
}

}  // namespace detail

/// 公网 URL 直传。direct 为 true 表示单聊（/v2/users），否则为群（/v2/groups）。
inline void uploadUrl(TokenSource token, bool direct, const std::string& target, int fileType,
                      const std::string& url, Done done, Logger log = {}) {
    auto ctx = std::make_shared<const detail::Context>(detail::Context{std::move(token), std::move(log), std::move(done), direct, target});
    detail::requestFileInfo(ctx, json{{"file_type", fileType}, {"url", url}, {"srv_send_msg", false}},
        "URL 直传回调", "QQ 官方富媒体上传失败", "富媒体 URL 直传失败", "富媒体上传未返回 file_info",
        "URL 直传成功，获得 file_info");
}

/// 本地文件分片上传。data 为完整文件内容；调用方应先确认不超过 kHardLimitBytes。
inline void uploadBytes(TokenSource token, bool direct, const std::string& target, int fileType,
                        const std::string& fileName, std::string data, Done done, Logger log = {}) {
    auto ctx = std::make_shared<const detail::Context>(detail::Context{std::move(token), std::move(log), std::move(done), direct, target});
    auto bytes = std::make_shared<const std::string>(std::move(data));
    detail::guarded(ctx, "upload_prepare 入口", [&] {
        const std::string md5 = hexDigest(*bytes, EVP_md5());
        const std::string sha1 = hexDigest(*bytes, EVP_sha1());
        const std::string md5Prefix = hexDigest(bytes->substr(0, bytes->size() < kMd5PrefixBytes ? bytes->size() : kMd5PrefixBytes), EVP_md5());
        if (md5.empty() || sha1.empty() || md5Prefix.empty()) {
            ctx->say(LogLevel::warn, "计算媒体摘要失败，跳过: " + fileName);
            ctx->fail("计算本地媒体摘要失败");
            return;
        }
        const std::string prepPath = basePath(direct, target) + "/upload_prepare";
        // file_size / block_size 在官方规格里是字符串，不是数字。
        const auto body = json{{"file_type", fileType}, {"file_size", std::to_string(bytes->size())}, {"file_name", fileName},
                               {"md5", md5}, {"sha1", sha1}, {"md5_10m", md5Prefix}};
        ctx->say(LogLevel::info, "发起 upload_prepare " + prepPath + " file_type=" + std::to_string(fileType) + " file_name=" + fileName);
        api::httpsSend(api::kHost, detail::apiPost(ctx, prepPath, body), [ctx, bytes, fileName, fileType, prepPath](
            drogon::ReqResult rr, const drogon::HttpResponsePtr& resp) {
            detail::guarded(ctx, "upload_prepare 回调", [&] {
                if (rr != drogon::ReqResult::Ok || !resp || resp->statusCode() >= 300) {
                    ctx->say(LogLevel::warn, "POST " + prepPath + " failed: " + detail::httpFailure(resp));
                    ctx->fail("分片上传准备失败", "QQ 官方分片上传准备失败");
                    return;
                }
                const auto j = json::parse(resp->body(), nullptr, false);
                const json* d = dataObject(j);
                if (!d) { ctx->say(LogLevel::warn, "upload_prepare 响应无效: " + std::string(resp->body())); ctx->fail("upload_prepare 响应无效"); return; }
                auto job = std::make_shared<detail::Chunked>();
                job->data = bytes; job->fileName = fileName; job->fileType = fileType;
                job->uploadId = d->value("upload_id", std::string());
                const std::size_t blockSize = d->contains("block_size") ? jsonSize((*d)["block_size"], std::size_t(1024 * 1024)) : std::size_t(1024 * 1024);
                if (job->uploadId.empty() || !d->contains("parts") || !(*d)["parts"].is_array()) {
                    ctx->say(LogLevel::warn, "upload_prepare 缺少 upload_id/parts: " + std::string(resp->body()));
                    ctx->fail("upload_prepare 缺少分片信息");
                    return;
                }
                std::size_t offset = 0;
                for (const auto& pj : (*d)["parts"]) {
                    if (!pj.is_object()) continue;
                    detail::Part p;
                    p.index = pj.contains("index") ? jsonInt(pj["index"], 0) : 0;
                    p.presignedUrl = pj.value("presigned_url", std::string());
                    if (p.presignedUrl.empty() || offset >= bytes->size()) { job->parts.clear(); break; }
                    const std::size_t bs = pj.contains("block_size") ? jsonSize(pj["block_size"], blockSize) : blockSize;
                    p.offset = offset;
                    p.size = bs < bytes->size() - offset ? bs : bytes->size() - offset;
                    offset += p.size;
                    job->parts.push_back(std::move(p));
                }
                if (job->parts.empty()) { ctx->say(LogLevel::warn, "upload_prepare 返回的分片列表无效"); ctx->fail("upload_prepare 分片列表无效"); return; }
                ctx->say(LogLevel::info, "upload_prepare 成功 upload_id=" + job->uploadId + " block_size=" + std::to_string(blockSize)
                    + " parts=" + std::to_string(job->parts.size()));
                detail::uploadPart(ctx, job, 0);
            });
        });
    });
}

}  // namespace catofficial::media
