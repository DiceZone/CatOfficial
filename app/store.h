#pragma once

// 本地存储（SQLite）：
//   · identities：OpenID ↔ OneBot 数字 ID。官方只给 OpenID，OneBot 应用只认数字，
//     所以每个 OpenID 分配一个固定的 13 位号码（群 1 开头、用户 2 开头，与 Dice!Next
//     相同）；已做官民绑定的直接使用真实 QQ 号 / 群号，应用里的旧数据因此能接上。
//   · names / members：平台给过的昵称、群成员与身份，供 get_*_info 使用。
//   · messages：消息索引。OneBot 的 message_id 是 int32，官方消息 ID 是字符串；
//     引用回复、get_msg、撤回都靠这张表换算。

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace catofficial::app {

enum class Kind { user, group };
inline const char* kindName(Kind kind) { return kind == Kind::group ? "group" : "user"; }

class Store {
public:
    static constexpr int64_t kGroupBase = 1000000000000;
    static constexpr int64_t kUserBase = 2000000000000;

    struct Message {
        int64_t id = 0;              // OneBot message_id
        std::string officialId;
        std::string refIdx;          // 引用索引（REFIDX_…）；引用回复靠它
        int scene = 0;               // event::Scene
        std::string peer;            // 单聊为用户 OpenID，群为 group_openid
        std::string sender;          // 发送者 OpenID；机器人自己发的为空
        int64_t time = 0;
        bool self = false;
        nlohmann::json message = nlohmann::json::array();   // OneBot 消息段
    };

    struct Member { std::string groupOpenId, userOpenId, role, card; int64_t lastSeen = 0; };

    struct Identity {
        Kind kind = Kind::user;
        std::string openId, name;
        int64_t number = 0;
        bool bound = false;
        bool direct = false;
    };

    explicit Store(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "无法打开数据库";
            sqlite3_close(db_); db_ = nullptr;
            throw std::runtime_error("打开数据库失败：" + error);
        }
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA busy_timeout=5000");
        exec("CREATE TABLE IF NOT EXISTS identities(kind TEXT NOT NULL, open_id TEXT NOT NULL, number INTEGER NOT NULL,"
             " bound INTEGER NOT NULL DEFAULT 0, direct INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(kind, open_id))");
        exec("CREATE UNIQUE INDEX IF NOT EXISTS identities_number ON identities(kind, number)");
        // 已分配过的最大号码。号码一旦发出就不再回收，绑定或解绑后空出的号码也不会给别人。
        exec("CREATE TABLE IF NOT EXISTS counters(kind TEXT PRIMARY KEY, last INTEGER NOT NULL)");
        exec("CREATE TABLE IF NOT EXISTS names(kind TEXT NOT NULL, open_id TEXT NOT NULL, name TEXT NOT NULL,"
             " updated INTEGER NOT NULL, PRIMARY KEY(kind, open_id))");
        exec("CREATE TABLE IF NOT EXISTS members(group_id TEXT NOT NULL, user_id TEXT NOT NULL, role TEXT NOT NULL DEFAULT 'member',"
             " card TEXT NOT NULL DEFAULT '', last_seen INTEGER NOT NULL, PRIMARY KEY(group_id, user_id))");
        exec("CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY AUTOINCREMENT, official_id TEXT NOT NULL,"
             " scene INTEGER NOT NULL, peer TEXT NOT NULL, sender TEXT NOT NULL, time INTEGER NOT NULL,"
             " self INTEGER NOT NULL, message TEXT NOT NULL)");
        exec("CREATE INDEX IF NOT EXISTS messages_official ON messages(official_id)");
        if (!hasColumn("messages", "ref_idx")) exec("ALTER TABLE messages ADD COLUMN ref_idx TEXT NOT NULL DEFAULT ''");
        exec("CREATE INDEX IF NOT EXISTS messages_ref ON messages(ref_idx)");
    }
    ~Store() { sqlite3_close(db_); }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /// OpenID 对应的数字 ID；第一次见到时分配。
    int64_t number(Kind kind, const std::string& openId) {
        if (openId.empty()) return 0;
        std::lock_guard lock(mu_);
        if (auto existing = numberLocked(kind, openId)) return *existing;
        const int64_t base = kind == Kind::group ? kGroupBase : kUserBase;
        int64_t last = base;
        {
            Statement s(db_, "SELECT last FROM counters WHERE kind=?");
            s.bind(1, kindName(kind));
            if (s.step()) last = sqlite3_column_int64(s.raw(), 0);
        }
        {
            Statement s(db_, "SELECT MAX(number) FROM identities WHERE kind=? AND number>? AND number<?");
            s.bind(1, kindName(kind)); s.bind(2, base); s.bind(3, base + kGroupBase);
            if (s.step() && sqlite3_column_type(s.raw(), 0) != SQLITE_NULL) last = (std::max)(last, sqlite3_column_int64(s.raw(), 0));
        }
        const int64_t next = last + 1;
        {
            Statement s(db_, "INSERT INTO counters(kind, last) VALUES(?,?) ON CONFLICT(kind) DO UPDATE SET last=excluded.last");
            s.bind(1, kindName(kind)); s.bind(2, next); s.run();
        }
        Statement insert(db_, "INSERT INTO identities(kind, open_id, number, bound) VALUES(?,?,?,0)");
        insert.bind(1, kindName(kind)); insert.bind(2, openId); insert.bind(3, next);
        insert.run();
        return next;
    }

    /// 数字 ID 对应的 OpenID；未知时为空。
    std::string openId(Kind kind, int64_t number) {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT open_id FROM identities WHERE kind=? AND number=?");
        s.bind(1, kindName(kind)); s.bind(2, number);
        return s.step() ? s.text(0) : std::string();
    }

    /// 官民绑定：让 OpenID 改用真实号码。号码已被别的 OpenID 占用时返回 false。
    bool bind(Kind kind, const std::string& openId, int64_t realNumber) {
        if (openId.empty() || realNumber <= 0) return false;
        std::lock_guard lock(mu_);
        {
            Statement s(db_, "SELECT open_id FROM identities WHERE kind=? AND number=?");
            s.bind(1, kindName(kind)); s.bind(2, realNumber);
            if (s.step() && s.text(0) != openId) return false;
        }
        Statement s(db_, "INSERT INTO identities(kind, open_id, number, bound) VALUES(?,?,?,1)"
                         " ON CONFLICT(kind, open_id) DO UPDATE SET number=excluded.number, bound=1");
        s.bind(1, kindName(kind)); s.bind(2, openId); s.bind(3, realNumber);
        s.run();
        return true;
    }

    bool bound(Kind kind, const std::string& openId) {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT bound FROM identities WHERE kind=? AND open_id=?");
        s.bind(1, kindName(kind)); s.bind(2, openId);
        return s.step() && sqlite3_column_int(s.raw(), 0) != 0;
    }

    void markDirect(const std::string& userOpenId) {
        if (userOpenId.empty()) return;
        number(Kind::user, userOpenId);
        std::lock_guard lock(mu_);
        Statement s(db_, "UPDATE identities SET direct=1 WHERE kind='user' AND open_id=?");
        s.bind(1, userOpenId); s.run();
    }
    /// 私聊过的用户（OneBot 的好友列表）。
    std::vector<std::string> directUsers() {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT open_id FROM identities WHERE kind='user' AND direct=1 ORDER BY number");
        std::vector<std::string> out;
        while (s.step()) out.push_back(s.text(0));
        return out;
    }
    std::vector<std::string> groups() {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT open_id FROM identities WHERE kind='group' ORDER BY number");
        std::vector<std::string> out;
        while (s.step()) out.push_back(s.text(0));
        return out;
    }

    /// 只记平台给出的非空名称，不让空值或失败覆盖已知名称。
    void rememberName(Kind kind, const std::string& openId, const std::string& name) {
        if (openId.empty() || name.empty() || name == openId || name.size() > 1024) return;
        std::lock_guard lock(mu_);
        Statement s(db_, "INSERT INTO names(kind, open_id, name, updated) VALUES(?,?,?,?)"
                         " ON CONFLICT(kind, open_id) DO UPDATE SET name=excluded.name, updated=excluded.updated");
        s.bind(1, kindName(kind)); s.bind(2, openId); s.bind(3, name); s.bind(4, static_cast<int64_t>(std::time(nullptr)));
        s.run();
    }
    std::string name(Kind kind, const std::string& openId) {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT name FROM names WHERE kind=? AND open_id=?");
        s.bind(1, kindName(kind)); s.bind(2, openId);
        return s.step() ? s.text(0) : std::string();
    }

    void rememberMember(const std::string& groupOpenId, const std::string& userOpenId, const std::string& role) {
        if (groupOpenId.empty() || userOpenId.empty()) return;
        std::lock_guard lock(mu_);
        Statement s(db_, "INSERT INTO members(group_id, user_id, role, last_seen) VALUES(?,?,?,?)"
                         " ON CONFLICT(group_id, user_id) DO UPDATE SET last_seen=excluded.last_seen,"
                         " role=CASE WHEN excluded.role='' THEN members.role ELSE excluded.role END");
        s.bind(1, groupOpenId); s.bind(2, userOpenId); s.bind(3, role); s.bind(4, static_cast<int64_t>(std::time(nullptr)));
        s.run();
    }
    void forgetMember(const std::string& groupOpenId, const std::string& userOpenId) {
        std::lock_guard lock(mu_);
        Statement s(db_, "DELETE FROM members WHERE group_id=? AND user_id=?");
        s.bind(1, groupOpenId); s.bind(2, userOpenId); s.run();
    }
    void forgetGroupMembers(const std::string& groupOpenId) {
        std::lock_guard lock(mu_);
        Statement s(db_, "DELETE FROM members WHERE group_id=?");
        s.bind(1, groupOpenId); s.run();
    }
    std::optional<Member> member(const std::string& groupOpenId, const std::string& userOpenId) {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT group_id, user_id, role, card, last_seen FROM members WHERE group_id=? AND user_id=?");
        s.bind(1, groupOpenId); s.bind(2, userOpenId);
        if (!s.step()) return std::nullopt;
        return Member{s.text(0), s.text(1), s.text(2), s.text(3), sqlite3_column_int64(s.raw(), 4)};
    }
    std::vector<Member> members(const std::string& groupOpenId) {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT group_id, user_id, role, card, last_seen FROM members WHERE group_id=? ORDER BY last_seen DESC");
        s.bind(1, groupOpenId);
        std::vector<Member> out;
        while (s.step()) out.push_back(Member{s.text(0), s.text(1), s.text(2), s.text(3), sqlite3_column_int64(s.raw(), 4)});
        return out;
    }

    /// 记录一条消息，返回 OneBot message_id。
    int64_t rememberMessage(const Message& m) {
        std::lock_guard lock(mu_);
        Statement s(db_, "INSERT INTO messages(official_id, scene, peer, sender, time, self, message, ref_idx) VALUES(?,?,?,?,?,?,?,?)");
        s.bind(1, m.officialId); s.bind(2, static_cast<int64_t>(m.scene)); s.bind(3, m.peer); s.bind(4, m.sender);
        s.bind(5, m.time); s.bind(6, static_cast<int64_t>(m.self ? 1 : 0)); s.bind(7, m.message.dump()); s.bind(8, m.refIdx);
        s.run();
        // OneBot 的 message_id 是 int32；超出时取低 31 位，与旧记录冲突的概率可以忽略。
        return sqlite3_last_insert_rowid(db_) & 0x7fffffff;
    }
    std::optional<Message> messageById(int64_t id) {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT id, official_id, scene, peer, sender, time, self, message, ref_idx FROM messages WHERE id=?");
        s.bind(1, id);
        return s.step() ? std::optional<Message>(row(s)) : std::nullopt;
    }
    std::optional<Message> messageByOfficialId(const std::string& officialId) {
        if (officialId.empty()) return std::nullopt;
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT id, official_id, scene, peer, sender, time, self, message, ref_idx FROM messages WHERE official_id=? ORDER BY id DESC LIMIT 1");
        s.bind(1, officialId);
        return s.step() ? std::optional<Message>(row(s)) : std::nullopt;
    }
    std::optional<Message> messageByRefIdx(const std::string& refIdx) {
        if (refIdx.empty()) return std::nullopt;
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT id, official_id, scene, peer, sender, time, self, message, ref_idx FROM messages WHERE ref_idx=? ORDER BY id DESC LIMIT 1");
        s.bind(1, refIdx);
        return s.step() ? std::optional<Message>(row(s)) : std::nullopt;
    }
    /// 某个会话里最近的消息，新的在前。
    std::vector<Message> recentMessages(const std::string& peer, int limit) {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT id, official_id, scene, peer, sender, time, self, message, ref_idx FROM messages WHERE peer=? ORDER BY id DESC LIMIT ?");
        s.bind(1, peer); s.bind(2, static_cast<int64_t>(limit));
        std::vector<Message> out;
        while (s.step()) out.push_back(row(s));
        return out;
    }
    void setRefIdx(int64_t id, const std::string& refIdx) {
        std::lock_guard lock(mu_);
        Statement s(db_, "UPDATE messages SET ref_idx=? WHERE id=? AND ref_idx=''");
        s.bind(1, refIdx); s.bind(2, id); s.run();
    }

    /// 已做官民绑定的身份。
    std::vector<Identity> boundIdentities() {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT i.kind, i.open_id, i.number, i.bound, i.direct, COALESCE(n.name, '') FROM identities i"
                         " LEFT JOIN names n ON n.kind=i.kind AND n.open_id=i.open_id WHERE i.bound=1 ORDER BY i.kind, i.number");
        std::vector<Identity> out;
        while (s.step()) out.push_back(identityRow(s));
        return out;
    }
    /// 按号码、OpenID 或名称查身份。
    std::vector<Identity> searchIdentities(const std::string& query, int limit) {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT i.kind, i.open_id, i.number, i.bound, i.direct, COALESCE(n.name, '') FROM identities i"
                         " LEFT JOIN names n ON n.kind=i.kind AND n.open_id=i.open_id"
                         " WHERE i.open_id NOT LIKE 'bot:%' AND (?='' OR CAST(i.number AS TEXT) LIKE ? OR i.open_id LIKE ? OR n.name LIKE ?)"
                         " ORDER BY i.kind, i.number LIMIT ?");
        const auto like = "%" + query + "%";
        s.bind(1, query); s.bind(2, like); s.bind(3, like); s.bind(4, like); s.bind(5, static_cast<int64_t>(limit));
        std::vector<Identity> out;
        while (s.step()) out.push_back(identityRow(s));
        return out;
    }
    /// 解除官民绑定：改回分配的号码。返回新号码，未绑定时返回 0。
    int64_t unbind(Kind kind, const std::string& openId) {
        {
            std::lock_guard lock(mu_);
            Statement check(db_, "SELECT bound FROM identities WHERE kind=? AND open_id=?");
            check.bind(1, kindName(kind)); check.bind(2, openId);
            if (!check.step() || sqlite3_column_int(check.raw(), 0) == 0) return 0;
            Statement s(db_, "DELETE FROM identities WHERE kind=? AND open_id=?");
            s.bind(1, kindName(kind)); s.bind(2, openId); s.run();
        }
        return number(kind, openId);
    }
    /// 全部会话最近的消息，新的在前；beforeId 大于 0 时只取更早的。
    std::vector<Message> recentAll(int limit, int64_t beforeId) {
        std::lock_guard lock(mu_);
        Statement s(db_, "SELECT id, official_id, scene, peer, sender, time, self, message, ref_idx FROM messages"
                         " WHERE (?<=0 OR id<?) ORDER BY id DESC LIMIT ?");
        s.bind(1, beforeId); s.bind(2, beforeId); s.bind(3, static_cast<int64_t>(limit));
        std::vector<Message> out;
        while (s.step()) out.push_back(row(s));
        return out;
    }
    int64_t count(const char* table) {
        std::lock_guard lock(mu_);
        Statement s(db_, (std::string("SELECT COUNT(*) FROM ") + table).c_str());
        return s.step() ? sqlite3_column_int64(s.raw(), 0) : 0;
    }

    /// 删除早于 cutoff 的消息索引。
    void pruneMessages(int64_t cutoff) {
        std::lock_guard lock(mu_);
        Statement s(db_, "DELETE FROM messages WHERE time<?");
        s.bind(1, cutoff); s.run();
    }

