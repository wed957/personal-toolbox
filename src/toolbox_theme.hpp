#pragma once

// Shared design system for the toolbox component windows.
// Rendering goes through GDI+ (same pipeline as the launcher): anti-aliased
// shapes and ClearType-grid-fit text on top of the paper-grid backdrop.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace toolbox_theme {

constexpr COLORREF kCanvas = RGB(243, 244, 241);
constexpr COLORREF kPaper = RGB(255, 255, 255);
constexpr COLORREF kInk = RGB(16, 17, 15);
constexpr COLORREF kMuted = RGB(106, 110, 100);
constexpr COLORREF kLine = RGB(211, 214, 207);
constexpr COLORREF kCoral = RGB(255, 101, 79);
constexpr COLORREF kBlue = RGB(40, 100, 255);
constexpr COLORREF kLime = RGB(200, 244, 61);
constexpr COLORREF kSuccess = RGB(28, 124, 84);
constexpr COLORREF kDanger = RGB(217, 63, 50);

// Tile index of each component inside the shared 2x2 icon-family motif.
enum FamilyTile {
    kTileIcc = 0,
    kTileMux = 1,
    kTileIyx = 2,
};

enum class Icon {
    Palette,
    Panels,
    Gauge,
    Keyboard,
    FolderPlus,
    FolderOpen,
    ArrowUpRight,
    CircleCheck,
    CircleAlert,
    Activity,
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

inline ULONG_PTR init_gdi_plus() {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    return token;
}

inline void shut_down_gdi_plus(ULONG_PTR token) {
    if (token != 0) {
        Gdiplus::GdiplusShutdown(token);
    }
}

// COLORREF layout is 0x00BBGGRR: red lives in the low byte.
inline Gdiplus::Color color(unsigned int rgb, BYTE alpha = 255) {
    return Gdiplus::Color(alpha,
                          static_cast<BYTE>(rgb & 0xff),
                          static_cast<BYTE>((rgb >> 8) & 0xff),
                          static_cast<BYTE>((rgb >> 16) & 0xff));
}

// ---------------------------------------------------------------------------
// Font plumbing: native controls still take HFONT (owner-draw code derives the
// matching GDI+ font from the same LOGFONT so both paths agree).
// ---------------------------------------------------------------------------
inline HFONT create_font(UINT dpi, int pixels, int weight,
                         const wchar_t* family = L"Microsoft YaHei UI") {
    return CreateFontW(-scale(pixels, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, family);
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
        // Light chrome to match the launcher shell.
        const BOOL dark = FALSE;
        const COLORREF caption = kCanvas;
        const COLORREF text = kInk;
        const COLORREF border = RGB(228, 230, 223);
        set_attribute(window, 20, &dark, sizeof(dark));
        set_attribute(window, 34, &border, sizeof(border));
        set_attribute(window, 35, &caption, sizeof(caption));
        set_attribute(window, 36, &text, sizeof(text));
    }
    FreeLibrary(dwm);
}

namespace detail {

struct FontSpec {
    std::wstring family;
    float pixels;
    int style;

