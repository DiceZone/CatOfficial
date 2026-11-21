#pragma once

// 管理面板的数据接口：消息记录查询、在线聊天、自定义回复、官方图床、服务器性能。
// 路由在 webui.h 注册；这里的函数只处理参数与数据，结果经 Respond 返回（状态码 + JSON）。

#include "http.h"
#include "hub.h"
#include "imagecache.h"
#include "onebot.h"
#include "replies.h"
#include "store.h"
#include "sysinfo.h"

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace catofficial::app::panel {

using json = nlohmann::json;
using Respond = std::function<void(int status, json body)>;
using Params = std::function<std::string(const std::string& name)>;

/// 群头像：官方不提供；做了官民绑定的群用真实群号的 QQ 群头像。
inline std::string groupAvatar(Store& store, const std::string& groupOpenId) {
    if (auto n = store.boundNumber(Kind::group, groupOpenId)) return "https://p.qlogo.cn/gh/" + std::to_string(*n) + "/" + std::to_string(*n) + "/100";
    return {};
}
/// 用户头像：平台给过的优先，其次是官方按 AppID + OpenID 的头像地址。
inline std::string userAvatar(Hub& hub, Store& store, const std::string& botId, const std::string& openId) {
    auto avatar = store.avatar(openId);
    if (avatar.empty() && !botId.empty())
        if (auto relay = hub.relay(botId)) avatar = "https://q.qlogo.cn/qqapp/" + relay->config().appId + "/" + openId + "/100";
    return avatar;
}

inline int64_t intParam(const Params& p, const char* name, int64_t fallback) {
    try { const auto v = p(name); return v.empty() ? fallback : std::stoll(v); } catch (...) { return fallback; }
}

/// 图片在面板里的地址：缓存过的走面板，没缓存的用原地址。
inline std::string imageSrc(const std::string& url) {
    const auto name = ImageCache::get().cached(url);
    return name.empty() ? url : "/api/webui/image/" + name;
}

/// 一条消息给面板用的形状（消息记录与在线聊天共用）。
inline json messageJson(Hub& hub, const Store::Message& m) {
    const auto store = hub.store();
    const bool direct = m.scene == static_cast<int>(event::Scene::direct);
    const auto segments = onebot::withoutInlineData(m.message);
    json textual = json::array(), images = json::array(), parts = json::array();
    std::string markdown;
    for (const auto& seg : segments) {
        if (!seg.is_object()) continue;
        const auto type = seg.value("type", std::string());
        const auto data = seg.value("data", json::object());
        if (type == "image") {
            const auto urls = onebot::imageUrls(json::array({seg}));
            if (!urls.empty()) {
                const json img{{"url", urls.front()}, {"src", imageSrc(urls.front())}};
                images.push_back(img);
                parts.push_back(json{{"type", "image"}, {"url", urls.front()}, {"src", img["src"]}});
                continue;
            }
        } else if (type == "markdown") {
            const auto content = data.value("content", std::string());
            markdown += content;
            parts.push_back(json{{"type", "markdown"}, {"text", content}});
            continue;
        } else if (type == "reply") {
            parts.push_back(json{{"type", "reply"}, {"id", onebot::number(data.value("id", json())).value_or(0)}});
            continue;
        }
        textual.push_back(seg);
        parts.push_back(json{{"type", "text"}, {"text", onebot::plainText(json::array({seg}))}});
    }
    json item{{"id", m.id}, {"time", m.time}, {"self", m.self}, {"bot", m.bot},
              {"scene", direct ? "private" : m.scene == static_cast<int>(event::Scene::channel) ? "channel" : "group"},
              {"content", onebot::toCq(segments)}, {"text", onebot::plainText(textual)}, {"images", images},
              {"markdown", markdown}, {"parts", parts}, {"peer_openid", m.peer}};
    const auto kind = direct ? Kind::user : Kind::group;
    item["peer"] = store->number(kind, m.peer);
    item["peer_name"] = store->name(kind, m.peer);
    if (!m.bot.empty()) if (auto relay = hub.relay(m.bot)) item["bot_name"] = relay->displayName();
    if (m.self) {
        if (auto relay = m.bot.empty() ? nullptr : hub.relay(m.bot)) {
            item["sender_name"] = relay->displayName();
            item["avatar"] = relay->bot()->avatar();
        }
    } else if (!m.sender.empty()) {
        item["sender"] = store->number(Kind::user, m.sender);
        item["sender_openid"] = m.sender;
        item["sender_name"] = store->name(Kind::user, m.sender);
        if (!direct) if (auto member = store->member(m.peer, m.sender); member && !member->card.empty()) item["sender_card"] = member->card;
        auto avatar = store->avatar(m.sender);
        if (avatar.empty() && !m.bot.empty())
            if (auto relay = hub.relay(m.bot)) avatar = "https://q.qlogo.cn/qqapp/" + relay->config().appId + "/" + m.sender + "/100";
        item["avatar"] = avatar;
    }
    return item;
}

// ── 消息记录 ─────────────────────────────────────────────

inline void messages(Hub& hub, const Params& p, const Respond& respond) {
    Store::MessageQuery q;
    const auto page = std::max<int64_t>(1, intParam(p, "page", 1));
    q.limit = static_cast<int>(std::clamp<int64_t>(intParam(p, "size", 50), 1, 500));
    q.offset = (page - 1) * q.limit;
    q.ascending = p("order") == "asc";
    q.text = p("q");
    q.bot = p("bot");
    q.peer = p("peer");
    q.sender = p("sender");
    const auto scene = p("scene");
    q.scene = scene == "private" ? 1 : scene == "group" ? 2 : 0;
    q.from = intParam(p, "from", 0);
    q.to = intParam(p, "to", 0);
    auto [rows, total] = hub.store()->queryMessages(q);
    json items = json::array();
    for (const auto& m : rows) items.push_back(messageJson(hub, m));
    respond(200, json{{"total", total}, {"items", items}});
}

/// 筛选下拉的候选：机器人、发送者、会话。
inline void messageFilters(Hub& hub, const Respond& respond) {
    const auto store = hub.store();
    json bots = json::array();
    for (const auto& r : hub.relays()) bots.push_back({{"id", r->id()}, {"name", r->displayName()}});
    json senders = json::array();
    for (const auto& [openId, name] : store->senders(300))
        senders.push_back({{"openid", openId}, {"number", store->number(Kind::user, openId)}, {"name", name}});
    json conversations = json::array();
    for (const auto& c : store->conversations("", 300)) {
        const bool direct = c.scene == static_cast<int>(event::Scene::direct);
        const auto kind = direct ? Kind::user : Kind::group;
        conversations.push_back({{"openid", c.peer}, {"number", store->number(kind, c.peer)}, {"name", store->name(kind, c.peer)},
                                 {"scene", direct ? "private" : "group"}});
    }
    respond(200, json{{"bots", bots}, {"senders", senders}, {"conversations", conversations}});
}

// ── 在线聊天 ─────────────────────────────────────────────

inline void chatConversations(Hub& hub, const Params& p, const Respond& respond) {
    const auto store = hub.store();
    json list = json::array();
    for (const auto& c : store->conversations(p("bot"), 500)) {
        const bool direct = c.scene == static_cast<int>(event::Scene::direct);
        const auto kind = direct ? Kind::user : Kind::group;
        json item{{"peer", c.peer}, {"bot", c.bot}, {"scene", direct ? "private" : "group"}, {"number", store->number(kind, c.peer)},
                  {"name", store->name(kind, c.peer)}, {"time", c.time}, {"count", c.count}, {"last", messageJson(hub, c.last)}};
        item["avatar"] = direct ? userAvatar(hub, *store, c.bot, c.peer) : groupAvatar(*store, c.peer);
        if (!direct) item["member_count"] = store->groupInfo(c.peer).memberCount;
        list.push_back(item);
    }
    respond(200, list);
}

inline void chatMessages(Hub& hub, const Params& p, const Respond& respond) {
    const auto peer = p("peer");
    if (peer.empty()) { respond(400, json{{"error", "缺少会话"}}); return; }
    const auto limit = static_cast<int>(std::clamp<int64_t>(intParam(p, "limit", 50), 1, 200));
    json list = json::array();
    for (const auto& m : hub.store()->conversationMessages(peer, intParam(p, "before", 0), intParam(p, "after", 0), limit))
        list.push_back(messageJson(hub, m));
    respond(200, list);
}

inline std::string rfc3339After(int64_t seconds) {
    const std::time_t at = std::time(nullptr) + seconds;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &at);
#else
    gmtime_r(&at, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

inline std::shared_ptr<Relay> onlineRelayFor(Hub& hub, const std::string& id, const Respond& respond) {
    auto relay = hub.relay(id);
    if (!relay) { respond(400, json{{"error", "机器人未启用或不存在"}}); return nullptr; }
    if (!relay->bot()->online()) { respond(409, json{{"error", "机器人不在线"}}); return nullptr; }
    return relay;
}
inline json officialError(const json& r, const std::string& fallback);

/// 一个成员给面板用的形状。
inline json memberJson(Hub& hub, Store& store, const std::string& botId, const std::string& groupOpenId, const std::string& openId,
                       std::string role, std::string card, int64_t lastSeen) {
    if (role.empty() || card.empty())
        if (auto m = store.member(groupOpenId, openId)) { if (role.empty()) role = m->role; if (card.empty()) card = m->card; if (!lastSeen) lastSeen = m->lastSeen; }
    return json{{"openid", openId}, {"number", store.number(Kind::user, openId)}, {"name", store.name(Kind::user, openId)},
                {"card", card}, {"role", role.empty() ? "member" : role}, {"avatar", userAvatar(hub, store, botId, openId)}, {"last_seen", lastSeen}};
}

/// 列表里出现的人：记下名字与头像，并补上号码与名称，前端直接显示。
inline void annotatePeople(Hub& hub, Store& store, const std::string& botId, const std::string& groupOpenId, json& value) {
    if (value.is_array()) { for (auto& item : value) annotatePeople(hub, store, botId, groupOpenId, item); return; }
    if (!value.is_object()) return;
    std::string openId;
    for (const char* key : {"member_openid", "user_openid", "openid"}) if (openId.empty()) openId = protocol::text(value, key);
    if (!openId.empty()) {
        store.rememberName(Kind::user, openId, protocol::text(value, "username"));
        store.rememberAvatar(openId, protocol::text(value, "avatar"));
        value["_person"] = memberJson(hub, store, botId, groupOpenId, openId, protocol::text(value, "member_role"),
                                      protocol::text(value, "member_nickname"), 0);
    }
    for (auto& [key, child] : value.items()) if (key != "_person" && (child.is_array() || child.is_object())) annotatePeople(hub, store, botId, groupOpenId, child);
}

/// 会话资料（在线聊天右侧与「…」菜单用）。群：名称、成员数、简介、机器人身份、成员列表。
inline void chatPeer(Hub& hub, const Params& p, const Respond& respond) {
    const auto botId = p("bot"), peer = p("peer");
    const bool group = p("scene") != "private";
    auto store = hub.store();
    auto relay = hub.relay(botId);
    if (peer.empty()) { respond(400, json{{"error", "缺少会话"}}); return; }
    const auto kind = group ? Kind::group : Kind::user;
    auto base = [store, peer, kind, group, botId, &hub]() {
        json out{{"scene", group ? "group" : "private"}, {"openid", peer}, {"number", store->number(kind, peer)}, {"name", store->name(kind, peer)}};
        if (auto bound = store->boundNumber(kind, peer)) out["bound_number"] = *bound;
        if (group) {
            const auto info = store->groupInfo(peer);
            out["avatar"] = groupAvatar(*store, peer);
            out["member_count"] = info.memberCount;
            out["memo"] = info.memo;
            out["bot_role"] = info.botRole;
        } else {
            out["avatar"] = userAvatar(hub, *store, botId, peer);
        }
        return out;
    };
    if (!group || !relay || !relay->bot()->online() || peer.rfind("channel:", 0) == 0) {
        json out = base();
        if (group) {
            json members = json::array();
            for (const auto& m : store->members(peer)) members.push_back(memberJson(hub, *store, botId, peer, m.userOpenId, m.role, m.card, m.lastSeen));
            out["members"] = members;
            out["members_source"] = "seen";
        }
        respond(200, out);
        return;
    }
    // 群：刷新群资料 → 机器人在群里的身份 → 尝试官方成员列表（没有权限时用见过的成员）。
    Hub* hubPtr = &hub;
    relay->refreshGroupInfo(peer, true, [hubPtr, relay, store, peer, botId, base, respond](json) {
        relay->bot()->call(drogon::Get, "/v2/groups/" + peer + "/bot_state", json::object(), [hubPtr, relay, store, peer, botId, base, respond](json state) {
            if (state.value("ok", false)) store->rememberBotRole(peer, protocol::text(state.value("data", json::object()), "member_role"));
            relay->bot()->call(drogon::Get, "/v2/groups/" + peer + "/members", json::object(), [hubPtr, store, peer, botId, base, respond](json r) {
                json out = base();
                json members = json::array();
                if (r.value("ok", false)) {
                    auto data = r.value("data", json::object());
                    for (const auto& m : data.value("members", json::array())) {
                        if (!m.is_object()) continue;
                        const auto openId = protocol::text(m, "member_openid");
                        if (openId.empty()) continue;
                        store->rememberName(Kind::user, openId, protocol::text(m, "username"));
                        store->rememberAvatar(openId, protocol::text(m, "avatar"));
                        store->rememberMember(peer, openId, protocol::text(m, "member_role"), protocol::text(m, "member_nickname"));
                        members.push_back(memberJson(*hubPtr, *store, botId, peer, openId, protocol::text(m, "member_role"), protocol::text(m, "member_nickname"), 0));
                    }
                    out["members_source"] = "official";
                    out["members_next"] = protocol::text(data, "next_cursor");
                } else {
                    for (const auto& m : store->members(peer)) members.push_back(memberJson(*hubPtr, *store, botId, peer, m.userOpenId, m.role, m.card, m.lastSeen));
                    out["members_source"] = "seen";
                    out["members_error"] = r.value("message", std::string());
                }
                out["members"] = members;
                respond(200, out);
            });
        });
    });
}

/// 群管理（官方接口；大多要求机器人是群管理员）。
/// body：{bot, peer, action, member?, seconds?, blacklist?, request_id?, reason?, cursor?}
inline void chatManage(Hub& hub, const json& body, const Respond& respond) {
    auto relay = onlineRelayFor(hub, body.value("bot", std::string()), respond);
    if (!relay) return;
    const auto peer = body.value("peer", std::string());
    const auto action = body.value("action", std::string());
    const auto member = body.value("member", std::string());
    if (peer.empty() || peer.find_first_of("/?#") != std::string::npos || member.find_first_of("/?#") != std::string::npos) { respond(400, json{{"error", "参数无效"}}); return; }
    const std::string base = "/v2/groups/" + peer;
    auto store = hub.store();
    Hub* hubPtr = &hub;
    const auto botId = relay->id();
    auto reply = [hubPtr, store, botId, peer, respond](const std::string& fallback) {
        return [hubPtr, store, botId, peer, respond, fallback](json r) {
            if (!r.value("ok", false)) { respond(502, officialError(r, fallback)); return; }
            auto data = r.value("data", json::object());
            annotatePeople(*hubPtr, *store, botId, peer, data);
            respond(200, json{{"ok", true}, {"data", data}});
        };
    };
    auto call = [&](drogon::HttpMethod method, const std::string& path, const json& payload, const std::string& fallback) {
        relay->bot()->call(method, path, payload, reply(fallback));
    };
    auto needMember = [&] { if (member.empty()) { respond(400, json{{"error", "缺少成员"}}); return false; } return true; };
    if (action == "mute") {
        if (!needMember()) return;
        const auto seconds = body.value("seconds", int64_t(600));
        const bool remove = seconds <= 0;
        call(drogon::Post, base + "/restrict_chat_setting",
             json{{"members", json::array({json{{"op", remove ? "del" : "add"}, {"member_openid", member}, {"mute_expire_at", remove ? std::string() : rfc3339After(seconds)}}})}},
             remove ? "解除禁言失败" : "禁言失败");
    } else if (action == "mute_list") {
        call(drogon::Get, base + "/restrict_chat_setting", json::object(), "读取禁言列表失败");
    } else if (action == "kick") {
        if (!needMember()) return;
        call(drogon::Post, base + "/batch_remove_members",
             json{{"member_openids", json::array({member})}, {"add_to_member_blacklist", body.value("blacklist", false)}}, "移出成员失败");
    } else if (action == "blacklist") {
        json q = json::object();
        if (const auto c = body.value("cursor", std::string()); !c.empty()) q["cursor"] = c;
        call(drogon::Get, base + "/member_blacklist", q, "读取黑名单失败");
    } else if (action == "blacklist_add" || action == "blacklist_del") {
        if (!needMember()) return;
        call(drogon::Post, base + "/member_blacklist",
             json{{"op", action == "blacklist_add" ? "add" : "del"}, {"member_openids", json::array({member})}}, "修改黑名单失败");
    } else if (action == "join_requests") {
        json q{{"limit", 50}};
        if (const auto c = body.value("cursor", std::string()); !c.empty()) q["cursor"] = c;
        call(drogon::Get, base + "/join_request_list", q, "读取入群申请失败");
    } else if (action == "join_approve" || action == "join_decline") {
        if (!needMember()) return;
        json payload{{"op", action == "join_approve" ? "approve" : "decline"}};
        if (const auto id = body.value("request_id", std::string()); !id.empty()) payload["join_request_id"] = id;
        if (const auto reason = body.value("reason", std::string()); !reason.empty() && action == "join_decline") payload["reject_reason"] = reason;
        call(drogon::Post, base + "/approval_join_request/" + member, payload, "审批失败");
    } else {
        respond(400, json{{"error", "未知的操作"}});
    }
}

/// body：{bot, peer, scene: private|group, text?, markdown?, image: {name, base64}?, reply_to?}
inline void chatSend(Hub& hub, const json& body, const Respond& respond) {
    auto relay = hub.relay(body.value("bot", std::string()));
    if (!relay) { respond(400, json{{"error", "机器人未启用或不存在"}}); return; }
    if (!relay->bot()->online()) { respond(409, json{{"error", "机器人不在线"}}); return; }
    const auto peer = body.value("peer", std::string());
    const auto sceneName = body.value("scene", std::string());
    if (peer.empty()) { respond(400, json{{"error", "缺少会话"}}); return; }
    const auto scene = sceneName == "private" ? event::Scene::direct : peer.rfind("channel:", 0) == 0 ? event::Scene::channel : event::Scene::group;
    json segments = json::array();
    if (const auto replyTo = body.value("reply_to", int64_t(0)); replyTo > 0)
        segments.push_back(json{{"type", "reply"}, {"data", {{"id", std::to_string(replyTo)}}}});
    if (const auto md = body.value("markdown", std::string()); !md.empty())
        segments.push_back(json{{"type", "markdown"}, {"data", {{"content", md}}}});
    if (const auto text = body.value("text", std::string()); !text.empty()) segments.push_back(onebot::text(text));
    if (body.contains("image") && body["image"].is_object()) {
        const auto data = body["image"].value("base64", std::string());
        if (!data.empty())
            segments.push_back(json{{"type", "image"}, {"data", {{"file", "base64://" + data}, {"name", body["image"].value("name", std::string("image.png"))}}}});
    }
    if (segments.empty() || (segments.size() == 1 && segments[0].value("type", std::string()) == "reply")) { respond(400, json{{"error", "消息为空"}}); return; }
    relay->sendToPeer(scene, peer, segments, [respond](json r) {
        if (r.value("status", std::string()) == "ok") respond(200, json{{"ok", true}, {"message_id", r["data"].value("message_id", int64_t(0))}});
        else respond(502, json{{"error", r.value("message", std::string("发送失败"))}});
    });
}

// ── 自定义回复 ───────────────────────────────────────────

inline json replyList(Hub& hub) {
    json rules = json::array();
    if (auto service = hub.replies()) for (const auto& r : service->rules()) rules.push_back(r);
    json vars = json::array();
    for (const auto& [name, desc] : Hub::variableHelp()) vars.push_back({{"name", name}, {"desc", desc}});
    return json{{"rules", rules}, {"variables", vars}};
}

inline void replySave(Hub& hub, const std::string& id, json body, const Respond& respond) {
    auto service = hub.replies();
    if (!service) { respond(503, json{{"error", "自定义回复不可用"}}); return; }
    body["id"] = id;
    try { respond(200, json(service->save(parseReplyRule(body)))); }
    catch (const std::exception& e) { respond(400, json{{"error", e.what()}}); }
}

/// 面板里试一试：给一句话，看会命中哪条规则、回复成什么样。不看概率与冷却。
inline void replyTest(Hub& hub, const json& body, const Respond& respond) {
    auto service = hub.replies();
    if (!service) { respond(503, json{{"error", "自定义回复不可用"}}); return; }
    const auto relays = hub.relays();
    std::shared_ptr<Relay> relay;
    const auto botId = body.value("bot", std::string());
    for (const auto& r : relays) if (botId.empty() ? !relay : r->id() == botId) relay = r;
    ReplyInput in;
    in.botId = relay ? relay->id() : botId;
    in.group = body.value("scene", std::string("group")) != "private";
    in.conversation = "panel-test";
    in.text = event::trim(body.value("message", std::string()));
    // 示例消息：发送者「希亚」，群「测试群」。
    event::MessageEvent sample;
    sample.scene = in.group ? event::Scene::group : event::Scene::direct;
    sample.senderId = "panel-test-user";
    sample.targetId = in.group ? "panel-test-group" : sample.senderId;
    sample.username = "希亚";
    sample.memberNickname = "希亚";
    sample.content = in.text;
    auto vars = hub.variables(relay.get(), &sample);
    vars["sender"] = "希亚";
    if (in.group) { vars["group"] = "测试群"; vars["group_id"] = "1000000000001"; vars["sender_id"] = "2000000000001"; }
    else vars["sender_id"] = "2000000000001";
    if (body.contains("template")) {
        respond(200, json{{"text", service->render(body.value("template", std::string()), vars, {})}});
        return;
    }
    auto match = service->match(in, true);
    if (!match) { respond(200, json{{"matched", false}}); return; }
    respond(200, json{{"matched", true}, {"rule_id", match->rule.id}, {"rule_name", match->rule.name}, {"format", match->rule.format},
                      {"text", service->render(match->result, vars, match->captures)}});
}

// ── 服务器性能（与 Dice!Next 首页相同） ───────────────────

inline json sysinfoJson(int64_t since) {
    auto& sampler = sysinfo::HistorySampler::instance();
    const auto si = sampler.latest();
    json disks = json::array();
    for (const auto& d : si.disks)
        disks.push_back(json{{"path", d.path}, {"mount", d.mount}, {"label", d.label}, {"fs", d.fs}, {"model", d.model},
                             {"total_gb", d.totalGB}, {"used_gb", d.usedGB}, {"load", d.loadPct}});
    json samples = json::array();
    for (const auto& s : sampler.since(since)) samples.push_back(json::array({s.t, s.cpu, s.mem}));
    return json{{"os", si.os}, {"os_id", si.osId}, {"cpu_model", si.cpuModel}, {"cpu_cores", si.cpuCores}, {"cpu_physical", si.cpuPhysical},
                {"cpu_mhz", si.cpuMhz}, {"cpu_load", si.cpuLoadPct}, {"mem_total_mb", si.memTotalMB}, {"mem_used_mb", si.memUsedMB},
                {"mem_load", si.memLoadPct}, {"mem_speed_mhz", si.memSpeedMhz}, {"proc_mem_mb", si.procMemMB}, {"disks", disks},
                {"history", {{"interval", sysinfo::HistorySampler::kIntervalSeconds},
                             {"window", sysinfo::HistorySampler::kIntervalSeconds * static_cast<int>(sysinfo::HistorySampler::kCapacity)},
                             {"samples", samples}}}};
}

// ── 官方图床（素材管理） ─────────────────────────────────
// 官方文档「素材管理」：prepare → PUT 预签名地址 → complete；单张 ≤10MB，只收 png/jpeg/gif/webp，
// 扩展名须与类型一致；每个机器人最多 100 张；最后一次引用后 90 天过期。

inline std::string digestHex(const EVP_MD* type, const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), out, &len, type, nullptr);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (unsigned int i = 0; i < len; ++i) { s += hex[out[i] >> 4]; s += hex[out[i] & 15]; }
    return s;
}

