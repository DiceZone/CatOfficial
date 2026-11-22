// CatOfficial 的 Windows 启动器（移植自 Dice!Next 的 dice-next.exe）。
// 只链接 Win32 库、静态链接运行库（/MT），不依赖 app\ 里的任何 DLL：
// 处理待安装的更新 → 拉起 app\catofficial-core.exe → 立刻退出，常驻的只有 core 一个进程。
// core 要重启或安装更新时以「catofficial.exe --after <自己的 PID>」拉起本程序后退出，
// 本程序等那个进程真正消失（端口与单实例锁随之释放）后再替换文件、拉起新 core。
#include <windows.h>
#include <shellapi.h>
#include "../platform/startup_guard.h"
#include "../platform/update_schedule.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {

std::wstring executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return fs::path(std::wstring(buffer.data(), length)).parent_path().wstring();
}

std::wstring quoteArg(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    size_t slashCount = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashCount; continue; }
        if (ch == L'\"') out.append(slashCount * 2 + 1, L'\\');
        else out.append(slashCount, L'\\');
        out += ch;
        slashCount = 0;
    }
    out.append(slashCount * 2, L'\\');
    return out + L"\"";
}

std::wstring pathValue(const std::wstring& name) {
    DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    GetEnvironmentVariableW(name.c_str(), value.data(), needed);
    value.resize(needed - 1);
    return value;
}

bool noRestartRequested() {
    std::wstring value = pathValue(L"CATOFFICIAL_UPDATE_RESTART");
    std::transform(value.begin(), value.end(), value.begin(), towupper);
    return value == L"NO";
}

bool environmentMarkerPresent(const std::wstring& name) {
    std::wstring value = pathValue(name);
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    });
    if (first == value.end()) return false;
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }).base();
    value.assign(first, last);
    std::transform(value.begin(), value.end(), value.begin(), towupper);
    return value != L"0" && value != L"FALSE" && value != L"NO" &&
        value != L"OFF" && value != L"HOST" && value != L"BAREMETAL";
}

bool runningInContainer() {
    return environmentMarkerPresent(L"CATOFFICIAL_CONTAINER") ||
        environmentMarkerPresent(L"CONTAINER_SANDBOX_MOUNT_POINT") ||
        environmentMarkerPresent(L"KUBERNETES_SERVICE_HOST") ||
        environmentMarkerPresent(L"DOTNET_RUNNING_IN_CONTAINER") ||
        environmentMarkerPresent(L"container");
}

std::wstring wideError(const std::error_code& error) {
    const std::string message = error.message();
    return std::wstring(message.begin(), message.end());
}

bool moveTree(const fs::path& source, const fs::path& destination, const fs::path& rollback, std::wstring& error) {
    std::error_code ec;
    if (!fs::exists(source, ec)) return true;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) { error = wideError(ec); return false; }
    if (fs::exists(destination, ec)) {
        fs::create_directories(rollback.parent_path(), ec);
        if (ec) { error = wideError(ec); return false; }
        fs::rename(destination, rollback, ec);
        if (ec) { error = wideError(ec); return false; }
    }
    fs::rename(source, destination, ec);
    if (!ec) return true;
    ec.clear();
    if (fs::is_directory(source, ec)) {
        fs::copy(source, destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove_all(source, ec);
    } else {
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove(source, ec);
    }
    if (ec) { error = wideError(ec); return false; }
    return true;
}

std::wstring timestamp() {
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t out[32]{};
    swprintf_s(out, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return out;
}

std::string readUpdateMetadata(const fs::path& stage) {
    std::ifstream input(stage / L"update.json", std::ios::binary);
    if (!input) return "{}";
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.empty() || text.size() > 64 * 1024 ||
        text.find('{') == std::string::npos || text.rfind('}') == std::string::npos) {
        return "{}";
    }
    return text;
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch >= 0x20) {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

void writeUpdateResult(const fs::path& root, const std::string& metadata,
                       bool success, const std::string& message) {
    const fs::path updates = root / L"updates";
    const fs::path result = updates / L"last-result.json";
    const fs::path temporary = updates / L"last-result.tmp";
    std::error_code ec;
    fs::create_directories(updates, ec);
    if (ec) return;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return;
        output << "{\"schema\":1,\"success\":" << (success ? "true" : "false")
               << ",\"metadata\":" << (metadata.empty() ? "{}" : metadata)
               << ",\"message\":\"" << jsonEscape(message) << "\"}\n";
        if (!output) return;
    }
    fs::remove(result, ec);
    ec.clear();
    fs::rename(temporary, result, ec);
    if (ec) fs::remove(temporary, ec);
}

struct UpdateMove {
    fs::path destination;
    fs::path backup;
    bool hadExisting = false;
};

bool replaceUpdatePath(const fs::path& source, const fs::path& destination,
                       const fs::path& backup, std::vector<UpdateMove>& moves,
                       std::wstring& error) {
    std::error_code ec;
    if (!fs::exists(source, ec)) return true;
    UpdateMove record{destination, backup, fs::exists(destination, ec)};
    moves.push_back(record);
    return moveTree(source, destination, backup, error);
}

bool rollbackUpdateMoves(std::vector<UpdateMove>& moves) {
    bool ok = true;
    std::error_code ec;
    for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
        fs::remove_all(it->destination, ec);
        ec.clear();
        if (!it->hadExisting || !fs::exists(it->backup, ec)) continue;
        std::wstring error;
        const fs::path conflict = fs::path(it->backup.wstring() + L".restore-conflict");
        if (!moveTree(it->backup, it->destination, conflict, error)) {
            std::wcerr << L"CatOfficial rollback failed for " << it->destination.wstring()
                       << L": " << error << L"\n";
            ok = false;
        }
    }
    return ok;
}

