#pragma once

// 官民绑定指令（默认 .bind）。两种方式：
//   · 暗号：在官方机器人与原 QQ 机器人都在的群里发送「.bind 暗号」。两边在几秒内各收到
//     恰好一条相同的消息，就把官方的 OpenID 与原 QQ 号（以及群）关联起来。
//   · 邮件：「.bind email QQ号」把验证码发到 QQ 邮箱，「.bind code 验证码」完成绑定。
// 只在 CatOfficial 的数据库里做号码映射，不搬迁任何应用数据。

#include "config.h"
#include "legacy.h"
#include "mailer.h"
#include "store.h"

#include <catofficial/event.h>
#include <catofficial/log.h>

#include <drogon/drogon.h>
#include <openssl/rand.h>

#include <cctype>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace catofficial::app {

class BindService : public std::enable_shared_from_this<BindService> {
public:
    using Reply = std::function<void(const std::string& text)>;

    BindService(BindConfig config, std::shared_ptr<Store> store, std::shared_ptr<Mailer> mailer, Logger log)
        : config_(std::move(config)), store_(std::move(store)), mailer_(std::move(mailer)), log_(std::move(log)) {}

    /// 有没有接入原 QQ 机器人（没有时暗号绑定不可用）。
    void setLegacyAvailable(bool available) { legacyAvailable_ = available; }

    static std::string mask(int64_t qq) {
        auto s = std::to_string(qq);
        if (s.size() <= 4) return s;
        return s.substr(0, 2) + std::string(s.size() - 4, '*') + s.substr(s.size() - 2);
    }

