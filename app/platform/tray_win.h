#pragma once
// ─── CatOfficial — Windows 系统托盘 ─────────────────────────────
// 与 Dice!Next 相同的托盘图标与右键菜单：打开应用目录、显示/隐藏控制台、打开网页面板、
// 开机自启动、退出。托盘在自己的线程上运行：一个不显示的顶层窗口加消息循环（Shell_NotifyIcon）。
// 窗口不能是 message-only，因为要收到资源管理器重启时广播的「TaskbarCreated」。
// 只在 Windows 上生效，其他平台是空实现。

#include <cstdint>
#include <functional>
#include <string>
#include "tray_settings.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <mutex>
#include "autostart_win.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")   // 注册表（开机自启）

namespace catofficial::app {

namespace tray_detail {
    inline std::function<void()> g_onExit;   // 窗口过程不能捕获，放在这里
    inline uint16_t g_port = 18082;
    inline bool g_consoleVisible = true;

    constexpr UINT WM_TRAY = WM_USER + 1;
    constexpr UINT WM_TRAY_TEXT = WM_USER + 2;
    inline std::mutex g_textMutex;
    inline std::string g_text;
    inline HWND g_window = nullptr; // 受 g_textMutex 保护；g_nid 只在托盘线程上使用
    enum { ID_DIR = 1, ID_CONSOLE, ID_WEB, ID_AUTOSTART, ID_EXIT };

    // 资源管理器重启（崩溃、更新、DPI 变化）时会清掉所有托盘图标，再广播「TaskbarCreated」
    // 让各程序自己加回来。不响应的话图标就此消失，程序却还在运行，看起来像是退出了。
    inline UINT g_taskbarCreated = 0;
    inline NOTIFYICONDATAW g_nid{};

    inline std::wstring wideTooltip() { // 调用方持有 g_textMutex
        const auto utf8 = tray_settings::tooltip(g_text, g_port);
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(size), L'\0');
        if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
        return result;
    }

    inline LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_TRAY_TEXT) {
            std::lock_guard<std::mutex> lock(g_textMutex);
            const auto tip = wideTooltip();
            wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
            NOTIFYICONDATAW update = g_nid;
            update.uFlags = NIF_TIP;
            Shell_NotifyIconW(NIM_MODIFY, &update);
            return 0;
        }
        if (g_taskbarCreated != 0 && msg == g_taskbarCreated) {
            Shell_NotifyIconW(NIM_ADD, &g_nid);   // 已存在时返回 FALSE，无妨
            return 0;
        }
        if (msg == WM_CLOSE) {   // 程序自己退出（面板重启等）：结束消息循环，移除图标
            PostQuitMessage(0);
            return 0;
        }
        if (msg == WM_TRAY) {
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                POINT pt; GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, ID_DIR,     L"打开应用目录");
                // 文字表示点下去会做什么：正在显示 →「隐藏控制台」，已隐藏 →「显示控制台」。
                AppendMenuW(menu, MF_STRING, ID_CONSOLE, g_consoleVisible ? L"隐藏控制台" : L"显示控制台");
                AppendMenuW(menu, MF_STRING, ID_WEB,     L"打开网页面板");
                // 开机自启动：勾选反映当前状态，点击切换。
                AppendMenuW(menu, MF_STRING | (isAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                            ID_AUTOSTART, L"开机自启动");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_EXIT,    L"退出");
                SetForegroundWindow(hWnd);   // 点别处时菜单才会自动关闭
                int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(menu);
                switch (cmd) {
                    case ID_DIR: {
                        wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
                        std::wstring p(buf); auto s = p.find_last_of(L"\\/");
                        if (s != std::wstring::npos) p = p.substr(0, s);
                        ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        break;
                    }
                    case ID_CONSOLE: {
                        HWND con = GetConsoleWindow();
                        if (con) { g_consoleVisible = !g_consoleVisible; ShowWindow(con, g_consoleVisible ? SW_SHOW : SW_HIDE); }
                        break;
                    }
                    case ID_WEB: {
                        std::wstring url = L"http://localhost:" + std::to_wstring(g_port) + L"/";
                        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        break;
                    }
                    case ID_AUTOSTART:
                        setAutostart(!isAutostartEnabled());   // 切换开机自启
                        break;
                    case ID_EXIT:
                        if (g_onExit) g_onExit();
                        PostQuitMessage(0);
                        break;
                }
            }
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
} // namespace tray_detail

