#pragma once
// ─── CatOfficial — 开机自启动（Windows 当前用户）────────────────
// 通过 HKCU\Software\Microsoft\Windows\CurrentVersion\Run 写/删 exe 路径。
// 注：exe 启动时会 chdir 到自身目录（见 main.cpp），故从 Run 启动也能找到 data/。
// Windows 实现 + 其它平台 no-op。

#include <string>

#if defined(_WIN32)
#include <windows.h>

namespace catofficial::app {

inline const wchar_t* autostartName() { return L"CatOfficial"; }

/// 开机自启动登记的程序：发行包里是安装目录的启动器 catofficial.exe（它会先安装待装的更新再拉起 app\ 里的 core），
/// 开发时直接运行 core 则登记 core 自己。
inline std::wstring exePathW() {
    wchar_t buf[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring self(buf, n);
    const auto slash = self.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const auto dir = self.substr(0, slash);
        const auto parentSlash = dir.find_last_of(L"\\/");
        if (parentSlash != std::wstring::npos && _wcsicmp(dir.c_str() + parentSlash + 1, L"app") == 0) {
            const auto launcher = dir.substr(0, parentSlash) + L"\\catofficial.exe";
            if (GetFileAttributesW(launcher.c_str()) != INVALID_FILE_ATTRIBUTES) return launcher;
        }
    }
    return self;
}

inline bool isAutostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, sz = 0;
    bool ok = RegQueryValueExW(k, autostartName(), nullptr, &type, nullptr, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

inline bool setAutostart(bool enable) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok;
    if (enable) {
        std::wstring p = L"\"" + exePathW() + L"\"";
        ok = RegSetValueExW(k, autostartName(), 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(p.c_str()),
                            static_cast<DWORD>((p.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LONG r = RegDeleteValueW(k, autostartName());
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(k);
    return ok;
}

}  // namespace catofficial::app

#else
namespace catofficial::app {
inline bool isAutostartEnabled() { return false; }
inline bool setAutostart(bool) { return false; }
}
#endif
