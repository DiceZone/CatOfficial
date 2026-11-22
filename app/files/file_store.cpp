// 文件管理的实现（移植自 BDC 公寓守护进程的 filestore / trash / jobs，行为与提示语保持一致）。
#include "file_store.h"

#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace catofficial::app::files {

namespace {

std::string nowRfc3339(std::int64_t offsetSeconds = 0) {
    const std::time_t t = std::time(nullptr) + offsetSeconds;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::int64_t parseRfc3339(const std::string& text) {
    std::tm tm{};
    std::istringstream in(text);
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail()) return 0;
#ifdef _WIN32
    return static_cast<std::int64_t>(_mkgmtime(&tm));
#else
    return static_cast<std::int64_t>(timegm(&tm));
#endif
}

std::string fileTime(const fs::path& path) {
    std::error_code ec;
    const auto ftime = fs::last_write_time(path, ec);
    if (ec) return {};
    // 不依赖 clock_cast（旧的 libc++ 没有）：按两个时钟的「现在」换算。
    const auto system = std::chrono::system_clock::now() +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(ftime - fs::file_time_type::clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(system);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string randomHex(std::size_t bytes) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto value = static_cast<unsigned>(rng() & 0xff);
        out += digits[value >> 4];
        out += digits[value & 15];
    }
    return out;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// 比较路径时用的规范形式（Windows 上不分大小写）。
std::string pathKey(const fs::path& path) {
    auto text = path.lexically_normal().generic_u8string();
    std::string key(text.begin(), text.end());
    while (key.size() > 1 && key.back() == '/') key.pop_back();
#ifdef _WIN32
    key = lower(key);
#endif
    return key;
}

bool within(const fs::path& child, const fs::path& parent) {
    const auto c = pathKey(child), p = pathKey(parent);
    if (c == p) return true;
    if (p.empty()) return false;
    return c.size() > p.size() && c.compare(0, p.size(), p) == 0 && (p.back() == '/' || c[p.size()] == '/');
}

const std::regex& trashIdPattern() {
    static const std::regex pattern("^[0-9]{8}T[0-9]{6}-[0-9a-f]{8}$");
    return pattern;
}

std::string displayPath(const std::string& path) {
    return path.empty() ? "/" : path;
}

/// 跨盘或跨文件系统改名失败时退回「复制后删除」。
void moveAcross(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return;
    if (ec != std::errc::cross_device_link
#ifdef _WIN32
        && ec.value() != ERROR_NOT_SAME_DEVICE
#endif
    ) throw fs::filesystem_error("rename", from, to, ec);
    if (fs::is_directory(fs::symlink_status(from))) {
        fs::create_directories(to);
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    } else {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    }
    fs::remove_all(from);
}

// ── GBK 条目名 ────────────────────────────────────────────────
std::string gbkToUtf8(const std::string& text) {
#ifdef _WIN32
    const int wide = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide <= 0) return text;
    std::wstring w(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(936, 0, text.data(), static_cast<int>(text.size()), w.data(), wide);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, out.data(), bytes, nullptr, nullptr);
    return out;
#else
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == reinterpret_cast<iconv_t>(-1)) return text;
    std::string input = text;
    std::string out(text.size() * 3 + 4, '\0');
    char* in = input.data();
    char* dst = out.data();
    size_t inLeft = input.size(), outLeft = out.size();
    const auto result = iconv(cd, &in, &inLeft, &dst, &outLeft);
    iconv_close(cd);
    if (result == static_cast<size_t>(-1)) return text;
    out.resize(out.size() - outLeft);
    return out;
#endif
}

// ── minizip：用宽字符路径打开文件（中文路径在 Windows 上才能打开）────────
#ifdef _WIN32
voidpf ZCALLBACK openWide(voidpf, const void* filename, int mode) {
    const wchar_t* access = nullptr;
    if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) == ZLIB_FILEFUNC_MODE_READ) access = L"rb";
    else if (mode & ZLIB_FILEFUNC_MODE_EXISTING) access = L"r+b";
    else if (mode & ZLIB_FILEFUNC_MODE_CREATE) access = L"wb";
    FILE* file = nullptr;
    if (filename && access) _wfopen_s(&file, static_cast<const wchar_t*>(filename), access);
    return file;
}
uLong ZCALLBACK readFile(voidpf, voidpf stream, void* buf, uLong size) { return static_cast<uLong>(fread(buf, 1, size, static_cast<FILE*>(stream))); }
uLong ZCALLBACK writeFile(voidpf, voidpf stream, const void* buf, uLong size) { return static_cast<uLong>(fwrite(buf, 1, size, static_cast<FILE*>(stream))); }
ZPOS64_T ZCALLBACK tellFile(voidpf, voidpf stream) { return static_cast<ZPOS64_T>(_ftelli64(static_cast<FILE*>(stream))); }
long ZCALLBACK seekFile(voidpf, voidpf stream, ZPOS64_T offset, int origin) {
    int whence = SEEK_SET;
    if (origin == ZLIB_FILEFUNC_SEEK_CUR) whence = SEEK_CUR;
    else if (origin == ZLIB_FILEFUNC_SEEK_END) whence = SEEK_END;
    return _fseeki64(static_cast<FILE*>(stream), static_cast<__int64>(offset), whence) == 0 ? 0 : -1;
}
int ZCALLBACK closeFile(voidpf, voidpf stream) { return fclose(static_cast<FILE*>(stream)); }
int ZCALLBACK errorFile(voidpf, voidpf stream) { return ferror(static_cast<FILE*>(stream)); }