inline void updateSystemTrayText(const std::string& text) {
    std::string normalized, error;
    if (!tray_settings::normalize(text, normalized, error)) return;
    std::lock_guard<std::mutex> lock(tray_detail::g_textMutex);
    tray_detail::g_text = std::move(normalized);
    if (tray_detail::g_window) PostMessageW(tray_detail::g_window, tray_detail::WM_TRAY_TEXT, 0, 0);
}

/// 在后台线程启动托盘图标。用户选「退出」时调用 @p onExit（接到正常停止服务）。
/// @p startHidden：启动后把控制台窗口隐藏到托盘并禁用其关闭按钮，弹气泡告知「未退出，已最小化到托盘」。
inline void startSystemTray(uint16_t port, std::function<void()> onExit, bool startHidden = false, const std::string& text = "") {
    tray_detail::g_onExit = std::move(onExit);
    tray_detail::g_port = port;
    updateSystemTrayText(text);
    std::thread([startHidden] {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = tray_detail::WndProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"CatOfficialTray";
        RegisterClassW(&wc);
        // 在建窗口之前注册，不会漏掉广播；每个进程取到的值都相同。
        tray_detail::g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

        // 不显示的顶层窗口。不能用 message-only（HWND_MESSAGE）：那种窗口收不到广播，
        // 「TaskbarCreated」又是广播，图标就回不来了。WS_EX_TOOLWINDOW 让它不进任务栏。
        HWND hWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"CatOfficial",
                                    WS_OVERLAPPED, 0, 0, 0, 0,
                                    nullptr, nullptr, inst, nullptr);
        if (!hWnd) return;

        NOTIFYICONDATAW& nid = tray_detail::g_nid;
        nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hWnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = tray_detail::WM_TRAY;
        nid.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));            // 程序自带的图标…
        if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // …没有时用系统默认图标
        // 窗口与初始提示一起发布，启动早期保存的托盘文字不会丢。
        {
            std::lock_guard<std::mutex> lock(tray_detail::g_textMutex);
            const auto tip = tray_detail::wideTooltip();
            wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
            Shell_NotifyIconW(NIM_ADD, &nid);
            tray_detail::g_window = hWnd;
        }

        // 启动即最小化到托盘：隐藏控制台窗口并禁用其关闭按钮（避免误点 X 结束进程，
        // 退出统一走托盘「退出」），再弹一次气泡告知程序仍在后台运行。
        if (startHidden) {
            if (HWND con = GetConsoleWindow()) {
                if (HMENU sysMenu = GetSystemMenu(con, FALSE))
                    EnableMenuItem(sysMenu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
                ShowWindow(con, SW_HIDE);
                tray_detail::g_consoleVisible = false;   // 托盘菜单据此显示「显示控制台」
            }
            NOTIFYICONDATAW info = nid;
            info.uFlags = NIF_INFO;
            info.dwInfoFlags = NIIF_INFO;
            wcsncpy_s(info.szInfoTitle, nid.szTip, _TRUNCATE);
            wcsncpy_s(info.szInfo,
                L"CatOfficial 并未退出，而是最小化到此处。如需退出，请右键此图标选择“退出”。",
                _TRUNCATE);
            Shell_NotifyIconW(NIM_MODIFY, &info);
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
        { std::lock_guard<std::mutex> lock(tray_detail::g_textMutex); tray_detail::g_window = nullptr; }
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hWnd);
    }).detach();
}

/// 程序自己退出时移除托盘图标，不在通知区域留下要鼠标划过才消失的残影（面板重启时尤其明显）。
inline void stopSystemTray() {
    HWND window = nullptr;
    { std::lock_guard<std::mutex> lock(tray_detail::g_textMutex); window = tray_detail::g_window; }
    if (!window) return;
    SendMessageTimeoutW(window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
    // 消息循环在托盘线程上结束并删除图标；稍等它做完。
    for (int i = 0; i < 50; ++i) {
        { std::lock_guard<std::mutex> lock(tray_detail::g_textMutex); if (!tray_detail::g_window) return; }
        Sleep(10);
    }
}

}  // namespace catofficial::app

#else   // 非 Windows：空实现
namespace catofficial::app {
inline void startSystemTray(uint16_t, std::function<void()>, bool = false, const std::string& = "") {}
inline void updateSystemTrayText(const std::string&) {}
inline void stopSystemTray() {}
}
#endif