private:
    class Statement {
    public:
        Statement(sqlite3* db, const char* sql) : db_(db) {
            if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
                throw std::runtime_error(std::string("SQL 准备失败：") + sqlite3_errmsg(db));
        }
        ~Statement() { sqlite3_finalize(stmt_); }
        void bind(int i, const std::string& v) { sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }
        void bind(int i, const char* v) { sqlite3_bind_text(stmt_, i, v, -1, SQLITE_TRANSIENT); }
        void bind(int i, int64_t v) { sqlite3_bind_int64(stmt_, i, v); }
        bool step() {
            const int rc = sqlite3_step(stmt_);
            if (rc == SQLITE_ROW) return true;
            if (rc == SQLITE_DONE) return false;
            throw std::runtime_error(std::string("SQL 执行失败：") + sqlite3_errmsg(db_));
        }
        void run() { step(); }
        std::string text(int col) const {
            const auto* p = sqlite3_column_text(stmt_, col);
            return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(stmt_, col)) : std::string();
        }
        sqlite3_stmt* raw() const { return stmt_; }
    private:
        sqlite3* db_;
        sqlite3_stmt* stmt_ = nullptr;
    };

    static Identity identityRow(Statement& s) {
        Identity i;
        i.kind = s.text(0) == "group" ? Kind::group : Kind::user;
        i.openId = s.text(1);
        i.number = sqlite3_column_int64(s.raw(), 2);
        i.bound = sqlite3_column_int(s.raw(), 3) != 0;
        i.direct = sqlite3_column_int(s.raw(), 4) != 0;
        i.name = s.text(5);
        return i;
    }

    static Message row(Statement& s) {
        Message m;
        m.id = sqlite3_column_int64(s.raw(), 0) & 0x7fffffff;
        m.officialId = s.text(1);
        m.scene = sqlite3_column_int(s.raw(), 2);
        m.peer = s.text(3);
        m.sender = s.text(4);
        m.time = sqlite3_column_int64(s.raw(), 5);
        m.self = sqlite3_column_int(s.raw(), 6) != 0;
        m.message = nlohmann::json::parse(s.text(7), nullptr, false);
        if (!m.message.is_array()) m.message = nlohmann::json::array();
        m.refIdx = s.text(8);
        return m;
    }

    std::optional<int64_t> numberLocked(Kind kind, const std::string& openId) {
        Statement s(db_, "SELECT number FROM identities WHERE kind=? AND open_id=?");
        s.bind(1, kindName(kind)); s.bind(2, openId);
        if (s.step()) return sqlite3_column_int64(s.raw(), 0);
        return std::nullopt;
    }
    bool hasColumn(const char* table, const char* column) {
        Statement s(db_, (std::string("PRAGMA table_info(") + table + ")").c_str());
        while (s.step()) if (s.text(1) == column) return true;
        return false;
    }
    void exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : "未知错误";
            sqlite3_free(error);
            throw std::runtime_error(std::string("初始化数据库失败：") + message);
        }
    }

    std::mutex mu_;
    sqlite3* db_ = nullptr;
};

}  // namespace catofficial::app