zlib_filefunc64_def wideFileFuncs() {
    zlib_filefunc64_def def{};
    def.zopen64_file = openWide;
    def.zread_file = readFile;
    def.zwrite_file = writeFile;
    def.ztell64_file = tellFile;
    def.zseek64_file = seekFile;
    def.zclose_file = closeFile;
    def.zerror_file = errorFile;
    def.opaque = nullptr;
    return def;
}
#endif

unzFile openUnzip(const fs::path& path) {
#ifdef _WIN32
    auto funcs = wideFileFuncs();
    const std::wstring wide = path.wstring();
    return unzOpen2_64(wide.c_str(), &funcs);
#else
    return unzOpen64(path.c_str());
#endif
}

zipFile openZip(const fs::path& path) {
#ifdef _WIN32
    auto funcs = wideFileFuncs();
    const std::wstring wide = path.wstring();
    return zipOpen2_64(wide.c_str(), APPEND_STATUS_CREATE, nullptr, &funcs);
#else
    return zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
#endif
}

struct UnzipEntry { std::string name; bool isDir; std::uint64_t size; };

/// 读出当前条目的信息；置了 UTF-8 标志位的条目一律按 UTF-8，不受 encoding 影响。
UnzipEntry currentEntry(unzFile zip, const std::string& encoding) {
    unz_file_info64 info{};
    std::array<char, 4096> name{};
    if (unzGetCurrentFileInfo64(zip, &info, name.data(), static_cast<uLong>(name.size() - 1), nullptr, 0, nullptr, 0) != UNZ_OK)
        throw FileError("压缩包损坏，无法读取条目");
    std::string text(name.data());
    if (!(info.flag & (1u << 11)) && lower(encoding) == "gbk") text = gbkToUtf8(text);
    std::replace(text.begin(), text.end(), '\\', '/');
    const bool dir = !text.empty() && text.back() == '/';
    return {text, dir, static_cast<std::uint64_t>(info.uncompressed_size)};
}

/// zip slip：按 path.Clean("/"+name) 的语义夹回目标目录里，.. 不会走到外面去。
std::string cleanEntryName(const std::string& name) {
    std::vector<std::string> parts;
    std::stringstream in(name);
    std::string part;
    while (std::getline(in, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(part);
    }
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

}  // namespace

// ─── 记录 ───────────────────────────────────────────────────

json TrashEntry::toJson() const {
    json j{{"id", id}, {"path", path}, {"is_dir", isDir}, {"size", size}, {"deleted_at", deletedAt}, {"expires_at", expiresAt}};
    if (!deletedBy.empty()) j["deleted_by"] = deletedBy;
    return j;
}

json OpRecord::toJson() const {
    json j{{"t", time}, {"action", action}};
    if (!path.empty()) j["path"] = path;
    if (!target.empty()) j["target"] = target;
    if (count) j["count"] = count;
    if (bytes) j["bytes"] = bytes;
    if (!actor.empty()) j["actor"] = actor;
    if (!ip.empty()) j["ip"] = ip;
    if (!trashId.empty()) j["trash_id"] = trashId;
    if (permanent) j["permanent"] = true;
    return j;
}

void Job::finish(const std::string& error) {
    std::lock_guard lock(mu_);
    finished_ = std::chrono::steady_clock::now();
    if (!error.empty()) { status_ = "failed"; error_ = error; return; }
    status_ = "done";
    if (total_ > 0) done_ = total_.load();   // 成功时补满，免得停在 98%
}

json Job::snapshot() const {
    std::lock_guard lock(mu_);
    const auto end = status_ == "running" ? std::chrono::steady_clock::now() : finished_;
    const auto done = done_.load(), total = total_.load();
    int percent = 0;
    if (total > 0) percent = static_cast<int>(std::min<std::int64_t>(100, done * 100 / total));
    return json{{"id", id_}, {"kind", kind_}, {"status", status_}, {"done", done}, {"total", total}, {"percent", percent},
                {"current", current_}, {"error", error_},
                {"elapsed_secs", std::chrono::duration_cast<std::chrono::seconds>(end - started_).count()}};
}

bool Job::expired() const {
    std::lock_guard lock(mu_);
    return status_ != "running" && std::chrono::steady_clock::now() - finished_ > std::chrono::minutes(10);
}

// ─── FileStore ─────────────────────────────────────────────

FileStore::FileStore(fs::path installRoot, bool fullAccess) : full_(fullAccess) {
    std::error_code ec;
    install_ = fs::weakly_canonical(fs::absolute(installRoot, ec), ec);
    if (install_.empty()) install_ = installRoot;
    base_ = full_ ? fs::path("/") : install_;
    trashRoot_ = install_ / kReservedDir / "trash";
    opsLog_ = install_ / kReservedDir / "file-ops.jsonl";
    opsLogOld_ = install_ / kReservedDir / "file-ops.1.jsonl";
}

json FileStore::info() const {
    return json{{"full_access", full_}, {"root", full_ ? std::string("/") : utf8(install_.filename())},
                {"retention_days", kTrashRetentionSeconds / 86400}, {"max_upload_bytes", kMaxStreamUploadBytes}};
}

std::vector<std::string> FileStore::segments(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    auto flush = [&] {
        if (current.empty()) return;
        if (current == "." || current == "..") throw FileError("路径超出允许范围");
        for (unsigned char c : current) if (c < 0x20 || c == 0x7f) throw FileError("路径包含无效字符");
        parts.push_back(current);
        current.clear();
    };
    for (char c : path) {
        if (c == '/' || c == '\\') flush();
        else current += c;
    }
    flush();
    return parts;
}

std::string FileStore::joinSegments(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

fs::path FileStore::resolve(const std::string& path, bool allowRoot) const {
    const auto parts = segments(path);
    if (parts.empty()) {
        if (!allowRoot) throw FileError("不能对根目录这样操作");
#ifdef _WIN32
        if (full_) return {};   // 虚拟根：列出各个盘
#endif
        return base_;
    }
    fs::path real;
    std::size_t start = 0;
#ifdef _WIN32
    if (full_) {
        const auto& drive = parts[0];
        if (drive.size() != 2 || !std::isalpha(static_cast<unsigned char>(drive[0])) || drive[1] != ':') throw FileError("路径超出允许范围");
        real = fs::path(fromUtf8(drive + "\\"));
        start = 1;
    } else {
        real = base_;
    }
#else
    real = base_;
#endif
    for (std::size_t i = start; i < parts.size(); ++i) real /= fromUtf8(parts[i]);
    ensureInside(real);
    return real;
}

void FileStore::ensureInside(const fs::path& real) const {
    std::error_code ec;
    // 解析符号链接与目录联接之后再看：链接指向外面的，一样算越界。
    const auto canonical = fs::weakly_canonical(real, ec);
    const auto& checked = ec ? real : canonical;
    if (!full_ && !within(checked, install_)) throw FileError("路径超出了 CatOfficial 的安装目录");
    if (isReserved(checked) || isReserved(real)) throw FileError("这是 CatOfficial 的回收站与操作记录目录，不能在文件管理中修改");
}

bool FileStore::isReserved(const fs::path& real) const {
    return within(real, install_ / kReservedDir);
}

void FileStore::guard(const fs::path& real) const {
    ensureInside(real);
}

std::string FileStore::virtualPath(const fs::path& real) const {
    if (full_) {
        auto text = real.generic_u8string();
        std::string s(text.begin(), text.end());
        if (s.empty() || s[0] != '/') s = "/" + s;
        return s;
    }
    const auto relative = real.lexically_relative(install_).generic_u8string();
    std::string s(relative.begin(), relative.end());
    return s == "." ? "/" : "/" + s;
}

std::vector<Entry> FileStore::list(const std::string& path) const {
    const auto real = resolve(path);
    std::vector<Entry> entries;
#ifdef _WIN32
    if (real.empty()) {
        const DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
            if (drives & (1u << i)) entries.push_back(Entry{std::string(1, static_cast<char>('A' + i)) + ":", true, 0, ""});
        return entries;
    }
#endif
    std::error_code ec;
    if (!fs::is_directory(real, ec)) throw FileError("文件或目录不存在：" + displayPath(path));
    fs::directory_iterator it(real, fs::directory_options::skip_permission_denied, ec);
    if (ec) throw FileError("没有权限访问：" + displayPath(path));
    for (const auto& item : it) {
        if (entries.size() >= kMaxEntries) break;
        if (isReserved(item.path())) continue;
        std::error_code itemEc;
        const bool dir = item.is_directory(itemEc);
        Entry entry;
        entry.name = utf8(item.path().filename());
        entry.isDir = dir;
        entry.size = dir ? 0 : static_cast<std::uint64_t>(item.file_size(itemEc));
        if (itemEc) entry.size = 0;
        entry.modified = fileTime(item.path());
        entries.push_back(std::move(entry));
    }
    // 目录在前、同类按名字排：文件管理器不这么排就没法用
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return lower(a.name) < lower(b.name);
    });
    return entries;
}

