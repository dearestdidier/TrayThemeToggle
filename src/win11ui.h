// ============================================================================
//  win11ui.h —— Win11 风格的轻量 UI 辅助层
//
//  只做三件事：
//    1. 一套跟随深浅色主题的调色板
//    2. 圆角矩形 / 描边 / 字体 / DWM 圆角与深色标题栏
//    3. 自绘菜单项的度量与绘制（Win32 原生菜单不跟深色主题，只能自己画）
//
//  全部 inline，不引入额外编译单元。
// ============================================================================
#pragma once

#ifndef WIN11UI_H
#define WIN11UI_H

#include <windows.h>
#include <dwmapi.h>
#include <string>
#include <vector>

namespace ui {

// ---------------------------------------------------------------- DPI

inline int Scale(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

inline UINT DpiOf(HWND hwnd) {
    if (hwnd) {
        const UINT d = GetDpiForWindow(hwnd);
        if (d != 0) return d;
    }
    HDC dc = GetDC(nullptr);
    const UINT d = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY)) : 96u;
    if (dc) ReleaseDC(nullptr, dc);
    return d ? d : 96u;
}

// ---------------------------------------------------------------- 调色板
//
// 取色参考 Win11 内置应用的深/浅色表面色，保证和系统 UI 不打架。

struct Palette {
    bool dark = true;
    COLORREF windowBg;       // 窗口底色
    COLORREF surface;        // 输入框 / 卡片
    COLORREF text;           // 主文字
    COLORREF textSecondary;  // 次要文字
    COLORREF border;         // 边框
    COLORREF accent;         // 强调色（主按钮）
    COLORREF accentHover;
    COLORREF accentPressed;
    COLORREF onAccent;       // 强调色上的文字
    COLORREF buttonBg;
    COLORREF buttonHover;
    COLORREF buttonPressed;
    COLORREF buttonBorder;
    COLORREF menuBg;
    COLORREF menuHover;
    COLORREF menuBorder;
    COLORREF separator;
};

inline COLORREF Make(BYTE r, BYTE g, BYTE b) { return RGB(r, g, b); }

inline Palette GetPalette(bool dark) {
    Palette p{};
    p.dark = dark;
    if (dark) {
        p.windowBg = Make(32, 32, 32);
        p.surface = Make(45, 45, 45);
        p.text = Make(255, 255, 255);
        p.textSecondary = Make(158, 158, 158);
        p.border = Make(64, 64, 64);
        p.accent = Make(96, 205, 255);        // Win11 深色强调色
        p.accentHover = Make(130, 218, 255);
        p.accentPressed = Make(72, 182, 232);
        p.onAccent = Make(0, 0, 0);
        p.buttonBg = Make(48, 48, 48);
        p.buttonHover = Make(60, 60, 60);
        p.buttonPressed = Make(40, 40, 40);
        p.buttonBorder = Make(70, 70, 70);
        p.menuBg = Make(43, 43, 43);
        p.menuHover = Make(62, 62, 62);
        p.menuBorder = Make(74, 74, 74);
        p.separator = Make(72, 72, 72);
    } else {
        p.windowBg = Make(243, 243, 243);
        p.surface = Make(255, 255, 255);
        p.text = Make(26, 26, 26);
        p.textSecondary = Make(93, 93, 93);
        p.border = Make(214, 214, 214);
        p.accent = Make(0, 103, 192);         // Win11 浅色强调色
        p.accentHover = Make(25, 117, 197);
        p.accentPressed = Make(0, 90, 164);
        p.onAccent = Make(255, 255, 255);
        p.buttonBg = Make(253, 253, 253);
        p.buttonHover = Make(245, 245, 245);
        p.buttonPressed = Make(237, 237, 237);
        p.buttonBorder = Make(214, 214, 214);
        p.menuBg = Make(249, 249, 249);
        p.menuHover = Make(232, 232, 232);
        p.menuBorder = Make(214, 214, 214);
        p.separator = Make(226, 226, 226);
    }
    return p;
}

// ---------------------------------------------------------------- 绘制基元

inline void FillRectC(HDC dc, const RECT& rc, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

// 实心圆角矩形。radius 是圆角半径（像素），RoundRect 需要椭圆宽高所以 *2。
inline void FillRound(HDC dc, const RECT& rc, int radius, COLORREF fill) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

// 只描边，不填充
inline void FrameRound(HDC dc, const RECT& rc, int radius, COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

// 填充 + 描边。描边画在内侧一圈，避免圆角处出现毛边。
inline void CardRound(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    FillRound(dc, rc, radius, fill);
    RECT inner = rc;
    InflateRect(&inner, -1, -1);
    FrameRound(dc, inner, radius > 0 ? radius - 1 : 0, border, 1);
}

inline void DrawTextIn(HDC dc, const RECT& rc, const wchar_t* text, HFONT font, COLORREF color,
                       UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT r = rc;
    DrawTextW(dc, text, -1, &r, format);
    SelectObject(dc, old);
}

inline HFONT MakeFont(const wchar_t* face, int px, int weight, UINT dpi) {
    return CreateFontW(-Scale(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       face);
}

// Win11 默认用 Segoe UI Variable；老系统会自动回落到 Segoe UI
inline const wchar_t* UiFace() { return L"Segoe UI Variable Text"; }

// ---------------------------------------------------------------- DWM 外观

// 深色标题栏 + 圆角 + 标题栏/边框配色
inline void ApplyModernFrame(HWND hwnd, bool dark) {
    if (!hwnd) return;

    BOOL useDark = dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE：20 是新编号，19 用于 Win10 1809~1909
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark)))) {
        DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
    }

    // DWMWA_WINDOW_CORNER_PREFERENCE = 33，DWMWCP_ROUND = 2
    int corner = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));

    // 让标题栏和边框跟内容区同色，否则深浅色下会出现一圈突兀的色带
    const Palette p = GetPalette(dark);
    COLORREF caption = p.windowBg;
    COLORREF textColor = p.text;
    COLORREF borderColor = p.border;
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));      // CAPTION_COLOR
    DwmSetWindowAttribute(hwnd, 36, &textColor, sizeof(textColor));  // TEXT_COLOR
    DwmSetWindowAttribute(hwnd, 34, &borderColor, sizeof(borderColor));  // BORDER_COLOR
}

