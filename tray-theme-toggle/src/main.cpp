// ============================================================================
//  TrayThemeToggle —— 任务栏托盘「一键深浅色切换」
//
//  功能：
//    1. 常驻任务栏右下角托盘，图标随当前主题自动变化（浅色=太阳，深色=月亮）
//    2. 右键菜单：切换主题 / 开机自启 / 启用快捷键 / 关于 / 退出
//    3. 全局快捷键：按一次翻转一次（当前浅色 -> 变深色，反之亦然）
//       默认 Ctrl+Alt+S，可在托盘右键菜单「设置快捷键…」里自定义任意组合
//    4. 双击托盘图标 = 切换主题
//    5. 命令行 --toggle：静默翻转后退出，方便做快捷方式或脚本
//    6. 命令行 --diagnose：把自检结果写到同目录 diagnose.log
//
//  实现要点：
//    - 只修改 HKCU 下的两个 DWORD，不需要管理员权限
//    - 修改后广播系统消息，任务栏/开始菜单/资源管理器立即刷新，无需注销
//    - 单实例互斥，重复启动不会出现两个托盘图标
//    - 监听 TaskbarCreated，资源管理器重启后图标会自动回来
//
//  编译：见 build.bat（MinGW-w64 / g++）
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#include "resource.h"
#include "win11ui.h"

namespace {

// ---------------------------------------------------------------- 常量
constexpr wchar_t kPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"TrayThemeToggle";
constexpr wchar_t kAppKey[] = L"Software\\TrayThemeToggle";
constexpr wchar_t kClassName[] = L"TrayThemeToggleHiddenWnd";
constexpr wchar_t kMutexName[] = L"Local\\TrayThemeToggle_SingleInstance";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_TRAY_TOGGLE = 1001;
constexpr UINT ID_TRAY_HOTKEY = 1002;
constexpr UINT ID_TRAY_AUTOSTART = 1003;
constexpr UINT ID_TRAY_ABOUT = 1004;
constexpr UINT ID_TRAY_EXIT = 1005;
constexpr UINT ID_TRAY_SETTINGS = 1006;

// 快捷键设置窗口里的控件 ID
constexpr UINT IDC_HK_CAPTURE = 2001;
constexpr UINT IDC_HK_ENABLE = 2002;
constexpr UINT IDC_HK_OK = 2003;
constexpr UINT IDC_HK_CANCEL = 2004;
constexpr UINT IDC_HK_RESET = 2005;
constexpr UINT IDC_HK_DISPLAY = 2006;
constexpr UINT IDC_ABOUT_CLOSE = 2007;

constexpr int kHotkeyId = 1;
constexpr UINT kTrayIconId = 1;

// ---------------------------------------------------------------- 快捷键配置

// 默认快捷键。用 Ctrl+Alt+S 而不是 Ctrl+S：
// Ctrl+S 是几乎所有程序的「保存」，全局抢占它会让用户在编辑器里按不出保存。
struct HotkeyConfig {
    UINT modifiers = MOD_CONTROL | MOD_ALT;
    UINT vk = 'S';
    bool enabled = true;
};

const HotkeyConfig kDefaultHotkey{};

// ---------------------------------------------------------------- 全局状态
HWND g_hwnd = nullptr;
HICON g_iconSun = nullptr;
HICON g_iconMoon = nullptr;
HICON g_iconApp = nullptr;
HotkeyConfig g_hotkey;
bool g_hotkeyRegistered = false;
UINT g_taskbarCreatedMsg = 0;
bool g_trayAdded = false;
int g_iconCx = 0;
int g_iconCy = 0;

// ---------------------------------------------------------------- 前置声明
// UpdateTray 需要用到格式化函数，但它的定义在下面
std::wstring FormatHotkey(UINT mods, UINT vk);
// ShowHotkeyDialog 里会用到关于窗口来报错
void ShowAboutDialog(const std::wstring& title, const std::vector<std::wstring>& paragraphs);

// ================================================================ 路径与日志

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return L".";
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L'\\');
    return (p == std::wstring::npos) ? L"." : s.substr(0, p);
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return (n == 0) ? L"" : std::wstring(buf, n);
}

// 仅 --diagnose 时使用，正常运行时不会创建任何文件
FILE* g_log = nullptr;

void LogLine(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ================================================================ 注册表小工具

bool RegReadDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    DWORD value = 0;
    LONG rc = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return false;
    *out = value;
    return true;
}

bool RegWriteDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    LONG rc = RegSetValueExW(key, name, 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

// ================================================================ 主题读写

// 当前系统是否处于浅色模式。读不到时按 Windows 出厂默认「浅色」处理。
bool IsSystemLight() {
    DWORD v = 1;
    if (!RegReadDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme", &v)) {
        if (!RegReadDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", &v)) v = 1;
    }
    return v != 0;
}

// 广播主题变更，让资源管理器、任务栏、开始菜单立即重绘（不需要注销或重启 explorer）
//
// 注意：这里必须带超时。HWND_BROADCAST 会把消息发给所有顶层窗口，
// 只要有一个程序不处理消息，同步发送就会一直卡住。
// 绝对不能用 SMTO_NOTIMEOUTIFNOTHUNG —— 那个标志的含义是
// 「只要对方没被判定为挂起就不超时」，等于把超时关掉了。
// 也不要加 SMTO_BLOCK，那会让本线程在等待期间完全不处理自己的消息。
void BroadcastThemeChange() {
    DWORD_PTR result = 0;
    const UINT flags = SMTO_ABORTIFHUNG;

    // 最关键的一条：资源管理器靠它重新读取 ImmersiveColorSet 并刷新主题
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), flags, 400, &result);
    // 让自绘标题栏的窗口重画
    SendMessageTimeoutW(HWND_BROADCAST, WM_THEMECHANGED, 0, 0, flags, 250, &result);
}

// 应用主题：同时改「系统」与「应用」两项，保证任务栏和窗口一起变
bool ApplyTheme(bool light) {
    const DWORD v = light ? 1u : 0u;
    bool okSystem = RegWriteDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme", v);
    bool okApps = RegWriteDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", v);
    LogLine(L"ApplyTheme(light=%d): system=%d apps=%d\n", light ? 1 : 0, okSystem, okApps);
    BroadcastThemeChange();
    return okSystem || okApps;
}

// ================================================================ 图标与托盘

HICON LoadIconRes(int id, int cx, int cy) {
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id),
                                         IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
}

void RefreshIconMetrics() {
    g_iconCx = GetSystemMetrics(SM_CXSMICON);
    g_iconCy = GetSystemMetrics(SM_CYSMICON);
    if (g_iconCx <= 0) g_iconCx = 16;
    if (g_iconCy <= 0) g_iconCy = 16;
}

