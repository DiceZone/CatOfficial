#pragma once

// 官方机器人事件的中性解析：把 Gateway / Webhook 送来的 (t, d, id) 整理成与上层框架
// 无关的结构。这里只做协议层面能确定的事：认出场景、发送者与目标、去掉指向本机器人的
// @ 标记、算出去重键、标出可用于被动回复的会话。名称缓存、权限、上层事件类型由调用方
// 按自己的模型处理。

#include "protocol.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace catofficial::event {
using json = nlohmann::json;

enum class Scene { none, direct, group, channel };   // 单聊 / 群 / 频道

struct Mention {
    std::string id;     // member_openid / user_openid / id
    bool self = false;  // 指向本机器人
    json raw;
};

struct MessageEvent {
    std::string type;              // 原始事件名，如 GROUP_AT_MESSAGE_CREATE
    std::string eventId;           // 外层事件 ID
    std::string id;                // 消息 ID；没有时用事件 ID
    Scene scene = Scene::none;
    bool atSelf = false;           // GROUP_AT_MESSAGE_CREATE 只投递给被 @ 的机器人
    int64_t time = 0;
    json author = json::object();
    std::string senderId;
    std::string targetId;          // 单聊为对方 OpenID，群为 group_openid，频道为 channel_id
    std::string groupId;           // 群消息附带的 group_id（若有）
    std::string guildId, channelId;
    std::string role;              // member / admin / owner，其余值丢弃
    std::string groupName;
    std::string content;           // 群消息已去掉指向本机器人的 <@…> 与首尾空白
    std::vector<Mention> mentions; // 群消息的 mentions，按原顺序
    std::string quotedMessageId;   // message_reference.message_id
    std::string messageIndex;      // message_scene.ext 里的 msg_idx
    std::string dedupKey;          // 为空表示不去重
    json data;                     // 原始 d
};

struct Notice {
    std::string type, eventId;
    int64_t time = 0;
    std::string groupId, userId, operatorId;
    /// 本事件的 ID 可在 5 分钟内代替 msg_id 做被动回复的会话；replyTarget 为空表示不可用。
    Scene replyScene = Scene::none;
    std::string replyTarget;
    std::string joinRequestId, comment;            // GROUP_JOIN_REQUEST
    std::string interactionId;                     // INTERACTION_CREATE
    int interactionType = 0;
    json interactionData = json::object();
    std::string interactionScene;
    std::string guildId, channelId;                // GUILD_* / CHANNEL_*
    std::string username, unionOpenId, groupName;  // 事件携带的名称
    std::string dedupKey;
    json data;
};

inline bool isMessage(const std::string& type) {
    return type == "C2C_MESSAGE_CREATE" || type == "GROUP_AT_MESSAGE_CREATE"
        || type == "GROUP_MESSAGE_CREATE" || type == "AT_MESSAGE_CREATE";
}

inline std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

inline void eraseMention(std::string& content, const std::string& who) {
    if (who.empty()) return;
    for (const std::string& token : {"<@" + who + ">", "<@!" + who + ">"}) {
        std::size_t pos = 0;
        while ((pos = content.find(token, pos)) != std::string::npos) content.erase(pos, token.size());
    }
}