    bool operator<(const FontSpec& other) const {
        if (family != other.family) return family < other.family;
        if (pixels != other.pixels) return pixels < other.pixels;
        return style < other.style;
    }
};

inline const Gdiplus::FontFamily* resolve_family(const std::wstring& name) {
    static std::map<std::wstring, Gdiplus::FontFamily*> resolved;
    const auto found = resolved.find(name);
    if (found != resolved.end()) {
        return found->second;
    }
    auto* family = new Gdiplus::FontFamily(name.c_str());
    if (family->GetLastStatus() != Gdiplus::Ok) {
        delete family;
        family = new Gdiplus::FontFamily(L"Segoe UI");
        if (family->GetLastStatus() != Gdiplus::Ok) {
            delete family;
            wchar_t generic_name[LF_FACESIZE]{};
                Gdiplus::FontFamily::GenericSansSerif()->GetFamilyName(generic_name);
                family = new Gdiplus::FontFamily(generic_name);
        }
    }
    resolved[name] = family;
    return family;
}

inline int style_from_logfont(const LOGFONTW& lf) {
    int style = Gdiplus::FontStyleRegular;
    if (lf.lfWeight >= FW_BOLD) {
        style |= Gdiplus::FontStyleBold;
    } else if (lf.lfWeight >= FW_SEMIBOLD) {
        style |= Gdiplus::FontStyleBold;  // GDI+ has no semibold; approximate.
    }
    if (lf.lfItalic) {
        style |= Gdiplus::FontStyleItalic;
    }
    return style;
}

inline const Gdiplus::Font& font_for(HFONT handle) {
    static std::map<HFONT, Gdiplus::Font*> cache;
    const auto found = cache.find(handle);
    if (found != cache.end()) {
        return *found->second;
    }
    LOGFONTW lf{};
    GetObjectW(handle, sizeof(lf), &lf);
    const float pixels = lf.lfHeight < 0
                             ? static_cast<float>(-lf.lfHeight)
                             : static_cast<float>(lf.lfHeight);
    auto* font = new Gdiplus::Font(resolve_family(lf.lfFaceName), pixels,
                                   style_from_logfont(lf), Gdiplus::UnitPixel);
    cache[handle] = font;
    return *font;
}

inline void configure(Gdiplus::Graphics& graphics) {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    graphics.SetTextContrast(0);
}

inline Gdiplus::RectF to_rectf(const RECT& rect) {
    return Gdiplus::RectF(static_cast<Gdiplus::REAL>(rect.left),
                          static_cast<Gdiplus::REAL>(rect.top),
                          static_cast<Gdiplus::REAL>(rect.right - rect.left),
                          static_cast<Gdiplus::REAL>(rect.bottom - rect.top));
}

inline void rounded_path(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect,
                         float radius) {
    const float diameter =
        std::min(radius * 2.0f, std::min(rect.Width, rect.Height));
    if (diameter <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f,
                90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f,
                90.0f);
    path.CloseFigure();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
inline void fill_rect(HDC dc, const RECT& rect, COLORREF fill) {
    Gdiplus::SolidBrush brush(color(fill));
    Gdiplus::Graphics graphics(dc);
    detail::configure(graphics);
    graphics.FillRectangle(&brush, detail::to_rectf(rect));
}

inline void fill_round_rect(HDC dc, const RECT& rect, int radius,
                            COLORREF fill, COLORREF stroke = CLR_INVALID,
                            int stroke_width = 1) {
    Gdiplus::Graphics graphics(dc);
    detail::configure(graphics);
    Gdiplus::GraphicsPath path;
    detail::rounded_path(path, detail::to_rectf(rect),
                         static_cast<float>(radius));
    Gdiplus::SolidBrush brush(color(fill));
    graphics.FillPath(&brush, &path);
    if (stroke != CLR_INVALID) {
        Gdiplus::Pen pen(color(stroke), static_cast<Gdiplus::REAL>(stroke_width));
        graphics.DrawPath(&pen, &path);
    }
}

// Paper backdrop: canvas wash, fine engineering grid, frame rails and an
// accent signal tick under the header rule.
inline void draw_grid(HDC dc, const RECT& client, UINT dpi, COLORREF accent) {
    Gdiplus::Graphics graphics(dc);
    detail::configure(graphics);
    const Gdiplus::RectF bounds = detail::to_rectf(client);
    graphics.Clear(color(kCanvas));

    Gdiplus::Pen grid(color(kInk, 14), 1.0f);
    const float step = static_cast<float>(scale(42, dpi));
    const float origin_x = static_cast<float>(scale(38, dpi));
    const float origin_y = static_cast<float>(scale(42, dpi));
    for (float x = origin_x; x < bounds.GetRight(); x += step) {
        graphics.DrawLine(&grid, x, 0.0f, x, bounds.GetBottom());
    }
    for (float y = origin_y; y < bounds.GetBottom(); y += step) {
        graphics.DrawLine(&grid, 0.0f, y, bounds.GetRight(), y);
    }

    Gdiplus::Pen rail(color(kInk, 36), 1.0f);
    const float margin = static_cast<float>(scale(38, dpi));
    const float header_line = static_cast<float>(scale(82, dpi));
    graphics.DrawLine(&rail, 0.0f, header_line, bounds.GetRight(), header_line);
    graphics.DrawLine(&rail, margin, 0.0f, margin, bounds.GetBottom());
    graphics.DrawLine(&rail, bounds.GetRight() - margin, 0.0f,
                      bounds.GetRight() - margin, bounds.GetBottom());

    Gdiplus::Pen signal(color(accent), static_cast<Gdiplus::REAL>(scale(3, dpi)));
    graphics.DrawLine(&signal, static_cast<Gdiplus::REAL>(scale(120, dpi)), header_line,
                      static_cast<Gdiplus::REAL>(scale(168, dpi)), header_line);
}

inline void draw_text(HDC dc, const std::wstring& text, RECT rect, HFONT font,
                      COLORREF text_color,
                      UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                    DT_END_ELLIPSIS) {
    if (text.empty() || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    detail::configure(graphics);

    Gdiplus::StringFormat string_format;
    UINT align = format & (DT_CENTER | DT_RIGHT | DT_LEFT);
    if (align == DT_CENTER) {
        string_format.SetAlignment(Gdiplus::StringAlignmentCenter);
    } else if (align == DT_RIGHT) {
        string_format.SetAlignment(Gdiplus::StringAlignmentFar);
    } else {
        string_format.SetAlignment(Gdiplus::StringAlignmentNear);
    }
    if (format & DT_BOTTOM) {
        string_format.SetLineAlignment(Gdiplus::StringAlignmentFar);
    } else if (!(format & DT_VCENTER)) {
        string_format.SetLineAlignment(Gdiplus::StringAlignmentNear);
    } else {
        string_format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    }
    string_format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                                 Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    string_format.SetTrimming((format & DT_END_ELLIPSIS)
                                  ? Gdiplus::StringTrimmingEllipsisCharacter
                                  : Gdiplus::StringTrimmingNone);

    Gdiplus::SolidBrush brush(color(text_color));
    graphics.DrawString(text.c_str(), static_cast<INT>(text.size()),
                        &detail::font_for(font), detail::to_rectf(rect),
                        &string_format, &brush);
}

// ---------------------------------------------------------------------------
// Lucide vector glyphs (ISC License, (c) Lucide Contributors)
// ---------------------------------------------------------------------------
namespace detail {

inline Gdiplus::PointF point(const Gdiplus::RectF& b, float x, float y) {
    return Gdiplus::PointF(b.X + b.Width * x / 24.0f, b.Y + b.Height * y / 24.0f);
}

inline Gdiplus::RectF sub_rect(const Gdiplus::RectF& b, float x, float y,
                               float w, float h) {
    return Gdiplus::RectF(b.X + b.Width * x / 24.0f, b.Y + b.Height * y / 24.0f,
                          b.Width * w / 24.0f, b.Height * h / 24.0f);
}

inline void stroke_path(Gdiplus::Graphics& g, Gdiplus::GraphicsPath& path,
                        const Gdiplus::RectF& b, const Gdiplus::Color& c,
                        float units, float stroke_scale) {
    Gdiplus::Pen pen(c, units * stroke_scale);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&pen, &path);
}

inline void stroke_polyline(Gdiplus::Graphics& g,
                            const std::vector<Gdiplus::PointF>& points,
                            const Gdiplus::RectF& b,
                            const Gdiplus::Color& c, float units,
                            float stroke_scale) {
    Gdiplus::Pen pen(c, units * stroke_scale);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
}

inline void stroke_line(Gdiplus::Graphics& g, const Gdiplus::RectF& b,
                        float x1, float y1, float x2, float y2,
                        const Gdiplus::Color& c, float units,
                        float stroke_scale) {
    Gdiplus::Pen pen(c, units * stroke_scale);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, point(b, x1, y1), point(b, x2, y2));
}

inline void stroke_ellipse(Gdiplus::Graphics& g, const Gdiplus::RectF& b,
                           float x, float y, float w, float h,
                           const Gdiplus::Color& c, float units,
                           float stroke_scale) {
    Gdiplus::Pen pen(c, units * stroke_scale);
    g.DrawEllipse(&pen, sub_rect(b, x, y, w, h));
}

}  // namespace detail

inline void draw_icon(HDC dc, Icon icon, const RECT& rect, COLORREF icon_color,
                      int stroke_px = 2) {
    const Gdiplus::RectF b = detail::to_rectf(rect);
    if (b.Width <= 1.0f || b.Height <= 1.0f) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    detail::configure(graphics);
    const Gdiplus::Color c = color(icon_color);
    // Stroke width follows the requested pixel width, clamped to the box.
    const float units = std::max(stroke_px * 24.0f / b.Width, 1.2f);

    switch (icon) {
    case Icon::Palette: {
        Gdiplus::GraphicsPath path;
        const auto p = [&b](float x, float y) { return detail::point(b, x, y); };
        path.StartFigure();
        path.AddBezier(p(12, 22), p(6.48f, 22), p(2, 17.52f), p(2, 12));
        path.AddBezier(p(2, 12), p(2, 6.48f), p(6.48f, 2), p(12, 2));
        path.AddBezier(p(12, 2), p(17.52f, 2), p(22, 6.03f), p(22, 11));
        path.AddBezier(p(22, 11), p(22, 13.76f), p(19.76f, 16), p(17, 16));
        path.AddLine(p(17, 16), p(14.75f, 16));
        path.AddBezier(p(14.75f, 16), p(13.78f, 16), p(13, 16.78f), p(13, 17.75f));
        path.AddBezier(p(13, 17.75f), p(13, 18.13f), p(13.12f, 18.5f),
                       p(13.35f, 18.8f));
        path.AddLine(p(13.35f, 18.8f), p(13.65f, 19.2f));
        path.AddBezier(p(13.65f, 19.2f), p(14.48f, 20.31f), p(13.69f, 22),
                       p(12.3f, 22));
        path.CloseFigure();
        detail::stroke_path(graphics, path, b, c, units, b.Width / 24.0f);
        Gdiplus::SolidBrush brush(c);
        for (const Gdiplus::PointF& dot : {p(13.5f, 6.5f), p(17.5f, 10.5f),
                                           p(6.5f, 12.5f), p(8.5f, 7.5f)}) {
            const float r = b.Width * 0.55f / 24.0f;
            graphics.FillEllipse(&brush, dot.X - r, dot.Y - r, r * 2, r * 2);
        }
        break;
    }
    case Icon::Panels: {
        Gdiplus::GraphicsPath panel;
        detail::rounded_path(panel, detail::sub_rect(b, 3, 3, 18, 18),
                             b.Width * 2.0f / 24.0f);
        detail::stroke_path(graphics, panel, b, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 3, 9, 21, 9, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 9, 21, 9, 9, c, units, b.Width / 24.0f);
        break;
    }
    case Icon::Gauge:
        detail::stroke_ellipse(graphics, b, 2, 2, 20, 20, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 12, 14, 16, 10, c, units, b.Width / 24.0f);
        break;
    case Icon::Keyboard: {
        const float s = b.Width / 24.0f;
        Gdiplus::GraphicsPath body;
        detail::rounded_path(body, detail::sub_rect(b, 2, 4, 20, 16),
                             2.0f * s);
        detail::stroke_path(graphics, body, b, c, units, s);
        for (const auto& dot : std::vector<std::pair<float, float>>{
                 {6, 8}, {10, 8}, {14, 8}, {18, 8},
                 {6, 12}, {10, 12}, {14, 12}, {18, 12}}) {
            Gdiplus::SolidBrush brush(c);
            const float r = 0.9f * s;
            graphics.FillEllipse(&brush, b.X + dot.first * s - r / 2,
                                 b.Y + dot.second * s - r / 2, r, r);
        }
        detail::stroke_line(graphics, b, 7.5f, 16.5f, 16.5f, 16.5f, c, units, s);
        break;
    }
    case Icon::FolderPlus: {
        Gdiplus::GraphicsPath folder;
        folder.StartFigure();
        folder.AddLine(detail::point(b, 2, 20), detail::point(b, 2, 5));
        folder.AddLine(detail::point(b, 2, 5), detail::point(b, 9, 5));
        folder.AddLine(detail::point(b, 9, 5), detail::point(b, 11, 7));
        folder.AddLine(detail::point(b, 11, 7), detail::point(b, 22, 7));
        folder.AddLine(detail::point(b, 22, 7), detail::point(b, 22, 20));
        folder.CloseFigure();
        detail::stroke_path(graphics, folder, b, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 12, 11, 12, 17, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 9, 14, 15, 14, c, units, b.Width / 24.0f);
        break;
    }
    case Icon::FolderOpen: {
        Gdiplus::GraphicsPath folder;
        const auto p = [&b](float x, float y) { return detail::point(b, x, y); };
        folder.StartFigure();
        folder.AddLine(p(6, 14), p(7.5f, 11.1f));
        folder.AddBezier(p(7.84f, 10.43f), p(8.51f, 10), p(9.24f, 10), p(20, 10));
        folder.AddBezier(p(21.1f, 10), p(22, 10.9f), p(22, 12), p(21.94f, 12.5f));
        folder.AddLine(p(21.94f, 12.5f), p(20.4f, 18.5f));
        folder.AddBezier(p(20.17f, 19.38f), p(19.38f, 20), p(18.45f, 20), p(4, 20));
        folder.AddBezier(p(2.9f, 20), p(2, 19.1f), p(2, 18), p(2, 5));
        folder.AddBezier(p(2, 3.9f), p(2.9f, 3), p(4, 3), p(7.9f, 3));
        folder.AddBezier(p(8.58f, 3), p(9.21f, 3.34f), p(9.59f, 3.9f), p(10.4f, 5.1f));
        folder.AddBezier(p(10.77f, 5.66f), p(11.4f, 6), p(12.07f, 6), p(18, 6));
        folder.AddBezier(p(19.1f, 6), p(20, 6.9f), p(20, 8), p(20, 10));
        detail::stroke_path(graphics, folder, b, c, units, b.Width / 24.0f);
        break;
    }
    case Icon::ArrowUpRight:
        detail::stroke_polyline(
            graphics,
            {detail::point(b, 7, 7), detail::point(b, 17, 7),
             detail::point(b, 17, 17)},
            b, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 7, 17, 17, 7, c, units, b.Width / 24.0f);
        break;
    case Icon::CircleCheck:
        detail::stroke_ellipse(graphics, b, 2, 2, 20, 20, c, units, b.Width / 24.0f);
        detail::stroke_polyline(
            graphics,
            {detail::point(b, 9, 12), detail::point(b, 11, 14),
             detail::point(b, 15, 10)},
            b, c, units, b.Width / 24.0f);
        break;
    case Icon::CircleAlert:
        detail::stroke_ellipse(graphics, b, 2, 2, 20, 20, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 12, 8, 12, 12, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 12, 16, 12.01f, 16, c, units, b.Width / 24.0f);
        break;
    case Icon::Activity:
        detail::stroke_polyline(
            graphics,
            {detail::point(b, 22, 12), detail::point(b, 18, 12),
             detail::point(b, 15, 21), detail::point(b, 9, 3),
             detail::point(b, 6, 12), detail::point(b, 2, 12)},
            b, c, units, b.Width / 24.0f);
        break;
    case Icon::RotateCcw: {
        detail::stroke_polyline(
            graphics,
            {detail::point(b, 3, 7), detail::point(b, 3, 3), detail::point(b, 7, 3)},
            b, c, units, b.Width / 24.0f);
        Gdiplus::GraphicsPath arc;
        arc.StartFigure();
        arc.AddArc(detail::sub_rect(b, 3, 3, 18, 18), 160.0f, 300.0f);
        detail::stroke_path(graphics, arc, b, c, units, b.Width / 24.0f);
        break;
    }
    case Icon::RefreshCw: {
        detail::stroke_polyline(
            graphics,
            {detail::point(b, 20, 7), detail::point(b, 20, 3), detail::point(b, 16, 3)},
            b, c, units, b.Width / 24.0f);
        detail::stroke_polyline(
            graphics,
            {detail::point(b, 4, 17), detail::point(b, 4, 21), detail::point(b, 8, 21)},
            b, c, units, b.Width / 24.0f);
        Gdiplus::GraphicsPath arc;
        arc.StartFigure();
        arc.AddArc(detail::sub_rect(b, 3, 3, 18, 18), -30.0f, 280.0f);
        detail::stroke_path(graphics, arc, b, c, units, b.Width / 24.0f);
        break;
    }
    case Icon::Check:
        detail::stroke_polyline(
            graphics,
            {detail::point(b, 5, 12), detail::point(b, 10, 17),
             detail::point(b, 20, 7)},
            b, c, units, b.Width / 24.0f);
        break;
    case Icon::Monitor: {
        Gdiplus::GraphicsPath screen;
        detail::rounded_path(screen, detail::sub_rect(b, 2, 3, 20, 14),
                             2.0f * b.Width / 24.0f);
        detail::stroke_path(graphics, screen, b, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 8, 21, 16, 21, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 12, 17, 12, 21, c, units, b.Width / 24.0f);
        break;
    }
    case Icon::Undo: {
        detail::stroke_polyline(
            graphics,
            {detail::point(b, 9, 7), detail::point(b, 4, 12), detail::point(b, 9, 17)},
            b, c, units, b.Width / 24.0f);
        detail::stroke_line(graphics, b, 4, 12, 14, 12, c, units, b.Width / 24.0f);
        Gdiplus::GraphicsPath arc;
        arc.StartFigure();
        arc.AddArc(detail::sub_rect(b, 8, 7, 14, 14), -80.0f, 250.0f);
        detail::stroke_path(graphics, arc, b, c, units, b.Width / 24.0f);
        break;
    }
    case Icon::Zap:
        detail::stroke_polyline(
            graphics,
            {detail::point(b, 13, 2), detail::point(b, 4, 14),
             detail::point(b, 11, 14), detail::point(b, 10, 22),
             detail::point(b, 20, 9), detail::point(b, 13, 9),
             detail::point(b, 13, 2)},
            b, c, units, b.Width / 24.0f);
        break;
    }
}

// Runtime window icon twin of the shared .ico family: dark tile canvas with
// only this component's tile filled.
inline HICON create_app_icon(int size, Icon icon, COLORREF accent) {
    FamilyTile tile = kTileIcc;
    switch (icon) {
    case Icon::Palette: tile = kTileIcc; break;
    case Icon::Panels: tile = kTileMux; break;
    case Icon::Gauge: tile = kTileIyx; break;
    default: tile = kTileIyx; break;
    }

    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&bitmap);
    detail::configure(graphics);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float u = size / 100.0f;
    const float inset = 4.0f * u;
    const Gdiplus::RectF bg(inset, inset, size - inset * 2.0f, size - inset * 2.0f);
    Gdiplus::GraphicsPath bg_path;
    detail::rounded_path(bg_path, bg, 21.0f * u);
    Gdiplus::SolidBrush bg_brush(color(0x10110F));
    graphics.FillPath(&bg_brush, &bg_path);