std::string FileStore::read(const std::string& path) const {
    const auto real = resolve(path, false);
    std::error_code ec;
    const auto status = fs::symlink_status(real, ec);
    if (ec || !fs::exists(status)) throw FileError("文件或目录不存在：" + displayPath(path));
    if (!fs::is_regular_file(status)) throw FileError("不是普通文件：" + displayPath(path));
    if (fs::file_size(real, ec) > kMaxReadBytes) throw FileError("文件过大：" + displayPath(path));
    std::ifstream in(real, std::ios::binary);
    if (!in) throw FileError("没有权限访问：" + displayPath(path));
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > kMaxReadBytes) throw FileError("文件过大：" + displayPath(path));
    return data;
}

namespace {
/// 先写临时文件再改名：写到一半断了不会留下半截文件（骰子读到半截 JSON 会起不来）。
fs::path writeTemp(const fs::path& target, std::string_view data) {
    fs::create_directories(target.parent_path());
    const auto temporary = target.parent_path() / fromUtf8(".upload-" + randomHex(8));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw FileError("没有权限写入这个目录");
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out) {
            std::error_code ec;
            fs::remove(temporary, ec);
            throw FileError("写入失败，磁盘可能已满");
        }
    }
    return temporary;
}
}  // namespace

std::uint64_t FileStore::write(const std::string& path, const std::string& content) {
    if (content.size() > kMaxReadBytes) throw FileError("文件过大：" + displayPath(path));
    const auto real = resolve(path, false);
    std::error_code ec;
    if (fs::is_directory(real, ec)) throw FileError("已存在同名目录：" + displayPath(path));
    const auto temporary = writeTemp(real, content);
    fs::rename(temporary, real, ec);
    if (ec) { fs::remove(temporary, ec); throw FileError("没有权限写入：" + displayPath(path)); }
    return content.size();
}

