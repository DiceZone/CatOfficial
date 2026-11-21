// CatOfficial 单元测试：不连网，只测转换、存储与事件分派。

#include "../app/onebot.h"
#include "../app/relay.h"
#include "../app/store.h"

#include <catofficial/bot.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") 失败\n"; } } while (0)
#define CHECK_EQ(a, b) do { ++checks; const auto va = (a); const auto vb = (b); if (!(va == vb)) { ++failures; \
    std::cerr << __FILE__ << ":" << __LINE__ << ": " #a " == " #b " 失败：" << va << " != " << vb << "\n"; } } while (0)

std::vector<std::pair<std::string, std::function<void()>>>& tests() { static std::vector<std::pair<std::string, std::function<void()>>> t; return t; }
struct Register { Register(const char* name, std::function<void()> fn) { tests().emplace_back(name, std::move(fn)); } };
#define TEST(name) void name(); Register reg_##name(#name, name); void name()

using json = nlohmann::json;
using namespace catofficial;
using namespace catofficial::app;

std::filesystem::path tempDb(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("catofficial-test-" + name + ".db");
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(p.string() + suffix, ec);
    return p;
}

TEST(CqRoundTripKeepsSpecialCharacters) {
    const auto segments = onebot::parseCq("a&#91;b&#93; [CQ:at,qq=123] [CQ:image,file=https://x/y?a=1&#44;2]");
    CHECK_EQ(segments.size(), std::size_t(4));
    CHECK_EQ(segments[0]["data"]["text"].get<std::string>(), std::string("a[b] "));
    CHECK_EQ(segments[1]["type"].get<std::string>(), std::string("at"));
    CHECK_EQ(segments[1]["data"]["qq"].get<std::string>(), std::string("123"));
    CHECK_EQ(segments[3]["data"]["file"].get<std::string>(), std::string("https://x/y?a=1,2"));
    CHECK_EQ(onebot::toCq(segments), std::string("a&#91;b&#93; [CQ:at,qq=123] [CQ:image,file=https://x/y?a=1&#44;2]"));
    CHECK_EQ(onebot::parseCq("[CQ:broken").size(), std::size_t(1));
}

TEST(GroupAtMessageBecomesAtSelfPlusText) {
    const json d{{"id", "m1"}, {"content", "<@BOTID> .r 3d6 <@USERB>"}, {"group_openid", "G1"},
                 {"author", {{"member_openid", "USERA"}}},
                 {"mentions", json::array({json{{"member_openid", "BOTID"}, {"is_you", true}}, json{{"member_openid", "USERB"}}})}};
    auto e = event::parseMessage("GROUP_AT_MESSAGE_CREATE", d, "ev1", "BOTID", "APP");
    CHECK(e.has_value());
    onebot::Resolver r;
    r.user = [](const std::string& id) { return id == "USERB" ? int64_t(2000000000002) : int64_t(2000000000001); };
    const auto segs = onebot::incomingSegments(*e, 12345, r);
    CHECK_EQ(segs.size(), std::size_t(3));
    CHECK_EQ(segs[0]["type"].get<std::string>(), std::string("at"));
    CHECK_EQ(segs[0]["data"]["qq"].get<std::string>(), std::string("12345"));
    CHECK_EQ(segs[1]["data"]["text"].get<std::string>(), std::string(".r 3d6 "));
    CHECK_EQ(segs[2]["data"]["qq"].get<std::string>(), std::string("2000000000002"));
}