bool applyUpdate(const fs::path& root, const fs::path& stage) {
    std::error_code ec;
    if (!fs::is_regular_file(stage / L"app" / L"catofficial-core.exe", ec) ||
        !fs::is_regular_file(stage / L"catofficial.exe", ec) ||
        !fs::is_regular_file(stage / L"web" / L"index.html", ec)) {
        std::wcerr << L"CatOfficial update failed: staged package is incomplete.\n";
        return false;
    }

    const fs::path rollback = root / L"updates" / L"rollbacks" / timestamp();
    std::vector<UpdateMove> moves;
    std::wstring error;
    auto replace = [&](const fs::path& source, const fs::path& destination,
                       const fs::path& backup) {
        return replaceUpdatePath(source, destination, backup, moves, error);
    };
    auto fail = [&](const std::wstring& item) {
        std::wcerr << L"CatOfficial update failed while replacing " << item << L": "
                   << error << L"\n";
        if (rollbackUpdateMoves(moves))
            std::wcerr << L"CatOfficial restored the previous version automatically.\n";
        else
            std::wcerr << L"CatOfficial rollback was incomplete; keep " << rollback.wstring()
                       << L" for manual recovery.\n";
        return false;
    };

    // 只替换程序文件；config.json 与 data\ 不在更新包里，永远不碰。
    for (const wchar_t* item : {L"app", L"web"}) {
        if (!replace(stage / item, root / item, rollback / item)) return fail(item);
    }
    for (const wchar_t* file : {L"catofficial.exe", L"update-mirrors.json", L"LICENSE", L"README.md",
                                L"使用说明.txt"}) {
        if (!replace(stage / file, root / file, rollback / file)) return fail(file);
    }

    fs::remove_all(stage, ec);
    std::wcout << L"CatOfficial update applied. Rollback: " << rollback.wstring() << L"\n";
    return true;
}

// Waits until a process is really gone.  The port and the single-instance lock
// are released only then, so a restart has to hold off until it happens.
void waitForProcessExit(DWORD processId) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) return;
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
}

// Starts the core and returns immediately.  This program is a launcher, not a
// supervisor: it prepares the directory, hands off and exits, so CatOfficial
// leaves exactly one process resident.  Every dependency now sits next to the
// core inside app\, the first directory Windows searches for an executable's
// imports, so there is no DLL search path to arrange here either.
bool launchCore(const fs::path& root, const std::vector<std::wstring>& args) {
    const fs::path core = root / L"app" / L"catofficial-core.exe";
    if (!fs::is_regular_file(core)) {
        std::wcerr << L"CatOfficial core is missing: " << core.wstring() << L"\n";
        return false;
    }
    // The core reads this to decide whether a staged update can be applied.
    SetEnvironmentVariableW(L"CATOFFICIAL_MANAGED", L"1");
    SetCurrentDirectoryW(root.c_str());

    std::wstring commandLine = quoteArg(core.wstring());
    for (const auto& arg : args) commandLine += L" " + quoteArg(arg);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(core.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        root.c_str(), &startup, &process)) {
        std::wcerr << L"CatOfficial could not start core (Win32 error " << GetLastError() << L").\n";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// Applies a staged update if one is waiting.  It runs here, in the launcher,
// with no core alive and nothing to co-ordinate with, so every file can be
// replaced — catofficial.exe included: Windows refuses to overwrite a running
// image but allows renaming it, which is what moveTree does.
// Returns whether an update was present.
bool applyPendingUpdate(const fs::path& root) {
    const fs::path stage = root / L"updates" / L"pending";
    std::error_code ec;
    if (!fs::is_directory(stage, ec)) return false;
    if (!catofficial::app::update::pendingUpdateMayApply(stage)) return false;
    if (runningInContainer()) {
        std::wcerr << L"CatOfficial: a container environment was detected; the staged "
                      L"program update was not applied. Pull a new image and recreate "
                      L"the container instead.\n";
        return false;
    }
    const std::string metadata = readUpdateMetadata(stage);
    const bool applied = applyUpdate(root, stage);
    writeUpdateResult(root, metadata, applied,
        applied ? "更新文件已成功应用" : "更新文件应用失败，已尝试回滚到原版本");
    return true;
}

int runLauncher(const fs::path& root, const std::vector<std::wstring>& args, DWORD waitFor) {
    if (waitFor != 0) waitForProcessExit(waitFor);
    const bool updated = applyPendingUpdate(root);
    if (updated && noRestartRequested()) {
        std::wcout << L"CatOfficial update finished; CATOFFICIAL_UPDATE_RESTART=NO prevents core start.\n";
        return 0;
    }
    return launchCore(root, args) ? 0 : 1;
}

} // namespace

int wmain() {
    int argc = 0;
    LPWSTR* rawArgs = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!rawArgs) return 1;
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(rawArgs[i]);
    LocalFree(rawArgs);

    const fs::path root = executableDirectory();
    if (root.empty()) return 1;
    // Before restoring/updating or loading the core (an archive preview may contain only this EXE).
    if (!catofficial::app::startup::allowLaunch(root.wstring(), root.wstring())) return 2;

    // "--after <pid>": the core is shutting down for a restart, or to let a
    // staged update be applied.  Anything else is forwarded to the core.
    DWORD waitFor = 0;
    if (args.size() >= 2 && args[0] == L"--after") {
        try {
            waitFor = static_cast<DWORD>(std::stoul(args[1]));
        } catch (...) {
            waitFor = 0;
        }
        args.erase(args.begin(), args.begin() + 2);
    }
    return runLauncher(root, args, waitFor);
}