std::pair<std::uint64_t, std::optional<TrashEntry>> FileStore::upload(const std::string& path, const fs::path& source, const std::string& by) {
    const auto real = resolve(path, false);
    std::error_code ec;
    const auto size = fs::file_size(source, ec);
    if (ec) throw FileError("上传的内容读取失败");
    if (size > kMaxStreamUploadBytes) throw FileError("文件过大：" + displayPath(path));
    fs::create_directories(real.parent_path(), ec);
    const auto temporary = real.parent_path() / fromUtf8(".upload-" + randomHex(8));
    fs::copy_file(source, temporary, fs::copy_options::overwrite_existing, ec);
    if (ec) throw FileError("没有权限写入这个目录");
    std::optional<TrashEntry> replaced;
    // 收完了才挪开旧的：传到一半断掉时，原来那份还在原处
    if (fs::exists(fs::symlink_status(real, ec))) {
        try { replaced = moveToTrash(real, by); }
        catch (...) { fs::remove(temporary, ec); throw; }
    }
    fs::rename(temporary, real, ec);
    if (ec) { fs::remove(temporary, ec); throw FileError("没有权限写入：" + displayPath(path)); }
    return {size, replaced};
}

std::optional<TrashEntry> FileStore::discard(const std::string& path, bool recursive, bool permanent, const std::string& by) {
    const auto real = resolve(path, false);
    std::error_code ec;
    const auto status = fs::symlink_status(real, ec);
    if (ec || !fs::exists(status)) throw FileError("文件或目录不存在：" + displayPath(path));
    if (fs::is_directory(status) && !recursive && !fs::is_empty(real, ec))
        throw FileError("目录不是空的：" + displayPath(path));
    if (permanent) {
        fs::remove_all(real, ec);
        if (ec) throw FileError("没有权限删除：" + displayPath(path));
        return std::nullopt;
    }
    return moveToTrash(real, by);
}

void FileStore::mkdir(const std::string& path) {
    const auto real = resolve(path, false);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(real, ec))) throw FileError("已存在：" + displayPath(path));
    fs::create_directories(real, ec);
    if (ec) throw FileError("没有权限创建：" + displayPath(path));
}

std::optional<TrashEntry> FileStore::rename(const std::string& from, const std::string& to, bool overwrite, const std::string& by) {
    const auto source = resolve(from, false);
    const auto target = resolve(to, false);
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(source, ec))) throw FileError("文件或目录不存在：" + displayPath(from));
    if (within(target, source) && pathKey(target) != pathKey(source)) throw FileError("不能把目录移动到它自己里面");
    std::optional<TrashEntry> replaced;
    if (fs::exists(fs::symlink_status(target, ec)) && pathKey(target) != pathKey(source)) {
        if (!overwrite) throw FileError("已存在：" + displayPath(to));
        replaced = moveToTrash(target, by);
    }
    fs::create_directories(target.parent_path(), ec);
    try { moveAcross(source, target); }
    catch (const fs::filesystem_error&) { throw FileError("没有权限移动：" + displayPath(from)); }
    return replaced;
}

void FileStore::copyRecursive(const fs::path& source, const fs::path& target) {
    const auto status = fs::symlink_status(source);
    // 不跟随符号链接：跟着走就可能把范围外面的东西复制进来
    if (fs::is_symlink(status)) throw FileError("不支持复制符号链接");
    if (!fs::is_directory(status)) {
        fs::create_directories(target.parent_path());
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        return;
    }
    fs::create_directories(target);
    for (const auto& item : fs::directory_iterator(source)) copyRecursive(item.path(), target / item.path().filename());
}

std::optional<TrashEntry> FileStore::copy(const std::string& from, const std::string& to, bool overwrite, const std::string& by) {
    const auto source = resolve(from, false);
    const auto target = resolve(to, false);
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(source, ec))) throw FileError("文件或目录不存在：" + displayPath(from));
    // 复制到自己内部会无限递归，直到磁盘满
    if (within(target, source)) throw FileError("不能把目录复制到它自己里面");
    std::optional<TrashEntry> replaced;
    if (fs::exists(fs::symlink_status(target, ec))) {
        if (!overwrite) throw FileError("已存在：" + displayPath(to));
        // 覆盖前先挪开旧的：用户点「覆盖」想要的是「变成新的那份」，旧的进回收站
        replaced = moveToTrash(target, by);
    }
    try { copyRecursive(source, target); }
    catch (const FileError&) { throw; }
    catch (const fs::filesystem_error&) { throw FileError("复制失败：没有权限或磁盘已满"); }
    return replaced;
}

bool FileStore::exists(const std::string& path) const {
    const auto real = resolve(path);
    std::error_code ec;
    return !real.empty() && fs::exists(fs::symlink_status(real, ec));
}

void FileStore::walk(const fs::path& real, const std::function<void(const fs::path&, bool)>& visit) const {
    const auto status = fs::symlink_status(real);
    if (fs::is_symlink(status)) return;   // 打包符号链接会把范围外的东西带进来
    const bool dir = fs::is_directory(status);
    visit(real, dir);
    if (!dir) return;
    for (const auto& item : fs::directory_iterator(real, fs::directory_options::skip_permission_denied)) {
        if (isReserved(item.path())) continue;   // 回收站与操作记录不算用户的文件
        walk(item.path(), visit);
    }
}

std::int64_t FileStore::countItems(const std::vector<std::string>& items) const {
    std::int64_t total = 0;
    for (const auto& item : items) {
        const auto real = resolve(item, false);
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(real, ec))) throw FileError("文件或目录不存在：" + displayPath(item));
        try { walk(real, [&](const fs::path&, bool) { ++total; }); }
        catch (const fs::filesystem_error&) { throw FileError("没有权限访问：" + displayPath(item)); }
    }
    return total;
}