// ---------------------------------------------------------------- 自绘菜单
//
// Win32 的 TrackPopupMenu 完全不受深色主题影响，只能把每一项设成 MF_OWNERDRAW
// 自己画。这样颜色、圆角、间距都能和对话框保持一致。

struct MenuItem {
    std::wstring text;
    std::wstring shortcut;   // 右侧的快捷键提示
    bool separator = false;
    bool checked = false;
    bool showCheck = false;  // 预留勾选位（保持文字对齐）
    bool disabled = false;
};

// 菜单项高度
inline int MenuItemHeight(const MenuItem& it, UINT dpi) {
    return it.separator ? Scale(9, dpi) : Scale(32, dpi);
}

// 计算菜单需要的宽度（取所有项的最大值）
inline int MenuItemWidth(HDC dc, const MenuItem& it, HFONT font, HFONT small, UINT dpi) {
    if (it.separator) return Scale(120, dpi);

    HGDIOBJ old = SelectObject(dc, font);
    SIZE ts{};
    GetTextExtentPoint32W(dc, it.text.c_str(), static_cast<int>(it.text.size()), &ts);

    SIZE ss{};
    if (!it.shortcut.empty()) {
        SelectObject(dc, small);
        GetTextExtentPoint32W(dc, it.shortcut.c_str(), static_cast<int>(it.shortcut.size()), &ss);
    }
    SelectObject(dc, old);

    const int padLeft = Scale(10, dpi);
    const int checkCol = Scale(22, dpi);
    const int gap = Scale(26, dpi);
    const int padRight = Scale(18, dpi);
    return padLeft + checkCol + ts.cx + gap + ss.cx + padRight;
}

inline void DrawCheckMark(HDC dc, int cx, int cy, int size, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, size >= 16 ? 2 : 1, color);
    HGDIOBJ op = SelectObject(dc, pen);
    // 两段折线画勾
    const int x0 = cx - size / 2;
    const int y0 = cy;
    MoveToEx(dc, x0 + size / 5, y0, nullptr);
    LineTo(dc, x0 + size / 2 - size / 10, y0 + size / 3);
    LineTo(dc, x0 + size - size / 8, y0 - size / 3);
    SelectObject(dc, op);
    DeleteObject(pen);
}

inline void DrawMenuItem(HDC dc, const RECT& rc, const MenuItem& it, const Palette& p, UINT dpi,
                         bool selected, HFONT font, HFONT small) {
    if (it.separator) {
        const int y = (rc.top + rc.bottom) / 2;
        RECT line{rc.left + Scale(12, dpi), y, rc.right - Scale(12, dpi), y + 1};
        if (line.right > line.left) FillRectC(dc, line, p.separator);
        return;
    }

    // 悬停高亮：整行铺满，圆角小一点，避免视觉上缩进
    if (selected && !it.disabled) {
        RECT hl = rc;
        hl.left += Scale(4, dpi);
        hl.right -= Scale(4, dpi);
        hl.top += Scale(1, dpi);
        hl.bottom -= Scale(1, dpi);
        FillRound(dc, hl, Scale(5, dpi), p.menuHover);
    }

    const COLORREF fg = it.disabled ? p.textSecondary : p.text;
    const int cy = (rc.top + rc.bottom) / 2;

    // 勾选标记
    if (it.checked) {
        const int cx = rc.left + Scale(10, dpi) + Scale(9, dpi);
        DrawCheckMark(dc, cx, cy, Scale(13, dpi), it.disabled ? p.textSecondary : p.accent);
    }

    // 主文字
    RECT tr = rc;
    tr.left = rc.left + Scale(10, dpi) + Scale(22, dpi);
    tr.right = rc.right - Scale(18, dpi);
    DrawTextIn(dc, tr, it.text.c_str(), font, fg,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 快捷键提示右对齐
    if (!it.shortcut.empty()) {
        RECT sr = rc;
        sr.left = rc.left + Scale(120, dpi);
        sr.right = rc.right - Scale(18, dpi);
        DrawTextIn(dc, sr, it.shortcut.c_str(), small,
                   it.disabled ? p.textSecondary : p.textSecondary,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

}  // namespace ui

#endif  // WIN11UI_H
