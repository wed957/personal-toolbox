#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace Gdiplus;

namespace {

constexpr UINT_PTR kClockTimer = 2;
constexpr int kToolCount = 3;
constexpr int kFolderTarget = kToolCount;
constexpr int kNoTarget = -1;

// ---------------------------------------------------------------------------
// Palette — warm paper control deck (light, fully static rendering)
// ---------------------------------------------------------------------------
constexpr unsigned int kCanvas = 0xF3F4F1;
constexpr unsigned int kInk = 0x14150F;
constexpr unsigned int kMuted = 0x6A6E64;
constexpr unsigned int kCoral = 0xFF654F;
constexpr unsigned int kBlue = 0x2864FF;
constexpr unsigned int kLime = 0xC8F43D;
constexpr unsigned int kSuccess = 0x1C7C54;
constexpr unsigned int kError = 0xD93F32;

enum class LucideIcon {
    Palette,
    PanelsTopLeft,
    Gauge,
    FolderOpen,
    ArrowUpRight,
    CircleCheck,
    CircleAlert,
    Activity,
};

struct Tool {
    const wchar_t* eyebrow;
    const wchar_t* name;
    const wchar_t* description;
    const wchar_t* meta;
    const wchar_t* executable;
    unsigned int accent;
    unsigned int glyph;      // ink stamped on the accent chip
    LucideIcon icon;
};

constexpr std::array<Tool, kToolCount> kTools = {{
    {L"COLOR / 01", L"ICC SWITCH", L"显示器色彩配置与快速切换", L"PROFILE CONTROL",
     L"tools\\icc-switch-gui.exe", kCoral, 0x10110F, LucideIcon::Palette},
    {L"DISPLAY / 02", L"MUX", L"显示拓扑与屏幕布局管理", L"DISPLAY ROUTING",
     L"tools\\MUX.exe", kBlue, 0xFFFFFF, LucideIcon::PanelsTopLeft},
    {L"DRIVER / 03", L"IYX", L"驱动界面与磁轴键盘检测", L"LOCAL SERVICE",
     L"tools\\IYX.exe", kLime, 0x10110F, LucideIcon::Gauge},
}};

struct Layout {
    float scale = 1.0f;
    float width = 0.0f;
    float height = 0.0f;
    float padding = 0.0f;
    float header_line = 0.0f;
    RectF folder;
    std::array<RectF, kToolCount> cards;
    float status_y = 0.0f;
};

fs::path g_root;
std::array<bool, kToolCount> g_present{};
HWND g_window = nullptr;
HICON g_large_icon = nullptr;
HICON g_small_icon = nullptr;
ULONG_PTR g_gdiplus_token = 0;
Bitmap* g_frame = nullptr;   // composited output blitted to the screen
Bitmap* g_base = nullptr;    // cached idle scene: everything static
bool g_base_dirty = true;
bool g_status_dirty = true;  // status message patch needs recompose
std::wstring g_status = L"系统就绪 / 3 个模块在线";
bool g_status_error = false;
bool g_window_focused = true;
bool g_keyboard_navigation = false;
int g_hover_target = kNoTarget;
int g_pressed_target = kNoTarget;
int g_keyboard_target = 0;
UINT g_last_second = 0;

// Letter-spacing measurements are cached per string; nothing else is cached
// because repaints happen only on interaction state changes.
std::unordered_map<std::wstring, std::vector<float>> g_tracked_widths;

// Resolved font families (Bahnschrift is preferred; fall back gracefully).
const FontFamily* g_display_family = nullptr;
const FontFamily* g_text_family = nullptr;
const FontFamily* g_mono_family = nullptr;

Color make_color(unsigned int rgb, BYTE alpha = 255) {
    return Color(alpha, static_cast<BYTE>((rgb >> 16) & 0xff),
                 static_cast<BYTE>((rgb >> 8) & 0xff),
                 static_cast<BYTE>(rgb & 0xff));
}

unsigned int mix_rgb(unsigned int from, unsigned int to, float amount) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    const auto channel = [amount](unsigned int a, unsigned int b) {
        return static_cast<unsigned int>(a + (static_cast<float>(b) - a) * amount + 0.5f);
    };
    const unsigned int red = channel((from >> 16) & 0xff, (to >> 16) & 0xff);
    const unsigned int green = channel((from >> 8) & 0xff, (to >> 8) & 0xff);
    const unsigned int blue = channel(from & 0xff, to & 0xff);
    return (red << 16) | (green << 8) | blue;
}

float window_scale(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = get_dpi == nullptr ? 96 : get_dpi(window);
    return static_cast<float>(dpi) / 96.0f;
}

