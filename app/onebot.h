#pragma once

// OneBot V11 的消息段、CQ 码与官方消息之间的转换。这里全是纯函数：ID 换算通过
// Resolver 交给调用方，方便单元测试。

#include <catofficial/event.h>
#include <catofficial/media.h>
#include <catofficial/message.h>
#include <catofficial/protocol.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace catofficial::app::onebot {
using json = nlohmann::json;

// ── CQ 码 ──────────────────────────────────────────────────

inline std::string escape(std::string_view text, bool param) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '[': out += "&#91;"; break;
        case ']': out += "&#93;"; break;
        case ',': if (param) out += "&#44;"; else out += ch; break;
        default: out += ch;
        }
    }
    return out;
}

inline std::string unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '&') {
            if (text.substr(i, 5) == "&amp;") { out += '&'; i += 5; continue; }
            if (text.substr(i, 5) == "&#91;") { out += '['; i += 5; continue; }
            if (text.substr(i, 5) == "&#93;") { out += ']'; i += 5; continue; }
            if (text.substr(i, 5) == "&#44;") { out += ','; i += 5; continue; }
        }
        out += text[i++];
    }
    return out;
}

inline json text(const std::string& value) { return json{{"type", "text"}, {"data", {{"text", value}}}}; }

/// 把 CQ 码字符串拆成消息段。不完整的 [CQ: 原样当文字。
inline json parseCq(const std::string& raw) {
    json out = json::array();
    std::string pending;
    auto flush = [&] { if (!pending.empty()) { out.push_back(text(unescape(pending))); pending.clear(); } };
    for (std::size_t i = 0; i < raw.size();) {
        if (raw.compare(i, 4, "[CQ:") == 0) {
            const auto end = raw.find(']', i);
            if (end != std::string::npos) {
                const std::string body = raw.substr(i + 4, end - i - 4);
                const auto comma = body.find(',');
                const std::string type = body.substr(0, comma);
                if (!type.empty()) {
                    flush();
                    json data = json::object();
                    std::size_t p = comma == std::string::npos ? body.size() : comma + 1;
                    while (p < body.size()) {
                        const auto next = body.find(',', p);
                        const std::string pair = body.substr(p, next == std::string::npos ? std::string::npos : next - p);
                        const auto eq = pair.find('=');
                        if (eq != std::string::npos) data[pair.substr(0, eq)] = unescape(pair.substr(eq + 1));
                        if (next == std::string::npos) break;
                        p = next + 1;
                    }
                    out.push_back(json{{"type", type}, {"data", data}});
                    i = end + 1;
                    continue;
                }
            }
        }
        pending += raw[i++];
    }
    flush();
    return out;
}

inline std::string dataText(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    if (value.is_number_unsigned()) return std::to_string(value.get<uint64_t>());
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_null()) return {};
    return value.dump();
}

inline std::string toCq(const json& segments) {
    std::string out;
    if (!segments.is_array()) return out;
    for (const auto& seg : segments) {
        if (!seg.is_object()) continue;
        const auto type = seg.value("type", std::string());
        const auto data = seg.contains("data") && seg["data"].is_object() ? seg["data"] : json::object();
        if (type == "text") { out += escape(dataText(data.value("text", json(""))), false); continue; }
        out += "[CQ:" + type;
        for (const auto& item : data.items()) out += "," + item.key() + "=" + escape(dataText(item.value()), true);
        out += "]";
    }
    return out;
}

/// 应用发来的 message 可能是字符串（CQ 码）、单个消息段或消息段数组。
inline json normalize(const json& message, bool autoEscape) {
    if (message.is_string()) return autoEscape ? json::array({text(message.get<std::string>())}) : parseCq(message.get<std::string>());
    if (message.is_object()) return json::array({message});
    if (message.is_array()) return message;
    return json::array();
}

