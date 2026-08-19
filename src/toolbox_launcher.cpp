#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Gdiplus;

namespace {

constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kAnimationIntervalMs = 16;
constexpr int kToolCount = 3;
constexpr int kFolderTarget = kToolCount;
constexpr int kNoTarget = -1;

enum class LucideIcon {
    Palette,
    PanelsTopLeft,
    Gauge,
    FolderOpen,
    ArrowUpRight,
    CircleCheck,
    CircleAlert,
};

struct Tool {
    const wchar_t* eyebrow;
    const wchar_t* name;
    const wchar_t* description;
    const wchar_t* meta;
    const wchar_t* executable;
    unsigned int accent;
    unsigned int foreground;
    LucideIcon icon;
};

constexpr std::array<Tool, kToolCount> kTools = {{
    {L"COLOR / 01", L"ICC SWITCH", L"显示器色彩配置与快速切换", L"PROFILE CONTROL",
     L"tools\\icc-switch-gui.exe", 0xFF654F, 0x10110F, LucideIcon::Palette},
    {L"DISPLAY / 02", L"MUX", L"显示拓扑与屏幕布局管理", L"DISPLAY ROUTING",
     L"tools\\MUX.exe", 0x2864FF, 0xFFFFFF, LucideIcon::PanelsTopLeft},
    {L"DRIVER / 03", L"IYX", L"驱动界面与磁轴键盘检测", L"LOCAL SERVICE",
     L"tools\\IYX.exe", 0xC8F43D, 0x10110F, LucideIcon::Gauge},
}};

struct Layout {
    float scale = 1.0f;
    RectF folder;
    std::array<RectF, kToolCount> cards;
    float status_y = 0.0f;
};

fs::path g_root;
std::array<bool, kToolCount> g_present{};
std::array<float, kToolCount + 1> g_hover{};
HWND g_window = nullptr;
HICON g_large_icon = nullptr;
HICON g_small_icon = nullptr;
ULONG_PTR g_gdiplus_token = 0;
std::wstring g_status = L"系统就绪 / 3 个模块在线";
bool g_status_error = false;
bool g_tracking_mouse = false;
bool g_window_focused = true;
bool g_keyboard_navigation = false;
int g_hover_target = kNoTarget;
int g_pressed_target = kNoTarget;
int g_keyboard_target = 0;
POINT g_mouse = {-1, -1};
float g_intro = 0.0f;
float g_phase = 0.0f;
float g_status_flash = 0.0f;

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
    }
}

void draw_text(Graphics& graphics, const std::wstring& text, const wchar_t* family,
               float size, int style, const Color& color, const RectF& bounds,
               StringAlignment alignment = StringAlignmentNear,
               bool wrap = false) {
    Font font(family, size, style, UnitPixel);
    SolidBrush brush(color);
    StringFormat format;
    format.SetAlignment(alignment);
    format.SetLineAlignment(StringAlignmentNear);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) {
        format.SetFormatFlags(StringFormatFlagsNoWrap);
    }
    graphics.DrawString(text.c_str(), -1, &font, bounds, &format, &brush);
}

float window_scale(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT dpi = get_dpi == nullptr ? 96 : get_dpi(window);
    return static_cast<float>(dpi) / 96.0f;
}

Layout calculate_layout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    const float scale = window_scale(window);
    const float padding = 38.0f * scale;
    const float gap = 14.0f * scale;
    const float status_height = 66.0f * scale;
    const float cards_bottom = height - status_height - 12.0f * scale;
    float cards_top = std::max(260.0f * scale, height - 358.0f * scale);
    if (cards_bottom - cards_top < 238.0f * scale) {
        cards_top = cards_bottom - 238.0f * scale;
    }
    const float card_width = (width - padding * 2.0f - gap * 2.0f) / kToolCount;

    Layout layout;
    layout.scale = scale;
    layout.folder = RectF(width - padding - 178.0f * scale, 22.0f * scale,
                          178.0f * scale, 42.0f * scale);
    for (int index = 0; index < kToolCount; ++index) {
        layout.cards[index] = RectF(
            padding + index * (card_width + gap), cards_top,
            card_width, cards_bottom - cards_top);
    }
    layout.status_y = height - status_height;
    return layout;
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
    g_status_flash = 1.0f;
    if (g_window != nullptr) {
        InvalidateRect(g_window, nullptr, FALSE);
    }
}

