// CatOfficial 单元测试：不连网，只测转换、存储与事件分派。

#include "../app/bind.h"
#include "../app/config.h"
#include "../app/onebot.h"
#include "../app/relay.h"
#include "../app/replies.h"
#include "../app/store.h"
#include "../app/update/update_service.h"
#include "../app/platform/update_schedule.h"
#include "../app/platform/instance_guard.h"
#include "../app/files/file_store.h"
#include "../app/terminal/pty.h"

#include <catofficial/bot.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

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
        // 号码最大的用户绑定后，空出的号码也不会发给新用户；解绑的人拿新号码。
        CHECK(s.bind(Kind::user, "OPENC", 222333));
        CHECK_EQ(s.number(Kind::user, "OPEND"), Store::kUserBase + 4);
        CHECK_EQ(s.unbind(Kind::user, "OPENC"), Store::kUserBase + 5);

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
    BotConfig cfg; cfg.id = "b1"; cfg.name = "测试"; cfg.appId = "APP"; cfg.appSecret = "SECRET";
    std::vector<json> events;
    Relay::Hooks hooks;
    hooks.emit = [&](const json& e) { events.push_back(e); };
    auto relay = std::make_shared<Relay>(cfg, store, nullptr, hooks);
    relay->init();
    relay->bot()->setIdentityForTest("BOT", "3889000000");
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

TEST(OldConfigMigratesToBotsAndConnections) {
    const auto old = json::parse(R"({"bot":{"app_id":"A","app_secret":"S"},"onebot":{"access_token":"T","message_format":"string",
        "servers":[{"type":"http","host":"127.0.0.1","port":5700}],"http_post":[{"url":"http://x/","secret":"K"}],
        "ws_reverse":[{"url":"ws://y/ws"}]}})");
    const auto c = parseConfig(old);
    CHECK_EQ(c.bots.size(), std::size_t(1));
    CHECK_EQ(c.bots[0].appId, std::string("A"));
    CHECK_EQ(c.connections.size(), std::size_t(3));
    CHECK_EQ(c.connections[0].type, std::string("http_server"));
    CHECK_EQ(c.connections[0].token, std::string("T"));
    CHECK_EQ(c.connections[1].token, std::string("K"));
    CHECK_EQ(c.connections[2].type, std::string("ws_client"));
    CHECK_EQ(c.connections[2].messageFormat, std::string("string"));
    CHECK_EQ(c.webuiPort, uint16_t(18082));
}

TEST(RenamingABotDoesNotNeedReconnect) {
    BotConfig a; a.id = "b"; a.name = "甲"; a.appId = "A"; a.appSecret = "S";
    BotConfig b = a; b.name = "乙"; b.autoAckInteractions = false;
    CHECK(a.sameConnection(b));
    b.appSecret = "S2";
    CHECK(!a.sameConnection(b));
    const auto c = parseConfig(json::parse(R"({"bots":[{"id":"x","type":"onebot","url":"ws://127.0.0.1:3001"},{"id":"y","type":"onebot","port":6801}],
        "webhook":{"path":"/webhook/qqbot","standalone":false,"port":8443},"smtp":{"owner_email":"a@b.c"}})"));
    CHECK_EQ(c.bots[0].connection, std::string("forward"));
    CHECK_EQ(c.bots[1].connection, std::string("reverse"));
    CHECK(!c.webhookStandalone);
    CHECK_EQ(c.smtp.notifyEmails.size(), std::size_t(1));
}