    /// 解析指令：返回空表示不是绑定指令；否则返回参数（可能为空）。
    std::optional<std::string> parse(const std::string& raw) const {
        auto text = trim(raw);
        auto command = config_.command;
        auto matches = [&](const std::string& prefix) {
            if (text.size() < prefix.size()) return false;
            for (std::size_t i = 0; i < prefix.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
            return text.size() == prefix.size() || std::isspace(static_cast<unsigned char>(text[prefix.size()]));
        };
        std::string used;
        if (matches(command)) used = command;
        else if (command[0] == '.' && matches("\xE3\x80\x82" + command.substr(1))) used = "\xE3\x80\x82" + command.substr(1);   // 全角句号
        else return std::nullopt;
        return trim(text.substr(used.size()));
    }

    /// 官方机器人收到的消息。是绑定指令时处理并返回 true。
    bool onOfficial(const event::MessageEvent& e, Reply reply) {
        if (!config_.enabled) return false;
        const auto args = parse(e.content);
        if (!args) return false;
        const auto [verb, rest] = split(*args);
        if (verb.empty() || verb == "help") reply(help());
        else if (verb == "status") reply(status(e));
        else if (verb == "remove") reply(remove(e));
        else if (verb == "email") requestCode(e, rest, std::move(reply));
        else if (verb == "code") reply(verifyCode(e, rest));
        else pairOfficial(e, *args, std::move(reply));
        return true;
    }

    /// 原 QQ 机器人收到的群消息。
    void onLegacy(const LegacyMessage& m) {
        if (!config_.enabled) return;
        const auto args = parse(m.text);
        if (!args || args->empty()) return;
        const auto [verb, rest] = split(*args);
        if (verb == "help" || verb == "status" || verb == "remove" || verb == "email" || verb == "code") return;
        Pending p;
        p.phrase = *args;
        p.official = false;
        p.legacy = m;
        submit(std::move(p));
    }

    std::string help() const {
        const auto& c = config_.command;
        std::string text = "官民绑定 " + c + "\n";
        if (legacyAvailable_) text += c + " <暗号> // 在官方机器人与原机器人都在的群里发送，两边同时收到即完成绑定\n";
        if (emailAvailable()) {
            text += c + " email <QQ号> // 把验证码发到该 QQ 的邮箱\n";
            text += c + " code <验证码> // 用邮件里的验证码完成绑定\n";
        }
        text += c + " status // 查看当前绑定\n";
        text += c + " remove // 解除你的绑定";
        return text;
    }

private:
    struct Pending {
        uint64_t id = 0;
        std::string phrase;
        std::time_t at = 0;
        bool official = true;
        event::MessageEvent e;
        Reply reply;
        LegacyMessage legacy;
    };
    struct Code { int64_t qq = 0; std::string code; std::time_t expires = 0; int attempts = 0; };

    static std::string trim(const std::string& s) {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    }
    static std::pair<std::string, std::string> split(const std::string& args) {
        const auto space = args.find_first_of(" \t");
        std::string verb = args.substr(0, space);
        for (auto& c : verb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return {verb, space == std::string::npos ? std::string() : trim(args.substr(space))};
    }
    void say(LogLevel level, const std::string& message) const { if (log_) log_(level, "官民绑定：" + message); }
    bool emailAvailable() const { return config_.emailEnabled && mailer_ && mailer_->usable(); }

    std::string status(const event::MessageEvent& e) {
        std::string text;
        if (const auto qq = store_->boundNumber(Kind::user, e.senderId)) text = "你已关联 QQ " + mask(*qq);
        else text = "你还没有关联 QQ，当前号码为 " + std::to_string(store_->number(Kind::user, e.senderId));
        if (e.scene == event::Scene::group) {
            if (const auto g = store_->boundNumber(Kind::group, e.targetId)) text += "；本群已关联群号 " + std::to_string(*g);
            else text += "；本群还没有关联群号";
        }
        return text;
    }

    std::string remove(const event::MessageEvent& e) {
        const auto number = store_->unbind(Kind::user, e.senderId);
        if (!number) return "你还没有关联 QQ";
        say(LogLevel::info, "用户 " + e.senderId + " 解除绑定");
        return "已解除绑定，你现在的号码为 " + std::to_string(number);
    }

    // ── 邮件验证码 ─────────────────────────────────────────

    void requestCode(const event::MessageEvent& e, const std::string& qqText, Reply reply) {
        if (!emailAvailable()) { reply("没有开启邮件验证，请联系机器人主人"); return; }
        if (qqText.size() < 5 || qqText.size() > 12 || qqText.find_first_not_of("0123456789") != std::string::npos) {
            reply("请写上 QQ 号，例如：" + config_.command + " email 12345678");
            return;
        }
        const auto qq = std::stoll(qqText);
        const auto now = std::time(nullptr);
        std::string code;
        {
            std::lock_guard lock(mu_);
            auto& sent = sentTimes_[e.senderId];
            while (!sent.empty() && now - sent.front() > 86400) sent.pop_front();
            if (!sent.empty() && now - sent.back() < 60) { reply("请求太频繁，请 1 分钟后再试"); return; }
            if (sent.size() >= 5) { reply("今天请求验证码的次数已用完，请明天再试"); return; }
            sent.push_back(now);
            unsigned char raw[4];
            RAND_bytes(raw, sizeof(raw));
            const uint32_t value = (static_cast<uint32_t>(raw[0]) << 24 | raw[1] << 16 | raw[2] << 8 | raw[3]) % 1000000;
            code = std::to_string(value);
            code.insert(0, 6 - code.size(), '0');
            codes_[e.senderId] = Code{qq, code, now + config_.codeTtlMinutes * 60, 0};
        }
        const auto address = qqText + "@" + config_.emailDomain;
        const auto minutes = std::to_string(config_.codeTtlMinutes);
        const auto body = "你正在把 QQ " + qqText + " 关联到官方机器人上的身份。\n\n验证码：" + code + "\n\n"
            + minutes + " 分钟内在机器人对话里发送「" + config_.command + " code " + code + "」即可完成绑定。\n如果不是你本人操作，请忽略这封邮件。";
        auto weak = weak_from_this();
        mailer_->send(address, "QQ 身份绑定验证码", body, [reply, qqText, minutes, weak](bool ok, const std::string&) {
            drogon::app().getLoop()->queueInLoop([reply, ok, qqText, minutes, weak] {
                auto self = weak.lock(); if (!self) return;
                reply(ok ? "验证码已发到 " + mask(std::stoll(qqText)) + "@" + self->config_.emailDomain + "，请在 " + minutes + " 分钟内发送「"
                               + self->config_.command + " code 验证码」完成绑定"
                         : "验证码邮件发送失败，请稍后再试或联系机器人主人");
            });
        });
    }

    std::string verifyCode(const event::MessageEvent& e, const std::string& input) {
        Code code;
        {
            std::lock_guard lock(mu_);
            auto it = codes_.find(e.senderId);
            if (it == codes_.end()) return "请先发送「" + config_.command + " email QQ号」获取验证码";
            if (std::time(nullptr) > it->second.expires) { codes_.erase(it); return "验证码已过期，请重新获取"; }
            if (it->second.code != input) {
                if (++it->second.attempts >= 5) { codes_.erase(it); return "验证码错误次数过多，请重新获取"; }
                return "验证码不正确";
            }
            code = it->second;
            codes_.erase(it);
        }
        if (!store_->bind(Kind::user, e.senderId, code.qq)) return "QQ " + mask(code.qq) + " 已关联到另一个官方身份，如需更换请先在那边解除";
        say(LogLevel::info, "用户 " + e.senderId + " 通过邮件验证关联 QQ " + std::to_string(code.qq));
        return "绑定完成，你的身份已关联 QQ " + mask(code.qq);
    }

    // ── 暗号配对 ───────────────────────────────────────────

    void pairOfficial(const event::MessageEvent& e, const std::string& phrase, Reply reply) {
        if (!legacyAvailable_) {
            reply(emailAvailable() ? "这里没有接入原 QQ 机器人，无法用暗号绑定。可以改用「" + config_.command + " email QQ号」"
                                   : "这里没有接入原 QQ 机器人，无法用暗号绑定");
            return;
        }
        if (e.scene != event::Scene::group) { reply("暗号绑定需要在官方机器人与原机器人都在的群里发送"); return; }
        Pending p;
        p.phrase = phrase;
        p.official = true;
        p.e = e;
        p.reply = std::move(reply);
        submit(std::move(p));
    }

    void submit(Pending incoming) {
        const auto now = std::time(nullptr);
        incoming.at = now;
        std::vector<Pending> conflicts;
        std::optional<std::pair<Pending, Pending>> pair;
        uint64_t watch = 0;
        {
            std::lock_guard lock(mu_);
            pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                [&](const Pending& p) { return now - p.at > config_.windowSeconds; }), pending_.end());
            const bool sameSide = std::any_of(pending_.begin(), pending_.end(),
                [&](const Pending& p) { return p.phrase == incoming.phrase && p.official == incoming.official; });
            if (sameSide) {
                // 同一暗号在同一侧出现两次：分不清谁是谁，全部作废。
                for (auto it = pending_.begin(); it != pending_.end();) {
                    if (it->phrase == incoming.phrase) { conflicts.push_back(std::move(*it)); it = pending_.erase(it); } else ++it;
                }
                conflicts.push_back(std::move(incoming));
            } else {
                auto other = std::find_if(pending_.begin(), pending_.end(),
                    [&](const Pending& p) { return p.phrase == incoming.phrase && p.official != incoming.official; });
                if (other != pending_.end()) {
                    Pending counterpart = std::move(*other);
                    pending_.erase(other);
                    pair = incoming.official ? std::make_pair(std::move(incoming), std::move(counterpart))
                                             : std::make_pair(std::move(counterpart), std::move(incoming));
                } else {
                    incoming.id = ++nextId_;
                    if (incoming.official) watch = incoming.id;
                    pending_.push_back(std::move(incoming));
                }
            }
        }
        for (auto& c : conflicts)
            if (c.official && c.reply) c.reply("有人同时使用了同一个暗号，请换一个不容易撞的暗号再试");
        if (pair) complete(pair->first, pair->second.legacy);
        if (watch) {
            std::weak_ptr<BindService> weak = weak_from_this();
            drogon::app().getLoop()->runAfter(config_.windowSeconds + 1.0, [weak, watch] { if (auto self = weak.lock()) self->expire(watch); });
        }
    }

