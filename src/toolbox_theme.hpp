#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace toolbox_theme {

constexpr COLORREF kCanvas = RGB(243, 244, 240);
constexpr COLORREF kPaper = RGB(255, 255, 255);
constexpr COLORREF kInk = RGB(16, 17, 15);
constexpr COLORREF kMuted = RGB(76, 80, 72);
constexpr COLORREF kLine = RGB(211, 214, 207);
constexpr COLORREF kCoral = RGB(255, 101, 79);
constexpr COLORREF kBlue = RGB(40, 100, 255);
constexpr COLORREF kLime = RGB(200, 244, 61);
constexpr COLORREF kSuccess = RGB(28, 124, 84);
constexpr COLORREF kDanger = RGB(217, 63, 50);

enum class Icon {
    Palette,
    Panels,
    Gauge,
    FolderPlus,
    RotateCcw,
    RefreshCw,
    Check,
    Monitor,
    Undo,
    Zap,
};

inline int scale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

inline HFONT create_font(UINT dpi, int pixels, int weight,
                         const wchar_t* family = L"Microsoft YaHei UI") {
    return CreateFontW(-scale(pixels, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, family);
}

inline void style_title_bar(HWND window) {
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) {
        return;
    }
    const auto set_attribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (set_attribute != nullptr) {
        const BOOL dark = TRUE;
        const COLORREF caption = kInk;
        const COLORREF text = RGB(255, 255, 255);
        const COLORREF border = kInk;
        set_attribute(window, 20, &dark, sizeof(dark));
        set_attribute(window, 34, &border, sizeof(border));
        set_attribute(window, 35, &caption, sizeof(caption));
        set_attribute(window, 36, &text, sizeof(text));
    }
    FreeLibrary(dwm);
}

inline void fill_rect(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

inline void fill_round_rect(HDC dc, const RECT& rect, int radius,
                            COLORREF fill, COLORREF stroke = CLR_INVALID,
                            int stroke_width = 1) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, stroke_width,
                         stroke == CLR_INVALID ? fill : stroke);
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
              radius * 2, radius * 2);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

inline void draw_grid(HDC dc, const RECT& client, UINT dpi, COLORREF accent) {
    fill_rect(dc, client, kCanvas);
    HPEN grid = CreatePen(PS_SOLID, 1, RGB(225, 227, 222));
    const HGDIOBJ previous = SelectObject(dc, grid);
    const int step = scale(42, dpi);
    for (int x = scale(38, dpi); x < client.right; x += step) {
        MoveToEx(dc, x, 0, nullptr);
        LineTo(dc, x, client.bottom);
    }
    for (int y = scale(42, dpi); y < client.bottom; y += step) {
        MoveToEx(dc, 0, y, nullptr);
        LineTo(dc, client.right, y);
    }
    SelectObject(dc, previous);
    DeleteObject(grid);

    HPEN frame = CreatePen(PS_SOLID, 1, kLine);
    SelectObject(dc, frame);
    MoveToEx(dc, 0, scale(82, dpi), nullptr);
    LineTo(dc, client.right, scale(82, dpi));
    MoveToEx(dc, scale(38, dpi), 0, nullptr);
    LineTo(dc, scale(38, dpi), client.bottom);
    MoveToEx(dc, client.right - scale(38, dpi), 0, nullptr);
    LineTo(dc, client.right - scale(38, dpi), client.bottom);
    SelectObject(dc, previous);
    DeleteObject(frame);

    HPEN signal = CreatePen(PS_SOLID, scale(3, dpi), accent);
    SelectObject(dc, signal);
    MoveToEx(dc, scale(120, dpi), scale(82, dpi), nullptr);
    LineTo(dc, scale(168, dpi), scale(82, dpi));
    SelectObject(dc, previous);
    DeleteObject(signal);
}

inline void set_text(HDC dc, COLORREF color, HFONT font) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    SelectObject(dc, font);
}

inline void draw_text(HDC dc, const std::wstring& text, RECT rect, HFONT font,
                      COLORREF color, UINT format = DT_LEFT | DT_VCENTER |
                                                       DT_SINGLELINE |
                                                       DT_END_ELLIPSIS) {
    const HGDIOBJ previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              format | DT_NOPREFIX);
    SelectObject(dc, previous);
}

inline POINT icon_point(const RECT& rect, float x, float y) {
    return POINT{
        rect.left + static_cast<LONG>((rect.right - rect.left) * x / 24.0f + 0.5f),
        rect.top + static_cast<LONG>((rect.bottom - rect.top) * y / 24.0f + 0.5f)};
}

