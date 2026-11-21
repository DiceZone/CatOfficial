#pragma once

// 自定义回复：收到的消息命中关键词规则时，由 CatOfficial 直接回复，不经过 OneBot 应用。
// 规则存在 data/replies.json，与 Dice!Next 的自定义回复同一套写法：
//   · 条件：完全匹配 / 包含 / 前缀 / 正则，多个条件「任一满足」或「全部满足」；
//   · 回复：可写多条，按权重随机选一条；支持 {变量}、{a|b|c} 随机选一项、正则的 {$1}…；
//   · 限制：触发概率、冷却（群里按群算，私聊按人算）、生效范围、只对哪些机器人生效；
//   · 优先级数字越大越先匹配，默认 100。
// 自带一条「CatOfficial 状态」（#catofficial / #co），可以改，也可以恢复默认。

#include "paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <vector>

namespace catofficial::app {

struct ReplyCondition {
    std::string type = "keyword";   // keyword（完全匹配）/ search（包含）/ prefix / regex
    std::string content;
};

struct ReplyRule {
    std::string id;
    std::string name;
    bool enabled = true;
    bool builtin = false;               // 自带规则：可以修改，不能删除，可以恢复默认
    int priority = 100;
    std::vector<ReplyCondition> conditions;
    std::string logic = "or";           // or / and
    std::vector<std::string> results;
    std::vector<int> weights;           // 与 results 一一对应；缺省为 1
    std::string format = "plain";       // plain / markdown
    std::string scope = "all";          // all / group / private
    std::vector<std::string> bots;      // 空 = 全部官方机器人
    int prob = 100;                     // 触发概率 0–100
    int cooldownSec = 0;
    bool passThrough = false;           // 回复后仍把消息转给 OneBot 应用
};

inline void to_json(nlohmann::json& j, const ReplyRule& r) {
    nlohmann::json conditions = nlohmann::json::array();
    for (const auto& c : r.conditions) conditions.push_back({{"type", c.type}, {"content", c.content}});
    j = nlohmann::json{{"id", r.id}, {"name", r.name}, {"enabled", r.enabled}, {"builtin", r.builtin}, {"priority", r.priority},
                       {"conditions", conditions}, {"logic", r.logic}, {"results", r.results}, {"weights", r.weights},
                       {"format", r.format}, {"scope", r.scope}, {"bots", r.bots}, {"prob", r.prob},
                       {"cooldown_sec", r.cooldownSec}, {"pass_through", r.passThrough}};
}

/// 校验并读入一条规则；出错时抛出带中文说明的异常。
inline ReplyRule parseReplyRule(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("规则格式不对");
    ReplyRule r;
    r.id = j.value("id", std::string());
    r.name = j.value("name", std::string());
    r.enabled = j.value("enabled", true);
    r.builtin = j.value("builtin", false);
    r.priority = j.value("priority", 100);
    r.logic = j.value("logic", std::string("or")) == "and" ? "and" : "or";
    for (const auto& c : j.value("conditions", nlohmann::json::array())) {
        ReplyCondition cond{c.value("type", std::string("keyword")), c.value("content", std::string())};
        if (cond.type != "keyword" && cond.type != "search" && cond.type != "prefix" && cond.type != "regex")
            throw std::runtime_error("未知的匹配方式：" + cond.type);
        if (cond.content.empty()) continue;
        if (cond.type == "regex") {
            try { std::regex test(cond.content, std::regex::ECMAScript); }
            catch (const std::regex_error& e) { throw std::runtime_error("正则表达式无效：" + cond.content + "（" + e.what() + "）"); }
        }
        r.conditions.push_back(std::move(cond));
    }
    if (r.conditions.empty()) throw std::runtime_error("至少需要一个有内容的匹配条件");
    const auto results = j.value("results", nlohmann::json::array());
    const auto weights = j.value("weights", nlohmann::json::array());
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (!results[i].is_string() || results[i].get<std::string>().empty()) continue;
        r.results.push_back(results[i].get<std::string>());
        int w = i < weights.size() && weights[i].is_number_integer() ? weights[i].get<int>() : 1;
        r.weights.push_back(std::clamp(w, 0, 10000));
    }
    if (r.results.empty()) throw std::runtime_error("至少需要一条回复内容");
    if (std::none_of(r.weights.begin(), r.weights.end(), [](int w) { return w > 0; })) throw std::runtime_error("回复的抽取权重不能全为 0");
    r.format = j.value("format", std::string("plain")) == "markdown" ? "markdown" : "plain";
    r.scope = j.value("scope", std::string("all"));
    if (r.scope != "all" && r.scope != "group" && r.scope != "private") r.scope = "all";
    for (const auto& b : j.value("bots", nlohmann::json::array())) if (b.is_string() && !b.get<std::string>().empty()) r.bots.push_back(b);
    r.prob = std::clamp(j.value("prob", 100), 0, 100);
    r.cooldownSec = std::max(0, j.value("cooldown_sec", 0));
    r.passThrough = j.value("pass_through", false);
    return r;
}