void FileStore::compress(const std::vector<std::string>& items, const std::string& target, bool overwrite, const Progress& progress) {
    const auto destination = resolve(target, false);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(destination, ec)) && !overwrite) throw FileError("已存在：" + displayPath(target));
    std::vector<fs::path> sources;
    for (const auto& item : items) sources.push_back(resolve(item, false));
    if (progress) progress(0, "");

    // 写到临时文件再改名：中途失败不该留下一个半截的 zip，那东西看着像个正常备份，解开才发现是坏的。
    auto temporary = destination;
    temporary += ".partial";
    fs::create_directories(destination.parent_path(), ec);
    zipFile zip = openZip(temporary);
    if (!zip) throw FileError("没有权限写入：" + displayPath(target));
    auto fail = [&](const std::string& message) {
        zipClose(zip, nullptr);
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw FileError(message);
    };
    std::vector<char> buffer(256 * 1024);
    for (const auto& source : sources) {
        const auto base = source.parent_path();
        try {
            walk(source, [&](const fs::path& current, bool isDir) {
                auto relative = utf8(current.lexically_relative(base));
                std::replace(relative.begin(), relative.end(), '\\', '/');
                if (relative.empty() || relative == ".") return;
                if (pathKey(current) == pathKey(destination) || pathKey(current) == pathKey(temporary)) return;
                if (progress) progress(1, relative);
                zip_fileinfo info{};
                const std::string name = isDir ? relative + "/" : relative;
                // 置 UTF-8 标志位（第 11 位），别的解压工具就不会把中文名当本地编码读成乱码
                if (zipOpenNewFileInZip4_64(zip, name.c_str(), &info, nullptr, 0, nullptr, 0, nullptr,
                                            isDir ? 0 : Z_DEFLATED, isDir ? 0 : Z_DEFAULT_COMPRESSION, 0,
                                            -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY, nullptr, 0, 0, 1u << 11, 1) != ZIP_OK)
                    throw FileError("打包失败：无法写入 " + relative);
                if (!isDir) {
                    std::ifstream in(current, std::ios::binary);
                    if (!in) { zipCloseFileInZip(zip); throw FileError("没有权限读取：" + relative); }
                    while (in) {
                        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                        const auto count = in.gcount();
                        if (count > 0 && zipWriteInFileInZip(zip, buffer.data(), static_cast<unsigned>(count)) != ZIP_OK) {
                            zipCloseFileInZip(zip);
                            throw FileError("打包失败：写入 " + relative + " 时出错，磁盘可能已满");
                        }
                    }
                }
                zipCloseFileInZip(zip);
            });
        } catch (const FileError& e) {
            fail(e.what());
        } catch (const fs::filesystem_error&) {
            fail("打包失败：没有权限读取其中的文件");
        }
    }
    if (zipClose(zip, nullptr) != ZIP_OK) { fs::remove(temporary, ec); throw FileError("打包失败：无法完成压缩包"); }
    fs::rename(temporary, destination, ec);
    if (ec) { fs::remove(temporary, ec); throw FileError("没有权限写入：" + displayPath(target)); }
}

std::int64_t FileStore::countEntries(const std::string& archive) const {
    const auto source = resolve(archive, false);
    unzFile zip = openUnzip(source);
    if (!zip) throw FileError("不是有效的 zip 压缩包：" + displayPath(archive));
    unz_global_info64 info{};
    unzGetGlobalInfo64(zip, &info);
    unzClose(zip);
    return static_cast<std::int64_t>(info.number_entry);
}

std::vector<std::string> FileStore::peekConflicts(const std::string& archive, const std::string& target, const std::string& encoding, std::size_t limit) const {
    const auto source = resolve(archive, false);
    const auto destination = resolve(target);
    unzFile zip = openUnzip(source);
    if (!zip) throw FileError("不是有效的 zip 压缩包：" + displayPath(archive));
    std::vector<std::string> conflicts;
    for (int status = unzGoToFirstFile(zip); status == UNZ_OK && conflicts.size() < limit; status = unzGoToNextFile(zip)) {
        const auto entry = currentEntry(zip, encoding);
        if (entry.isDir) continue;
        const auto cleaned = cleanEntryName(entry.name);
        if (cleaned.empty()) continue;
        std::error_code ec;
        if (fs::exists(fs::symlink_status(destination / fromUtf8(cleaned), ec))) conflicts.push_back(entry.name);
    }
    unzClose(zip);
    return conflicts;
}