/// 消息段的纯文字摘要（日志用）。
inline std::string plainText(const json& segments) {
    std::string out;
    for (const auto& seg : segments) {
        if (!seg.is_object()) continue;
        const auto type = seg.value("type", std::string());
        const auto data = seg.value("data", json::object());
        if (type == "text") out += dataText(data.value("text", json("")));
        else if (type == "at") out += "@" + dataText(data.value("qq", json("")));
        else if (type == "image") out += "[图片]";
        else if (type == "record") out += "[语音]";
        else if (type == "video") out += "[视频]";
        else if (type == "file") out += "[文件]";
        else if (type == "face") out += "[表情]";
        else if (type != "reply") out += "[" + type + "]";
    }
    return out;
}

/// 记入消息索引前去掉 base64 内容（图片本体可能有几 MB），只留一个标记。
inline json withoutInlineData(const json& segments) {
    json out = segments;
    if (!out.is_array()) return out;
    for (auto& seg : out) {
        if (!seg.is_object() || !seg.contains("data") || !seg["data"].is_object()) continue;
        auto& data = seg["data"];
        for (const char* key : {"file", "url"})
            if (data.contains(key) && data[key].is_string() && data[key].get<std::string>().rfind("base64://", 0) == 0) data[key] = "base64://";
    }
    return out;
}

/// 消息里可以直接预览的图片地址。
inline std::vector<std::string> imageUrls(const json& segments) {
    std::vector<std::string> out;
    if (!segments.is_array()) return out;
    for (const auto& seg : segments) {
        if (!seg.is_object() || seg.value("type", std::string()) != "image" || !seg.contains("data")) continue;
        for (const char* key : {"url", "file"}) {
            const auto v = seg["data"].value(key, std::string());
            if (v.rfind("http://", 0) == 0 || v.rfind("https://", 0) == 0) { out.push_back(v); break; }
        }
    }
    return out;
}

// ── 收到的官方消息 → OneBot 消息段 ─────────────────────────

struct Resolver {
    /// 用户 OpenID → 数字 ID
    std::function<int64_t(const std::string& openId)> user;
    /// 被引用的消息 → OneBot message_id。参数为引用索引（REFIDX_…）与旧格式的官方消息 ID。
    std::function<std::optional<int64_t>(const event::MessageEvent& e)> quoted;
};

inline json attachmentSegment(const json& attachment) {
    const auto url = message::imageUrl(protocol::text(attachment, "url"));
    if (url.empty()) return nullptr;
    const auto mime = protocol::text(attachment, "content_type");
    const auto name = protocol::text(attachment, "filename");
    json data{{"file", url}, {"url", url}};
    if (attachment.contains("size")) data["file_size"] = attachment["size"];
    if (mime.rfind("image/", 0) == 0 || mime.empty()) {
        data["subType"] = 0;
        return json{{"type", "image"}, {"data", data}};
    }
    if (mime == "voice" || mime.rfind("audio/", 0) == 0) {
        const auto wav = message::imageUrl(protocol::text(attachment, "voice_wav_url"));
        if (!wav.empty()) data["url"] = wav;
        return json{{"type", "record"}, {"data", data}};
    }
    if (mime.rfind("video/", 0) == 0) return json{{"type", "video"}, {"data", data}};
    data["file"] = name.empty() ? url : name;
    data["name"] = name;
    return json{{"type", "file"}, {"data", data}};
}

/// 群消息正文里的 @：<@openid>、<@!openid>、<qqbot-at-user id="…" />、<qqbot-at-everyone />。
/// 返回匹配长度，0 表示不是 @ 标记。
inline std::size_t parseMention(std::string_view text, std::string& openId, bool& everyone) {
    everyone = false;
    if (text.starts_with("<@")) {
        const auto end = text.find('>');
        if (end == std::string_view::npos || end > 128) return 0;
        auto id = text.substr(2, end - 2);
        if (id.starts_with("!")) id.remove_prefix(1);
        if (id.empty() || id.find_first_of(" <\"'") != std::string_view::npos) return 0;
        openId = std::string(id);
        return end + 1;
    }
    if (text.starts_with("<qqbot-at-everyone")) {
        const auto end = text.find('>');
        if (end == std::string_view::npos || end > 64) return 0;
        everyone = true;
        return end + 1;
    }
    if (text.starts_with("<qqbot-at-user")) {
        const auto end = text.find('>');
        if (end == std::string_view::npos || end > 256) return 0;
        const auto tag = text.substr(0, end);
        const auto key = tag.find("id=\"");
        if (key == std::string_view::npos) return 0;
        const auto close = tag.find('"', key + 4);
        if (close == std::string_view::npos) return 0;
        openId = std::string(tag.substr(key + 4, close - key - 4));
        return openId.empty() ? 0 : end + 1;
    }
    return 0;
}