/// 解析消息事件；不是消息事件时返回空。selfId 是 READY 给出的机器人 ID。
inline std::optional<MessageEvent> parseMessage(const std::string& type, const json& d, const std::string& eventId,
                                                const std::string& selfId, const std::string& appId) {
    using protocol::text;
    if (!isMessage(type) || !d.is_object()) return std::nullopt;
    MessageEvent e;
    e.type = type; e.eventId = eventId; e.data = d;
    e.id = text(d, "id");
    if (e.id.empty()) e.id = eventId;
    e.time = protocol::eventTime(d, std::time(nullptr));
    e.author = protocol::object(d, "author");
    e.content = text(d, "content");
    e.messageIndex = protocol::sceneValue(d, "msg_idx");
    if (d.contains("message_reference") && d["message_reference"].is_object())
        e.quotedMessageId = text(d["message_reference"], "message_id");

    if (type == "C2C_MESSAGE_CREATE") {
        e.scene = Scene::direct;
        e.senderId = text(e.author, "user_openid");
        if (e.senderId.empty()) e.senderId = text(e.author, "id");
        e.targetId = e.senderId;
    } else if (type == "AT_MESSAGE_CREATE") {
        e.scene = Scene::channel;
        e.senderId = text(e.author, "id");
        e.guildId = text(d, "guild_id");
        e.channelId = text(d, "channel_id");
        e.targetId = e.channelId;
    } else {
        e.scene = Scene::group;
        e.atSelf = type == "GROUP_AT_MESSAGE_CREATE";
        e.senderId = text(e.author, "member_openid");
        if (e.senderId.empty()) e.senderId = text(e.author, "id");
        e.targetId = text(d, "group_openid");
        e.groupId = text(d, "group_id");
        e.groupName = text(d, "group_name");
        const auto role = text(e.author, "member_role");
        if (role == "member" || role == "admin" || role == "owner") e.role = role;
        // 官方正文会保留 <@OpenID>；若不移除，命令前缀不在首位，例如「@机器人 .r」会被
        // 当作普通文本。只剥离明确指向本机器人的标记。
        if (d.contains("mentions") && d["mentions"].is_array()) {
            for (const auto& mention : d["mentions"]) {
                if (!mention.is_object()) continue;
                Mention m;
                m.raw = mention;
                m.id = text(mention, "member_openid");
                if (m.id.empty()) m.id = text(mention, "user_openid");
                if (m.id.empty()) m.id = text(mention, "id");
                auto botApp = text(mention, "bot_appid");
                if (botApp.empty()) botApp = text(mention, "bot_app_id");
                m.self = mention.value("is_you", false) || m.id == selfId || m.id == appId || botApp == appId;
                if (m.self) { eraseMention(e.content, m.id); eraseMention(e.content, selfId); eraseMention(e.content, appId); }
                e.mentions.push_back(std::move(m));
            }
        }
        // 有些 GROUP_AT_MESSAGE_CREATE 载荷不带 mentions，只能安全地移除正文最前的一个
        // @ 标记；事件类型本身已证明它指向当前机器人。
        if (e.atSelf) {
            const auto begin = e.content.find_first_not_of(" \t\r\n");
            if (begin != std::string::npos && e.content.compare(begin, 2, "<@") == 0) {
                const auto end = e.content.find('>', begin + 2);
                if (end != std::string::npos) e.content.erase(begin, end - begin + 1);
            }
        }
        e.content = trim(e.content);
    }

    // 重连与 Webhook 重投都会重复送达同一条消息。
    const auto providerId = text(d, "id");
    if (!providerId.empty()) {
        std::string target = text(d, type == "AT_MESSAGE_CREATE" ? "channel_id" : "group_openid");
        if (type == "C2C_MESSAGE_CREATE") target = e.targetId;
        e.dedupKey = appId + '\x1f' + type + '\x1f' + target + '\x1f' + providerId + '\x1f' + e.messageIndex;
    }
    return e;
}