    const float pad = 19.0f * u;
    const float gap = 8.0f * u;
    const float tile_size = (size - 2.0f * pad - gap) / 2.0f;
    const float xs[2] = {pad, pad + tile_size + gap};
    const float ys[2] = {pad, pad + tile_size + gap};
    const unsigned int accents[3] = {static_cast<unsigned int>(kCoral),
                                     static_cast<unsigned int>(kBlue),
                                     static_cast<unsigned int>(kLime)};
    Gdiplus::Pen slot_pen(color(kPaper, 60), std::max(2.0f, 4.2f * u));
    for (int index = 0; index < 3; ++index) {
        Gdiplus::RectF cell(xs[index % 2], ys[index / 2], tile_size, tile_size);
        Gdiplus::GraphicsPath cell_path;
        detail::rounded_path(cell_path, cell, tile_size * 0.30f);
        if (index == static_cast<int>(tile)) {
            Gdiplus::SolidBrush fill(color(accents[index]));
            graphics.FillPath(&fill, &cell_path);
        } else {
            graphics.DrawPath(&slot_pen, &cell_path);
        }
    }

    HICON result = nullptr;
    bitmap.GetHICON(&result);
    return result;
}

// ---------------------------------------------------------------------------
// Owner-drawn controls
// ---------------------------------------------------------------------------
inline void draw_button(const DRAWITEMSTRUCT& item, const std::wstring& text,
                        Icon icon, UINT dpi, COLORREF accent,
                        bool primary = false) {
    RECT rect = item.rcItem;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const COLORREF fill = disabled ? RGB(225, 227, 222)
                          : primary ? (pressed ? RGB(12, 13, 12) : kInk)
                                    : (pressed ? RGB(232, 233, 229) : kPaper);
    const COLORREF foreground =
        disabled ? RGB(140, 143, 136) : primary ? kPaper : kInk;
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

    fill_round_rect(item.hDC, rect, 4, background,
                    selected ? background : kLine, 1);
    HFONT font = create_font(dpi, 14, FW_NORMAL);
    RECT text_rect{rect.left + scale(14, dpi), rect.top,
                   rect.right - scale(34, dpi), rect.bottom};
    draw_text(item.hDC, text, text_rect, font, foreground);
    DeleteObject(font);

    // Chevron hint on the closed box.
    if (item.itemID == static_cast<UINT>(-1)) {
        const int cx = rect.right - scale(20, dpi);
        const int cy = (rect.top + rect.bottom) / 2;
        const int arm = scale(4, dpi);
        Gdiplus::Graphics graphics(item.hDC);
        detail::configure(graphics);
        Gdiplus::Pen pen(color(foreground, 190),
                         static_cast<Gdiplus::REAL>(scale(2, dpi)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(&pen, static_cast<Gdiplus::REAL>(cx - arm),
                          static_cast<Gdiplus::REAL>(cy - arm / 2),
                          static_cast<Gdiplus::REAL>(cx),
                          static_cast<Gdiplus::REAL>(cy + arm / 2));
        graphics.DrawLine(&pen, static_cast<Gdiplus::REAL>(cx),
                          static_cast<Gdiplus::REAL>(cy + arm / 2),
                          static_cast<Gdiplus::REAL>(cx + arm),
                          static_cast<Gdiplus::REAL>(cy - arm / 2));
    }
}

}  // namespace toolbox_theme