inline json incomingSegments(const event::MessageEvent& e, int64_t selfNumber, const Resolver& resolve) {
    json out = json::array();
    if ((!e.quotedIndex.empty() || !e.quotedMessageId.empty()) && resolve.quoted) {
        if (const auto id = resolve.quoted(e)) out.push_back(json{{"type", "reply"}, {"data", {{"id", std::to_string(*id)}}}});
    }
    // 只投递给被 @ 的机器人的消息，正文里的 @ 已被剥离；补回一个 @机器人，让应用的
    // 「to_me」判断照常生效。
    bool mentionsSelf = e.atSelf;
    for (const auto& mention : e.mentions) mentionsSelf = mentionsSelf || mention.self;
    if (e.scene == event::Scene::group && mentionsSelf && selfNumber > 0)
        out.push_back(json{{"type", "at"}, {"data", {{"qq", std::to_string(selfNumber)}}}});

    struct Pending { json segment; std::string url; bool used = false; };
    std::vector<Pending> attachments;
    const auto list = e.data.contains("attachments") && e.data["attachments"].is_array() ? e.data["attachments"] : json::array();
    for (const auto& attachment : list) {
        if (!attachment.is_object()) continue;
        auto segment = attachmentSegment(attachment);
        if (segment.is_null()) continue;
        const auto url = segment["data"].value("url", std::string());
        attachments.push_back(Pending{std::move(segment), url});
    }

    std::string pending;
    auto flush = [&] {
        if (pending.empty()) return;
        // 剥离 @ 之后常留下前导空格，去掉它，避免「 .r」这样的指令。
        if (out.empty() || out.back().value("type", std::string()) != "text") {
            const auto first = pending.find_first_not_of(' ');
            pending = first == std::string::npos ? std::string() : pending.substr(first);
        }
        if (!pending.empty()) out.push_back(text(pending));
        pending.clear();
    };
    const std::string& content = e.content;
    for (std::size_t p = 0; p < content.size();) {
        if (content[p] == '<') {
            if (const auto face = message::parseFace(std::string_view(content).substr(p))) {
                flush();
                if (face->type == 1) {
                    const bool numeric = !face->id.empty() && face->id.find_first_not_of("0123456789") == std::string::npos;
                    out.push_back(numeric ? json{{"type", "face"}, {"data", {{"id", face->id}}}} : text("[表情]"));
                } else {
                    std::string url = face->url;
                    for (auto& a : attachments) {
                        if (!a.used && a.segment.value("type", std::string()) == "image" && (url.empty() || a.url == url)) {
                            url = a.url; a.used = true; break;
                        }
                    }
                    if (url.empty()) out.push_back(text("[图片]"));
                    else out.push_back(json{{"type", "image"}, {"data", {{"file", url}, {"url", url}, {"subType", 1}}}});
                }
                p += face->length;
                continue;
            }
            std::string openId;
            bool everyone = false;
            if (const auto length = parseMention(std::string_view(content).substr(p), openId, everyone)) {
                flush();
                if (everyone) out.push_back(json{{"type", "at"}, {"data", {{"qq", "all"}}}});
                else {
                    const auto number = resolve.user ? resolve.user(openId) : 0;
                    out.push_back(json{{"type", "at"}, {"data", {{"qq", std::to_string(number)}}}});
                }
                p += length;
                continue;
            }
        }
        pending += content[p++];
    }
    flush();
    for (auto& a : attachments) if (!a.used) out.push_back(std::move(a.segment));
    return out;
}

// ── OneBot 消息段 → 官方发送计划 ───────────────────────────

struct OutMedia {
    enum class Source { url, path, base64 };
    int fileType = media::kImage;
    Source source = Source::url;
    std::string ref;     // URL、本地路径或 base64 内容
    std::string name;
};