// 刷新托盘图标与提示文字，使其反映当前真实状态
void UpdateTray(bool light) {
    if (!g_hwnd) return;
    if (g_iconCx == 0) RefreshIconMetrics();

    HICON icon = LoadIconRes(light ? IDI_SUN : IDI_MOON, g_iconCx, g_iconCy);
    if (!icon) icon = light ? g_iconSun : g_iconMoon;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = icon;

    // 提示里直接告诉用户「按下去会变成什么」，比只说当前状态更有用
    const std::wstring hk = g_hotkey.enabled
                                ? FormatHotkey(g_hotkey.modifiers, g_hotkey.vk)
                                : L"快捷键已关闭";
    if (light) {
        std::wstring tip = L"当前：浅色　|　右键菜单　|　" + hk + L" → 深色";
        wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    } else {
        std::wstring tip = L"当前：深色　|　右键菜单　|　" + hk + L" → 浅色";
        wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    }

    BOOL ok = FALSE;
    if (g_trayAdded) {
        ok = Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (!ok) {
        ok = Shell_NotifyIconW(NIM_ADD, &nid);
        if (ok) g_trayAdded = true;
        DWORD err = GetLastError();
        LogLine(L"Shell_NotifyIcon(ADD) ok=%d err=%lu\n", ok, ok ? 0 : err);
    }

    if (icon && icon != g_iconSun && icon != g_iconMoon) DestroyIcon(icon);
}

void RemoveTray() {
    if (!g_hwnd || !g_trayAdded) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

void ShowBalloon(const wchar_t* title, const wchar_t* text) {
    if (!g_hwnd || !g_trayAdded) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(nid.szInfoTitle, title);
    wcscpy_s(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ================================================================ 开机自启

bool IsAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(key, kRunValue, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && size > 2;
}

void SetAutostart(bool enable) {
    if (enable) {
        std::wstring path = ExePath();
        if (path.empty()) return;
        std::wstring quoted = L"\"" + path + L"\"";
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                            nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(key, kRunValue, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(quoted.c_str()),
                           static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
    } else {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, kRunValue);
            RegCloseKey(key);
        }
    }
}

// ================================================================ 快捷键

// 把 MOD_* 与虚拟键翻译成 "Ctrl+Alt+S" 这种可读文本
std::wstring FormatHotkey(UINT mods, UINT vk) {
    if (vk == 0) return L"（未设置）";

    std::wstring s;
    // 顺序固定：Ctrl、Alt、Shift、Win，和系统设置里的写法一致
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_WIN) s += L"Win+";

    // 虚拟键名。常见键给中文/短名，其余走 GetKeyNameTextW
    switch (vk) {
        case VK_F1:  s += L"F1";  break;
        case VK_F2:  s += L"F2";  break;
        case VK_F3:  s += L"F3";  break;
        case VK_F4:  s += L"F4";  break;
        case VK_F5:  s += L"F5";  break;
        case VK_F6:  s += L"F6";  break;
        case VK_F7:  s += L"F7";  break;
        case VK_F8:  s += L"F8";  break;
        case VK_F9:  s += L"F9";  break;
        case VK_F10: s += L"F10"; break;
        case VK_F11: s += L"F11"; break;
        case VK_F12: s += L"F12"; break;
        case VK_SPACE:   s += L"Space"; break;
        case VK_TAB:     s += L"Tab";   break;
        case VK_RETURN:  s += L"Enter"; break;
        case VK_ESCAPE:  s += L"Esc";   break;
        case VK_INSERT:  s += L"Insert";break;
        case VK_DELETE:  s += L"Delete";break;
        case VK_HOME:    s += L"Home";  break;
        case VK_END:     s += L"End";   break;
        case VK_PRIOR:   s += L"PageUp";   break;
        case VK_NEXT:    s += L"PageDown"; break;
        case VK_LEFT:    s += L"Left";  break;
        case VK_RIGHT:   s += L"Right"; break;
        case VK_UP:      s += L"Up";    break;
        case VK_DOWN:    s += L"Down";  break;
        default: {
            // 字母数字直接显示字符，其它用系统键名
            if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
                s += static_cast<wchar_t>(vk);
            } else {
                const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
                wchar_t name[64] = {};
                // 扩展键要加 0x01000000，否则左右键之类会显示成数字键盘键
                const LONG lParam = static_cast<LONG>(scan);
                if (GetKeyNameTextW(lParam | (1 << 24), name,
                                    static_cast<int>(std::size(name))) > 0) {
                    s += name;
                } else {
                    wchar_t buf[16];
                    swprintf_s(buf, L"VK_%02X", vk);
                    s += buf;
                }
            }
            break;
        }
    }
    return s;
}

// 读注册表里的快捷键配置；没配过就用默认值
HotkeyConfig LoadHotkeyConfig() {
    HotkeyConfig c = kDefaultHotkey;
    DWORD v = 0;
    if (RegReadDword(HKEY_CURRENT_USER, kAppKey, L"HotkeyModifiers", &v)) c.modifiers = v;
    if (RegReadDword(HKEY_CURRENT_USER, kAppKey, L"HotkeyVk", &v)) c.vk = v;
    if (RegReadDword(HKEY_CURRENT_USER, kAppKey, L"HotkeyEnabled", &v)) c.enabled = (v != 0);

    // 防御：注册表被手工改坏时回落到默认值，避免注册一个无效热键
    if (c.vk == 0 || c.vk > 0xFF) c = kDefaultHotkey;
    return c;
}

void SaveHotkeyConfig(const HotkeyConfig& c) {
    RegWriteDword(HKEY_CURRENT_USER, kAppKey, L"HotkeyModifiers", c.modifiers);
    RegWriteDword(HKEY_CURRENT_USER, kAppKey, L"HotkeyVk", c.vk);
    RegWriteDword(HKEY_CURRENT_USER, kAppKey, L"HotkeyEnabled", c.enabled ? 1u : 0u);
}

// 按当前 g_hotkey 注册（或注销）热键。返回是否处于「已生效」状态。
bool ApplyHotkey(bool showErrorIfFailed) {
    if (g_hwnd == nullptr) return false;
    UnregisterHotKey(g_hwnd, kHotkeyId);
    g_hotkeyRegistered = false;

    if (!g_hotkey.enabled) {
        LogLine(L"ApplyHotkey: disabled\n");
        return false;
    }
    if (g_hotkey.vk == 0) return false;

    // MOD_NOREPEAT：按住不放不会连续触发，避免一秒翻转几十次
    const UINT mods = g_hotkey.modifiers | MOD_NOREPEAT;
    if (RegisterHotKey(g_hwnd, kHotkeyId, mods, g_hotkey.vk)) {
        g_hotkeyRegistered = true;
        LogLine(L"ApplyHotkey: OK %ls\n", FormatHotkey(g_hotkey.modifiers, g_hotkey.vk).c_str());
        return true;
    }

    LogLine(L"ApplyHotkey: FAILED err=%lu\n", GetLastError());
    if (showErrorIfFailed) {
        std::wstring msg = L"快捷键 " + FormatHotkey(g_hotkey.modifiers, g_hotkey.vk) +
                           L" 注册失败，可能已被其他程序占用。";
        ShowAboutDialog(L"注册失败", {msg, L"可以在「设置快捷键」里换一个组合。"});
    }
    return false;
}

// ================================================================ 核心动作

// 翻转：当前浅色 -> 深色；当前深色 -> 浅色
void ToggleTheme() {
    const bool nowLight = IsSystemLight();
    const bool target = !nowLight;
    ApplyTheme(target);
    UpdateTray(target);
}

// ================================================================ 现代对话框

// 自绘标题栏高度（96 DPI 下的基准值，绘制时按 DPI 缩放）
constexpr int kCaptionBarH = 40;

// 从当前键盘状态算出修饰键
UINT CurrentModifiers() {
    UINT m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) m |= MOD_WIN;
    return m;
}

bool IsModifierVk(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU ||
           vk == VK_LMENU || vk == VK_RMENU || vk == VK_SHIFT || vk == VK_LSHIFT ||
           vk == VK_RSHIFT || vk == VK_LWIN || vk == VK_RWIN;
}

