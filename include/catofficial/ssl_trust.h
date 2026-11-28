#pragma once

// 出站 TLS 信任锚。Windows 由调用方用系统证书库校验；其余平台加载
// SSL_CERT_FILE / SSL_CERT_DIR 与常见系统 CA 目录（含 Android）。
// HTTPS 与 QQ Gateway WebSocket 共用，避免一边能连、一边证书校验失败。

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cstdlib>

namespace catofficial::tls {

inline void loadClientTrust(SSL_CTX* ctx) {
#ifdef _WIN32
    (void)ctx;
#else
    if (!ctx) return;
    if (const char* file = std::getenv("SSL_CERT_FILE"); file && *file)
        SSL_CTX_load_verify_locations(ctx, file, nullptr);
    if (const char* dir = std::getenv("SSL_CERT_DIR"); dir && *dir)
        SSL_CTX_load_verify_locations(ctx, nullptr, dir);
    SSL_CTX_set_default_verify_paths(ctx);
    // 目录不存在时失败并被下面清掉；Linux、Android、部分容器镜像各认其中一条。
    for (const char* extra : {
             "/etc/ssl/certs",
             "/etc/pki/tls/certs",
             "/apex/com.android.conscrypt/cacerts",
             "/system/etc/security/cacerts",
         }) {
        SSL_CTX_load_verify_locations(ctx, nullptr, extra);
    }
    ERR_clear_error();
#endif
}

}  // namespace catofficial::tls