void FileStore::extract(const std::string& archive, const std::string& target, const std::string& encoding, bool overwrite, const Progress& progress) {
    const auto source = resolve(archive, false);
    const auto destination = resolve(target, false);
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) throw FileError("没有权限创建：" + displayPath(target));
    unzFile zip = openUnzip(source);
    if (!zip) throw FileError("不是有效的 zip 压缩包：" + displayPath(archive));
    struct Closer { unzFile z; ~Closer() { unzClose(z); } } closer{zip};
    if (progress) progress(0, "");
    std::uint64_t total = 0;
    std::vector<char> buffer(256 * 1024);
    for (int status = unzGoToFirstFile(zip); status == UNZ_OK; status = unzGoToNextFile(zip)) {
        const auto entry = currentEntry(zip, encoding);
        const auto cleaned = cleanEntryName(entry.name);
        if (cleaned.empty()) continue;
        const auto out = destination / fromUtf8(cleaned);
        // 解析后的真实路径仍要在目标目录里（也挡住范围外的链接）
        std::error_code canonicalEc;
        const auto real = fs::weakly_canonical(out, canonicalEc);
        if (!within(canonicalEc ? out : real, fs::weakly_canonical(destination, canonicalEc)))
            throw FileError("压缩包里有越界路径「" + entry.name + "」，已中止");
        ensureInside(out);
        if (progress) progress(1, entry.name);
        if (entry.isDir) {
            fs::create_directories(out, ec);
            continue;
        }
        if (fs::exists(fs::symlink_status(out, ec)) && !overwrite) throw FileError("已存在：" + entry.name);
        // zip 炸弹：几十 KB 的包能解出几十 GB
        total += entry.size;
        if (total > kMaxExtractBytes) throw FileError("解压后体积超过 " + std::to_string(kMaxExtractBytes >> 20) + " MB，已中止");
        fs::create_directories(out.parent_path(), ec);
        if (unzOpenCurrentFile(zip) != UNZ_OK) throw FileError("压缩包损坏，无法读取「" + entry.name + "」");
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        if (!file) { unzCloseCurrentFile(zip); throw FileError("没有权限写入：" + entry.name); }
        std::uint64_t written = 0;
        int read = 0;
        while ((read = unzReadCurrentFile(zip, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
            written += static_cast<std::uint64_t>(read);
            if (written > kMaxExtractBytes) { unzCloseCurrentFile(zip); throw FileError("解压后体积超过限制，已中止"); }
            file.write(buffer.data(), read);
        }
        unzCloseCurrentFile(zip);
        if (read < 0) throw FileError("压缩包损坏，「" + entry.name + "」解压失败");
        if (!file) throw FileError("写入「" + entry.name + "」失败，磁盘可能已满");
    }
}

std::pair<fs::path, std::string> FileStore::fileForDownload(const std::string& path) const {
    const auto real = resolve(path, false);
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(real, ec))) throw FileError("不是普通文件：" + displayPath(path));
    return {real, utf8(real.filename())};
}

// ─── 回收站 ─────────────────────────────────────────────────

std::int64_t FileStore::measure(const fs::path& real) {
    std::int64_t total = 0;
    std::size_t seen = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(real, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) return -1;
        // QQ_DATA 这类目录动辄几十万个文件，量完再挪会让删除卡住
        if (++seen > 200000) return -1;
        std::error_code itemEc;
        if (it->is_regular_file(itemEc)) total += static_cast<std::int64_t>(it->file_size(itemEc));
    }
    return total;
}

TrashEntry FileStore::moveToTrash(const fs::path& real, const std::string& by) {
    std::error_code ec;
    const auto status = fs::symlink_status(real, ec);
    if (ec || !fs::exists(status)) throw FileError("文件或目录不存在");
    TrashEntry entry;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", &tm);
    entry.id = std::string(stamp) + "-" + randomHex(4);
    entry.path = virtualPath(real);
    entry.isDir = fs::is_directory(status);
    entry.size = entry.isDir ? measure(real) : static_cast<std::int64_t>(fs::file_size(real, ec));
    entry.deletedAt = nowRfc3339();
    entry.expiresAt = nowRfc3339(kTrashRetentionSeconds);
    entry.deletedBy = by;
    const auto folder = trashRoot_ / entry.id;
    fs::create_directories(folder, ec);
    if (ec) throw FileError("无法创建回收站目录");
    {
        std::ofstream meta(folder / "meta.json", std::ios::binary | std::ios::trunc);
        meta << entry.toJson().dump();
        if (!meta) { fs::remove_all(folder, ec); throw FileError("无法写入回收站记录"); }
    }
    try {
        moveAcross(real, folder / "item");
    } catch (const fs::filesystem_error&) {
        fs::remove_all(folder, ec);
        throw FileError("无法移入回收站：没有权限或文件正在使用");
    }
    return entry;
}

TrashEntry FileStore::readTrashEntry(const std::string& id) const {
    if (!std::regex_match(id, trashIdPattern())) throw FileError("回收站里没有这一项，可能已过期或已被删除");
    std::ifstream in(trashRoot_ / id / "meta.json", std::ios::binary);
    if (!in) throw FileError("回收站里没有这一项，可能已过期或已被删除");
    const auto j = json::parse(in, nullptr, false);
    if (!j.is_object()) throw FileError("回收站条目 " + id + " 的记录损坏");
    TrashEntry entry;
    entry.id = id;
    entry.path = j.value("path", std::string());
    entry.isDir = j.value("is_dir", false);
    entry.size = j.value("size", std::int64_t(0));
    entry.deletedAt = j.value("deleted_at", std::string());
    entry.expiresAt = j.value("expires_at", std::string());
    entry.deletedBy = j.value("deleted_by", std::string());
    return entry;
}

std::vector<TrashEntry> FileStore::trashList() const {
    std::vector<TrashEntry> entries;
    std::error_code ec;
    if (!fs::is_directory(trashRoot_, ec)) return entries;
    for (const auto& item : fs::directory_iterator(trashRoot_, ec)) {
        const auto id = utf8(item.path().filename());
        if (!item.is_directory() || !std::regex_match(id, trashIdPattern())) continue;
        try {
            entries.push_back(readTrashEntry(id));
        } catch (const FileError&) {
            // 记录坏了也要列出来，不然它就永远占着地方、谁也删不掉
            TrashEntry broken;
            broken.id = id;
            broken.path = "（记录损坏）";
            broken.size = -1;
            entries.push_back(broken);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const TrashEntry& a, const TrashEntry& b) { return a.id > b.id; });
    return entries;
}