//
// Win32 原生控件（BUTTON / EDIT / MessageBox）在 Win11 下依然是 Win95 观感，
// 深色模式下更是完全不跟随。这里改成整窗自绘：
//   - 窗口本体用 DWM 圆角 + 深色标题栏，和系统窗口一致
//   - 内容区全部 GDI 绘制，配色走 ui::Palette
//   - 按钮 / 复选框自己处理悬停、按下、焦点

enum class DlgMode { Hotkey, About };

struct UiButton {
    RECT rc{};
    std::wstring text;
    int id = 0;
    bool accent = false;   // 主按钮，用强调色填充
    bool hover = false;
    bool pressed = false;
};

struct UiDialog {
    HWND hwnd = nullptr;
    DlgMode mode = DlgMode::Hotkey;
    UINT dpi = 96;
    bool dark = true;

    HFONT fTitle = nullptr;    // 标题/主文字
    HFONT fBody = nullptr;     // 正文
    HFONT fSmall = nullptr;    // 次要说明
    HFONT fHotkey = nullptr;   // 快捷键大字
    HFONT fButton = nullptr;
    HFONT fAppName = nullptr;  // 关于页的应用名
    HFONT fVersion = nullptr;  // 版本号

    // 自绘标题栏
    RECT rcCaptionBar{};       // 整个标题栏区域
    RECT rcClose{};            // 关闭按钮
    RECT rcVersion{};          // 版本号行
    bool closeHover = false;
    bool closePressed = false;
    bool dragging = false;
    POINT dragStart{};

    // 快捷键模式
    UINT mods = 0;
    UINT vk = 0;
    bool enable = true;
    bool capturing = true;
    bool accepted = false;
    bool boxHover = false;

    // 关于模式
    std::vector<std::wstring> paragraphs;

    std::vector<UiButton> buttons;
    int hotBtn = -1;   // 悬停按钮索引
    int pressBtn = -1;

    // 布局算出来的矩形
    RECT rcCapture{};
    RECT rcCheck{};
    RECT rcTitle{};
    RECT rcAboutCard{};   // About 页段落背景卡片
};

UiDialog g_ui;

// ---------------------------------------------------------------- 字体

void RebuildFonts(UiDialog& d) {
    for (HFONT f : {d.fTitle, d.fBody, d.fSmall, d.fHotkey, d.fButton, d.fAppName, d.fVersion}) {
        if (f) DeleteObject(f);
    }
    // 字号层级：标题 > 应用名 > 正文 > 次要说明
    // 之前正文 14px 和标题 15px 太接近，读起来"描述比卡片名还大"，
    // 现在把正文压到 13px，说明压到 11.5px，层级拉开。
    d.fTitle = ui::MakeFont(ui::UiFace(), 16, FW_SEMIBOLD, d.dpi);
    d.fAppName = ui::MakeFont(ui::UiFace(), 20, FW_SEMIBOLD, d.dpi);
    d.fVersion = ui::MakeFont(ui::UiFace(), 11, FW_NORMAL, d.dpi);
    d.fBody = ui::MakeFont(ui::UiFace(), 13, FW_NORMAL, d.dpi);
    d.fSmall = ui::MakeFont(ui::UiFace(), 11, FW_NORMAL, d.dpi);
    d.fHotkey = ui::MakeFont(ui::UiFace(), 19, FW_SEMIBOLD, d.dpi);
    d.fButton = ui::MakeFont(ui::UiFace(), 13, FW_NORMAL, d.dpi);
}

// ---------------------------------------------------------------- 布局

void LayoutHotkeyDialog(UiDialog& d, const RECT& client) {
    const UINT p = d.dpi;
    const int pad = ui::Scale(24, p);
    const int w = client.right - client.left;

    // 让开系统标题栏（WS_CAPTION 客户区已从标题栏下方开始）
    int y = ui::Scale(18, p);
    d.rcTitle = RECT{pad, y, w - pad, y + ui::Scale(22, p)};
    y += ui::Scale(30, p);

    d.rcCapture = RECT{pad, y, w - pad, y + ui::Scale(58, p)};
    y = d.rcCapture.bottom + ui::Scale(10, p);

    // 提示文字占两行高度
    y += ui::Scale(34, p);

    d.rcCheck = RECT{pad, y, w - pad, y + ui::Scale(24, p)};
    y = d.rcCheck.bottom + ui::Scale(20, p);

    // 按钮：右对齐，从右往左排
    const int bh = ui::Scale(34, p);
    const int gap = ui::Scale(10, p);
    int rx = w - pad;
    for (int i = static_cast<int>(d.buttons.size()) - 1; i >= 0; --i) {
        UiButton& b = d.buttons[i];
        HDC dc = GetDC(d.hwnd);
        HGDIOBJ old = SelectObject(dc, d.fButton);
        SIZE sz{};
        GetTextExtentPoint32W(dc, b.text.c_str(), static_cast<int>(b.text.size()), &sz);
        SelectObject(dc, old);
        ReleaseDC(d.hwnd, dc);

        const int bw = sz.cx + ui::Scale(40, p);
        b.rc = RECT{rx - bw, y, rx, y + bh};
        rx -= bw + gap;
    }
}