fs::path executable_directory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return fs::current_path();
        }
        if (size < buffer.size() - 1) {
            return fs::path(buffer.data(), buffer.data() + size).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

void refresh_presence() {
    for (int index = 0; index < kToolCount; ++index) {
        g_present[index] = fs::is_regular_file(g_root / kTools[index].executable);
    }
}

bool all_tools_present() {
    for (const Tool& tool : kTools) {
        if (!fs::is_regular_file(g_root / tool.executable)) {
            return false;
        }
    }
    return true;
}

void set_status(const std::wstring& text, bool error = false) {
    g_status = text;
    g_status_error = error;
    g_status_dirty = true;
    if (g_window != nullptr) {
        InvalidateRect(g_window, nullptr, FALSE);
    }
}

// The base bitmap holds the idle scene; only small dynamic patches (clock,
// uptime, status message, hovered card, inverted folder pill, focus rings)
// are redrawn on top of it.
struct Layout;
void render_base(const Layout& layout);

void restore_base(Graphics& graphics, const RectF& region) {
    if (g_base == nullptr || region.Width <= 0.0f || region.Height <= 0.0f) {
        return;
    }
    const int x = static_cast<int>(std::floor(region.X));
    const int y = static_cast<int>(std::floor(region.Y));
    const int w = static_cast<int>(std::ceil(region.GetRight())) - x;
    const int h = static_cast<int>(std::ceil(region.GetBottom())) - y;
    graphics.DrawImage(g_base, RectF(static_cast<float>(x), static_cast<float>(y),
                                     static_cast<float>(w), static_cast<float>(h)),
                       static_cast<REAL>(x), static_cast<REAL>(y),
                       static_cast<REAL>(w), static_cast<REAL>(h), UnitPixel);
}

void launch_tool(int index) {
    const Tool& tool = kTools[index];
    const fs::path path = g_root / tool.executable;
    if (!fs::is_regular_file(path)) {
        g_present[index] = false;
        g_base_dirty = true;  // card footer flips to MODULE MISSING
        set_status(std::wstring(L"缺少模块 / ") + tool.name, true);
        return;
    }

    std::wstring command_line = L"\"" + path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, g_root.c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        set_status(std::wstring(L"启动失败 / ") + tool.name + L" / ERROR " +
                       std::to_wstring(error),
                   true);
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    set_status(std::wstring(L"已启动 / ") + tool.name);
}

void open_tools_directory(HWND owner) {
    const HINSTANCE result = ShellExecuteW(owner, L"open", g_root.c_str(), nullptr,
                                           g_root.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        set_status(L"无法打开工具目录", true);
    } else {
        set_status(L"已打开 / 工具目录");
    }
}

void add_rounded_path(GraphicsPath& path, const RectF& rect, float radius) {
    const float diameter = std::min(radius * 2.0f, std::min(rect.Width, rect.Height));
    if (diameter <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void fill_round_rect(Graphics& graphics, const RectF& rect, float radius,
                     const Color& color) {
    GraphicsPath path;
    add_rounded_path(path, rect, radius);
    SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

void stroke_round_rect(Graphics& graphics, const RectF& rect, float radius,
                       const Color& color, float width, DashStyle dash = DashStyleSolid) {
    GraphicsPath path;
    add_rounded_path(path, rect, radius);
    Pen pen(color, width);
    pen.SetDashStyle(dash);
    graphics.DrawPath(&pen, &path);
}

// ---------------------------------------------------------------------------
// Lucide vector glyphs (ISC License, (c) Lucide Contributors)
// ---------------------------------------------------------------------------
PointF icon_point(const RectF& bounds, float x, float y) {
    return PointF(bounds.X + bounds.Width * x / 24.0f,
                  bounds.Y + bounds.Height * y / 24.0f);
}

RectF icon_rect(const RectF& bounds, float x, float y, float width, float height) {
    return RectF(bounds.X + bounds.Width * x / 24.0f,
                 bounds.Y + bounds.Height * y / 24.0f,
                 bounds.Width * width / 24.0f,
                 bounds.Height * height / 24.0f);
}

void draw_lucide_icon(Graphics& graphics, LucideIcon icon, const RectF& bounds,
                      const Color& color, float stroke = 2.0f) {
    if (bounds.Width <= 1.0f || bounds.Height <= 1.0f) {
        return;
    }
    Pen pen(color, stroke * bounds.Width / 24.0f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush brush(color);
    const auto point = [&bounds](float x, float y) { return icon_point(bounds, x, y); };

    switch (icon) {
    case LucideIcon::Palette: {
        GraphicsPath path;
        path.StartFigure();
        path.AddBezier(point(12.0f, 22.0f), point(6.48f, 22.0f),
                       point(2.0f, 17.52f), point(2.0f, 12.0f));
        path.AddBezier(point(2.0f, 12.0f), point(2.0f, 6.48f),
                       point(6.48f, 2.0f), point(12.0f, 2.0f));
        path.AddBezier(point(12.0f, 2.0f), point(17.52f, 2.0f),
                       point(22.0f, 6.03f), point(22.0f, 11.0f));
        path.AddBezier(point(22.0f, 11.0f), point(22.0f, 13.76f),
                       point(19.76f, 16.0f), point(17.0f, 16.0f));
        path.AddLine(point(17.0f, 16.0f), point(14.75f, 16.0f));
        path.AddBezier(point(14.75f, 16.0f), point(13.78f, 16.0f),
                       point(13.0f, 16.78f), point(13.0f, 17.75f));
        path.AddBezier(point(13.0f, 17.75f), point(13.0f, 18.13f),
                       point(13.12f, 18.5f), point(13.35f, 18.8f));
        path.AddLine(point(13.35f, 18.8f), point(13.65f, 19.2f));
        path.AddBezier(point(13.65f, 19.2f), point(14.48f, 20.31f),
                       point(13.69f, 22.0f), point(12.3f, 22.0f));
        path.AddLine(point(12.3f, 22.0f), point(12.0f, 22.0f));
        path.CloseFigure();
        graphics.DrawPath(&pen, &path);
        const std::array<PointF, 4> centers = {
            point(13.5f, 6.5f), point(17.5f, 10.5f),
            point(6.5f, 12.5f), point(8.5f, 7.5f)};
        for (const PointF& center : centers) {
            const float radius = bounds.Width * 0.5f / 24.0f;
            graphics.FillEllipse(&brush, center.X - radius, center.Y - radius,
                                 radius * 2.0f, radius * 2.0f);
        }
        break;
    }
    case LucideIcon::PanelsTopLeft: {
        GraphicsPath panel;
        add_rounded_path(panel, icon_rect(bounds, 3.0f, 3.0f, 18.0f, 18.0f),
                         bounds.Width * 2.0f / 24.0f);
        graphics.DrawPath(&pen, &panel);
        graphics.DrawLine(&pen, point(3.0f, 9.0f), point(21.0f, 9.0f));
        graphics.DrawLine(&pen, point(9.0f, 21.0f), point(9.0f, 9.0f));
        break;
    }
    case LucideIcon::Gauge:
        graphics.DrawLine(&pen, point(12.0f, 14.0f), point(16.0f, 10.0f));
        graphics.DrawArc(&pen, icon_rect(bounds, 2.0f, 2.0f, 20.0f, 20.0f),
                         136.0f, 268.0f);
        break;
    case LucideIcon::FolderOpen: {
        GraphicsPath folder;
        folder.StartFigure();
        folder.AddLine(point(6.0f, 14.0f), point(7.5f, 11.1f));
        folder.AddBezier(point(7.84f, 10.43f), point(8.51f, 10.0f),
                         point(9.24f, 10.0f), point(20.0f, 10.0f));
        folder.AddBezier(point(21.1f, 10.0f), point(22.0f, 10.9f),
                         point(22.0f, 12.0f), point(21.94f, 12.5f));
        folder.AddLine(point(21.94f, 12.5f), point(20.4f, 18.5f));
        folder.AddBezier(point(20.17f, 19.38f), point(19.38f, 20.0f),
                         point(18.45f, 20.0f), point(4.0f, 20.0f));
        folder.AddBezier(point(2.9f, 20.0f), point(2.0f, 19.1f),
                         point(2.0f, 18.0f), point(2.0f, 5.0f));
        folder.AddBezier(point(2.0f, 3.9f), point(2.9f, 3.0f),
                         point(4.0f, 3.0f), point(7.9f, 3.0f));
        folder.AddBezier(point(8.58f, 3.0f), point(9.21f, 3.34f),
                         point(9.59f, 3.9f), point(10.4f, 5.1f));
        folder.AddBezier(point(10.77f, 5.66f), point(11.4f, 6.0f),
                         point(12.07f, 6.0f), point(18.0f, 6.0f));
        folder.AddBezier(point(19.1f, 6.0f), point(20.0f, 6.9f),
                         point(20.0f, 8.0f), point(20.0f, 10.0f));
        graphics.DrawPath(&pen, &folder);
        break;
    }
    case LucideIcon::ArrowUpRight:
        graphics.DrawLines(&pen, std::array<PointF, 3>{
                                     point(7.0f, 7.0f), point(17.0f, 7.0f),
                                     point(17.0f, 17.0f)}.data(), 3);
        graphics.DrawLine(&pen, point(7.0f, 17.0f), point(17.0f, 7.0f));
        break;
    case LucideIcon::CircleCheck:
        graphics.DrawEllipse(&pen, icon_rect(bounds, 2.0f, 2.0f, 20.0f, 20.0f));
        graphics.DrawLines(&pen, std::array<PointF, 3>{
                                     point(9.0f, 12.0f), point(11.0f, 14.0f),
                                     point(15.0f, 10.0f)}.data(), 3);
        break;
    case LucideIcon::CircleAlert:
        graphics.DrawEllipse(&pen, icon_rect(bounds, 2.0f, 2.0f, 20.0f, 20.0f));
        graphics.DrawLine(&pen, point(12.0f, 8.0f), point(12.0f, 12.0f));
        graphics.DrawLine(&pen, point(12.0f, 16.0f), point(12.01f, 16.0f));
        break;
    case LucideIcon::Activity:
        graphics.DrawLines(&pen, std::array<PointF, 6>{
                                     point(22.0f, 12.0f), point(18.0f, 12.0f),
                                     point(15.0f, 21.0f), point(9.0f, 3.0f),
                                     point(6.0f, 12.0f), point(2.0f, 12.0f)}.data(), 6);
        break;
    }
}

// ---------------------------------------------------------------------------
// Typography helpers
// ---------------------------------------------------------------------------
const FontFamily* resolve_family(const std::initializer_list<const wchar_t*>& names) {
    for (const wchar_t* name : names) {
        FontFamily* family = new FontFamily(name);
        if (family->GetLastStatus() == Ok) {
            return family;
        }
        delete family;
    }
    return FontFamily::GenericSansSerif();
}

void init_font_families() {
    g_display_family = resolve_family({L"Bahnschrift", L"Segoe UI"});
    g_text_family = resolve_family({L"Microsoft YaHei UI", L"Microsoft YaHei",
                                    L"Segoe UI"});
    g_mono_family = resolve_family({L"Consolas", L"Cascadia Mono", L"Courier New"});
}

void draw_text(Graphics& graphics, const std::wstring& text, const FontFamily* family,
               float size, int style, const Color& color, const RectF& bounds,
               StringAlignment alignment = StringAlignmentNear, bool wrap = false) {
    const auto snap = [](float value) { return std::floor(value + 0.5f); };
    const RectF aligned_bounds(snap(bounds.X), snap(bounds.Y),
                               snap(bounds.Width), snap(bounds.Height));
    const float aligned_size = std::floor(size * 2.0f + 0.5f) / 2.0f;
    Font font(family, aligned_size, style, UnitPixel);
    SolidBrush brush(color);
    StringFormat format;
    format.SetAlignment(alignment);
    format.SetLineAlignment(StringAlignmentNear);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) {
        format.SetFormatFlags(StringFormatFlagsNoWrap);
    }
    const TextRenderingHint previous_hint = graphics.GetTextRenderingHint();
    graphics.SetTextRenderingHint(aligned_size >= 24.0f
                                      ? TextRenderingHintAntiAliasGridFit
                                      : TextRenderingHintClearTypeGridFit);
    graphics.DrawString(text.c_str(), -1, &font, aligned_bounds, &format, &brush);
    graphics.SetTextRenderingHint(previous_hint);
}

// Letter-spaced small caps row: per-glyph advances are cached per string.
const std::vector<float>& tracked_widths(Graphics& graphics,
                                         const std::wstring& text,
                                         const FontFamily* family, float size,
                                         int style) {
    const std::wstring key = text + L'|' +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(family)) + L'|' +
        std::to_wstring(static_cast<long long>(size * 16.0f)) + L'|' +
        std::to_wstring(style);
    const auto found = g_tracked_widths.find(key);
    if (found != g_tracked_widths.end()) {
        return found->second;
    }
    Font font(family, size, style, UnitPixel);
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(StringFormatFlagsNoWrap |
                          StringFormatFlagsMeasureTrailingSpaces);
    const RectF layout(0.0f, 0.0f, 4096.0f, 512.0f);
    std::vector<float> widths;
    widths.reserve(text.size());
    for (wchar_t ch : text) {
        const std::wstring single(1, ch);
        RectF measured{};
        graphics.MeasureString(single.c_str(), 1, &font, layout, &format,
                               &measured);
        widths.push_back(measured.Width);
    }
    return g_tracked_widths.emplace(key, std::move(widths)).first->second;
}

void draw_text_tracked(Graphics& graphics, const std::wstring& text,
                       const FontFamily* family, float size, int style,
                       const Color& color, const RectF& bounds, float tracking,
                       StringAlignment alignment = StringAlignmentNear) {
    if (text.empty() || bounds.Width <= 1.0f) {
        return;
    }
    const std::vector<float>& widths =
        tracked_widths(graphics, text, family, size, style);
    SolidBrush brush(color);
    StringFormat format(StringFormat::GenericTypographic());
    format.SetFormatFlags(StringFormatFlagsNoWrap |
                          StringFormatFlagsMeasureTrailingSpaces);
    const TextRenderingHint previous_hint = graphics.GetTextRenderingHint();
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    float total = -tracking;
    for (float width : widths) {
        total += width + tracking;
    }
    float x = bounds.X;
    if (alignment == StringAlignmentCenter) {
        x += (bounds.Width - total) / 2.0f;
    } else if (alignment == StringAlignmentFar) {
        x += bounds.Width - total;
    }
    Font font(family, size, style, UnitPixel);
    const float y = bounds.Y;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (x > bounds.GetRight() + tracking) {
            break;
        }
        const std::wstring single(1, text[index]);
        graphics.DrawString(single.c_str(), 1, &font, PointF(x, y), &format,
                            &brush);
        x += widths[index] + tracking;
    }
    graphics.SetTextRenderingHint(previous_hint);
}

// Hollow display type: stroked outline of the string.
void draw_text_outline(Graphics& graphics, const std::wstring& text,
                       const FontFamily* family, int style, float em_size,
                       const Color& color, float stroke_width, const RectF& bounds,
                       StringAlignment alignment = StringAlignmentNear) {
    GraphicsPath path;
    StringFormat format;
    format.SetAlignment(alignment);
    format.SetLineAlignment(StringAlignmentNear);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    path.AddString(text.c_str(), static_cast<INT>(text.size()), family, style,
                   em_size, bounds, &format);
    Pen pen(color, stroke_width);
    pen.SetLineJoin(LineJoinRound);
    graphics.DrawPath(&pen, &path);
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------
std::wstring clock_text() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::wstring uptime_text() {
    static const ULONGLONG start = GetTickCount64();
    const ULONGLONG seconds = (GetTickCount64() - start) / 1000ULL;
    wchar_t buffer[24]{};
    swprintf_s(buffer, L"T+%02u:%02u:%02u", static_cast<unsigned>(seconds / 3600ULL),
               static_cast<unsigned>((seconds / 60ULL) % 60ULL),
               static_cast<unsigned>(seconds % 60ULL));
    return buffer;
}

void draw_background(Graphics& graphics, float width, float height,
                     const Layout& layout) {
    graphics.Clear(make_color(kCanvas));

    // Fine engineering grid.
    const float scale = layout.scale;
    Pen grid(make_color(kInk, 13), 1.0f);
    const float step = 44.0f * scale;
    for (float x = std::fmod(layout.padding, step) - step; x < width; x += step) {
        graphics.DrawLine(&grid, x, 0.0f, x, height);
    }
    for (float y = 0.0f; y < height; y += step) {
        graphics.DrawLine(&grid, 0.0f, y, width, y);
    }

    // Frame rails.
    Pen frame(make_color(kInk, 34), 1.0f);
    const float padding = layout.padding;
    graphics.DrawLine(&frame, padding, 0.0f, padding, height);
    graphics.DrawLine(&frame, width - padding, 0.0f, width - padding, height);
    graphics.DrawLine(&frame, 0.0f, layout.header_line, width, layout.header_line);
    graphics.DrawLine(&frame, 0.0f, layout.status_y, width, layout.status_y);
}

void draw_folder_control(Graphics& graphics, const Layout& layout) {
    const float scale = layout.scale;
    const bool hover = g_hover_target == kFolderTarget && g_window_focused;
    const float amount = hover ? 1.0f : 0.0f;
    RectF rect = layout.folder;
    fill_round_rect(graphics, rect, 9.0f * scale,
                    make_color(mix_rgb(0xFFFFFF, 0x14150F, amount)));
    stroke_round_rect(graphics, rect, 9.0f * scale,
                      make_color(kInk, static_cast<BYTE>(46 + amount * 90)),
                      1.0f * scale);
    const unsigned int content = mix_rgb(kInk, 0xFFFFFF, amount);
    RectF icon(rect.X + 14.0f * scale, rect.Y + 10.0f * scale,
               18.0f * scale, 18.0f * scale);
    draw_lucide_icon(graphics, LucideIcon::FolderOpen, icon,
                     make_color(content), 1.8f);
    draw_text(graphics, L"打开工具目录", g_text_family, 13.5f * scale,
              FontStyleRegular, make_color(content, 240),
              RectF(rect.X + 42.0f * scale, rect.Y + 9.0f * scale,
                    rect.Width - 50.0f * scale, 22.0f * scale));
}

void draw_focus_ring(Graphics& graphics, const Layout& layout, int target) {
    if (!g_window_focused || !g_keyboard_navigation || target == kNoTarget) {
        return;
    }
    const float scale = layout.scale;
    RectF rect;
    float radius = 18.0f * scale;
    Color color = make_color(kBlue, 220);
    if (target == kFolderTarget) {
        rect = layout.folder;
        radius = 10.0f * scale;
    } else {
        rect = layout.cards[target];
        color = make_color(kTools[target].accent, 230);
    }
    stroke_round_rect(graphics,
                      RectF(rect.X - 4.0f * scale, rect.Y - 4.0f * scale,
                            rect.Width + 8.0f * scale, rect.Height + 8.0f * scale),
                      radius, color, 1.6f * scale, DashStyleDash);
}

// Static header: everything except the clock (drawn per-second as a patch).
void draw_header(Graphics& graphics, const Layout& layout) {
    const float scale = layout.scale;
    const float padding = layout.padding;
    const float width = layout.width;

    // Top rail.
    SolidBrush marker(make_color(kCoral));
    graphics.FillRectangle(&marker, padding, 34.0f * scale, 4.0f * scale,
                           13.0f * scale);
    draw_text_tracked(graphics, L"PERSONAL TOOLBOX", g_display_family,
                      11.5f * scale, FontStyleBold,
                      make_color(kInk),
                      RectF(padding + 14.0f * scale, 32.0f * scale,
                            320.0f * scale, 18.0f * scale),
                      2.6f * scale);
    draw_text_tracked(graphics, L"WINDOWS X64 · V1.1", g_display_family,
                      10.5f * scale, FontStyleBold,
                      make_color(kMuted),
                      RectF(width - padding - 130.0f * scale, 33.0f * scale,
                            130.0f * scale, 18.0f * scale),
                      2.2f * scale, StringAlignmentFar);

    // Display title with hollow echo behind.
    const RectF title_bounds(padding, 62.0f * scale, width * 0.62f, 150.0f * scale);
    draw_text_outline(graphics, L"自用工具箱", g_text_family, FontStyleBold,
                      104.0f * scale, make_color(kInk, 52), 1.4f * scale,
                      RectF(title_bounds.X + 7.0f * scale,
                            title_bounds.Y + 9.0f * scale, title_bounds.Width,
                            title_bounds.Height));
    draw_text(graphics, L"自用工具箱", g_text_family, 100.0f * scale, FontStyleBold,
              make_color(kInk), title_bounds);

    draw_text_tracked(graphics, L"DISPLAY / COLOR / DRIVER — ONE CONTROL DECK",
                      g_display_family, 13.0f * scale, FontStyleRegular,
                      make_color(kMuted),
                      RectF(padding + 3.0f * scale, 208.0f * scale,
                            width * 0.58f, 22.0f * scale),
                      2.0f * scale);

    // Oversized hollow module counter.
    const RectF number_bounds(width - padding - 240.0f * scale,
                              58.0f * scale, 240.0f * scale, 160.0f * scale);
    draw_text_outline(graphics, L"03", g_display_family, FontStyleBold,
                      132.0f * scale, make_color(kCoral, 150), 2.0f * scale,
                      number_bounds, StringAlignmentFar);
    SolidBrush live_dot(make_color(kLime));
    const float dot_r = 3.4f * scale;
    graphics.FillEllipse(&live_dot, width - padding - 152.0f * scale,
                         216.0f * scale - dot_r, dot_r * 2.0f, dot_r * 2.0f);
    draw_text_tracked(graphics, L"MODULES ONLINE", g_display_family, 10.5f * scale,
                      FontStyleBold, make_color(kMuted),
                      RectF(width - padding - 144.0f * scale, 206.0f * scale,
                            144.0f * scale, 18.0f * scale),
                      2.2f * scale, StringAlignmentFar);

    // Vertical rail note on the right edge.
    const GraphicsState rail_state = graphics.Save();
    graphics.TranslateTransform(width - padding + 14.0f * scale,
                                layout.header_line +
                                    (layout.status_y - layout.header_line) / 2.0f);
    graphics.RotateTransform(90.0f);
    draw_text_tracked(graphics, L"LOCAL EXECUTION ONLY — SELF-HOSTED UTILITY SYSTEM",
                      g_display_family, 9.5f * scale, FontStyleBold,
                      make_color(kInk, 56),
                      RectF(-180.0f * scale, -6.0f * scale, 360.0f * scale,
                            16.0f * scale),
                      2.0f * scale, StringAlignmentCenter);
    graphics.Restore(rail_state);
}

// Per-second patches: header clock + status-bar uptime.
void draw_clock_patches(Graphics& graphics, const Layout& layout) {
    const float scale = layout.scale;
    draw_text_tracked(graphics, clock_text(), g_mono_family, 12.0f * scale,
                      FontStyleBold, make_color(kInk),
                      RectF(layout.width - layout.padding - 260.0f * scale,
                            31.0f * scale, 120.0f * scale, 20.0f * scale),
                      1.2f * scale, StringAlignmentFar);
    const float uptime_right = layout.folder.X - 18.0f * scale - 100.0f * scale;
    draw_text_tracked(graphics, uptime_text(), g_mono_family, 11.0f * scale,
                      FontStyleBold, make_color(kMuted, 220),
                      RectF(uptime_right - 112.0f * scale,
                            layout.status_y + 23.0f * scale,
                            112.0f * scale, 20.0f * scale),
                      0.8f * scale, StringAlignmentFar);
}

void draw_tool_card(Graphics& graphics, const Layout& layout, int index) {
    const Tool& tool = kTools[index];
    const float scale = layout.scale;
    const bool hover = g_hover_target == index && g_window_focused;
    const float amount = hover ? 1.0f : 0.0f;
    RectF rect = layout.cards[index];
    rect.Y -= amount * 8.0f * scale;
    const float pressed = g_pressed_target == index ? 3.0f * scale : 0.0f;
    rect.Y += pressed;

    // Grounding shadow deepens on hover.
    fill_round_rect(graphics,
                    RectF(rect.X + 4.0f * scale, rect.Y + 10.0f * scale,
                          rect.Width, rect.Height),
                    16.0f * scale,
                    make_color(kInk, static_cast<BYTE>(20 + amount * 22)));

    fill_round_rect(graphics, rect, 16.0f * scale, make_color(0xFFFFFF));
    stroke_round_rect(graphics, rect, 16.0f * scale,
                      make_color(mix_rgb(kInk, tool.accent, amount * 0.55f),
                                 static_cast<BYTE>(40 + amount * 80)),
                      1.0f * scale);

    // Accent edge bar.
    fill_round_rect(graphics,
                    RectF(rect.X, rect.Y + 18.0f * scale, 4.0f * scale,
                          rect.Height - 36.0f * scale),
                    2.0f * scale,
                    make_color(tool.accent, static_cast<BYTE>(200 + amount * 55)));

    const Color foreground = make_color(kInk);
    const Color quiet = make_color(kMuted);
    const float inset = 26.0f * scale;

    // Ghost numeral clipped inside the card.
    {
        GraphicsState state = graphics.Save();
        graphics.SetClip(RectF(rect.X, rect.Y, rect.Width, rect.Height));
        draw_text_outline(graphics, index == 0 ? L"01" : index == 1 ? L"02" : L"03",
                          g_display_family, FontStyleBold, 92.0f * scale,
                          make_color(tool.accent,
                                     static_cast<BYTE>(95 + amount * 70)),
                          1.6f * scale,
                          RectF(rect.GetRight() - inset - 150.0f * scale,
                                rect.GetBottom() - 178.0f * scale,
                                150.0f * scale, 134.0f * scale),
                          StringAlignmentFar);
        graphics.Restore(state);
    }

    draw_text_tracked(graphics, tool.eyebrow, g_display_family, 11.0f * scale,
                      FontStyleBold, quiet,
                      RectF(rect.X + inset + 4.0f * scale, rect.Y + 26.0f * scale,
                            rect.Width - inset * 2.0f - 52.0f * scale,
                            16.0f * scale),
                      2.4f * scale);

    // Accent icon chip.
    const float chip = 44.0f * scale;
    const float chip_grow = amount * 3.0f * scale;
    const RectF chip_bounds(rect.GetRight() - inset - chip + chip_grow / 2.0f,
                            rect.Y + 20.0f * scale - chip_grow / 2.0f,
                            chip + chip_grow, chip + chip_grow);
    fill_round_rect(graphics, chip_bounds, 12.0f * scale,
                    make_color(tool.accent, static_cast<BYTE>(225 + amount * 30)));
    draw_lucide_icon(graphics, tool.icon,
                     RectF(chip_bounds.X + chip * 0.22f, chip_bounds.Y + chip * 0.22f,
                           chip * 0.56f, chip * 0.56f),
                     make_color(tool.glyph), 2.0f);

    draw_text(graphics, tool.name, g_display_family, 30.0f * scale, FontStyleBold,
              foreground,
              RectF(rect.X + inset, rect.Y + 88.0f * scale,
                    rect.Width - inset * 2.0f, 46.0f * scale));
    draw_text(graphics, tool.description, g_text_family, 14.5f * scale,
              FontStyleRegular, quiet,
              RectF(rect.X + inset, rect.Y + 136.0f * scale,
                    rect.Width - inset * 2.0f, 54.0f * scale),
              StringAlignmentNear, true);

    const float footer_y = rect.GetBottom() - 58.0f * scale;
    Pen divider(make_color(kInk, 28), 1.0f * scale);
    graphics.DrawLine(&divider, rect.X + inset, footer_y,
                      rect.GetRight() - inset, footer_y);

    const LucideIcon state_icon =
        g_present[index] ? LucideIcon::CircleCheck : LucideIcon::CircleAlert;
    draw_lucide_icon(graphics, state_icon,
                     RectF(rect.X + inset, footer_y + 18.0f * scale,
                           15.0f * scale, 15.0f * scale),
                     make_color(g_present[index] ? kSuccess : kError), 1.8f);
    draw_text_tracked(graphics, g_present[index] ? tool.meta : L"MODULE MISSING",
                      g_display_family, 10.0f * scale, FontStyleBold, quiet,
                      RectF(rect.X + inset + 24.0f * scale, footer_y + 17.0f * scale,
                            rect.Width - 96.0f * scale, 18.0f * scale),
                      1.8f * scale);

    const float arrow_shift = amount * 4.0f * scale;
    draw_lucide_icon(graphics, LucideIcon::ArrowUpRight,
                     RectF(rect.GetRight() - inset - 20.0f * scale + arrow_shift,
                           footer_y + 15.0f * scale - arrow_shift,
                           20.0f * scale, 20.0f * scale),
                     foreground, 1.9f);
}

// Static right cluster of the status bar (uptime is a per-second patch).
void draw_status_bar(Graphics& graphics, const Layout& layout) {
    const float scale = layout.scale;
    const float deck_right = layout.folder.X - 18.0f * scale;
    draw_lucide_icon(graphics, LucideIcon::Activity,
                     RectF(deck_right - 230.0f * scale,
                           layout.status_y + 24.0f * scale, 15.0f * scale,
                           15.0f * scale),
                     make_color(kMuted, 200), 1.8f);
    draw_text_tracked(graphics, L"DECK ONLINE", g_display_family, 9.5f * scale,
                      FontStyleBold, make_color(kMuted, 170),
                      RectF(deck_right - 96.0f * scale,
                            layout.status_y + 25.0f * scale,
                            96.0f * scale, 16.0f * scale),
                      2.0f * scale, StringAlignmentFar);
}

// Per-event patch: status icon + message on the left of the status bar.
void draw_status_message(Graphics& graphics, const Layout& layout) {
    const float scale = layout.scale;
    const float padding = layout.padding;
    const float icon_size = 17.0f * scale;
    const unsigned int status_color = g_status_error ? kError : kSuccess;
    draw_lucide_icon(graphics,
                     g_status_error ? LucideIcon::CircleAlert : LucideIcon::CircleCheck,
                     RectF(padding, layout.status_y + 23.0f * scale, icon_size,
                           icon_size),
                     make_color(status_color), 1.8f);
    draw_text(graphics, g_status, g_text_family, 13.0f * scale, FontStyleRegular,
              make_color(kInk, 210),
              RectF(padding + 27.0f * scale, layout.status_y + 21.0f * scale,
                    layout.width * 0.55f, 25.0f * scale));
}

Layout calculate_layout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    const float scale = window_scale(window);
    const float padding = 44.0f * scale;
    const float gap = 16.0f * scale;
    const float status_height = 64.0f * scale;
    const float header_line = 246.0f * scale;
    const float cards_bottom = height - status_height - 22.0f * scale;
    float cards_top = header_line + 26.0f * scale;
    if (cards_bottom - cards_top < 240.0f * scale) {
        cards_top = cards_bottom - 240.0f * scale;
    }
    const float card_width = (width - padding * 2.0f - gap * 2.0f) / kToolCount;

    Layout layout;
    layout.scale = scale;
    layout.width = width;
    layout.height = height;
    layout.padding = padding;
    layout.header_line = header_line;
    layout.folder = RectF(width - padding - 156.0f * scale,
                          height - status_height + 13.0f * scale,
                          156.0f * scale, 38.0f * scale);
    for (int index = 0; index < kToolCount; ++index) {
        layout.cards[index] = RectF(
            padding + index * (card_width + gap), cards_top,
            card_width, cards_bottom - cards_top);
    }
    layout.status_y = height - status_height;
    return layout;
}

// Redraw the raw paper background inside a region (used before repainting a
// lifted card so no ghost edge of the idle card remains underneath).
void erase_region(Graphics& graphics, const Layout& layout, const RectF& region) {
    const GraphicsState state = graphics.Save();
    graphics.SetClip(region);
    draw_background(graphics, layout.width, layout.height, layout);
    graphics.Restore(state);
}

void render_base(const Layout& layout) {
    // Base always depicts the idle scene regardless of live interaction state.
    const int saved_hover = g_hover_target;
    const int saved_pressed = g_pressed_target;
    const bool saved_keyboard = g_keyboard_navigation;
    g_hover_target = kNoTarget;
    g_pressed_target = kNoTarget;
    g_keyboard_navigation = false;

    Graphics graphics(g_base);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    graphics.SetTextContrast(0);
    draw_background(graphics, layout.width, layout.height, layout);
    draw_header(graphics, layout);
    for (int index = 0; index < kToolCount; ++index) {
        draw_tool_card(graphics, layout, index);
    }
    draw_folder_control(graphics, layout);
    draw_status_bar(graphics, layout);

    g_hover_target = saved_hover;
    g_pressed_target = saved_pressed;
    g_keyboard_navigation = saved_keyboard;
}

void render(HWND window, HDC target) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    if (g_frame == nullptr || g_base == nullptr ||
        g_frame->GetWidth() != static_cast<UINT>(width) ||
        g_frame->GetHeight() != static_cast<UINT>(height)) {
        delete g_frame;
        delete g_base;
        // PARGB is GDI+'s native format — non-alpha formats get converted on
        // every DrawImage, which is the classic hidden perf trap.
        g_frame = new Bitmap(width, height, PixelFormat32bppPARGB);
        g_base = new Bitmap(width, height, PixelFormat32bppPARGB);
        g_base_dirty = true;
        g_status_dirty = true;
    }

    const Layout layout = calculate_layout(window);
    if (g_base_dirty) {
        render_base(layout);
        g_base_dirty = false;
        g_status_dirty = true;
    }

    // Compose: cached idle scene + small dynamic patches. Repaint cost is a
    // couple of blits plus whatever is actually alive right now.
    Graphics graphics(g_frame);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    graphics.SetTextContrast(0);
    graphics.DrawImage(g_base, 0.0f, 0.0f,
                       static_cast<REAL>(width), static_cast<REAL>(height));

    const float scale = layout.scale;
    restore_base(graphics, RectF(layout.width - layout.padding - 262.0f * scale,
                                 28.0f * scale, 126.0f * scale, 26.0f * scale));
    const float uptime_right = layout.folder.X - 118.0f * scale;
    restore_base(graphics, RectF(uptime_right - 114.0f * scale,
                                 layout.status_y + 20.0f * scale,
                                 118.0f * scale, 26.0f * scale));
    restore_base(graphics, RectF(layout.padding - 4.0f * scale,
                                 layout.status_y + 16.0f * scale,
                                 layout.width * 0.55f + 60.0f * scale,
                                 34.0f * scale));
    draw_clock_patches(graphics, layout);
    draw_status_message(graphics, layout);

    if (g_window_focused) {
        int active_card = kNoTarget;
        if (g_pressed_target >= 0 && g_pressed_target < kToolCount) {
            active_card = g_pressed_target;
        } else if (g_hover_target >= 0 && g_hover_target < kToolCount) {
            active_card = g_hover_target;
        }
        if (active_card != kNoTarget) {
            RectF region = layout.cards[active_card];
            region.X -= 8.0f * scale;
            region.Y -= 16.0f * scale;
            region.Width += 16.0f * scale;
            region.Height += 38.0f * scale;
            erase_region(graphics, layout, region);
            draw_tool_card(graphics, layout, active_card);
        }
        if (g_hover_target == kFolderTarget) {
            RectF region = layout.folder;
            region.X -= 4.0f * scale;
            region.Y -= 4.0f * scale;
            region.Width += 8.0f * scale;
            region.Height += 8.0f * scale;
            restore_base(graphics, region);
            draw_folder_control(graphics, layout);
        }
        draw_focus_ring(graphics, layout, g_keyboard_navigation ? g_keyboard_target
                                                                : kNoTarget);
    }

    Graphics output(target);
    output.SetCompositingMode(CompositingModeSourceCopy);
    output.DrawImage(g_frame, 0, 0);
}

// ---------------------------------------------------------------------------
// Interaction plumbing
// ---------------------------------------------------------------------------
bool point_in_rect(const RectF& rect, POINT point) {
    return point.x >= rect.X && point.x <= rect.GetRight() &&
           point.y >= rect.Y && point.y <= rect.GetBottom();
}

int hit_test(HWND window, POINT point) {
    const Layout layout = calculate_layout(window);
    if (point_in_rect(layout.folder, point)) {
        return kFolderTarget;
    }
    for (int index = 0; index < kToolCount; ++index) {
        RectF hit = layout.cards[index];
        hit.Y -= 10.0f * layout.scale;
        hit.Height += 18.0f * layout.scale;
        if (point_in_rect(hit, point)) {
            return index;
        }
    }
    return kNoTarget;
}

void activate_target(HWND window, int target) {
    if (target >= 0 && target < kToolCount) {
        launch_tool(target);
    } else if (target == kFolderTarget) {
        open_tools_directory(window);
    }
}

void update_pointer(HWND window, LPARAM l_param) {
    const POINT point = {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    const int target = hit_test(window, point);
    if (target != g_hover_target) {
        g_hover_target = target;
        InvalidateRect(window, nullptr, FALSE);
    }
}

HICON create_app_icon(int size) {
    // Runtime twin of src\toolbox.ico: dark tile canvas, three accents + slot.
    Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.Clear(Color(0, 0, 0, 0));
    const float u = size / 100.0f;
    const float inset = 4.0f * u;
    fill_round_rect(graphics, RectF(inset, inset, size - inset * 2.0f,
                                    size - inset * 2.0f),
                    21.0f * u, make_color(0x10110F));
    const float pad = 19.0f * u;
    const float gap = 8.0f * u;
    const float tile = (size - 2.0f * pad - gap) / 2.0f;
    const float xs[2] = {pad, pad + tile + gap};
    const float ys[2] = {pad, pad + tile + gap};
    const unsigned int colors[3] = {kCoral, kBlue, kLime};
    for (int i = 0; i < 3; ++i) {
        fill_round_rect(graphics,
                        RectF(xs[i % 2], ys[i / 2], tile, tile),
                        tile * 0.30f, make_color(colors[i]));
    }
    HICON icon = nullptr;
    bitmap.GetHICON(&icon);
    return icon;
}

void style_title_bar(HWND window) {
    const BOOL dark = FALSE;
    const COLORREF caption = RGB(243, 244, 241);
    const COLORREF text = RGB(20, 21, 15);
    const COLORREF border = RGB(228, 230, 223);
    constexpr DWORD use_immersive_dark_mode = 20;
    constexpr DWORD border_color = 34;
    constexpr DWORD caption_color = 35;
    constexpr DWORD text_color = 36;
    DwmSetWindowAttribute(window, use_immersive_dark_mode, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, caption_color, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, text_color, &text, sizeof(text));
    DwmSetWindowAttribute(window, border_color, &border, sizeof(border));
}

void center_window(HWND window) {
    RECT rect{};
    GetWindowRect(window, &rect);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    const int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param,
                             LPARAM l_param) {
    switch (message) {
    case WM_CREATE:
        g_window = window;
        refresh_presence();
        style_title_bar(window);
        SetTimer(window, kClockTimer, 1000, nullptr);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        render(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(l_param);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(l_param);
        const float scale = window_scale(window);
        info->ptMinTrackSize.x = static_cast<LONG>(900.0f * scale);
        info->ptMinTrackSize.y = static_cast<LONG>(620.0f * scale);
        return 0;
    }
    case WM_MOUSEMOVE:
        update_pointer(window, l_param);
        return 0;
    case WM_MOUSELEAVE:
        g_hover_target = kNoTarget;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        g_keyboard_navigation = false;
        POINT point = {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        g_pressed_target = hit_test(window, point);
        if (g_pressed_target != kNoTarget) {
            g_keyboard_target = g_pressed_target;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT point = {GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        const int released_target = hit_test(window, point);
        const int pressed_target = g_pressed_target;
        g_pressed_target = kNoTarget;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        InvalidateRect(window, nullptr, FALSE);
        if (pressed_target != kNoTarget && pressed_target == released_target) {
            activate_target(window, pressed_target);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_pressed_target = kNoTarget;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        g_keyboard_navigation = true;
        if (w_param == VK_TAB || w_param == VK_RIGHT || w_param == VK_DOWN) {
            g_keyboard_target = (g_keyboard_target + 1) % (kToolCount + 1);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (w_param == VK_LEFT || w_param == VK_UP) {
            g_keyboard_target = (g_keyboard_target + kToolCount) % (kToolCount + 1);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (w_param == VK_RETURN || w_param == VK_SPACE) {
            activate_target(window, g_keyboard_target);
            return 0;
        }
        if (w_param >= '1' && w_param <= '3') {
            g_keyboard_target = static_cast<int>(w_param - '1');
            activate_target(window, g_keyboard_target);
            return 0;
        }
        break;
    case WM_SETFOCUS:
        g_window_focused = true;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        g_window_focused = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l_param) == HTCLIENT && g_hover_target != kNoTarget) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (w_param == kClockTimer) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            if (now.wSecond != g_last_second) {
                g_last_second = now.wSecond;
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, kClockTimer);
        g_window = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

int check_mode() {
    g_root = executable_directory();
    return all_tools_present() ? 0 : 1;
}

void enable_high_dpi() {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    const auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"),
                       "SetProcessDpiAwarenessContext"));
    if (set_context == nullptr || !set_context(reinterpret_cast<HANDLE>(-4))) {
        SetProcessDPIAware();
    }
}

float system_scale() {
    HDC screen = GetDC(nullptr);
    const int dpi = screen == nullptr ? 96 : GetDeviceCaps(screen, LOGPIXELSX);
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }
    return static_cast<float>(dpi) / 96.0f;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line,
                    int show_command) {
    std::wstring arguments = command_line == nullptr ? L"" : command_line;
    const std::size_t first = arguments.find_first_not_of(L" \t\r\n");
    if (first != std::wstring::npos) {
        arguments.erase(0, first);
    } else {
        arguments.clear();
    }
    if (arguments.find(L"--check") != std::wstring::npos) {
        return check_mode();
    }

    enable_high_dpi();
    GdiplusStartupInput gdiplus_input;
    if (GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr) != Ok) {
        MessageBoxW(nullptr, L"无法初始化图形渲染器。", L"自用工具箱",
                    MB_OK);
        return 1;
    }

    init_font_families();
    g_root = executable_directory();
    g_large_icon = create_app_icon(32);
    g_small_icon = create_app_icon(16);

    const wchar_t* class_name = L"PersonalToolboxCanvas";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = g_large_icon;
    window_class.hIconSm = g_small_icon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = class_name;
    if (!RegisterClassExW(&window_class)) {
        MessageBoxW(nullptr, L"无法注册工具箱窗口。", L"自用工具箱",
                    MB_OK);
        DestroyIcon(g_large_icon);
        DestroyIcon(g_small_icon);
        GdiplusShutdown(g_gdiplus_token);
        return 1;
    }

    const float scale = system_scale();
    HWND window = CreateWindowExW(
        0, class_name, L"自用工具箱 / PERSONAL SYSTEM", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(1180.0f * scale), static_cast<int>(760.0f * scale),
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"无法创建工具箱窗口。", L"自用工具箱",
                    MB_OK);
        DestroyIcon(g_large_icon);
        DestroyIcon(g_small_icon);
        GdiplusShutdown(g_gdiplus_token);
        return 1;
    }

    center_window(window);
    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    DestroyIcon(g_large_icon);
    DestroyIcon(g_small_icon);
    GdiplusShutdown(g_gdiplus_token);
    return static_cast<int>(message.wParam);
}