/// 自带的「CatOfficial 状态」规则。
inline ReplyRule builtinStatusRule() {
    ReplyRule r;
    r.id = "builtin-status";
    r.name = "CatOfficial 状态";
    r.builtin = true;
    r.conditions = {{"keyword", "#catofficial"}, {"keyword", "#co"}};
    r.results = {"CatOfficial 状态\n版本：{version}\n平台：{platform}\n运行时长：{uptime}"};
    r.weights = {1};
    return r;
}

/// 一次匹配的上下文。
struct ReplyInput {
    std::string botId;
    bool group = false;
    std::string conversation;   // 冷却按会话算：群为 group_openid，私聊为用户 OpenID
    std::string text;           // 消息纯文本（已去掉 @机器人）
};

struct ReplyMatch {
    ReplyRule rule;
    std::string result;                  // 选中的回复（未替换变量）
    std::vector<std::string> captures;   // 正则捕获组，[0] 为整体
};

/// 变量表：名字 → 值。由调用方按当前消息与运行状态填好。
using ReplyVariables = std::map<std::string, std::string>;

/// 把 {变量}、{$1}、{a|b|c} 替换掉；认不出的保留原样。
inline std::string renderReply(const std::string& text, const ReplyVariables& vars, const std::vector<std::string>& captures,
                               std::mt19937& rng) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '{') { out += text[i++]; continue; }
        const auto end = text.find('}', i + 1);
        if (end == std::string::npos || end - i > 400) { out += text[i++]; continue; }
        const auto inner = text.substr(i + 1, end - i - 1);
        std::optional<std::string> value;
        if (inner.size() >= 2 && inner[0] == '$' && std::all_of(inner.begin() + 1, inner.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            const auto n = static_cast<std::size_t>(std::stoul(inner.substr(1)));
            value = n < captures.size() ? captures[n] : std::string();
        } else if (auto it = vars.find(inner); it != vars.end()) {
            value = it->second;
        } else if (inner.find('|') != std::string::npos && inner.find('\n') == std::string::npos) {
            std::vector<std::string> options;
            for (std::size_t p = 0;;) {
                const auto bar = inner.find('|', p);
                options.push_back(inner.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
                if (bar == std::string::npos) break;
                p = bar + 1;
            }
            value = options[std::uniform_int_distribution<std::size_t>(0, options.size() - 1)(rng)];
        }
        if (value) { out += *value; i = end + 1; }
        else { out += text[i++]; }
    }
    return out;
}

class ReplyService {
public:
    explicit ReplyService(std::filesystem::path file) : file_(std::move(file)) { load(); }

    std::vector<ReplyRule> rules() const { std::lock_guard lock(mu_); return rules_; }

    /// 新增或修改一条规则（id 为空时新增），返回保存后的规则。
    ReplyRule save(ReplyRule rule) {
        std::lock_guard lock(mu_);
        if (rule.id.empty()) {
            rule.id = "r" + std::to_string(std::time(nullptr)) + std::to_string(std::uniform_int_distribution<int>(100, 999)(rng_));
            rule.builtin = false;
            rules_.push_back(rule);
        } else {
            auto it = std::find_if(rules_.begin(), rules_.end(), [&](const ReplyRule& r) { return r.id == rule.id; });
            if (it == rules_.end()) throw std::runtime_error("规则不存在");
            rule.builtin = it->builtin;
            *it = rule;
        }
        persist();
        return rule;
    }
    void remove(const std::string& id) {
        std::lock_guard lock(mu_);
        auto it = std::find_if(rules_.begin(), rules_.end(), [&](const ReplyRule& r) { return r.id == id; });
        if (it == rules_.end()) throw std::runtime_error("规则不存在");
        if (it->builtin) throw std::runtime_error("自带规则不能删除，可以停用");
        rules_.erase(it);
        persist();
    }
    /// 自带规则恢复默认（保留启用状态）。
    ReplyRule reset(const std::string& id) {
        std::lock_guard lock(mu_);
        auto it = std::find_if(rules_.begin(), rules_.end(), [&](const ReplyRule& r) { return r.id == id; });
        if (it == rules_.end() || !it->builtin) throw std::runtime_error("只有自带规则可以恢复默认");
        const bool enabled = it->enabled;
        *it = builtinStatusRule();
        it->enabled = enabled;
        persist();
        return *it;
    }