void launch_tool(int index) {
    const Tool& tool = kTools[index];
    const fs::path path = g_root / tool.executable;
    if (!fs::is_regular_file(path)) {
        g_present[index] = false;
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

float ease_out_cubic(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    const float inverse = 1.0f - value;
    return 1.0f - inverse * inverse * inverse;
}

void draw_background(Graphics& graphics, float width, float height,
                     const Layout& layout) {
    graphics.Clear(make_color(0xF3F4F0));
    const float scale = layout.scale;
    Pen grid(make_color(0x10110F, 18), 1.0f);
    const float step = 42.0f * scale;
    const float offset_x = std::fmod(g_phase * 7.0f, step);
    for (float x = -step + offset_x; x < width + step; x += step) {
        graphics.DrawLine(&grid, x, 0.0f, x, height);
    }
    for (float y = 0.0f; y < height; y += step) {
        graphics.DrawLine(&grid, 0.0f, y, width, y);
    }

    Pen frame(make_color(0x10110F, 45), 1.0f);
    const float padding = 38.0f * scale;
    graphics.DrawLine(&frame, padding, 0.0f, padding, height);
    graphics.DrawLine(&frame, width - padding, 0.0f, width - padding, height);
    graphics.DrawLine(&frame, 0.0f, 80.0f * scale, width, 80.0f * scale);
    graphics.DrawLine(&frame, 0.0f, layout.status_y, width, layout.status_y);

    const float rail_width = std::max(60.0f * scale, width - padding * 2.0f);
    const float rail_x = padding + std::fmod(g_phase * 70.0f, rail_width);
    Pen signal(make_color(0xFF654F), 3.0f * scale);
    graphics.DrawLine(&signal, rail_x, 80.0f * scale,
                      std::min(rail_x + 48.0f * scale, width - padding),
                      80.0f * scale);
}

void draw_folder_control(Graphics& graphics, const Layout& layout) {
    const float scale = layout.scale;
    const float hover = g_hover[kFolderTarget];
    const RectF rect = layout.folder;
    const unsigned int background = mix_rgb(0x10110F, 0x2C2F29, hover);
    fill_round_rect(graphics, rect, 6.0f * scale, make_color(background));
    if (g_window_focused && g_keyboard_navigation &&
        g_keyboard_target == kFolderTarget) {
        stroke_round_rect(graphics,
                          RectF(rect.X - 3.0f * scale, rect.Y - 3.0f * scale,
                                rect.Width + 6.0f * scale, rect.Height + 6.0f * scale),
                          7.0f * scale, make_color(0x2864FF), 2.0f * scale);
    }
    RectF icon(rect.X + 15.0f * scale, rect.Y + 11.0f * scale,
               20.0f * scale, 20.0f * scale);
    draw_lucide_icon(graphics, LucideIcon::FolderOpen, icon,
                     make_color(0xFFFFFF), 1.8f);
    draw_text(graphics, L"打开工具目录", L"Microsoft YaHei UI", 13.0f * scale,
              FontStyleRegular, make_color(0xFFFFFF),
              RectF(rect.X + 45.0f * scale, rect.Y + 10.0f * scale,
                    rect.Width - 55.0f * scale, 24.0f * scale));
}

void draw_header(Graphics& graphics, float width, const Layout& layout) {
    const float scale = layout.scale;
    const float padding = 38.0f * scale;
    const BYTE intro_alpha = static_cast<BYTE>(255.0f * ease_out_cubic(g_intro));

    draw_text(graphics, L"PERSONAL TOOLBOX / WINDOWS", L"Bahnschrift",
              11.0f * scale, FontStyleBold, make_color(0x10110F, intro_alpha),
              RectF(padding + 12.0f * scale, 30.0f * scale,
                    360.0f * scale, 24.0f * scale));
    SolidBrush marker(make_color(0xFF654F, intro_alpha));
    graphics.FillRectangle(&marker, padding, 31.0f * scale,
                           4.0f * scale, 14.0f * scale);

    const float title_offset = (1.0f - ease_out_cubic(g_intro)) * 24.0f * scale;
    draw_text(graphics, L"自用工具箱", L"Microsoft YaHei UI", 58.0f * scale,
              FontStyleBold, make_color(0x10110F, intro_alpha),
              RectF(padding, 104.0f * scale + title_offset,
                    width * 0.62f, 90.0f * scale));
    draw_text(graphics, L"DISPLAY  /  COLOR  /  DRIVER", L"Bahnschrift",
              13.0f * scale, FontStyleRegular, make_color(0x4C5048, intro_alpha),
              RectF(padding + 3.0f * scale, 190.0f * scale + title_offset,
                    width * 0.55f, 28.0f * scale));

    const float parallax = g_mouse.x < 0 ? 0.0f
        : (static_cast<float>(g_mouse.x) / std::max(width, 1.0f) - 0.5f) * 8.0f * scale;
    const float number_x = width - 230.0f * scale + parallax;
    draw_text(graphics, L"03", L"Bahnschrift", 94.0f * scale, FontStyleBold,
              make_color(0x10110F, 20),
              RectF(number_x, 87.0f * scale, 190.0f * scale, 110.0f * scale),
              StringAlignmentFar);
    draw_text(graphics, L"ACTIVE MODULES", L"Bahnschrift", 10.0f * scale,
              FontStyleBold, make_color(0x10110F, 145),
              RectF(width - 260.0f * scale, 202.0f * scale,
                    220.0f * scale, 20.0f * scale), StringAlignmentFar);
    draw_folder_control(graphics, layout);
}

void draw_tool_card(Graphics& graphics, const Layout& layout, int index) {
    const Tool& tool = kTools[index];
    const float scale = layout.scale;
    const float hover = g_hover[index];
    const float card_intro = ease_out_cubic(g_intro * 1.45f - index * 0.14f);
    RectF rect = layout.cards[index];
    rect.Y += (1.0f - card_intro) * 34.0f * scale - hover * 8.0f * scale;
    const float pressed = g_pressed_target == index ? 3.0f * scale : 0.0f;
    rect.Y += pressed;

    RectF shadow(rect.X, rect.Y + (8.0f - hover * 3.0f) * scale,
                 rect.Width, rect.Height);
    fill_round_rect(graphics, shadow, 8.0f * scale,
                    make_color(0x10110F, static_cast<BYTE>(18 + hover * 18)));
    const unsigned int accent = mix_rgb(tool.accent, 0xFFFFFF, hover * 0.08f);
    fill_round_rect(graphics, rect, 8.0f * scale, make_color(accent));

    if (g_window_focused && g_keyboard_navigation && g_keyboard_target == index) {
        stroke_round_rect(graphics,
                          RectF(rect.X - 3.0f * scale, rect.Y - 3.0f * scale,
                                rect.Width + 6.0f * scale, rect.Height + 6.0f * scale),
                          9.0f * scale, make_color(0x10110F), 2.0f * scale,
                          DashStyleDash);
    } else if (hover > 0.02f) {
        stroke_round_rect(graphics, rect, 8.0f * scale,
                          make_color(tool.foreground, static_cast<BYTE>(55 + hover * 80)),
                          1.0f * scale);
    }

    const Color foreground = make_color(tool.foreground);
    const Color quiet = make_color(tool.foreground, 155);
    const float inset = 20.0f * scale;
    draw_text(graphics, tool.eyebrow, L"Bahnschrift", 10.0f * scale,
              FontStyleBold, quiet,
              RectF(rect.X + inset, rect.Y + 21.0f * scale,
                    rect.Width - inset * 2.0f, 18.0f * scale));

    RectF icon_bounds(rect.GetRight() - 52.0f * scale,
                      rect.Y + 18.0f * scale + hover * 2.0f * scale,
                      28.0f * scale, 28.0f * scale);
    draw_lucide_icon(graphics, tool.icon, icon_bounds, foreground, 1.8f);

    draw_text(graphics, tool.name, L"Bahnschrift", 25.0f * scale,
              FontStyleBold, foreground,
              RectF(rect.X + inset, rect.Y + 88.0f * scale,
                    rect.Width - inset * 2.0f, 42.0f * scale));
    draw_text(graphics, tool.description, L"Microsoft YaHei UI", 13.0f * scale,
              FontStyleRegular, quiet,
              RectF(rect.X + inset, rect.Y + 132.0f * scale,
                    rect.Width - inset * 2.0f, 52.0f * scale),
              StringAlignmentNear, true);

    const float footer_y = rect.GetBottom() - 55.0f * scale;
    Pen divider(make_color(tool.foreground, 55), 1.0f * scale);
    graphics.DrawLine(&divider, rect.X + inset, footer_y,
                      rect.GetRight() - inset, footer_y);

    const LucideIcon state_icon = g_present[index]
        ? LucideIcon::CircleCheck : LucideIcon::CircleAlert;
    draw_lucide_icon(graphics, state_icon,
                     RectF(rect.X + inset, footer_y + 17.0f * scale,
                           16.0f * scale, 16.0f * scale),
                     foreground, 1.8f);
    draw_text(graphics, g_present[index] ? tool.meta : L"MODULE MISSING",
              L"Bahnschrift", 9.5f * scale, FontStyleBold, quiet,
              RectF(rect.X + inset + 24.0f * scale, footer_y + 16.0f * scale,
                    rect.Width - 92.0f * scale, 20.0f * scale));

    const float arrow_shift = hover * 3.0f * scale;
    draw_lucide_icon(graphics, LucideIcon::ArrowUpRight,
                     RectF(rect.GetRight() - inset - 20.0f * scale + arrow_shift,
                           footer_y + 14.0f * scale - arrow_shift,
                           20.0f * scale, 20.0f * scale),
                     foreground, 1.9f);
}

void draw_status_bar(Graphics& graphics, float width, float height,
                     const Layout& layout) {
    const float scale = layout.scale;
    const float padding = 38.0f * scale;
    const float icon_size = 18.0f * scale;
    const unsigned int status_color = g_status_error ? 0xD93F32 : 0x1C7C54;
    const BYTE pulse_alpha = static_cast<BYTE>(190 + g_status_flash * 65);
    draw_lucide_icon(graphics,
                     g_status_error ? LucideIcon::CircleAlert : LucideIcon::CircleCheck,
                     RectF(padding, layout.status_y + 22.0f * scale,
                           icon_size, icon_size),
                     make_color(status_color, pulse_alpha), 1.8f);
    draw_text(graphics, g_status, L"Microsoft YaHei UI", 12.0f * scale,
              FontStyleRegular, make_color(0x343730),
              RectF(padding + 28.0f * scale, layout.status_y + 20.0f * scale,
                    width * 0.65f, 25.0f * scale));
    draw_text(graphics, L"MAIN / ONLINE", L"Bahnschrift", 10.0f * scale,
              FontStyleBold, make_color(0x4C5048),
              RectF(width - padding - 180.0f * scale,
                    layout.status_y + 22.0f * scale,
                    180.0f * scale, 22.0f * scale), StringAlignmentFar);
    (void)height;
}

void render(HWND window, HDC target) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    Bitmap buffer(width, height, PixelFormat32bppPARGB);
    Graphics graphics(&buffer);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    const Layout layout = calculate_layout(window);
    draw_background(graphics, static_cast<float>(width), static_cast<float>(height), layout);
    draw_header(graphics, static_cast<float>(width), layout);
    for (int index = 0; index < kToolCount; ++index) {
        draw_tool_card(graphics, layout, index);
    }
    draw_status_bar(graphics, static_cast<float>(width), static_cast<float>(height), layout);

    Graphics output(target);
    output.DrawImage(&buffer, 0, 0, width, height);
}

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
    g_mouse.x = GET_X_LPARAM(l_param);
    g_mouse.y = GET_Y_LPARAM(l_param);
    const int target = hit_test(window, g_mouse);
    if (target != g_hover_target) {
        g_hover_target = target;
        InvalidateRect(window, nullptr, FALSE);
    }
    if (!g_tracking_mouse) {
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window;
        TrackMouseEvent(&tracking);
        g_tracking_mouse = true;
    }
}