TEST(AttachmentsAndQuotesBecomeSegments) {
    const json d{{"id", "m2"}, {"content", "看图"}, {"author", {{"user_openid", "U"}}},
                 {"message_scene", {{"ext", json::array({"ref_msg_idx=REFIDX_OLD", "msg_idx=REFIDX_NEW"})}}},
                 {"msg_elements", json::array({json{{"msg_idx", "REFIDX_OLD"}, {"content", "pong"}}})},
                 {"attachments", json::array({json{{"content_type", "image/png"}, {"url", "multimedia.nt.qq.com/a.png"}},
                                              json{{"content_type", "voice"}, {"url", "https://v/x.silk"}, {"voice_wav_url", "https://v/x.wav"}}})}};
    auto e = event::parseMessage("C2C_MESSAGE_CREATE", d, "ev2", "BOT", "APP");
    CHECK(e.has_value());
    onebot::Resolver r;
    r.user = [](const std::string&) { return int64_t(2000000000001); };
    r.quoted = [](const event::MessageEvent& q) -> std::optional<int64_t> {
        if (q.quotedIndex == "REFIDX_OLD" && q.quoted.value("content", std::string()) == "pong") return 42;
        return std::nullopt;
    };
    CHECK_EQ(e->messageIndex, std::string("REFIDX_NEW"));
    const auto segs = onebot::incomingSegments(*e, 1, r);
    CHECK_EQ(segs.size(), std::size_t(4));
    CHECK_EQ(segs[0]["type"].get<std::string>(), std::string("reply"));
    CHECK_EQ(segs[0]["data"]["id"].get<std::string>(), std::string("42"));
    CHECK_EQ(segs[1]["data"]["text"].get<std::string>(), std::string("看图"));
    CHECK_EQ(segs[2]["type"].get<std::string>(), std::string("image"));
    CHECK_EQ(segs[2]["data"]["url"].get<std::string>(), std::string("https://multimedia.nt.qq.com/a.png"));
    CHECK_EQ(segs[3]["type"].get<std::string>(), std::string("record"));
    CHECK_EQ(segs[3]["data"]["url"].get<std::string>(), std::string("https://v/x.wav"));
}

TEST(OutgoingPlanResolvesMentionsRepliesAndMedia) {
    onebot::OutResolver r;
    r.user = [](int64_t n) { return n == 2000000000002 ? std::string("USERB") : std::string(); };
    const auto segs = onebot::parseCq("[CQ:reply,id=42][CQ:at,qq=2000000000002] 你好[CQ:at,qq=999][CQ:image,file=base64://aGk=][CQ:image,file=file:///C:/a.png][CQ:face,id=1][CQ:poke,qq=1]");
    const auto p = onebot::plan(segs, true, r);
    CHECK_EQ(p.replyId, int64_t(42));
    CHECK_EQ(p.text, std::string("<@USERB> 你好@999"));
    CHECK_EQ(p.media.size(), std::size_t(2));
    CHECK(p.media[0].source == onebot::OutMedia::Source::base64);
    CHECK_EQ(p.media[0].ref, std::string("aGk="));
    CHECK(p.media[1].source == onebot::OutMedia::Source::path);
    CHECK_EQ(p.media[1].ref, std::string("C:/a.png"));
    CHECK_EQ(p.skipped.size(), std::size_t(2));
    // 私聊里没有 @。
    CHECK_EQ(onebot::plan(onebot::parseCq("[CQ:at,qq=2000000000002]hi"), false, r).text, std::string("hi"));
}

TEST(StoreAllocatesStableNumbersAndHonoursBindings) {
    const auto path = tempDb("store");
    {
        Store s(utf8(path));
        const auto a = s.number(Kind::user, "OPENA");
        const auto b = s.number(Kind::user, "OPENB");
        const auto g = s.number(Kind::group, "GROUP");
        CHECK_EQ(a, Store::kUserBase + 1);
        CHECK_EQ(b, Store::kUserBase + 2);
        CHECK_EQ(g, Store::kGroupBase + 1);
        CHECK_EQ(s.number(Kind::user, "OPENA"), a);
        CHECK_EQ(s.openId(Kind::user, b), std::string("OPENB"));
        CHECK(s.bind(Kind::user, "OPENA", 123456));
        CHECK_EQ(s.number(Kind::user, "OPENA"), int64_t(123456));
        CHECK(!s.bind(Kind::user, "OPENB", 123456));
        CHECK(s.bound(Kind::user, "OPENA"));
        // 绑定不影响后续分配。
        CHECK_EQ(s.number(Kind::user, "OPENC"), Store::kUserBase + 3);

        Store::Message m;
        m.officialId = "OFF1"; m.scene = 2; m.peer = "GROUP"; m.sender = "OPENA"; m.time = 100;
        m.message = json::array({onebot::text("hi")});
        const auto id = s.rememberMessage(m);
        CHECK(s.messageById(id).has_value());
        CHECK_EQ(s.messageByOfficialId("OFF1")->id, id);
        s.pruneMessages(200);
        CHECK(!s.messageById(id).has_value());
    }
    Store again(utf8(path));
    CHECK_EQ(again.number(Kind::user, "OPENB"), Store::kUserBase + 2);
}