TrashEntry FileStore::trashRestore(const std::string& id, const std::string& by, bool overwrite) {
    auto entry = readTrashEntry(id);
    const auto target = resolve(entry.path, false);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(target, ec))) {
        if (!overwrite) throw FileError("原位置已有同名项目：" + entry.path, 409);
        moveToTrash(target, by);
    }
    fs::create_directories(target.parent_path(), ec);
    try { moveAcross(trashRoot_ / id / "item", target); }
    catch (const fs::filesystem_error&) { throw FileError("没有权限还原到：" + entry.path); }
    fs::remove_all(trashRoot_ / id, ec);
    return entry;
}

std::vector<TrashEntry> FileStore::trashDelete(const std::vector<std::string>& ids) {
    std::vector<TrashEntry> removed;
    for (const auto& id : ids) {
        if (!std::regex_match(id, trashIdPattern())) throw FileError("回收站里没有这一项，可能已过期或已被删除");
        TrashEntry entry;
        try { entry = readTrashEntry(id); } catch (const FileError&) { entry.id = id; }
        std::error_code ec;
        fs::remove_all(trashRoot_ / id, ec);
        if (ec) throw FileError("删除回收站条目失败：文件可能正在使用");
        removed.push_back(entry);
    }
    return removed;
}

std::vector<TrashEntry> FileStore::trashEmpty() {
    auto entries = trashList();
    for (const auto& entry : entries) {
        std::error_code ec;
        fs::remove_all(trashRoot_ / entry.id, ec);
    }
    return entries;
}

int FileStore::purgeExpiredTrash() {
    int removed = 0;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    for (const auto& entry : trashList()) {
        const auto expires = parseRfc3339(entry.expiresAt);
        if (expires <= 0 || now < expires) continue;
        std::error_code ec;
        fs::remove_all(trashRoot_ / entry.id, ec);
        if (ec) continue;
        ++removed;
        OpRecord op;
        op.action = "trash-expire";
        op.path = entry.path;
        op.trashId = entry.id;
        op.actor = "system";
        record(op);
    }
    return removed;
}

// ─── 操作记录 ───────────────────────────────────────────────

void FileStore::record(OpRecord op) {
    if (op.time.empty()) op.time = nowRfc3339();
    std::lock_guard lock(opsMu_);
    std::error_code ec;
    fs::create_directories(opsLog_.parent_path(), ec);
    // 超过 2 MB 换一份：留当前和上一份，够看几个月
    if (fs::exists(opsLog_, ec) && fs::file_size(opsLog_, ec) >= (2u << 20)) {
        fs::remove(opsLogOld_, ec);
        fs::rename(opsLog_, opsLogOld_, ec);
    }
    std::ofstream out(opsLog_, std::ios::binary | std::ios::app);
    out << op.toJson().dump() << '\n';
}

std::vector<OpRecord> FileStore::readOps(std::size_t limit) const {
    std::vector<OpRecord> all;
    std::lock_guard lock(opsMu_);
    for (const auto& file : {opsLogOld_, opsLog_}) {
        std::ifstream in(file, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const auto j = json::parse(line, nullptr, false);
            if (!j.is_object()) continue;
            OpRecord op;
            op.time = j.value("t", std::string());
            op.action = j.value("action", std::string());
            op.path = j.value("path", std::string());
            op.target = j.value("target", std::string());
            op.count = j.value("count", 0);
            op.bytes = j.value("bytes", std::int64_t(0));
            op.actor = j.value("actor", std::string());
            op.ip = j.value("ip", std::string());
            op.trashId = j.value("trash_id", std::string());
            op.permanent = j.value("permanent", false);
            all.push_back(std::move(op));
        }
    }
    std::vector<OpRecord> out;
    for (auto it = all.rbegin(); it != all.rend() && out.size() < limit; ++it) out.push_back(*it);
    return out;
}

// ─── 后台任务 ───────────────────────────────────────────────

std::shared_ptr<Job> FileStore::startJob(const std::string& kind, std::function<void(Job&)> work) {
    auto job = std::make_shared<Job>(randomHex(8), kind);
    {
        std::lock_guard lock(jobsMu_);
        for (auto it = jobs_.begin(); it != jobs_.end();) it = it->second->expired() ? jobs_.erase(it) : std::next(it);
        jobs_[job->id()] = job;
    }
    std::thread([job, work = std::move(work)] {
        try {
            work(*job);
            job->finish("");
        } catch (const FileError& e) {
            job->finish(e.what());
        } catch (const fs::filesystem_error&) {
            job->finish("操作失败：没有权限或磁盘已满");
        } catch (const std::exception& e) {
            job->finish(e.what());
        }
    }).detach();
    return job;
}

std::shared_ptr<Job> FileStore::job(const std::string& id) {
    std::lock_guard lock(jobsMu_);
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second;
}

// ─── 面板入口 ───────────────────────────────────────────────

