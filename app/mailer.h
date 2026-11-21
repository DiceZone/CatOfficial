#pragma once

// 用 libcurl 经 SMTP 发信：给主人的通知（机器人掉线等）与官民绑定的验证码。
// 每封信在独立线程里发送，结果通过回调返回，不阻塞事件循环。

#include "config.h"

#include <catofficial/log.h>

#include <curl/curl.h>
#include <drogon/utils/Utilities.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace catofficial::app {

class Mailer {
public:
    using Done = std::function<void(bool ok, const std::string& error)>;

    Mailer(SmtpConfig config, Logger log) : config_(std::move(config)), log_(std::move(log)) {}

    bool usable() const { return config().usable(); }
    SmtpConfig config() const { std::lock_guard lock(mu_); return config_; }
    void setConfig(SmtpConfig config) { std::lock_guard lock(mu_); config_ = std::move(config); }

    void send(const std::string& to, const std::string& subject, const std::string& body, Done done = {}) {
        auto config = this->config();
        if (!config.usable()) { if (done) done(false, "未启用或未配置 SMTP"); return; }
        auto log = log_;
        std::thread([config, log, to, subject, body, done] {
            std::string error;
            const bool ok = deliver(config, to, subject, body, error);
            if (log) log(ok ? LogLevel::info : LogLevel::warn, ok ? "邮件已发送至 " + to + "：" + subject : "邮件发送失败（" + to + "）：" + error);
            if (done) done(ok, error);
        }).detach();
    }

    /// 给主人发通知；同一 key 在 throttleSeconds 内只发一次。
    void notifyOwner(const std::string& key, const std::string& subject, const std::string& body, int throttleSeconds = 600) {
        const auto config = this->config();
        if (!config.usable() || config.ownerEmail.empty()) return;
        {
            std::lock_guard lock(mu_);
            const auto now = std::time(nullptr);
            auto& last = lastSent_[key];
            if (last && now - last < throttleSeconds) return;
            last = now;
        }
        send(config.ownerEmail, subject, body);
    }

    static std::string encodeHeader(const std::string& text) { return "=?UTF-8?B?" + drogon::utils::base64Encode(text) + "?="; }

    static std::string message(const SmtpConfig& config, const std::string& to, const std::string& subject, const std::string& body) {
        char date[64];
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &tm);
        std::string encoded = drogon::utils::base64Encode(body);
        std::string wrapped;
        for (std::size_t i = 0; i < encoded.size(); i += 76) wrapped += encoded.substr(i, 76) + "\r\n";
        return "Date: " + std::string(date) + "\r\n"
               "From: " + encodeHeader("CatOfficial") + " <" + config.from + ">\r\n"
               "To: <" + to + ">\r\n"
               "Subject: " + encodeHeader(subject) + "\r\n"
               "MIME-Version: 1.0\r\n"
               "Content-Type: text/plain; charset=UTF-8\r\n"
               "Content-Transfer-Encoding: base64\r\n"
               "\r\n" + wrapped;
    }

private:
    struct Upload { const std::string* data; std::size_t offset = 0; };

    static std::size_t read(char* buffer, std::size_t size, std::size_t count, void* user) {
        auto* upload = static_cast<Upload*>(user);
        const std::size_t room = size * count;
        const std::size_t left = upload->data->size() - upload->offset;
        const std::size_t n = left < room ? left : room;
        std::memcpy(buffer, upload->data->data() + upload->offset, n);
        upload->offset += n;
        return n;
    }

    static bool deliver(const SmtpConfig& config, const std::string& to, const std::string& subject, const std::string& body, std::string& error) {
        CURL* curl = curl_easy_init();
        if (!curl) { error = "无法初始化 libcurl"; return false; }
        const std::string url = std::string(config.security == "ssl" ? "smtps://" : "smtp://") + config.host + ":" + std::to_string(config.port);
        const std::string payload = message(config, to, subject, body);
        Upload upload{&payload};
        curl_slist* recipients = curl_slist_append(nullptr, ("<" + to + ">").c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (config.security == "starttls") curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        if (!config.username.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERNAME, config.username.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, config.password.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, ("<" + config.from + ">").c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, &Mailer::read);
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        char message[CURL_ERROR_SIZE] = {};
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
        const CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) error = message[0] ? message : curl_easy_strerror(rc);
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        return rc == CURLE_OK;
    }

    SmtpConfig config_;
    Logger log_;
    mutable std::mutex mu_;
    std::map<std::string, std::time_t> lastSent_;
};

}  // namespace catofficial::app
