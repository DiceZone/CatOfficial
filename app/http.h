#pragma once

// 用 libcurl 做的同步 HTTP 请求，只在工作线程里调用：下载图片缓存、把素材 PUT 到预签名地址。
// 官方接口本身仍走 drogon 的异步客户端。

#include <curl/curl.h>

#include <cctype>
#include <cstddef>
#include <string>

namespace catofficial::app::http {

struct Response {
    long status = 0;
    std::string body, contentType, etag, error;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

namespace detail {
struct Sink { std::string* out; std::size_t limit; bool overflow = false; };
inline std::size_t write(char* data, std::size_t size, std::size_t count, void* user) {
    auto* sink = static_cast<Sink*>(user);
    const auto n = size * count;
    if (sink->out->size() + n > sink->limit) { sink->overflow = true; return 0; }
    sink->out->append(data, n);
    return n;
}
inline std::size_t header(char* data, std::size_t size, std::size_t count, void* user) {
    auto* r = static_cast<Response*>(user);
    std::string line(data, size * count);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        const auto last = value.find_last_not_of(" \t\r\n");
        value = first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
        if (key == "etag") r->etag = value;
    }
    return size * count;
}
}  // namespace detail

/// GET；响应体超过 maxBytes 时失败。
inline Response get(const std::string& url, std::size_t maxBytes, long timeoutSeconds = 30) {
    Response r;
    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "无法初始化 curl"; return r; }
    detail::Sink sink{&r.body, maxBytes};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CatOfficial");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail::write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    const auto code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    char* type = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
    if (type) r.contentType = type;
    if (sink.overflow) r.error = "文件过大";
    else if (code != CURLE_OK) r.error = curl_easy_strerror(code);
    curl_easy_cleanup(curl);
    return r;
}

/// PUT 二进制内容（素材上传到预签名地址）。
inline Response put(const std::string& url, const std::string& body, const std::string& contentType, long timeoutSeconds = 120) {
    Response r;
    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "无法初始化 curl"; return r; }
    detail::Sink sink{&r.body, 1 << 20};
    curl_slist* headers = curl_slist_append(nullptr, ("Content-Type: " + contentType).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail::write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, detail::header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    const auto code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    if (code != CURLE_OK) r.error = curl_easy_strerror(code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return r;
}

}  // namespace catofficial::app::http