/// 解析消息以外的事件（入群、退群、好友、成员变动、主动消息开关、加群申请、互动、
/// 订阅状态、频道事件）。未知类型只填通用字段。
inline Notice parseNotice(const std::string& type, const json& data, const std::string& eventId,
                          const std::string& selfId, const std::string& appId) {
    using protocol::text;
    Notice n;
    n.type = type; n.eventId = eventId; n.data = data;
    n.time = protocol::eventTime(data, std::time(nullptr));
    auto replyTo = [&n](Scene scene, const std::string& target) { n.replyScene = scene; n.replyTarget = target; };

    if (type == "GROUP_ADD_ROBOT" || type == "GROUP_DEL_ROBOT") {
        n.groupId = text(data, "group_openid");
        n.userId = selfId;
        n.operatorId = text(data, "op_member_openid");
        replyTo(Scene::group, n.groupId);
    } else if (type == "FRIEND_ADD" || type == "FRIEND_DEL") {
        n.userId = text(data, "openid");
        if (type == "FRIEND_ADD") replyTo(Scene::direct, n.userId);
    } else if (type == "GROUP_MEMBER_ADD" || type == "GROUP_MEMBER_REMOVE") {
        n.groupId = text(data, "group_openid");
        n.userId = text(data, "member_openid");
    } else if (type == "GROUP_MSG_RECEIVE" || type == "GROUP_MSG_REJECT") {
        n.groupId = text(data, "group_openid");
        n.operatorId = text(data, "op_member_openid");
        if (type == "GROUP_MSG_RECEIVE") replyTo(Scene::group, n.groupId);
    } else if (type == "GROUP_JOIN_REQUEST") {
        n.groupId = text(data, "group_openid");
        n.userId = text(data, "member_openid");
        n.operatorId = text(data, "invited_by");
        n.joinRequestId = text(data, "join_request_id");
        const auto verify = protocol::object(data, "verify_info");
        n.comment = text(verify, "verify_message");
        if (n.comment.empty() && verify.contains("review_qa_list") && verify["review_qa_list"].is_array()) {
            for (const auto& qa : verify["review_qa_list"]) {
                if (!n.comment.empty()) n.comment += '\n';
                n.comment += text(qa, "question") + "：" + text(qa, "answer");
            }
        }
    } else if (type == "C2C_MSG_RECEIVE" || type == "C2C_MSG_REJECT") {
        n.userId = text(data, "openid");
        if (type == "C2C_MSG_RECEIVE") replyTo(Scene::direct, n.userId);
    } else if (type == "INTERACTION_CREATE") {
        n.groupId = text(data, "group_openid");
        n.userId = text(data, n.groupId.empty() ? "user_openid" : "group_member_openid");
        const auto details = protocol::object(data, "data"), resolved = protocol::object(details, "resolved");
        if (n.userId.empty()) n.userId = text(resolved, "user_id");
        n.interactionType = protocol::integer(data, "type", protocol::integer(details, "type"));
        n.interactionId = text(data, "id");
        n.interactionData = details;
        n.interactionScene = text(data, "scene");
        n.guildId = text(data, "guild_id");
        n.channelId = text(data, "channel_id");
        if (!n.interactionId.empty()) {
            n.dedupKey = appId + '\x1f' + type + '\x1f' + n.interactionId;
            // 被动回复用外层事件 ID，不是 d.id（互动回调 ID），也不是 button_data。
            if (n.interactionType == 11 || n.interactionType == 12) {
                const auto target = n.groupId.empty() ? n.userId : n.groupId;
                if (!target.empty()) replyTo(n.groupId.empty() ? Scene::direct : Scene::group, target);
            }
        }
    } else if (type == "SUBSCRIBE_MESSAGE_STATUS") {
        n.groupId = text(data, "group_openid");
        n.userId = text(data, "openid");
    } else if (type.rfind("GUILD_", 0) == 0 || type.rfind("CHANNEL_", 0) == 0) {
        n.userId = text(data, "op_user_id");
        n.guildId = type.rfind("GUILD_", 0) == 0 ? text(data, "id") : text(data, "guild_id");
        if (type.rfind("CHANNEL_", 0) == 0) n.channelId = text(data, "id");
    }

    const auto author = protocol::object(data, "author");
    n.username = text(author, "username");
    if (n.username.empty()) n.username = text(data, "username");
    n.unionOpenId = text(author, "union_openid");
    n.groupName = text(data, "group_name");
    return n;
}

}  // namespace catofficial::event