inline void icon_line(HDC dc, const RECT& rect, float x1, float y1,
                      float x2, float y2) {
    const POINT first = icon_point(rect, x1, y1);
    const POINT second = icon_point(rect, x2, y2);
    MoveToEx(dc, first.x, first.y, nullptr);
    LineTo(dc, second.x, second.y);
}

inline void icon_polyline(HDC dc, const RECT& rect, const float* points,
                          int count) {
    POINT output[12]{};
    count = std::min(count, 12);
    for (int index = 0; index < count; ++index) {
        output[index] = icon_point(rect, points[index * 2], points[index * 2 + 1]);
    }
    Polyline(dc, output, count);
}

inline void draw_icon(HDC dc, Icon icon, const RECT& rect, COLORREF color,
                      int stroke = 2) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, stroke), color);
    HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
    const HGDIOBJ previous_pen = SelectObject(dc, pen);
    const HGDIOBJ previous_brush = SelectObject(dc, hollow);

    const auto ellipse = [&](float x, float y, float width, float height) {
        const POINT top_left = icon_point(rect, x, y);
        const POINT bottom_right = icon_point(rect, x + width, y + height);
        Ellipse(dc, top_left.x, top_left.y, bottom_right.x, bottom_right.y);
    };

    switch (icon) {
    case Icon::Palette:
        ellipse(2, 2, 20, 20);
        ellipse(8, 7, 1, 1);
        ellipse(6, 12, 1, 1);
        ellipse(13, 6, 1, 1);
        ellipse(17, 10, 1, 1);
        break;
    case Icon::Panels: {
        const POINT top_left = icon_point(rect, 3, 3);
        const POINT bottom_right = icon_point(rect, 21, 21);
        RoundRect(dc, top_left.x, top_left.y, bottom_right.x, bottom_right.y,
                  scale(3, 96), scale(3, 96));
        icon_line(dc, rect, 3, 9, 21, 9);
        icon_line(dc, rect, 9, 9, 9, 21);
        break;
    }
    case Icon::Gauge:
        ellipse(3, 3, 18, 18);
        icon_line(dc, rect, 12, 14, 16, 10);
        break;
    case Icon::FolderPlus: {
        const float path[] = {2, 19, 2, 6, 9, 6, 11, 8, 22, 8, 22, 19, 2, 19};
        icon_polyline(dc, rect, path, 7);
        icon_line(dc, rect, 12, 12, 12, 17);
        icon_line(dc, rect, 9.5f, 14.5f, 14.5f, 14.5f);
        break;
    }
    case Icon::RotateCcw: {
        const float arrow[] = {3, 7, 3, 3, 7, 3};
        icon_polyline(dc, rect, arrow, 3);
        Arc(dc, icon_point(rect, 3, 3).x, icon_point(rect, 3, 3).y,
            icon_point(rect, 21, 21).x, icon_point(rect, 21, 21).y,
            icon_point(rect, 3, 7).x, icon_point(rect, 3, 7).y,
            icon_point(rect, 20, 13).x, icon_point(rect, 20, 13).y);
        break;
    }
    case Icon::RefreshCw: {
        const float top[] = {20, 7, 20, 3, 16, 3};
        const float bottom[] = {4, 17, 4, 21, 8, 21};
        icon_polyline(dc, rect, top, 3);
        icon_polyline(dc, rect, bottom, 3);
        Arc(dc, icon_point(rect, 3, 3).x, icon_point(rect, 3, 3).y,
            icon_point(rect, 21, 21).x, icon_point(rect, 21, 21).y,
            icon_point(rect, 20, 7).x, icon_point(rect, 20, 7).y,
            icon_point(rect, 4, 17).x, icon_point(rect, 4, 17).y);
        break;
    }
    case Icon::Check: {
        const float check[] = {5, 12, 10, 17, 20, 7};
        icon_polyline(dc, rect, check, 3);
        break;
    }
    case Icon::Monitor: {
        const POINT top_left = icon_point(rect, 2, 3);
        const POINT bottom_right = icon_point(rect, 22, 17);
        RoundRect(dc, top_left.x, top_left.y, bottom_right.x, bottom_right.y,
                  scale(3, 96), scale(3, 96));
        icon_line(dc, rect, 8, 21, 16, 21);
        icon_line(dc, rect, 12, 17, 12, 21);
        break;
    }
    case Icon::Undo: {
        const float arrow[] = {9, 7, 4, 12, 9, 17};
        icon_polyline(dc, rect, arrow, 3);
        icon_line(dc, rect, 4, 12, 14, 12);
        Arc(dc, icon_point(rect, 8, 7).x, icon_point(rect, 8, 7).y,
            icon_point(rect, 22, 21).x, icon_point(rect, 22, 21).y,
            icon_point(rect, 14, 12).x, icon_point(rect, 14, 12).y,
            icon_point(rect, 20, 18).x, icon_point(rect, 20, 18).y);
        break;
    }
    case Icon::Zap: {
        const float bolt[] = {13, 2, 4, 14, 11, 14, 10, 22, 20, 9, 13, 9, 13, 2};
        icon_polyline(dc, rect, bolt, 7);
        break;
    }
    }

    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

