#pragma once

// 出站 HTTPS：系统 getaddrinfo 解析 + OpenSSL（SNI 与证书校验）。
// 与 Gateway WebSocket 同一策略，各平台共用：Drogon 按 IP 连接时不带 SNI，
// 它的 c-ares 在部分 VPN/CNAME 下也会失败。Windows 用系统证书库校验，
// 其余平台用 SSL_CERT_FILE / 系统 CA 目录。

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include "ssl_trust.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace catofficial::api {
namespace https_detail {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalid = INVALID_SOCKET;
inline void closeSocket(Socket s) { if (s != kInvalid) closesocket(s); }
#else
using Socket = int;
inline constexpr Socket kInvalid = -1;
inline void closeSocket(Socket s) { if (s != kInvalid) close(s); }
#endif

inline void setTimeout(Socket s, int seconds) {
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(seconds) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

inline Socket connectTcp(const std::string& host, uint16_t port, int seconds) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* results = nullptr;
    const auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || !results) return kInvalid;
    Socket connected = kInvalid;
    for (auto* entry = results; entry; entry = entry->ai_next) {
        const auto socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (socket == kInvalid) continue;
        setTimeout(socket, seconds);
        if (::connect(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
            connected = socket;
            break;
        }
        closeSocket(socket);
    }
    freeaddrinfo(results);
    return connected;
}

inline bool writeAll(SSL* ssl, const void* source, size_t size) {
    const auto* data = static_cast<const unsigned char*>(source);
    size_t offset = 0;
    while (offset < size) {
        const int written = SSL_write(ssl, data + offset, static_cast<int>(size - offset));
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

inline std::string headerValue(std::string_view headers, const char* name) {
    const std::string key = std::string("\r\n") + name + ":";
    std::string lower(headers);
    std::string needle = key;
    auto asciiLower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::transform(lower.begin(), lower.end(), lower.begin(), asciiLower);
    std::transform(needle.begin(), needle.end(), needle.begin(), asciiLower);
    auto pos = lower.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
    auto end = headers.find("\r\n", pos);
    return std::string(headers.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
}

inline std::optional<std::string> decodeChunked(std::string_view in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        const auto nl = in.find("\r\n", i);
        if (nl == std::string_view::npos) return std::nullopt;
        size_t size = 0;
        bool digit = false;
        for (size_t j = i; j < nl; ++j) {
            const unsigned char c = static_cast<unsigned char>(in[j]);
            if (c == ';') break;
            if (std::isspace(c)) continue;
            size_t v = 0;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
            else return std::nullopt;
            digit = true;
            size = (size << 4) | v;
        }
        if (!digit) return std::nullopt;
        i = nl + 2;
        if (size == 0) return out;
        if (i + size + 2 > in.size()) return std::nullopt;
        out.append(in.data() + i, size);
        i += size;
        if (in[i] != '\r' || in[i + 1] != '\n') return std::nullopt;
        i += 2;
        if (out.size() > 16 * 1024 * 1024) return std::nullopt;
    }
    return std::nullopt;
}

inline std::optional<std::pair<int, std::string>> parseHttp(const std::string& raw, bool closed) {
    const auto split = raw.find("\r\n\r\n");
    if (split == std::string::npos) return std::nullopt;
    const auto head = std::string_view(raw).substr(0, split);
    const auto rest = std::string_view(raw).substr(split + 4);
    const auto lineEnd = head.find("\r\n");
    const auto line = head.substr(0, lineEnd);
    const auto sp = line.find(' ');
    if (sp == std::string_view::npos) return std::nullopt;
    const int status = std::atoi(std::string(line.substr(sp + 1, 3)).c_str());
    const auto encoding = headerValue(head, "Transfer-Encoding");
    std::string encLower = encoding;
    std::transform(encLower.begin(), encLower.end(), encLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (encLower.find("chunked") != std::string::npos) {
        auto body = decodeChunked(rest);
        if (!body) return std::nullopt;
        return std::make_pair(status, std::move(*body));
    }
    const auto lengthText = headerValue(head, "Content-Length");
    if (!lengthText.empty()) {
        const auto length = static_cast<size_t>(std::strtoull(lengthText.c_str(), nullptr, 10));
        if (rest.size() < length) return std::nullopt;
        return std::make_pair(status, std::string(rest.substr(0, length)));
    }
    if (!closed) return std::nullopt;
    return std::make_pair(status, std::string(rest));
}

#ifdef _WIN32
inline bool verifyWindows(SSL* ssl, const std::string& host) {
    X509* peer = SSL_get1_peer_certificate(ssl);
    if (!peer) return false;
    const int encodedSize = i2d_X509(peer, nullptr);
    std::vector<unsigned char> encoded(encodedSize > 0 ? static_cast<size_t>(encodedSize) : 0);
    unsigned char* cursor = encoded.data();
    const bool encodedOk = encodedSize > 0 && i2d_X509(peer, &cursor) == encodedSize;
    X509_free(peer);
    if (!encodedOk) return false;
    PCCERT_CONTEXT certificate = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, encoded.data(), static_cast<DWORD>(encoded.size()));
    if (!certificate) return false;
    CERT_CHAIN_PARA chainParameters{};
    chainParameters.cbSize = sizeof(chainParameters);
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    const bool chained = CertGetCertificateChain(nullptr, certificate, nullptr, nullptr,
        &chainParameters, 0, nullptr, &chain) == TRUE;
    bool verified = false;
    if (chained) {
        std::wstring hostname(host.begin(), host.end());
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslParameters{};
        sslParameters.cbSize = sizeof(sslParameters);
        sslParameters.dwAuthType = AUTHTYPE_SERVER;
        sslParameters.pwszServerName = hostname.data();
        CERT_CHAIN_POLICY_PARA policyParameters{};
        policyParameters.cbSize = sizeof(policyParameters);
        policyParameters.pvExtraPolicyPara = &sslParameters;
        CERT_CHAIN_POLICY_STATUS policyStatus{};
        policyStatus.cbSize = sizeof(policyStatus);
        verified = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
            &policyParameters, &policyStatus) == TRUE && policyStatus.dwError == 0;
        CertFreeCertificateChain(chain);
    }
    CertFreeCertificateContext(certificate);
    return verified;
}
#endif

inline void loadTrust(SSL_CTX* ctx) { tls::loadClientTrust(ctx); }

inline std::string mimeOf(const drogon::HttpRequestPtr& req) {
    auto header = req->getHeader("content-type");
    if (!header.empty()) return header;
    switch (req->contentType()) {
        case drogon::CT_APPLICATION_JSON: return "application/json; charset=utf-8";
        case drogon::CT_APPLICATION_OCTET_STREAM: return "application/octet-stream";
        case drogon::CT_APPLICATION_X_FORM: return "application/x-www-form-urlencoded";
        default: return {};
    }
}

inline std::string serialize(const std::string& host, const drogon::HttpRequestPtr& req) {
    std::string path = req->path().empty() ? "/" : req->path();
    if (path.find('?') == std::string::npos && !req->query().empty()) {
        path += '?';
        path += req->query();
    }
    std::string out;
    out += req->getMethodString();
    out += ' ';
    out += path;
    out += " HTTP/1.1\r\nHost: ";
    out += host;
    out += "\r\nConnection: close\r\nAccept: */*\r\n";
    const auto mime = mimeOf(req);
    if (!mime.empty()) {
        out += "Content-Type: ";
        out += mime;
        out += "\r\n";
    }
    for (const auto& item : req->headers()) {
        const auto& key = item.first;
        if (key == "host" || key == "connection" || key == "content-length" || key == "content-type") continue;
        out += key;
        out += ": ";
        out += item.second;
        out += "\r\n";
    }
    const auto body = req->body();
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";
    out.append(body.data(), body.size());
    return out;
}

inline drogon::ReqResult perform(const std::string& host, uint16_t port, int seconds,
                                 const drogon::HttpRequestPtr& req, drogon::HttpResponsePtr& out) {
    const auto socket = connectTcp(host, port, seconds);
    if (socket == kInvalid) return drogon::ReqResult::BadServerAddress;
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        closeSocket(socket);
        return drogon::ReqResult::EncryptionFailure;
    }
#ifdef _WIN32
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
#else
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    loadTrust(ctx);
#endif
    SSL* ssl = SSL_new(ctx);
    drogon::ReqResult result = drogon::ReqResult::NetworkFailure;
    if (!ssl || SSL_set_tlsext_host_name(ssl, host.c_str()) != 1 ||
        SSL_set_fd(ssl, static_cast<int>(socket)) != 1 || SSL_connect(ssl) != 1) {
        result = drogon::ReqResult::HandshakeError;
    }
#ifdef _WIN32
    else if (!verifyWindows(ssl, host)) {
        result = drogon::ReqResult::InvalidCertificate;
    }
#else
    else if (SSL_get_verify_result(ssl) != X509_V_OK) {
        result = drogon::ReqResult::InvalidCertificate;
    }
#endif
    else {
        const auto wire = serialize(host, req);
        if (!writeAll(ssl, wire.data(), wire.size())) {
            result = drogon::ReqResult::NetworkFailure;
        } else {
            std::string raw;
            char buffer[4096];
            bool closed = false;
            while (raw.size() < 16 * 1024 * 1024) {
                const int n = SSL_read(ssl, buffer, sizeof(buffer));
                if (n > 0) {
                    raw.append(buffer, static_cast<size_t>(n));
                    if (auto parsed = parseHttp(raw, false)) {
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(parsed->first));
                        resp->setBody(std::move(parsed->second));
                        out = std::move(resp);
                        result = drogon::ReqResult::Ok;
                        closed = true;
                        break;
                    }
                    continue;
                }
                closed = true;
                break;
            }
            if (result != drogon::ReqResult::Ok) {
                if (auto parsed = parseHttp(raw, closed)) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(parsed->first));
                    resp->setBody(std::move(parsed->second));
                    out = std::move(resp);
                    result = drogon::ReqResult::Ok;
                } else {
                    result = drogon::ReqResult::BadResponse;
                }
            }
        }
    }
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
    closeSocket(socket);
    return result;
}