/// 按文件头认图片类型，返回 {content_type, 扩展名}；不支持时为空。
inline std::pair<std::string, std::string> materialType(const std::string& bytes) {
    auto starts = [&](const char* magic, std::size_t n) { return bytes.size() >= n && bytes.compare(0, n, magic, n) == 0; };
    if (starts("\x89PNG", 4)) return {"image/png", ".png"};
    if (starts("\xFF\xD8\xFF", 3)) return {"image/jpeg", ".jpg"};
    if (starts("GIF8", 4)) return {"image/gif", ".gif"};
    if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) return {"image/webp", ".webp"};
    return {};
}

inline std::shared_ptr<Relay> onlineRelay(Hub& hub, const std::string& id, const Respond& respond) {
    auto relay = hub.relay(id);
    if (!relay) { respond(400, json{{"error", "机器人未启用或不存在"}}); return nullptr; }
    if (!relay->bot()->online()) { respond(409, json{{"error", "机器人不在线"}}); return nullptr; }
    return relay;
}

inline json officialError(const json& r, const std::string& fallback) {
    auto message = r.value("message", std::string());
    if (message.empty()) message = fallback;
    json out{{"error", message}};
    if (r.contains("error_code")) out["code"] = r["error_code"];
    return out;
}

inline void materialList(Hub& hub, const Params& p, const Respond& respond) {
    auto relay = onlineRelay(hub, p("bot"), respond);
    if (!relay) return;
    json query = json::object();
    if (const auto cursor = p("cursor"); !cursor.empty()) query["cursor"] = cursor;
    relay->bot()->call(drogon::Get, "/v2/media-images", query, [respond](json r) {
        if (!r.value("ok", false)) { respond(502, officialError(r, "获取素材列表失败")); return; }
        respond(200, r.value("data", json::object()));
    });
}