    void expire(uint64_t id) {
        Pending timedOut;
        {
            std::lock_guard lock(mu_);
            auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.id == id; });
            if (it == pending_.end()) return;
            timedOut = std::move(*it);
            pending_.erase(it);
        }
        if (timedOut.reply)
            timedOut.reply(std::to_string(config_.windowSeconds) + " 秒内原机器人那边没有收到同一条消息。请确认原机器人也在这个群，并且两边收到的内容完全一致");
    }

    void complete(const Pending& official, const LegacyMessage& legacy) {
        const auto& e = official.e;
        std::string text;
        const auto bound = store_->boundNumber(Kind::user, e.senderId);
        if (bound && *bound == legacy.userId) text = "你的身份早已关联 QQ " + mask(legacy.userId);
        else if (store_->bind(Kind::user, e.senderId, legacy.userId)) {
            text = "绑定完成，你的身份已关联 QQ " + mask(legacy.userId);
            say(LogLevel::info, "用户 " + e.senderId + " 用暗号关联 QQ " + std::to_string(legacy.userId));
        } else {
            text = "QQ " + mask(legacy.userId) + " 已关联到另一个官方身份，如需更换请先在那边解除";
            if (official.reply) official.reply(text);
            return;
        }
        if (const auto group = store_->boundNumber(Kind::group, e.targetId)) {
            text += *group == legacy.groupId ? "；本群已关联群号 " + std::to_string(legacy.groupId)
                                             : "；本群已关联到其他群号，如有误请联系机器人主人";
        } else if (legacy.role == "owner" || legacy.role == "admin") {
            if (store_->bind(Kind::group, e.targetId, legacy.groupId)) {
                store_->clearGroupVotes(e.targetId);
                text += "；本群已关联群号 " + std::to_string(legacy.groupId);
                say(LogLevel::info, "群 " + e.targetId + " 由群主或管理员确认，关联群号 " + std::to_string(legacy.groupId));
            } else text += "；群号 " + std::to_string(legacy.groupId) + " 已关联到另一个官方群";
        } else {
            const int votes = store_->addGroupVote(e.targetId, legacy.groupId, legacy.userId);
            if (votes >= config_.groupConfirmUsers && store_->bind(Kind::group, e.targetId, legacy.groupId)) {
                store_->clearGroupVotes(e.targetId);
                text += "；本群已关联群号 " + std::to_string(legacy.groupId);
                say(LogLevel::info, "群 " + e.targetId + " 经 " + std::to_string(votes) + " 位成员确认，关联群号 " + std::to_string(legacy.groupId));
            } else {
                text += "；本群还需要 " + std::to_string(config_.groupConfirmUsers - votes) + " 位成员（或一位群主、管理员）完成绑定，才会关联群号";
            }
        }
        if (official.reply) official.reply(text);
    }

    BindConfig config_;
    std::shared_ptr<Store> store_;
    std::shared_ptr<Mailer> mailer_;
    Logger log_;
    std::atomic<bool> legacyAvailable_{false};
    std::mutex mu_;
    std::deque<Pending> pending_;
    uint64_t nextId_ = 0;
    std::map<std::string, Code> codes_;
    std::map<std::string, std::deque<std::time_t>> sentTimes_;
};

}  // namespace catofficial::app