struct OutPlan {
    std::string text;            // 群里的 @ 已写成 <@openid>
    int64_t replyId = 0;         // 引用的 OneBot message_id
    std::vector<OutMedia> media;
    json markdown;               // 扩展段 markdown：{content} 或模板
    json keyboard;               // 扩展段 keyboard
    std::vector<std::string> skipped;   // 官方没有对应能力、被略过的消息段类型
    bool empty() const { return text.empty() && media.empty() && markdown.is_null(); }
};

struct OutResolver {
    /// 数字 ID → 用户 OpenID
    std::function<std::string(int64_t number)> user;
};

inline std::optional<int64_t> number(const json& value) {
    if (value.is_number_integer()) return value.get<int64_t>();
    if (value.is_number_unsigned()) return static_cast<int64_t>(value.get<uint64_t>());
    if (value.is_string()) {
        const auto& s = value.get_ref<const std::string&>();
        if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 18) return std::nullopt;
        return std::stoll(s);
    }
    return std::nullopt;
}

inline std::optional<OutMedia> mediaSource(const json& data, int fileType) {
    std::string ref = dataText(data.value("file", json("")));
    if (ref.empty()) ref = dataText(data.value("url", json("")));
    if (ref.empty()) return std::nullopt;
    OutMedia m;
    m.fileType = fileType;
    m.name = dataText(data.value("name", json("")));
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) { m.source = OutMedia::Source::url; m.ref = ref; }
    else if (ref.rfind("base64://", 0) == 0) { m.source = OutMedia::Source::base64; m.ref = ref.substr(9); }
    else if (ref.rfind("file:///", 0) == 0) {
        m.source = OutMedia::Source::path;
        m.ref = ref.substr(8);
        // file:///C:/x → C:/x；file:///home/x → /home/x
        if (!(m.ref.size() > 1 && m.ref[1] == ':')) m.ref = "/" + m.ref;
    } else if (ref.rfind("file://", 0) == 0) { m.source = OutMedia::Source::path; m.ref = ref.substr(7); }
    else { m.source = OutMedia::Source::path; m.ref = ref; }
    if (m.source == OutMedia::Source::url) {
        // 有的应用把 URL 放在 url、把文件名放在 file。
        const auto url = dataText(data.value("url", json("")));
        if (url.rfind("http", 0) == 0) m.ref = url;
    }
    return m;
}

inline OutPlan plan(const json& segments, bool group, const OutResolver& resolve) {
    OutPlan out;
    for (const auto& seg : segments) {
        if (!seg.is_object()) continue;
        const auto type = seg.value("type", std::string());
        const auto data = seg.contains("data") && seg["data"].is_object() ? seg["data"] : json::object();
        if (type == "text") out.text += dataText(data.value("text", json("")));
        else if (type == "at") {
            const auto qq = dataText(data.value("qq", json("")));
            if (!group) continue;
            if (qq == "all") { out.text += "<qqbot-at-everyone />"; continue; }
            const auto n = number(data.value("qq", json("")));
            const auto openId = n && resolve.user ? resolve.user(*n) : std::string();
            if (openId.empty()) { const auto name = dataText(data.value("name", json(""))); out.text += "@" + (name.empty() ? qq : name); }
            else out.text += "<@" + openId + ">";
        }
        else if (type == "reply") {
            if (const auto id = number(data.value("id", json("")))) out.replyId = *id;
        }
        else if (type == "image") { if (auto m = mediaSource(data, media::kImage)) out.media.push_back(*m); }
        else if (type == "video") { if (auto m = mediaSource(data, media::kVideo)) out.media.push_back(*m); }
        else if (type == "record") { if (auto m = mediaSource(data, media::kVoice)) out.media.push_back(*m); }
        else if (type == "file") { if (auto m = mediaSource(data, media::kFile)) out.media.push_back(*m); }
        else if (type == "markdown") out.markdown = data.contains("content") && data["content"].is_string() ? json{{"content", data["content"]}} : data;
        else if (type == "keyboard") out.keyboard = data;
        else if (type == "face") out.skipped.push_back(type);
        else out.skipped.push_back(type);
    }
    return out;
}

}  // namespace catofficial::app::onebot