// 关于页布局：原生标题栏 + 应用名/版本 + 段落 + 按钮
void LayoutAboutDialog(UiDialog& d, const RECT& client) {
    const UINT p = d.dpi;
    const int pad = ui::Scale(24, p);
    const int w = client.right - client.left;

    int y = ui::Scale(8, p);
    d.rcTitle = RECT{pad, y, w - pad, y + ui::Scale(26, p)};   // 应用名
    y = d.rcTitle.bottom + ui::Scale(2, p);
    d.rcVersion = RECT{pad, y, w - pad, y + ui::Scale(16, p)};  // 版本号
    y = d.rcVersion.bottom + ui::Scale(16, p);

    // 段落背景卡片（用来与“确定”按钮做视觉区分）
    int cardTop = y;
    const int cardPadY = ui::Scale(10, p);
    const int cardPadX = ui::Scale(16, p);

    // 注意：段落测量必须用 d.fSmall —— 与 MeasureAboutHeight（算窗口高度）和
    // PaintUi（实际绘制）保持同一个字号，否则按钮会被算到窗口底边之外而被裁掉。
    HDC dc = GetDC(d.hwnd);
    HGDIOBJ old = SelectObject(dc, d.fSmall);
    for (const auto& para : d.paragraphs) {
        RECT r{pad, y, w - pad, y + ui::Scale(400, p)};
        DrawTextW(dc, para.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
        y = r.bottom + ui::Scale(16, p);   // 段后距加大，段落之间更开阔
    }
    SelectObject(dc, old);
    ReleaseDC(d.hwnd, dc);
    int cardBottom = y;

    d.rcAboutCard = RECT{pad - cardPadX, cardTop - cardPadY, w - pad + cardPadX, cardBottom + cardPadY};

    const int bh = ui::Scale(32, p);
    const int gap = ui::Scale(10, p);
    int rx = w - pad;
    y += ui::Scale(20, p);   // 与“确定”按钮拉开更大距离
    for (int i = static_cast<int>(d.buttons.size()) - 1; i >= 0; --i) {
        UiButton& b = d.buttons[i];
        HDC dc2 = GetDC(d.hwnd);
        HGDIOBJ o2 = SelectObject(dc2, d.fButton);
        SIZE sz{};
        GetTextExtentPoint32W(dc2, b.text.c_str(), static_cast<int>(b.text.size()), &sz);
        SelectObject(dc2, o2);
        ReleaseDC(d.hwnd, dc2);

        const int bw = sz.cx + ui::Scale(36, p);
        b.rc = RECT{rx - bw, y, rx, y + bh};
        rx -= bw + gap;
    }
}

// 关于窗口的内容高度需要先量一次才知道，所以单独算
int MeasureAboutHeight(UiDialog& d, int widthPx) {
    const UINT p = d.dpi;
    const int pad = ui::Scale(24, p);
    int y = ui::Scale(8, p);
    y += ui::Scale(26, p);   // 应用名
    y += ui::Scale(2, p);
    y += ui::Scale(16, p);   // 版本号
    y += ui::Scale(16, p);

    HDC dc = GetDC(d.hwnd);
    HGDIOBJ old = SelectObject(dc, d.fSmall);  // 关于页描述用次要字号，比标题小一圈
    for (const auto& para : d.paragraphs) {
        RECT r{pad, y, widthPx - pad, y + ui::Scale(400, p)};
        DrawTextW(dc, para.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
        y = r.bottom + ui::Scale(16, p);   // 段后距与 LayoutAboutDialog 保持一致
    }
    SelectObject(dc, old);
    ReleaseDC(d.hwnd, dc);

    const int buttonGap = ui::Scale(20, p);   // 段落与按钮的间距
    return y + buttonGap + ui::Scale(32, p);   // 段落 + 按钮间距 + 按钮高（与 LayoutAboutDialog 对齐，不额外加底边距）
}

// ---------------------------------------------------------------- 绘制

void DrawUiButton(HDC dc, const UiDialog& d, const UiButton& b, const ui::Palette& pal) {
    const int radius = ui::Scale(6, d.dpi);

    if (b.accent) {
        COLORREF fill = pal.accent;
        if (b.pressed) fill = pal.accentPressed;
        else if (b.hover) fill = pal.accentHover;
        ui::FillRound(dc, b.rc, radius, fill);
        ui::DrawTextIn(dc, b.rc, b.text.c_str(), d.fButton, pal.onAccent,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        COLORREF fill = pal.buttonBg;
        if (b.pressed) fill = pal.buttonPressed;
        else if (b.hover) fill = pal.buttonHover;
        ui::CardRound(dc, b.rc, radius, fill, pal.buttonBorder);
        ui::DrawTextIn(dc, b.rc, b.text.c_str(), d.fButton, pal.text,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void DrawCheckbox(HDC dc, const UiDialog& d, const RECT& rc, bool checked, bool hover,
                  const ui::Palette& pal, const std::wstring& label) {
    const UINT p = d.dpi;
    const int box = ui::Scale(18, p);
    const int cy = (rc.top + rc.bottom) / 2;

    RECT br{rc.left, cy - box / 2, rc.left + box, cy - box / 2 + box};
    const int radius = ui::Scale(4, p);

    if (checked) {
        COLORREF fill = hover ? pal.accentHover : pal.accent;
        ui::FillRound(dc, br, radius, fill);
        // 白/黑勾由 onAccent 决定，深色模式下 accent 是浅蓝配黑勾
        ui::DrawCheckMark(dc, (br.left + br.right) / 2, cy, ui::Scale(11, p), pal.onAccent);
    } else {
        ui::CardRound(dc, br, radius, hover ? pal.buttonHover : pal.surface,
                      hover ? pal.accent : pal.border);
    }

    RECT tr = rc;
    tr.left = br.right + ui::Scale(10, p);
    ui::DrawTextIn(dc, tr, label.c_str(), d.fBody, pal.text,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void PaintUi(HWND hwnd, UiDialog& d) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    const ui::Palette pal = ui::GetPalette(d.dark);

    // 双缓冲，避免自绘闪烁
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    ui::FillRectC(mem, client, pal.windowBg);

    if (d.mode == DlgMode::Hotkey) {
        // 标题
        ui::DrawTextIn(mem, d.rcTitle, L"按下新的组合键", d.fTitle, pal.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 捕获框：浅色下白底，深色下比窗口略亮的表面色
        const int radius = ui::Scale(7, d.dpi);
        COLORREF boxFill = pal.surface;
        COLORREF boxBorder = d.boxHover ? pal.accent : pal.border;
        ui::CardRound(mem, d.rcCapture, radius, boxFill, boxBorder);

        std::wstring shown;
        COLORREF shownColor = pal.text;
        if (d.vk == 0) {
            shown = L"请按下组合键…";
            shownColor = pal.textSecondary;
        } else {
            shown = FormatHotkey(d.mods, d.vk);
        }
        ui::DrawTextIn(mem, d.rcCapture, shown.c_str(), d.fHotkey, shownColor,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 提示
        RECT hint{d.rcCapture.left, d.rcCapture.bottom + ui::Scale(10, d.dpi), d.rcCapture.right,
                  d.rcCapture.bottom + ui::Scale(44, d.dpi)};
        const wchar_t* hintText;
        if (d.vk == 0) {
            hintText = L"至少按下一个主键：字母、数字、F1~F12 或方向键";
        } else if ((d.mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) == 0) {
            hintText = L"建议同时按住 Ctrl 或 Alt，单独一个键会干扰正常打字";
        } else {
            hintText = L"重新按一次可更换组合，Esc 取消，Enter 保存";
        }
        ui::DrawTextIn(mem, hint, hintText, d.fSmall, pal.textSecondary,
                       DT_LEFT | DT_TOP | DT_WORDBREAK);

        DrawCheckbox(mem, d, d.rcCheck, d.enable, false, pal, L"启用这个全局快捷键");

    } else {
        // 应用名（大字号）+ 版本号（小字号、次要色）
        ui::DrawTextIn(mem, d.rcTitle, L"主题切换器", d.fAppName, pal.text,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        ui::DrawTextIn(mem, d.rcVersion, L"v1.2  By Didier", d.fVersion, pal.textSecondary,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        const UINT p = d.dpi;
        int y = d.rcVersion.bottom + ui::Scale(16, p);
        const int pad = ui::Scale(24, p);

        // 段落背景卡片
        const int cardRadius = ui::Scale(10, p);
        ui::CardRound(mem, d.rcAboutCard, cardRadius, pal.surface, pal.border);

        for (const auto& para : d.paragraphs) {
            RECT r{pad, y, client.right - pad, y + ui::Scale(400, p)};
            DrawTextW(mem, para.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
            ui::DrawTextIn(mem, r, para.c_str(), d.fSmall, pal.textSecondary,
                           DT_LEFT | DT_TOP | DT_WORDBREAK);
            y = r.bottom + ui::Scale(16, p);   // 段后距与 LayoutAboutDialog 保持一致
        }
    }

    for (const auto& b : d.buttons) DrawUiButton(mem, d, b, pal);

    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- 交互

int HitButton(const UiDialog& d, POINT pt) {
    for (size_t i = 0; i < d.buttons.size(); ++i) {
        if (PtInRect(&d.buttons[i].rc, pt)) return static_cast<int>(i);
    }
    return -1;
}

void CloseUi(UiDialog& d, bool accepted) {
    d.accepted = accepted;
    if (d.hwnd) DestroyWindow(d.hwnd);
}

LRESULT CALLBACK UiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UiDialog& d = g_ui;

    switch (msg) {
        case WM_CREATE: {
            d.hwnd = hwnd;
            d.dpi = ui::DpiOf(hwnd);
            RebuildFonts(d);
            ui::ApplyModernFrame(hwnd, d.dark);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            PaintUi(hwnd, d);
            return 0;

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            bool needRepaint = false;

            const int hb = HitButton(d, pt);
            if (hb != d.hotBtn) {
                d.hotBtn = hb;
                for (size_t i = 0; i < d.buttons.size(); ++i)
                    d.buttons[i].hover = (static_cast<int>(i) == hb);
                needRepaint = true;
            }
            if (d.mode == DlgMode::Hotkey) {
                const bool bh = PtInRect(&d.rcCapture, pt) != 0;
                if (bh != d.boxHover) {
                    d.boxHover = bh;
                    needRepaint = true;
                }
            }
            if (needRepaint) InvalidateRect(hwnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE: {
            bool needRepaint = false;
            if (d.hotBtn != -1) {
                d.hotBtn = -1;
                for (auto& b : d.buttons) b.hover = false;
                needRepaint = true;
            }
            if (d.boxHover) {
                d.boxHover = false;
                needRepaint = true;
            }
            if (needRepaint) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            const int hb = HitButton(d, pt);
            if (hb >= 0) {
                d.pressBtn = hb;
                d.buttons[hb].pressed = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (d.mode == DlgMode::Hotkey && PtInRect(&d.rcCheck, pt)) {
                d.enable = !d.enable;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if (d.dragging) {
                d.dragging = false;
                ReleaseCapture();
                return 0;
            }

            if (d.pressBtn >= 0) {
                const int hb = HitButton(d, pt);
                const int pressed = d.pressBtn;
                d.pressBtn = -1;
                for (auto& b : d.buttons) b.pressed = false;
                ReleaseCapture();

                if (hb == pressed && hb >= 0) {
                    const int id = d.buttons[hb].id;
                    if (id == IDC_HK_OK) {
                        CloseUi(d, true);
                    } else if (id == IDC_HK_CANCEL) {
                        CloseUi(d, false);
                    } else if (id == IDC_HK_RESET) {
                        d.mods = kDefaultHotkey.modifiers;
                        d.vk = kDefaultHotkey.vk;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    } else if (id == IDC_ABOUT_CLOSE) {
                        CloseUi(d, false);
                    }
                } else {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (d.mode != DlgMode::Hotkey) {
                if (wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_SPACE) {
                    CloseUi(d, false);
                    return 0;
                }
                return 0;
            }

            const UINT vk = static_cast<UINT>(wParam);

            if (IsModifierVk(vk)) {
                const UINT m = CurrentModifiers();
                if (m != 0) {
                    std::wstring s;
                    if (m & MOD_CONTROL) s += L"Ctrl+";
                    if (m & MOD_ALT) s += L"Alt+";
                    if (m & MOD_SHIFT) s += L"Shift+";
                    if (m & MOD_WIN) s += L"Win+";
                    s += L"…";
                    // 临时预览：直接改显示用的 vk=0 + 自定义文本不方便，
                    // 这里用 mods 预览 + vk 保持 0，绘制时走 secondary 分支
                    d.mods = m;
                    d.vk = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            if (vk == VK_ESCAPE && CurrentModifiers() == 0) {
                CloseUi(d, false);
                return 0;
            }

            if (vk == VK_RETURN && CurrentModifiers() == 0) {
                if (d.vk != 0) CloseUi(d, true);
                return 0;
            }

            if (vk == VK_TAB && CurrentModifiers() == 0) {
                d.enable = !d.enable;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            d.mods = CurrentModifiers();
            d.vk = vk;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_SYSKEYUP:
            return 0;

        case WM_DPICHANGED: {
            d.dpi = HIWORD(wParam);
            RebuildFonts(d);
            RECT* r = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_CLOSE:
            CloseUi(d, false);
            return 0;

        case WM_DESTROY:
            for (HFONT f : {d.fTitle, d.fBody, d.fSmall, d.fHotkey, d.fButton}) {
                if (f) DeleteObject(f);
            }
            d.fTitle = d.fBody = d.fSmall = d.fHotkey = d.fButton = nullptr;
            d.hwnd = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

const wchar_t* kUiClass = L"TrayThemeToggleModernUi";

void EnsureUiClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = UiWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kUiClass;
    RegisterClassExW(&wc);
    done = true;
}

// 在光标附近弹出窗口，并保证不超出工作区。
// 只负责定位，尺寸由调用方先用 SetWindowPos 定好。
void PlaceNearCursor(HWND hwnd, int w, int h) {
    POINT pt{};
    GetCursorPos(&pt);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);

    int x = pt.x - w / 2;
    int y = pt.y - h - 12;
    if (y < mi.rcWork.top) y = pt.y + 12;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 通用模态循环：直到窗口销毁
void RunModal(HWND dlg) {
    SetForegroundWindow(dlg);
    SetFocus(dlg);
    MSG msg{};
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            SendMessageW(dlg, msg.message, msg.wParam, msg.lParam);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---------------------------------------------------------------- 快捷键设置

void ShowHotkeyDialog() {
    // 打开期间先注销热键，否则用户按下的组合会被自己抢走
    if (g_hotkeyRegistered) UnregisterHotKey(g_hwnd, kHotkeyId);

    EnsureUiClass();

    // 同一时间只允许一个对话框：若已有对话框，先销毁，避免 g_ui 单例被覆盖。
    if (g_ui.hwnd) DestroyWindow(g_ui.hwnd);

    g_ui = UiDialog{};
    g_ui.mode = DlgMode::Hotkey;
    g_ui.dark = !IsSystemLight();
    g_ui.mods = g_hotkey.modifiers;
    g_ui.vk = g_hotkey.vk;
    g_ui.enable = g_hotkey.enabled;

    g_ui.buttons = {
        UiButton{RECT{}, L"恢复默认", IDC_HK_RESET, false, false, false},
        UiButton{RECT{}, L"取消", IDC_HK_CANCEL, false, false, false},
        UiButton{RECT{}, L"确定", IDC_HK_OK, true, false, false},
    };

    const UINT p = ui::DpiOf(nullptr);
    const int w = ui::Scale(440, p);
    const int h = ui::Scale(280, p);   // 280px，避免底部空隙过高

    HWND dlg = CreateWindowExW(0, kUiClass, L"设置快捷键",
                               WS_CAPTION | WS_SYSMENU, 0, 0, w, h, g_hwnd, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!dlg) {
        ApplyHotkey(false);
        return;
    }

    // 布局依赖客户区尺寸，建完窗口再算
    RECT client{};
    GetClientRect(dlg, &client);
    LayoutHotkeyDialog(g_ui, client);
    PlaceNearCursor(dlg, w, h);

    RunModal(dlg);

    if (g_ui.accepted) {
        g_hotkey.modifiers = g_ui.mods;
        g_hotkey.vk = g_ui.vk;
        g_hotkey.enabled = g_ui.enable;
        SaveHotkeyConfig(g_hotkey);
        if (!ApplyHotkey(false)) {
            std::wstring m = L"快捷键 " + FormatHotkey(g_hotkey.modifiers, g_hotkey.vk) +
                             L" 没能注册成功，可能已被其他程序占用。";
            ShowAboutDialog(L"注册失败", {m, L"请在「设置快捷键」里换一个组合。"});
        }
    } else {
        ApplyHotkey(false);
    }
    UpdateTray(IsSystemLight());
}

// ---------------------------------------------------------------- 关于 / 提示

void ShowAboutDialog(const std::wstring& title, const std::vector<std::wstring>& paragraphs) {
    EnsureUiClass();

    // 同一时间只允许一个对话框：若已有对话框，先销毁，避免 g_ui 单例被覆盖
    // 导致两个窗口读取同一份状态而内容混错、无法关闭。
    if (g_ui.hwnd) DestroyWindow(g_ui.hwnd);

    g_ui = UiDialog{};
    g_ui.mode = DlgMode::About;
    g_ui.dark = !IsSystemLight();
    g_ui.paragraphs = paragraphs;
    g_ui.buttons = {UiButton{RECT{}, L"确定", IDC_ABOUT_CLOSE, true, false, false}};

    const UINT p = ui::DpiOf(nullptr);
    const int w = ui::Scale(460, p);

    HWND dlg = CreateWindowExW(0, kUiClass, title.c_str(),
                               WS_CAPTION | WS_SYSMENU, 0, 0, w, ui::Scale(285, p),
                               g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) return;

    // 原生标题栏，客户区在标题栏下方
    RECT client{};
    GetClientRect(dlg, &client);
    const int contentH = MeasureAboutHeight(g_ui, client.right);

    // MeasureAboutHeight 算的是客户区内容高度；窗口外部还要算上标题栏
    // 和边框，否则会挤掉底部（导致“确定”按钮被裁掉）。最小高度保底 285px。
    RECT outer{0, 0, w, std::max(contentH, ui::Scale(285, p))};
    AdjustWindowRectEx(&outer, WS_CAPTION | WS_SYSMENU, FALSE, 0);
    const int outerH = outer.bottom - outer.top;

    SetWindowPos(dlg, nullptr, 0, 0, w, outerH, SWP_NOMOVE | SWP_NOZORDER);

    GetClientRect(dlg, &client);
    LayoutAboutDialog(g_ui, client);
    PlaceNearCursor(dlg, w, outerH);

    RunModal(dlg);
}

// ================================================================ 自绘右键菜单
//
// TrackPopupMenu 画出来的是系统菜单，深色模式下依然是白底黑字，
// 而且没法改圆角/间距。这里完全自己画：
//   - 一个 WS_POPUP 窗口当菜单
//   - 鼠标悬停高亮、点击触发、点外面自动关
//   - 配色走 ui::Palette，跟随当前主题

struct PopupMenu {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool dark = true;
    std::vector<ui::MenuItem> items;
    std::vector<int> ids;          // 与 items 一一对应，分隔符填 0
    std::vector<RECT> rowRects;    // 每项的位置
    int hot = -1;
    int chosen = 0;                // 最终选中的命令 ID
    HFONT font = nullptr;
    HFONT small = nullptr;
    int width = 0;
    int height = 0;
};

PopupMenu g_menu;

void LayoutPopupMenu(PopupMenu& m) {
    const UINT p = m.dpi;
    const int padY = ui::Scale(5, p);

    // 先算宽度
    HDC dc = GetDC(m.hwnd);
    int maxW = 0;
    for (const auto& it : m.items) {
        const int w = ui::MenuItemWidth(dc, it, m.font, m.small, p);
        if (w > maxW) maxW = w;
    }
    ReleaseDC(m.hwnd, dc);
    m.width = maxW;

    // 再排高度
    int y = padY;
    m.rowRects.clear();
    for (const auto& it : m.items) {
        const int ih = ui::MenuItemHeight(it, p);
        m.rowRects.push_back(RECT{0, y, m.width, y + ih});
        y += ih;
    }
    m.height = y + padY;
}

void PaintPopupMenu(PopupMenu& m) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(m.hwnd, &ps);
    RECT client{};
    GetClientRect(m.hwnd, &client);
    const ui::Palette pal = ui::GetPalette(m.dark);

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ ob = SelectObject(mem, bmp);

    // 菜单整体：圆角卡片 + 描边
    ui::FillRectC(mem, client, pal.menuBg);

    for (size_t i = 0; i < m.items.size(); ++i) {
        ui::DrawMenuItem(mem, m.rowRects[i], m.items[i], pal, m.dpi,
                         static_cast<int>(i) == m.hot, m.font, m.small);
    }

    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(m.hwnd, &ps);
}

int HitRow(PopupMenu& m, POINT pt) {
    for (size_t i = 0; i < m.rowRects.size(); ++i) {
        if (PtInRect(&m.rowRects[i], pt) && !m.items[i].separator) return static_cast<int>(i);
    }
    return -1;
}

LRESULT CALLBACK PopupMenuProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PopupMenu& m = g_menu;

    switch (msg) {
        case WM_CREATE:
            m.hwnd = hwnd;
            m.dpi = ui::DpiOf(hwnd);
            m.font = ui::MakeFont(ui::UiFace(), 14, FW_NORMAL, m.dpi);
            m.small = ui::MakeFont(ui::UiFace(), 12, FW_NORMAL, m.dpi);
            ui::ApplyModernFrame(hwnd, m.dark);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            PaintPopupMenu(m);
            return 0;

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int h = HitRow(m, pt);
            if (h != m.hot) {
                m.hot = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (m.hot != -1) {
                m.hot = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int h = HitRow(m, pt);
            if (h >= 0) {
                m.chosen = m.ids[h];
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_RBUTTONUP:
        case WM_KILLFOCUS:
            DestroyWindow(hwnd);
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                m.chosen = 0;
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DESTROY:
            if (m.font) DeleteObject(m.font);
            if (m.small) DeleteObject(m.small);
            m.font = m.small = nullptr;
            m.hwnd = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 显示菜单，返回选中的命令 ID（0 = 未选择）
int ShowPopupMenu(const std::vector<ui::MenuItem>& items, const std::vector<int>& ids) {
    static bool clsDone = false;
    const wchar_t* kCls = L"TrayThemeTogglePopupMenu";
    if (!clsDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PopupMenuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kCls;
        RegisterClassExW(&wc);
        clsDone = true;
    }

    g_menu = PopupMenu{};
    g_menu.dark = !IsSystemLight();
    g_menu.items = items;
    g_menu.ids = ids;
    g_menu.dpi = ui::DpiOf(g_hwnd);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kCls, L"", WS_POPUP, 0, 0, 10,
                                10, g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 0;

    LayoutPopupMenu(g_menu);

    POINT pt{};
    GetCursorPos(&pt);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);

    int x = pt.x;
    int y = pt.y;
    // 靠近屏幕边缘时翻转，别让菜单跑到屏幕外
    if (x + g_menu.width > mi.rcWork.right) x = pt.x - g_menu.width;
    if (y + g_menu.height > mi.rcWork.bottom) y = pt.y - g_menu.height;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, g_menu.width, g_menu.height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    // 必须抢焦点，否则点菜单外面关不掉
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 点击菜单以外的任何地方就关闭
        if (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN) {
            POINT p2{msg.pt.x, msg.pt.y};
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            if (!PtInRect(&wr, p2)) {
                DestroyWindow(hwnd);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return g_menu.chosen;
}

void ShowContextMenu() {
    const bool light = IsSystemLight();
    const bool autostart = IsAutostartEnabled();
    const std::wstring hk = FormatHotkey(g_hotkey.modifiers, g_hotkey.vk);

    std::vector<ui::MenuItem> items;
    std::vector<int> ids;

    auto add = [&](const std::wstring& text, const std::wstring& sc, int id, bool checked,
                   bool showCheck) {
        ui::MenuItem it;
        it.text = text;
        it.shortcut = sc;
        it.checked = checked;
        it.showCheck = showCheck;
        items.push_back(it);
        ids.push_back(id);
    };
    auto addSep = [&]() {
        ui::MenuItem it;
        it.separator = true;
        items.push_back(it);
        ids.push_back(0);
    };

    add(light ? L"切换为深色模式" : L"切换为浅色模式", hk, ID_TRAY_TOGGLE, false, false);
    addSep();
    add(L"开机自动启动", L"", ID_TRAY_AUTOSTART, autostart, true);
    add(L"启用全局快捷键", L"", ID_TRAY_HOTKEY, g_hotkey.enabled, true);
    add(L"设置快捷键…", hk, ID_TRAY_SETTINGS, false, false);
    addSep();
    add(L"关于", L"", ID_TRAY_ABOUT, false, false);
    add(L"退出", L"", ID_TRAY_EXIT, false, false);

    const int cmd = ShowPopupMenu(items, ids);

    switch (cmd) {
        case ID_TRAY_TOGGLE:
            ToggleTheme();
            break;
        case ID_TRAY_AUTOSTART:
            SetAutostart(!IsAutostartEnabled());
            break;
        case ID_TRAY_HOTKEY:
            g_hotkey.enabled = !g_hotkey.enabled;
            ApplyHotkey(false);
            SaveHotkeyConfig(g_hotkey);
            UpdateTray(IsSystemLight());
            break;
        case ID_TRAY_SETTINGS:
            ShowHotkeyDialog();
            break;
        case ID_TRAY_ABOUT:
            ShowAboutDialog(L"关于 主题切换器",
                            {L"双击托盘图标或按全局快捷键，即可在深色与浅色模式之间切换。",
                             L"· 右键菜单 →「设置快捷键…」可自定义组合键\n"
                             L"· 「开机自动启动」写入当前用户的启动项\n"
                             L"· 只修改当前用户的主题设置，不需要管理员权限",
                             L"提示：全局快捷键会优先于其他程序响应，建议使用 Ctrl+Alt 组合，"
                             L"避免和常用软件的单键快捷键冲突。"});
            break;
        case ID_TRAY_EXIT:
            DestroyWindow(g_hwnd);
            break;
        default:
            break;
    }
}

// ================================================================ 窗口过程

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            return 0;

        case WM_TRAYICON:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONDBLCLK:
                    ToggleTheme();
                    return 0;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowContextMenu();
                    return 0;
                default:
                    return 0;
            }

        case WM_HOTKEY:
            if (static_cast<int>(wParam) == kHotkeyId) ToggleTheme();
            return 0;

        case WM_APP + 3:  // 外部（--settings）请求打开快捷键设置
            ShowHotkeyDialog();
            return 0;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            // 用户可能在「设置」里手动改了主题，这里同步一下图标
            UpdateTray(IsSystemLight());
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            // 分辨率或缩放变了，按新的托盘图标尺寸重新加载
            RefreshIconMetrics();
            UpdateTray(IsSystemLight());
            return 0;

        case WM_ENDSESSION:
            RemoveTray();
            return 0;

        case WM_DESTROY:
            UnregisterHotKey(hwnd, kHotkeyId);
            RemoveTray();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    // 资源管理器重启后任务栏会重建，需要重新添加托盘图标
    if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg) {
        g_trayAdded = false;
        UpdateTray(IsSystemLight());
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ================================================================ 命令行

struct Args {
    bool toggle = false;
    bool diagnose = false;
    bool settings = false;
};

Args ParseArgs() {
    Args a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return a;
    for (int i = 1; i < argc; ++i) {
        if (lstrcmpiW(argv[i], L"--toggle") == 0 || lstrcmpiW(argv[i], L"/toggle") == 0) {
            a.toggle = true;
        } else if (lstrcmpiW(argv[i], L"--diagnose") == 0 || lstrcmpiW(argv[i], L"/diagnose") == 0) {
            a.diagnose = true;
        } else if (lstrcmpiW(argv[i], L"--settings") == 0 ||
                   lstrcmpiW(argv[i], L"/settings") == 0) {
            a.settings = true;
        }
    }
    LocalFree(argv);
    return a;
}

// ================================================================ 自检

int RunDiagnose() {
    std::wstring logPath = ExeDir() + L"\\diagnose.log";
    _wfopen_s(&g_log, logPath.c_str(), L"w, ccs=UTF-8");
    if (!g_log) return 1;

    LogLine(L"=== TrayThemeToggle 自检 ===\n\n");

    // 1) 系统信息
    LogLine(L"[系统]\n");
    LogLine(L"  可执行文件: %ls\n", ExePath().c_str());
    {
        OSVERSIONINFOEXW vi = {};
        vi.dwOSVersionInfoSize = sizeof(vi);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        if (GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&vi))) {
            LogLine(L"  系统版本: %lu.%lu build %lu\n", vi.dwMajorVersion, vi.dwMinorVersion,
                    vi.dwBuildNumber);
        } else {
            LogLine(L"  系统版本: 读取失败\n");
        }
#pragma GCC diagnostic pop
    }
    LogLine(L"  小图标尺寸: %dx%d\n", GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON));

    // 2) 注册表读取
    LogLine(L"\n[注册表读取]\n");
    DWORD sys = 0xFFFFFFFF, app = 0xFFFFFFFF, cp = 0xFFFFFFFF;
    bool okSys = RegReadDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme", &sys);
    bool okApp = RegReadDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", &app);
    bool okCp = RegReadDword(HKEY_CURRENT_USER, kPersonalizeKey, L"ColorPrevalence", &cp);
    LogLine(L"  SystemUsesLightTheme: %ls (%lu)\n", okSys ? L"OK" : L"读取失败", sys);
    LogLine(L"  AppsUseLightTheme   : %ls (%lu)\n", okApp ? L"OK" : L"读取失败", app);
    LogLine(L"  ColorPrevalence     : %ls (%lu)\n", okCp ? L"OK" : L"读取失败", cp);
    LogLine(L"  当前判定为: %ls\n", IsSystemLight() ? L"浅色" : L"深色");

    // 3) 注册表写入（写回原值，不改变状态）
    LogLine(L"\n[注册表写入]\n");
    DWORD keep = okSys ? sys : 1;
    bool w = RegWriteDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme", keep);
    LogLine(L"  写入 SystemUsesLightTheme=%lu : %ls\n", keep, w ? L"OK" : L"失败（被拒绝）");

    // 4) 图标资源
    LogLine(L"\n[图标资源]\n");
    for (int id : {IDI_SUN, IDI_MOON, IDI_APP}) {
        HICON i16 = LoadIconRes(id, 16, 16);
        HICON i32 = LoadIconRes(id, 32, 32);
        LogLine(L"  id=%d  16px=%p  32px=%p\n", id, i16, i32);
        if (i16) DestroyIcon(i16);
        if (i32) DestroyIcon(i32);
    }

    // 5) 托盘注册
    LogLine(L"\n[托盘]\n");
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TrayThemeToggleDiagWnd";
    RegisterClassExW(&wc);
    HWND probe = CreateWindowExW(WS_EX_TOOLWINDOW, L"TrayThemeToggleDiagWnd", L"", WS_POPUP, 0, 0,
                                 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!probe) {
        LogLine(L"  创建测试窗口失败\n");
    } else {
        HICON ic = LoadIconRes(IDI_SUN, GetSystemMetrics(SM_CXSMICON),
                               GetSystemMetrics(SM_CYSMICON));
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = probe;
        nid.uID = 999;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_APP + 9;
        nid.hIcon = ic;
        wcscpy_s(nid.szTip, L"TrayThemeToggle 自检");
        BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
        DWORD err = GetLastError();
        LogLine(L"  Shell_NotifyIcon(ADD): %ls (err=%lu)\n", ok ? L"OK" : L"失败", ok ? 0 : err);
        if (!ok) {
            LogLine(L"  提示: err=5 表示权限/完整性级别不足（例如以低权限沙箱运行），\n");
            LogLine(L"        正常双击运行时进程为普通用户权限，不会出现该问题。\n");
        } else {
            Sleep(300);
            Shell_NotifyIconW(NIM_DELETE, &nid);
            LogLine(L"  已成功加入托盘并移除（说明托盘链路正常）\n");
        }
        if (ic) DestroyIcon(ic);
        DestroyWindow(probe);
    }

    // 6) 快捷键
    LogLine(L"\n[快捷键]\n");
    {
        HotkeyConfig cfg = LoadHotkeyConfig();
        LogLine(L"  配置: %ls  启用=%d\n", FormatHotkey(cfg.modifiers, cfg.vk).c_str(),
                cfg.enabled ? 1 : 0);
        if (cfg.enabled && cfg.vk != 0) {
            BOOL hk = RegisterHotKey(nullptr, 0xBEEF, cfg.modifiers | MOD_NOREPEAT, cfg.vk);
            DWORD hkErr = GetLastError();
            LogLine(L"  试注册: %ls (err=%lu)\n", hk ? L"可用" : L"被占用", hk ? 0 : hkErr);
            if (hk) UnregisterHotKey(nullptr, 0xBEEF);
        }
        // 顺便报一下默认组合是否可用，方便用户判断要不要换
        if (!(cfg.modifiers == kDefaultHotkey.modifiers && cfg.vk == kDefaultHotkey.vk)) {
            BOOL d = RegisterHotKey(nullptr, 0xBEEE, kDefaultHotkey.modifiers | MOD_NOREPEAT,
                                    kDefaultHotkey.vk);
            LogLine(L"  默认 %ls: %ls\n", FormatHotkey(kDefaultHotkey.modifiers, kDefaultHotkey.vk).c_str(),
                    d ? L"可用" : L"被占用");
            if (d) UnregisterHotKey(nullptr, 0xBEEE);
        }
    }

    // 7) 开机自启
    LogLine(L"\n[开机自启]\n");
    LogLine(L"  当前: %ls\n", IsAutostartEnabled() ? L"已启用" : L"未启用");

    // 8) 进程完整性级别
    LogLine(L"\n[进程权限]\n");
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD len = 0;
        GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &len);
        BYTE buf[512];
        if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &len)) {
            auto* tml = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf);
            DWORD rid = *GetSidSubAuthority(tml->Label.Sid,
                                            *GetSidSubAuthorityCount(tml->Label.Sid) - 1);
            const wchar_t* lvl = L"未知";
            if (rid == 0x1000) lvl = L"低 (Low)";
            else if (rid == 0x2000) lvl = L"中 (Medium，正常)";
            else if (rid == 0x3000) lvl = L"高 (High，管理员)";
            LogLine(L"  完整性级别: 0x%lX  %ls\n", rid, lvl);
        }
        CloseHandle(tok);
    }

    LogLine(L"\n=== 自检结束 ===\n");
    fclose(g_log);
    g_log = nullptr;

    std::wstring msg = L"自检完成，结果已写入：\n" + logPath;
    ShowAboutDialog(L"自检", {msg});
    return 0;
}

}  // namespace

// ================================================================ 入口

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    const Args args = ParseArgs();

    if (args.diagnose) return RunDiagnose();

    // 先建互斥体判断是否已有实例。注意：--toggle 必须在下面「已存在就退出」之前处理，
    // 否则 GUI 常驻时再调用 --toggle 会被当成重复启动直接退出，什么也不做。
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    const bool alreadyRunning = (mutex != nullptr) && (GetLastError() == ERROR_ALREADY_EXISTS);

    if (args.toggle) {
        if (alreadyRunning) {
            // 已有实例在跑：让它自己切换，这样托盘图标能同步刷新。
            // 约定用 WM_APP+2 作为「外部请求翻转」的私有消息。
            HWND target = FindWindowW(kClassName, nullptr);
            if (target) {
                PostMessageW(target, WM_TRAYICON, 0, MAKELPARAM(WM_LBUTTONDBLCLK, 0));
                if (mutex) CloseHandle(mutex);
                return 0;
            }
            // 找不到窗口就自己动手（例如 GUI 刚好在退出）
        }
        // 没有实例在跑：自己翻转一次然后退出，不建窗口、不显示托盘
        ApplyTheme(!IsSystemLight());
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    if (alreadyRunning) {
        // --settings：让常驻实例弹出快捷键设置窗口
        if (args.settings) {
            HWND target = FindWindowW(kClassName, nullptr);
            if (target) {
                PostMessageW(target, WM_APP + 3, 0, 0);  // WM_APP+3 = 打开设置
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // 预加载大图标（托盘会在运行时按 DPI 重新按尺寸加载）
    g_iconSun = LoadIconRes(IDI_SUN, 0, 0);
    g_iconMoon = LoadIconRes(IDI_MOON, 0, 0);
    g_iconApp = LoadIconRes(IDI_APP, 0, 0);
    RefreshIconMetrics();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hIcon = g_iconApp;
    wc.hIconSm = g_iconApp;
    if (!RegisterClassExW(&wc)) {
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    // 纯消息窗口：不可见、不进任务栏、不抢焦点
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP, 0, 0,
                                0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // 读取保存的快捷键配置（没配过就用默认 Ctrl+Alt+S）并注册
    g_hotkey = LoadHotkeyConfig();
    ApplyHotkey(true);

    UpdateTray(IsSystemLight());

    if (g_hotkeyRegistered) {
        std::wstring tip = L"双击托盘图标，或按 " +
                           FormatHotkey(g_hotkey.modifiers, g_hotkey.vk) + L" 切换深浅色";
        ShowBalloon(L"主题切换器已启动", tip.c_str());
    } else if (!g_hotkey.enabled) {
        ShowBalloon(L"主题切换器已启动", L"双击托盘图标切换深浅色（快捷键已关闭）");
    } else {
        ShowBalloon(L"主题切换器已启动",
                    L"快捷键注册失败，可在托盘右键菜单里重新设置");
    }

    // --settings 且当前没有常驻实例：启动后直接把设置窗口弹出来
    if (args.settings) {
        PostMessageW(hwnd, WM_APP + 3, 0, 0);
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_iconSun) DestroyIcon(g_iconSun);
    if (g_iconMoon) DestroyIcon(g_iconMoon);
    if (g_iconApp) DestroyIcon(g_iconApp);
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