json handleAction(FileStore& store, const json& body, const std::string& actor, const std::string& ip) {
    const auto action = body.value("action", std::string());
    const auto path = body.value("path", std::string());
    const auto target = body.value("target", std::string());
    const bool overwrite = body.value("overwrite", false);
    auto op = [&](OpRecord record) {
        record.actor = actor;
        record.ip = ip;
        store.record(std::move(record));
    };
    auto trashJson = [](const std::optional<TrashEntry>& entry) { return entry ? entry->toJson() : json(nullptr); };
    try {
        if (action == "list") {
            json entries = json::array();
            for (const auto& e : store.list(path))
                entries.push_back(json{{"name", e.name}, {"is_dir", e.isDir}, {"size", e.size}, {"modified", e.modified}});
            return json{{"entries", entries}, {"path", path}, {"full_access", store.fullAccess()}};
        }
        if (action == "read") {
            const auto data = store.read(path);
            return json{{"content_base64", drogon::utils::base64Encode(data)}, {"size", data.size()}};
        }
        if (action == "write") {
            const auto content = drogon::utils::base64Decode(body.value("content_base64", std::string()));
            const auto written = store.write(path, content);
            op(OpRecord{{}, "write", path, {}, {}, {}, {}, 0, static_cast<std::int64_t>(written)});
            return json{{"written", written}};
        }
        if (action == "delete") {
            const bool permanent = body.value("permanent", false);
            const auto entry = store.discard(path, body.value("recursive", false), permanent, actor);
            OpRecord record{{}, "delete", path};
            record.permanent = permanent;
            if (entry) { record.trashId = entry->id; record.bytes = entry->size; }
            op(record);
            return json{{"deleted", true}, {"trash", trashJson(entry)}};
        }
        if (action == "mkdir") {
            store.mkdir(path);
            op(OpRecord{{}, "mkdir", path});
            return json{{"created", true}};
        }
        if (action == "rename" || action == "copy") {
            const auto replaced = action == "rename" ? store.rename(path, target, overwrite, actor) : store.copy(path, target, overwrite, actor);
            OpRecord record{{}, action, path, target};
            if (replaced) record.trashId = replaced->id;
            op(record);
            return json{{action == "rename" ? "renamed" : "copied", true}, {"replaced", trashJson(replaced)}};
        }
        if (action == "trash-list") {
            json entries = json::array();
            for (const auto& e : store.trashList()) entries.push_back(e.toJson());
            return json{{"entries", entries}, {"retention_days", kTrashRetentionSeconds / 86400}};
        }
        if (action == "trash-restore") {
            const auto ids = body.value("ids", std::vector<std::string>{});
            if (ids.size() != 1) throw FileError("一次还原一项");
            try {
                const auto entry = store.trashRestore(ids[0], actor, overwrite);
                op(OpRecord{{}, "trash-restore", entry.path, {}, {}, {}, entry.id});
                return json{{"restored", entry.toJson()}};
            } catch (const FileError& e) {
                if (e.status == 409) {
                    const std::string message = e.what();
                    throw FileError(message, 409);
                }
                throw;
            }
        }
        if (action == "trash-delete" || action == "trash-empty") {
            std::vector<TrashEntry> removed;
            if (action == "trash-empty") {
                removed = store.trashEmpty();
            } else {
                const auto ids = body.value("ids", std::vector<std::string>{});
                if (ids.empty() || ids.size() > 200) throw FileError("没有选中要删除的项目");
                removed = store.trashDelete(ids);
            }
            for (const auto& e : removed) {
                OpRecord record{{}, action, e.path, {}, {}, {}, e.id, 0, e.size};
                record.permanent = true;
                op(record);
            }
            return json{{"removed", removed.size()}};
        }
        if (action == "ops") {
            auto limit = body.value("limit", 200);
            if (limit <= 0 || limit > 500) limit = 500;
            json records = json::array();
            for (const auto& r : store.readOps(static_cast<std::size_t>(limit))) records.push_back(r.toJson());
            return json{{"records", records}};
        }
        if (action == "compress") {
            auto items = body.value("items", std::vector<std::string>{});
            if (items.empty() && !path.empty()) items.push_back(path);
            if (items.empty()) throw FileError("没有要打包的内容");
            // 先数一遍，进度条才有分母；打包几百 MB 要几分钟，所以立刻返回任务，进度另外查。
            const auto count = store.countItems(items);
            FileStore* s = &store;
            const auto job = store.startJob("compress", [s, items, target, overwrite, count](Job& j) {
                j.setTotal(count);
                s->compress(items, target, overwrite, [&j](std::int64_t delta, const std::string& name) { j.advance(delta, name); });
            });
            OpRecord record{{}, "compress", {}, target};
            record.count = static_cast<int>(items.size());
            op(record);
            return json{{"job", job->snapshot()}};
        }
        if (action == "extract") {
            const auto total = store.countEntries(path);
            const auto encoding = body.value("encoding", std::string("utf8"));
            FileStore* s = &store;
            const auto job = store.startJob("extract", [s, path, target, encoding, overwrite, total](Job& j) {
                j.setTotal(total);
                s->extract(path, target, encoding, overwrite, [&j](std::int64_t delta, const std::string& name) { j.advance(delta, name); });
            });
            OpRecord record{{}, "extract", path, target};
            record.count = static_cast<int>(total);
            op(record);
            return json{{"job", job->snapshot()}};
        }
        if (action == "peek") {
            // 动手之前先问「会撞到什么」，让界面能提示是否覆盖
            std::string lowerPath = path;
            std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lowerPath.size() > 4 && lowerPath.substr(lowerPath.size() - 4) == ".zip" && !target.empty())
                return json{{"conflicts", store.peekConflicts(path, target, body.value("encoding", std::string("utf8")), 20)}};
            json conflicts = json::array();
            if (store.exists(target)) conflicts.push_back(target);
            return json{{"conflicts", conflicts}};
        }
    } catch (const FileError&) {
        throw;
    } catch (const fs::filesystem_error& e) {
        const auto code = e.code();
        if (code == std::errc::no_such_file_or_directory) throw FileError("文件或目录不存在：" + displayPath(path));
        if (code == std::errc::permission_denied) throw FileError("没有权限访问：" + displayPath(path));
        if (code == std::errc::file_exists) throw FileError("已存在：" + displayPath(target.empty() ? path : target));
        // 系统给的错误文字可能是本地编码、还带绝对路径，不往外传
        throw FileError("操作失败（错误代码 " + std::to_string(code.value()) + "）", 500);
    }
    throw FileError("未知的操作");
}

}  // namespace catofficial::app::files