HICON create_app_icon(int size) {
    Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.Clear(Color(0, 0, 0, 0));
    const float inset = std::max(1.0f, size * 0.06f);
    fill_round_rect(graphics, RectF(inset, inset, size - inset * 2.0f,
                                    size - inset * 2.0f),
                    size * 0.18f, make_color(0x10110F));
    draw_lucide_icon(graphics, LucideIcon::PanelsTopLeft,
                     RectF(size * 0.2f, size * 0.2f,
                           size * 0.6f, size * 0.6f),
                     make_color(0xC8F43D), 2.1f);
    HICON icon = nullptr;
    bitmap.GetHICON(&icon);
    return icon;
}

void style_title_bar(HWND window) {
    const BOOL dark = TRUE;
    const COLORREF caption = RGB(16, 17, 15);
    const COLORREF text = RGB(255, 255, 255);
    const COLORREF border = RGB(16, 17, 15);
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
        SetTimer(window, kAnimationTimer, kAnimationIntervalMs, nullptr);
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
        info->ptMinTrackSize.x = static_cast<LONG>(860.0f * scale);
        info->ptMinTrackSize.y = static_cast<LONG>(620.0f * scale);
        return 0;
    }
    case WM_MOUSEMOVE:
        update_pointer(window, l_param);
        return 0;
    case WM_MOUSELEAVE:
        g_tracking_mouse = false;
        g_hover_target = kNoTarget;
        g_mouse = {-1, -1};
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
        if (w_param == kAnimationTimer) {
            g_intro = std::min(1.0f, g_intro + 0.028f);
            g_phase = std::fmod(g_phase + 0.016f, 1000.0f);
            g_status_flash = std::max(0.0f, g_status_flash - 0.035f);
            for (int index = 0; index <= kToolCount; ++index) {
                const float target = index == g_hover_target ? 1.0f : 0.0f;
                g_hover[index] += (target - g_hover[index]) * 0.18f;
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, kAnimationTimer);
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