    /// 找出命中的规则（优先级高的先）；考虑范围、机器人、概率与冷却，命中后开始冷却。
    /// dryRun（面板里试一试）不看概率与冷却，也不开始冷却。
    std::optional<ReplyMatch> match(const ReplyInput& in, bool dryRun = false) {
        std::lock_guard lock(mu_);
        std::vector<const ReplyRule*> order;
        for (const auto& r : rules_) order.push_back(&r);
        std::stable_sort(order.begin(), order.end(), [](const ReplyRule* a, const ReplyRule* b) { return a->priority > b->priority; });
        const auto now = std::time(nullptr);
        for (const auto* rule : order) {
            if (!rule->enabled) continue;
            if (rule->scope == "group" && !in.group) continue;
            if (rule->scope == "private" && in.group) continue;
            if (!rule->bots.empty() && std::find(rule->bots.begin(), rule->bots.end(), in.botId) == rule->bots.end()) continue;
            std::vector<std::string> captures;
            if (!matches(*rule, in.text, captures)) continue;
            const auto key = rule->id + "|" + in.botId + "|" + in.conversation;
            if (!dryRun && rule->cooldownSec > 0) {
                auto it = cooldowns_.find(key);
                if (it != cooldowns_.end() && now - it->second < rule->cooldownSec) continue;
            }
            if (!dryRun && rule->prob < 100 && std::uniform_int_distribution<int>(1, 100)(rng_) > rule->prob) continue;
            if (!dryRun && rule->cooldownSec > 0) cooldowns_[key] = now;
            return ReplyMatch{*rule, pick(*rule), std::move(captures)};
        }
        return std::nullopt;
    }

    std::string render(const std::string& text, const ReplyVariables& vars, const std::vector<std::string>& captures) {
        std::lock_guard lock(mu_);
        return renderReply(text, vars, captures, rng_);
    }

    static bool conditionMatches(const ReplyCondition& c, const std::string& text, std::vector<std::string>& captures) {
        if (c.type == "keyword") return text == c.content;
        if (c.type == "search") return text.find(c.content) != std::string::npos;
        if (c.type == "prefix") return text.rfind(c.content, 0) == 0;
        try {
            std::smatch m;
            const std::regex re(c.content, std::regex::ECMAScript);
            if (!std::regex_search(text, m, re)) return false;
            captures.clear();
            for (const auto& part : m) captures.push_back(part.str());
            return true;
        } catch (const std::exception&) { return false; }
    }
    static bool matches(const ReplyRule& rule, const std::string& text, std::vector<std::string>& captures) {
        const bool all = rule.logic == "and";
        bool any = false;
        for (const auto& c : rule.conditions) {
            const bool hit = conditionMatches(c, text, captures);
            if (all && !hit) return false;
            any = any || hit;
            if (!all && hit) return true;
        }
        return all ? !rule.conditions.empty() : any;
    }

private:
    std::string pick(const ReplyRule& rule) {
        int total = 0;
        for (std::size_t i = 0; i < rule.results.size(); ++i) total += i < rule.weights.size() ? rule.weights[i] : 1;
        if (total <= 0) return rule.results.front();
        int roll = std::uniform_int_distribution<int>(1, total)(rng_);
        for (std::size_t i = 0; i < rule.results.size(); ++i) {
            roll -= i < rule.weights.size() ? rule.weights[i] : 1;
            if (roll <= 0) return rule.results[i];
        }
        return rule.results.back();
    }

    void load() {
        std::lock_guard lock(mu_);
        std::ifstream in(file_, std::ios::binary);
        bool hasBuiltin = false;
        if (in) {
            const auto list = nlohmann::json::parse(in, nullptr, false);
            if (list.is_array()) {
                for (const auto& item : list) {
                    try {
                        auto rule = parseReplyRule(item);
                        if (rule.id.empty()) continue;
                        hasBuiltin = hasBuiltin || rule.id == "builtin-status";
                        if (rule.id == "builtin-status") rule.builtin = true;
                        rules_.push_back(std::move(rule));
                    } catch (const std::exception&) {}
                }
            }
        }
        if (!hasBuiltin) { rules_.insert(rules_.begin(), builtinStatusRule()); persist(); }
    }
    void persist() {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& r : rules_) list.push_back(r);
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec);
        auto temp = file_;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << list.dump(2);
        }
        std::filesystem::rename(temp, file_, ec);
    }

    std::filesystem::path file_;
    mutable std::mutex mu_;
    std::vector<ReplyRule> rules_;
    std::map<std::string, std::time_t> cooldowns_;
    std::mt19937 rng_{std::random_device{}()};
};

}  // namespace catofficial::app