inline void materialDelete(Hub& hub, const std::string& botId, const std::string& imageId, const Respond& respond) {
    auto relay = onlineRelay(hub, botId, respond);
    if (!relay) return;
    if (imageId.empty() || imageId.find_first_of("/?#") != std::string::npos) { respond(400, json{{"error", "素材 ID 无效"}}); return; }
    relay->bot()->call(drogon::Delete, "/v2/media-images/" + imageId + "/delete", json::object(), [respond](json r) {
        if (!r.value("ok", false)) { respond(502, officialError(r, "删除失败")); return; }
        respond(200, json{{"ok", true}});
    });
}

/// body：{bot, file_name, base64}
inline void materialUpload(Hub& hub, const json& body, const Respond& respond) {
    auto relay = onlineRelay(hub, body.value("bot", std::string()), respond);
    if (!relay) return;
    auto bytes = std::make_shared<std::string>(drogon::utils::base64Decode(body.value("base64", std::string())));
    if (bytes->empty()) { respond(400, json{{"error", "图片内容为空"}}); return; }
    if (bytes->size() > 10u * 1024 * 1024) { respond(400, json{{"error", "图片超过 10MB"}}); return; }
    const auto [contentType, ext] = materialType(*bytes);
    if (contentType.empty()) { respond(400, json{{"error", "只支持 PNG、JPEG、GIF、WebP 图片"}}); return; }
    // 扩展名须与类型一致：去掉原扩展名，换成按内容认出的。
    auto name = body.value("file_name", std::string("image"));
    if (const auto dot = name.find_last_of('.'); dot != std::string::npos && dot > 0) name.resize(dot);
    if (name.empty() || name.size() > 100) name = "image";
    name += ext;
    const auto md5 = digestHex(EVP_md5(), *bytes);
    json prepare{{"file_name", name}, {"file_size", static_cast<uint64_t>(bytes->size())}, {"content_type", contentType},
                 {"md5", md5}, {"sha1", digestHex(EVP_sha1(), *bytes)}};
    auto bot = relay->bot();
    bot->call(drogon::Post, "/v2/media-images/prepare", prepare, [bot, bytes, contentType = contentType, md5, respond](json r) {
        if (!r.value("ok", false)) { respond(502, officialError(r, "上传准备失败")); return; }
        const auto data = r.value("data", json::object());
        const auto uploadId = protocol::text(data, "upload_id");
        const auto url = protocol::text(data, "presigned_url");
        if (uploadId.empty() || url.rfind("https://", 0) != 0) { respond(502, json{{"error", "官方没有返回上传地址"}}); return; }
        auto loop = drogon::app().getLoop();
        std::thread([bot, bytes, contentType, md5, uploadId, url, respond, loop] {
            const auto put = http::put(url, *bytes, contentType);
            loop->queueInLoop([bot, put, md5, uploadId, respond] {
                if (!put.ok()) { respond(502, json{{"error", "上传到存储失败：" + (put.error.empty() ? "HTTP " + std::to_string(put.status) : put.error)}}); return; }
                bot->call(drogon::Post, "/v2/media-images/complete", json{{"upload_id", uploadId}, {"md5", md5}}, [respond](json r) {
                    if (!r.value("ok", false)) { respond(502, officialError(r, "完成上传失败")); return; }
                    respond(200, r.value("data", json::object()));
                });
            });
        }).detach();
    });
}

}  // namespace catofficial::app::panel