TEST(BotDeduplicatesAndRemembersPassiveEvents) {
    int messages = 0, notices = 0;
    Bot::Handlers h;
    h.message = [&](const event::MessageEvent&) { ++messages; };
    h.notice = [&](const event::Notice&) { ++notices; };
    auto bot = Bot::create(Bot::Options{"APP", "SECRET"}, h);
    bot->setIdentityForTest("BOT", "3889000000");
    const json d{{"id", "m1"}, {"content", "hi"}, {"author", {{"user_openid", "U"}}}};
    bot->dispatch("C2C_MESSAGE_CREATE", d, "e1");
    bot->dispatch("C2C_MESSAGE_CREATE", d, "e1");
    CHECK_EQ(messages, 1);
    bot->dispatch("FRIEND_ADD", json{{"openid", "U2"}}, "e2");
    CHECK_EQ(notices, 1);
}

TEST(RelayEmitsOneBotMessageEvents) {
    const auto path = tempDb("relay");
    auto store = std::make_shared<Store>(utf8(path));
    Config cfg; cfg.appId = "APP"; cfg.appSecret = "SECRET";
    auto relay = std::make_shared<Relay>(cfg, store, nullptr);
    relay->init();
    relay->bot()->setIdentityForTest("BOT", "3889000000");
    std::vector<json> events;
    relay->addSink([&](const json& e) { events.push_back(e); });
    relay->bot()->dispatch("GROUP_AT_MESSAGE_CREATE",
        json{{"id", "gm1"}, {"content", " .r"}, {"group_openid", "G1"}, {"author", {{"member_openid", "U1"}, {"member_role", "admin"}}}}, "e1");
    CHECK_EQ(events.size(), std::size_t(1));
    const auto& e = events[0];
    CHECK_EQ(e["post_type"].get<std::string>(), std::string("message"));
    CHECK_EQ(e["message_type"].get<std::string>(), std::string("group"));
    CHECK_EQ(e["self_id"].get<int64_t>(), int64_t(3889000000));
    CHECK_EQ(e["group_id"].get<int64_t>(), Store::kGroupBase + 1);
    CHECK_EQ(e["user_id"].get<int64_t>(), Store::kUserBase + 1);
    CHECK_EQ(e["raw_message"].get<std::string>(), std::string("[CQ:at,qq=3889000000].r"));
    CHECK_EQ(e["sender"]["role"].get<std::string>(), std::string("admin"));
    CHECK_EQ(e["sender"]["avatar"].get<std::string>(), std::string("https://q.qlogo.cn/qqapp/APP/U1/640"));

    // get_msg 能按 message_id 取回。
    json got;
    relay->handle("get_msg", json{{"message_id", e["message_id"]}}, [&](json r) { got = r; });
    CHECK_EQ(got["status"].get<std::string>(), std::string("ok"));
    CHECK_EQ(got["data"]["group_id"].get<int64_t>(), Store::kGroupBase + 1);

    // 引用一条没见过的消息：按事件带来的内容补一条索引，reply 段与 get_msg 照样可用。
    relay->bot()->dispatch("GROUP_MESSAGE_CREATE", json{{"id", "gm2"}, {"content", "引用"}, {"group_openid", "G1"},
        {"author", {{"member_openid", "U1"}}},
        {"message_scene", {{"ext", json::array({"ref_msg_idx=REFIDX_UNSEEN", "msg_idx=REFIDX_GM2"})}}},
        {"msg_elements", json::array({json{{"msg_idx", "REFIDX_UNSEEN"}, {"content", "很久以前的话"}}})}}, "e3");
    CHECK_EQ(events.size(), std::size_t(2));
    CHECK_EQ(events[1]["message"][0]["type"].get<std::string>(), std::string("reply"));
    const auto quotedId = std::stoll(events[1]["message"][0]["data"]["id"].get<std::string>());
    relay->handle("get_msg", json{{"message_id", quotedId}}, [&](json r) { got = r; });
    CHECK_EQ(got["data"]["raw_message"].get<std::string>(), std::string("很久以前的话"));
    // 再次引用同一条，不会重复补。
    relay->bot()->dispatch("GROUP_MESSAGE_CREATE", json{{"id", "gm3"}, {"content", "再引用"}, {"group_openid", "G1"},
        {"author", {{"member_openid", "U1"}}},
        {"message_scene", {{"ext", json::array({"ref_msg_idx=REFIDX_UNSEEN", "msg_idx=REFIDX_GM3"})}}},
        {"msg_elements", json::array({json{{"msg_idx", "REFIDX_UNSEEN"}, {"content", "很久以前的话"}}})}}, "e4");
    CHECK_EQ(events[2]["message"][0]["data"]["id"].get<std::string>(), std::to_string(quotedId));
    // 引用机器人自己发过的消息：引用索引与发送时拿到的不同，按内容对上原消息。
    Store::Message sent;
    sent.officialId = "BOTMSG"; sent.refIdx = "REFIDX_SENT_A"; sent.scene = static_cast<int>(event::Scene::group);
    sent.peer = "G1"; sent.time = 100; sent.self = true; sent.message = json::array({onebot::text("pong")});
    const auto sentId = store->rememberMessage(sent);
    relay->bot()->dispatch("GROUP_MESSAGE_CREATE", json{{"id", "gm4"}, {"content", "引用"}, {"group_openid", "G1"},
        {"author", {{"member_openid", "U1"}}},
        {"message_scene", {{"ext", json::array({"ref_msg_idx=REFIDX_SENT_B", "msg_idx=REFIDX_GM4"})}}},
        {"msg_elements", json::array({json{{"msg_idx", "REFIDX_SENT_B"}, {"content", "pong"}}})}}, "e5");
    CHECK_EQ(events[3]["message"][0]["data"]["id"].get<std::string>(), std::to_string(sentId));
    events.erase(events.begin() + 1, events.end());

    // 不支持的动作照 OneBot 惯例返回 1404。
    relay->handle("send_like", json::object(), [&](json r) { got = r; });
    CHECK_EQ(got["retcode"].get<int>(), 1404);

    relay->bot()->dispatch("GROUP_JOIN_REQUEST", json{{"group_openid", "G1"}, {"member_openid", "U9"}, {"join_request_id", "R1"},
        {"verify_info", {{"verify_message", "我是希亚"}}}}, "e2");
    CHECK_EQ(events.size(), std::size_t(2));
    CHECK_EQ(events[1]["post_type"].get<std::string>(), std::string("request"));
    CHECK_EQ(events[1]["flag"].get<std::string>(), std::string("G1|U9|R1"));
    CHECK_EQ(events[1]["comment"].get<std::string>(), std::string("我是希亚"));
}

}  // namespace

int main() {
    for (auto& [name, fn] : tests()) {
        const int before = failures;
        try { fn(); } catch (const std::exception& e) { ++failures; std::cerr << name << " 抛出异常：" << e.what() << "\n"; }
        std::cout << (failures == before ? "  OK   " : "  FAIL ") << name << "\n";
    }
    std::cout << checks << " 项检查，" << failures << " 项失败\n";
    return failures == 0 ? 0 : 1;
}