inline HICON create_app_icon(int size, Icon icon, COLORREF accent) {
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    const HGDIOBJ previous = SelectObject(memory, color);
    RECT bounds{0, 0, size, size};
    fill_round_rect(memory, bounds, std::max(2, size / 6), kInk);
    const int inset = std::max(3, size / 5);
    RECT icon_bounds{inset, inset, size - inset, size - inset};
    draw_icon(memory, icon, icon_bounds, accent, std::max(1, size / 16));
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON result = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return result;
}

inline void draw_button(const DRAWITEMSTRUCT& item, const std::wstring& text,
                        Icon icon, UINT dpi, COLORREF accent,
                        bool primary = false) {
    RECT rect = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const COLORREF fill = disabled
                              ? RGB(225, 227, 222)
                              : primary ? (pressed ? RGB(12, 13, 12) : kInk)
                                        : (pressed ? RGB(232, 233, 229) : kPaper);
    const COLORREF foreground = disabled ? RGB(140, 143, 136)
                                          : primary ? kPaper : kInk;
    if (pressed) {
        OffsetRect(&rect, 0, scale(1, dpi));
    }
    fill_round_rect(item.hDC, rect, scale(6, dpi), fill,
                    primary ? kInk : kLine, 1);

    RECT marker{rect.left, rect.top, rect.left + scale(5, dpi), rect.bottom};
    fill_round_rect(item.hDC, marker, scale(3, dpi), accent);
    RECT icon_rect{rect.left + scale(16, dpi),
                   rect.top + (rect.bottom - rect.top - scale(20, dpi)) / 2,
                   rect.left + scale(36, dpi),
                   rect.top + (rect.bottom - rect.top + scale(20, dpi)) / 2};
    draw_icon(item.hDC, icon, icon_rect, foreground, scale(2, dpi));

    HFONT font = create_font(dpi, 13, FW_SEMIBOLD);
    RECT text_rect{rect.left + scale(46, dpi), rect.top,
                   rect.right - scale(12, dpi), rect.bottom};
    draw_text(item.hDC, text, text_rect, font, foreground);
    DeleteObject(font);

    if (focused && !disabled) {
        RECT focus = rect;
        InflateRect(&focus, -scale(5, dpi), -scale(5, dpi));
        DrawFocusRect(item.hDC, &focus);
    }
}

inline void draw_combo(const DRAWITEMSTRUCT& item, UINT dpi) {
    const bool selected = (item.itemState & ODS_SELECTED) != 0 &&
                          (item.itemState & ODS_COMBOBOXEDIT) == 0;
    const COLORREF background = selected ? kBlue : kPaper;
    const COLORREF foreground = selected ? kPaper : kInk;
    RECT rect = item.rcItem;
    fill_rect(item.hDC, rect, background);

    std::wstring text;
    if (item.itemID == static_cast<UINT>(-1)) {
        const int length = GetWindowTextLengthW(item.hwndItem);
        text.resize(static_cast<std::size_t>(std::max(0, length)) + 1);
        if (length > 0) {
            GetWindowTextW(item.hwndItem, text.data(), length + 1);
        }
        text.resize(static_cast<std::size_t>(std::max(0, length)));
    } else {
        const LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN,
                                            item.itemID, 0);
        if (length > 0) {
            text.resize(static_cast<std::size_t>(length));
            SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID,
                         reinterpret_cast<LPARAM>(text.data()));
        }
    }
    HFONT font = create_font(dpi, 14, FW_NORMAL);
    RECT text_rect{rect.left + scale(14, dpi), rect.top,
                   rect.right - scale(34, dpi), rect.bottom};
    draw_text(item.hDC, text, text_rect, font, foreground);
    DeleteObject(font);

}

}  // namespace toolbox_theme