inline void finish(std::function<void(drogon::ReqResult, const drogon::HttpResponsePtr&)> cb,
                   drogon::ReqResult result, drogon::HttpResponsePtr resp) {
    try {
        auto* loop = drogon::app().getLoop();
        if (loop) {
            loop->queueInLoop([cb = std::move(cb), result, resp = std::move(resp)]() { cb(result, resp); });
            return;
        }
    } catch (...) {}
    cb(result, resp);
}

}  // namespace https_detail

using HttpsCallback = std::function<void(drogon::ReqResult, const drogon::HttpResponsePtr&)>;

inline std::string httpsFailure(drogon::ReqResult result) {
    switch (result) {
        case drogon::ReqResult::BadServerAddress: return "无法连接服务器";
        case drogon::ReqResult::Timeout: return "网络超时";
        case drogon::ReqResult::HandshakeError: return "TLS 握手失败";
        case drogon::ReqResult::InvalidCertificate: return "TLS 证书校验失败";
        case drogon::ReqResult::EncryptionFailure: return "TLS 初始化失败";
        case drogon::ReqResult::NetworkFailure: return "网络连接失败";
        case drogon::ReqResult::BadResponse: return "服务器返回了无法解析的响应";
        default: return std::string(drogon::to_string_view(result));
    }
}

/// 出站 HTTPS。timeoutSeconds<=0 时默认 20 秒。
inline void httpsSend(const std::string& host, const drogon::HttpRequestPtr& req, HttpsCallback cb,
                      double timeoutSeconds = 0.0, uint16_t port = 443) {
    const int seconds = timeoutSeconds > 0 ? (std::max)(1, static_cast<int>(timeoutSeconds + 0.5)) : 20;
    std::thread([host, port, seconds, req, cb = std::move(cb)]() mutable {
        drogon::HttpResponsePtr resp;
        const auto result = https_detail::perform(host, port, seconds, req, resp);
        https_detail::finish(std::move(cb), result, std::move(resp));
    }).detach();
}

}  // namespace catofficial::api