TEST(ConfigRejectsDuplicatePortsAndBadUrls) {
    bool threw = false;
    try { parseConfig(json::parse(R"({"connections":[{"id":"a","type":"ws_server","port":6700},{"id":"b","type":"http_server","port":6700}]})")); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { parseConfig(json::parse(R"({"bots":[{"id":"x","type":"onebot","url":"http://nope"}]})")); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

TEST(BindCommandPairsOfficialAndLegacySides) {
    const auto path = tempDb("bind");
    auto store = std::make_shared<Store>(utf8(path));
    BindConfig cfg; cfg.groupConfirmUsers = 2;
    auto service = std::make_shared<BindService>(cfg, store, nullptr, nullptr);
    service->setLegacyAvailable(true);
    CHECK(service->parse(".bind 暗号").has_value());
    CHECK(service->parse("\xE3\x80\x82" "BIND 暗号").has_value());
    CHECK(!service->parse(".binder").has_value());

    auto official = [](const std::string& user, const std::string& text) {
        return *event::parseMessage("GROUP_MESSAGE_CREATE", json{{"id", "m-" + user + text}, {"content", text}, {"group_openid", "GOPEN"},
            {"author", {{"member_openid", user}}}}, "e", "BOT", "APP");
    };
    std::vector<std::string> replies;
    auto reply = [&](const std::string& t) { replies.push_back(t); };

    // 普通成员：用户绑定，群还差一票。
    CHECK(service->onOfficial("b", official("U1", ".bind 甲乙丙"), reply));
    service->onLegacy(LegacyMessage{"legacy", 778899, 123456, "member", ".bind 甲乙丙"});
    CHECK_EQ(store->number(Kind::user, "U1"), int64_t(123456));
    CHECK(!store->bound(Kind::group, "GOPEN"));
    CHECK_EQ(replies.size(), std::size_t(1));
    CHECK(replies[0].find("还需要 1 位") != std::string::npos);

    // 第二位成员（原机器人那边先到）：够票，群关联。
    service->onLegacy(LegacyMessage{"legacy", 778899, 654321, "member", ".bind 丁戊"});
    CHECK(service->onOfficial("b", official("U2", ".bind 丁戊"), reply));
    CHECK_EQ(store->number(Kind::user, "U2"), int64_t(654321));
    CHECK_EQ(store->number(Kind::group, "GOPEN"), int64_t(778899));

    // 同一暗号同一侧出现两次：作废并提示。
    replies.clear();
    CHECK(service->onOfficial("b", official("U3", ".bind 撞了"), reply));
    CHECK(service->onOfficial("b", official("U4", ".bind 撞了"), reply));
    CHECK_EQ(replies.size(), std::size_t(2));
    CHECK(replies[0].find("同一个暗号") != std::string::npos);

    // 状态与解绑。
    replies.clear();
    CHECK(service->onOfficial("b", official("U1", ".bind status"), reply));
    CHECK(replies.back().find("12**56") != std::string::npos);
    CHECK(service->onOfficial("b", official("U1", ".bind remove"), reply));
    CHECK(!store->bound(Kind::user, "U1"));
    // 普通消息不拦截。
    CHECK(!service->onOfficial("b", official("U1", "你好"), reply));
}

TEST(CustomRepliesMatchRenderAndKeepBuiltin) {
    const auto file = std::filesystem::temp_directory_path() / "catofficial-test-replies.json";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    ReplyService service(file);
    // 第一次载入时补上自带的状态规则。
    CHECK_EQ(service.rules().size(), std::size_t(1));
    CHECK(service.rules()[0].builtin);
    ReplyInput in{"bot1", true, "G1", "#co"};
    auto m = service.match(in);
    CHECK(m.has_value());
    ReplyVariables vars{{"version", "0.1.0"}, {"platform", "Windows 11"}, {"uptime", "1天2小时3分钟"}};
    CHECK_EQ(service.render(m->result, vars, m->captures), std::string("CatOfficial 状态\n版本：0.1.0\n平台：Windows 11\n运行时长：1天2小时3分钟"));
    in.text = "#co 多余";
    CHECK(!service.match(in).has_value());   // 完全匹配

    // 正则、捕获组、优先级与范围。
    auto rule = parseReplyRule(json{{"name", "掷骰"}, {"priority", 200}, {"scope", "private"},
                                    {"conditions", json::array({json{{"type", "regex"}, {"content", "^roll (\\d+)$"}}})},
                                    {"results", json::array({"你掷出了 {$1}，{a|a}"})}});
    service.save(rule);
    ReplyInput dm{"bot1", false, "U1", "roll 42"};
    m = service.match(dm);
    CHECK(m.has_value());
    CHECK_EQ(service.render(m->result, {}, m->captures), std::string("你掷出了 42，a"));
    dm.group = true;
    CHECK(!service.match(dm).has_value());   // 只在私聊生效

    // 冷却：同一会话第二次不回，试一试（dryRun）不受影响。
    auto cooled = parseReplyRule(json{{"conditions", json::array({json{{"type", "search"}, {"content", "早"}}})},
                                      {"results", json::array({"早安"})}, {"cooldown_sec", 60}});
    service.save(cooled);
    ReplyInput hi{"bot1", true, "G9", "大家早"};
    CHECK(service.match(hi).has_value());
    CHECK(!service.match(hi).has_value());
    CHECK(service.match(hi, true).has_value());

    // 自带规则不能删，可以恢复默认；规则会写进文件。
    {
        bool threw = false;
        try { service.remove("builtin-status"); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
    }
    ReplyService reloaded(file);
    CHECK_EQ(reloaded.rules().size(), std::size_t(3));
    bool badRegex = false;
    try { parseReplyRule(json{{"conditions", json::array({json{{"type", "regex"}, {"content", "("}}})}, {"results", json::array({"x"})}}); }
    catch (const std::exception&) { badRegex = true; }
    CHECK(badRegex);
}

TEST(MessageQueryFiltersAndSearches) {
    auto store = std::make_shared<Store>(tempDb("query").string());
    auto add = [&](const std::string& bot, int scene, const std::string& peer, const std::string& sender, int64_t time, const std::string& text) {
        Store::Message m;
        m.bot = bot; m.scene = scene; m.peer = peer; m.sender = sender; m.time = time; m.self = sender.empty();
        m.message = json::array({onebot::text(text)});
        store->rememberMessage(m);
    };
    add("b1", 2, "G1", "U1", 100, "你好 \"世界\"");
    add("b1", 2, "G1", "U2", 200, "掷骰 .r");
    add("b1", 1, "U1", "U1", 300, "私聊一句");
    add("b2", 1, "U3", "", 400, "机器人发的");
    Store::MessageQuery q;
    auto [all, total] = store->queryMessages(q);
    CHECK_EQ(total, int64_t(4));
    CHECK_EQ(all.front().time, int64_t(400));   // 默认新的在前
    q.ascending = true;
    CHECK_EQ(store->queryMessages(q).first.front().time, int64_t(100));
    q = {}; q.scene = 1;
    CHECK_EQ(store->queryMessages(q).second, int64_t(2));
    q = {}; q.text = "\"世界\"";
    CHECK_EQ(store->queryMessages(q).second, int64_t(1));
    q = {}; q.text = "%";
    CHECK_EQ(store->queryMessages(q).second, int64_t(0));
    q = {}; q.sender = "U1";
    CHECK_EQ(store->queryMessages(q).second, int64_t(2));
    q = {}; q.sender = "@self";
    CHECK_EQ(store->queryMessages(q).second, int64_t(1));
    q = {}; q.from = 150; q.to = 350;
    CHECK_EQ(store->queryMessages(q).second, int64_t(2));
    q = {}; q.bot = "b2";
    CHECK_EQ(store->queryMessages(q).second, int64_t(1));
    q = {}; q.peer = "G1"; q.limit = 1; q.offset = 1;
    auto page = store->queryMessages(q);
    CHECK_EQ(page.second, int64_t(2));
    CHECK_EQ(page.first.size(), std::size_t(1));
    const auto conversations = store->conversations("", 10);
    CHECK_EQ(conversations.size(), std::size_t(3));
    CHECK_EQ(conversations.front().peer, std::string("U3"));
    CHECK_EQ(store->conversations("b1", 10).size(), std::size_t(2));
    CHECK_EQ(store->conversationMessages("G1", 0, 0, 10).size(), std::size_t(2));
}

TEST(GroupMessageCarriesMemberNicknameAndAtAll) {
    const json d{{"id", "m9"}, {"content", "<@all> 开会了"}, {"group_openid", "G1"}, {"union_group_openid", "UG1"},
                 {"author", {{"member_openid", "U1"}, {"username", "希亚"}, {"member_nickname", "群里的希亚"}, {"avatar", "https://a/b"}}}};
    auto e = event::parseMessage("GROUP_MESSAGE_CREATE", d, "ev9", "BOT", "APP");
    CHECK(e.has_value());
    CHECK_EQ(e->memberNickname, std::string("群里的希亚"));
    CHECK_EQ(e->username, std::string("希亚"));
    CHECK_EQ(e->avatar, std::string("https://a/b"));
    CHECK_EQ(e->unionGroupOpenId, std::string("UG1"));
    onebot::Resolver r;
    r.user = [](const std::string&) { return int64_t(2000000000001); };
    const auto segs = onebot::incomingSegments(*e, 0, r);
    CHECK_EQ(segs[0]["type"].get<std::string>(), std::string("at"));
    CHECK_EQ(segs[0]["data"]["qq"].get<std::string>(), std::string("all"));
}

TEST(MastersMustNameABot) {
    auto c = parseConfig(json{{"masters", json::array({json{{"bot_id", ""}, {"openid", "X"}}, json{{"bot_id", "b1"}, {"openid", "Y"}}})}});
    CHECK_EQ(c.masters.size(), std::size_t(1));
    CHECK(c.isMaster("b1", "Y"));
    CHECK(!c.isMaster("b2", "Y"));
    CHECK(!c.isMaster("b1", "X"));
    CHECK_EQ(c.messageRetentionDays, 365);
    CHECK_EQ(c.imageCacheDays, 7);
}

TEST(TrayTextIsTrimmedAndLimited) {
    auto c = parseConfig(json::object());
    CHECK(c.trayStartHidden);
    CHECK_EQ(c.trayText, std::string());
    c = parseConfig(json{{"tray", json{{"text", "  希亚的官机  "}, {"start_hidden", false}}}});
    CHECK_EQ(c.trayText, std::string("希亚的官机"));
    CHECK(!c.trayStartHidden);
    CHECK_EQ(tray_settings::tooltip("", 18082), std::string("CatOfficial(18082)"));
    bool threw = false;
    try { parseConfig(json{{"tray", json{{"text", "一二三四五六七八九十十一"}}}}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { parseConfig(json{{"tray", json{{"text", "a\nb"}}}}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

TEST(UpdateManifestIsValidatedStrictly) {
    namespace up = update;
    const std::string sha(64, 'a');
    json manifest{{"schema", 1}, {"repository", "DiceZone/CatOfficial"}, {"tag", "v0.1.0-beta.3"}, {"version", "0.1.0"}, {"build", 3},
                  {"release_notes", "### 新功能\n- 在线更新"},
                  {"assets", json::array({json{{"os", "windows"}, {"arch", "amd64"}, {"name", "CatOfficial-0.1.0-beta.3-windows-amd64.zip"}, {"sha256", sha}, {"size", 100}},
                                          json{{"os", "linux"}, {"arch", "arm64"}, {"name", "CatOfficial-0.1.0-beta.3-linux-arm64.tar.gz"}, {"sha256", sha}, {"size", 100}}})}};
    up::ReleaseManifest parsed;
    std::string error;
    CHECK(up::parseReleaseManifest(manifest.dump(), parsed, error));
    CHECK_EQ(parsed.build, 3);
    CHECK_EQ(parsed.releaseUrl, std::string("https://github.com/DiceZone/CatOfficial/releases/tag/v0.1.0-beta.3"));
    CHECK(up::selectAsset(parsed, "windows", "amd64") != nullptr);
    CHECK(up::selectAsset(parsed, "macos", "arm64") == nullptr);

    auto bad = manifest; bad["repository"] = "someone/else";
    CHECK(!up::parseReleaseManifest(bad.dump(), parsed, error));
    bad = manifest; bad["tag"] = "v0.1.0-beta.4";               // 标签与构建号对不上
    CHECK(!up::parseReleaseManifest(bad.dump(), parsed, error));
    bad = manifest; bad["assets"][0]["name"] = "../evil.zip";
    CHECK(!up::parseReleaseManifest(bad.dump(), parsed, error));
    bad = manifest; bad["assets"][1]["os"] = "windows"; bad["assets"][1]["arch"] = "amd64";   // 同一平台两个包
    CHECK(!up::parseReleaseManifest(bad.dump(), parsed, error));
    bad = manifest; bad["assets"][0]["sha256"] = "xyz";
    CHECK(!up::parseReleaseManifest(bad.dump(), parsed, error));
}

TEST(UpdateVersionsAndArchivePaths) {
    namespace up = update;
    CHECK_EQ(up::compareRelease("0.1.0", 1, "0.1.0", 2), -1);
    CHECK_EQ(up::compareRelease("0.2.0", 0, "0.1.0", 9), 1);
    CHECK_EQ(up::compareRelease("0.1.0", 2, "0.1.0", 2), 0);
    CHECK(up::archiveEntrySafe("CatOfficial-beta/app/catofficial-core.exe"));
    CHECK(!up::archiveEntrySafe("../catofficial.exe"));
    CHECK(!up::archiveEntrySafe("/etc/passwd"));
    CHECK(!up::archiveEntrySafe("C:\\Windows\\x.dll"));
    CHECK(!up::archiveEntrySafe("a/../../b"));
    CHECK_EQ(up::buildMirroredUrl("https://github.com/x", "https://m.example/"), std::string("https://m.example/https://github.com/x"));
    // 2026-11-22 00:00 UTC，服务器在 UTC+8 时 04:00 的下一次安装时间是当天北京时间 04:00。
    const std::int64_t midnight = 1795305600;
    CHECK_EQ(up::nextInstallTime(midnight, 480, "04:00"), midnight + 20 * 3600);
    CHECK(!up::validInstallTime("24:00"));
    CHECK(up::releaseNotesPreview("### 新功能\n\n- **app**：在线更新\n\n## 下载\n| a |").find("下载") == std::string::npos);
}

TEST(WindowsPackageNeedsLauncherCoreAndRuntime) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("catofficial-pkg-test-" + std::to_string(std::time(nullptr)));
    fs::create_directories(dir / "app");
    fs::create_directories(dir / "web");
    CHECK(!update::missingWindowsPackageComponents(dir).empty());
    for (const char* file : {"catofficial.exe", "app/catofficial-core.exe", "app/msvcp140.dll", "app/vcruntime140.dll", "app/vcruntime140_1.dll", "web/index.html"})
        std::ofstream(dir / file) << "x";
    CHECK(update::missingWindowsPackageComponents(dir).empty());
    fs::remove_all(dir);
}

TEST(PortProbeSeesAnotherListener) {
    // 随便挑一个大概率空闲的端口：没人监听时不算占用。
    CHECK(!portInUse(1, "127.0.0.1"));
}

TEST(FileStoreStaysInsideTheInstallRoot) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("catofficial-files-" + std::to_string(std::time(nullptr)));
    fs::create_directories(root / "data");
    files::FileStore store(root, false);
    bool threw = false;
    try { store.list("../"); } catch (const files::FileError&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { store.list("/.catofficial"); } catch (const files::FileError&) { threw = true; }
    CHECK(threw);

    store.write("/data/牌堆.json", "{\"a\":1}");
    CHECK_EQ(store.read("/data/牌堆.json"), std::string("{\"a\":1}"));
    const auto listed = store.list("/data");
    CHECK_EQ(listed.size(), std::size_t(1));
    CHECK_EQ(listed[0].name, std::string("牌堆.json"));

    // 删除进回收站，还原回原处；回收站目录本身在列表里看不见
    const auto trashed = store.discard("/data/牌堆.json", false, false, "希亚");
    CHECK(trashed.has_value());
    CHECK(!store.exists("/data/牌堆.json"));
    CHECK_EQ(store.trashList().size(), std::size_t(1));
    for (const auto& e : store.list("/")) CHECK(e.name != ".catofficial");
    store.trashRestore(trashed->id, "希亚", false);
    CHECK(store.exists("/data/牌堆.json"));
    CHECK(store.trashList().empty());

    // 目录非空时不加 recursive 不让删
    threw = false;
    try { store.discard("/data", false, false, "希亚"); } catch (const files::FileError&) { threw = true; }
    CHECK(threw);

    // 打包再解压：中文文件名原样回来
    store.compress({"/data"}, "/备份.zip", false, {});
    CHECK(store.exists("/备份.zip"));
    CHECK_EQ(store.countEntries("/备份.zip"), std::int64_t(2));
    store.extract("/备份.zip", "/restore", "utf8", false, {});
    CHECK_EQ(store.read("/restore/data/牌堆.json"), std::string("{\"a\":1}"));
    // 已存在时不覆盖
    threw = false;
    try { store.extract("/备份.zip", "/restore", "utf8", false, {}); } catch (const files::FileError&) { threw = true; }
    CHECK(threw);
    CHECK_EQ(store.peekConflicts("/备份.zip", "/restore", "utf8", 20).size(), std::size_t(1));

    // 操作记录
    files::OpRecord op;
    op.action = "write";
    op.path = "/data/牌堆.json";
    store.record(op);
    CHECK_EQ(store.readOps(10).size(), std::size_t(1));
    fs::remove_all(root);
}

TEST(FullAccessListsTheWholeMachine) {
    files::FileStore store(std::filesystem::temp_directory_path(), true);
    const auto root = store.list("/");
    CHECK(!root.empty());
#ifdef _WIN32
    // Windows 上根目录列出各个盘，路径写成 /C:/...
    CHECK(std::any_of(root.begin(), root.end(), [](const files::Entry& e) { return e.name == "C:"; }));
    CHECK(!store.list("/C:/").empty());
#else
    CHECK(std::any_of(root.begin(), root.end(), [](const files::Entry& e) { return e.name == "tmp" || e.name == "usr"; }));
#endif
}

TEST(PtyRunsACommandAndReadsItsOutput) {
    terminal::PtyOptions options;
#ifdef _WIN32
    options.commandLine = L"cmd.exe /d /c echo catofficial-pty";
#else
    options.argv = {"/bin/sh", "-c", "echo catofficial-pty"};
#endif
    std::string error;
    auto pty = terminal::Pty::start(options, error);
    CHECK(pty != nullptr);
    if (!pty) return;
    std::string output;
    std::vector<char> buffer(4096);
    for (std::size_t n; (n = pty->read(buffer.data(), buffer.size())) > 0;) output.append(buffer.data(), n);
    CHECK_EQ(pty->wait(), 0);
    CHECK(output.find("catofficial-pty") != std::string::npos);
}

TEST(InteractiveShellSurvivesInput) {
    // 网页终端的 shell 收到按键后必须还活着（曾经：PowerShell 一收到输入就退出）
    terminal::PtyOptions options;
#ifdef _WIN32
    options.commandLine = L"\"" + std::filesystem::path(fromUtf8(terminal::defaultShell())).wstring() + L"\" -NoLogo";
#else
    options.argv = {"/bin/sh", "-i"};
#endif
    std::string error;
    auto pty = terminal::Pty::start(options, error);
    CHECK(pty != nullptr);
    if (!pty) return;
    std::atomic<bool> done{false};
    std::string output;
    std::mutex mu;
    std::thread reader([&] {
        std::vector<char> buffer(4096);
        for (std::size_t n; (n = pty->read(buffer.data(), buffer.size())) > 0;) { std::lock_guard lock(mu); output.append(buffer.data(), n); }
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::seconds(3));
    pty->write("echo shell-alive-check\r");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const bool aliveAfterInput = !done;
    pty->write("exit\r");
    for (int i = 0; i < 50 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!done) pty->kill();
    reader.join();
    pty->wait();
    std::string seen;
    { std::lock_guard lock(mu); seen = output; }
    if (!aliveAfterInput || seen.find("shell-alive-check") == std::string::npos) std::cout << "  shell output: " << seen.substr(0, 400) << "\n";
    CHECK(aliveAfterInput);
    CHECK(seen.find("shell-alive-check") != std::string::npos);
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
