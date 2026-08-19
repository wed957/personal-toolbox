#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kIccButton = 1001;
constexpr int kMuxButton = 1002;
constexpr int kIyxButton = 1003;
constexpr int kKeyboardButton = 1004;
constexpr int kFolderButton = 1005;

struct Tool {
    const wchar_t* name;
    const wchar_t* description;
    const wchar_t* executable;
};

constexpr Tool kTools[] = {
    {L"ICC Switch", L"显示器 ICC / ICM 色彩配置", L"tools\\icc-switch-gui.exe"},
    {L"MUX", L"显示拓扑和屏幕布局切换", L"tools\\MUX.exe"},
    {L"IYX Fast Launcher", L"启动 IYX 本地驱动界面", L"tools\\IYX.exe"},
    {L"键盘检查", L"只读监控磁轴键盘 HID 数据", L"tools\\keyboard-check.exe"},
};

HWND g_status = nullptr;
HFONT g_body_font = nullptr;
HFONT g_title_font = nullptr;
fs::path g_root;
bool g_status_error = false;

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

HWND make_control(HWND parent, const wchar_t* class_name, const wchar_t* text,
                  DWORD style, int x, int y, int width, int height, int id = 0) {
    return CreateWindowExW(
        0, class_name, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
}

void set_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void set_status(const std::wstring& text, bool error = false) {
    if (g_status == nullptr) {
        return;
    }
    SetWindowTextW(g_status, text.c_str());
    g_status_error = error;
    InvalidateRect(g_status, nullptr, TRUE);
}

bool all_tools_present() {
    for (const Tool& tool : kTools) {
        if (!fs::is_regular_file(g_root / tool.executable)) {
            return false;
        }
    }
    return true;
}

std::wstring tool_status() {
    std::wstring text = L"工具状态\r\n";
    for (const Tool& tool : kTools) {
        const bool present = fs::is_regular_file(g_root / tool.executable);
        text += present ? L"[已就绪] " : L"[缺少]   ";
        text += tool.name;
        text += L"  -  ";
        text += tool.description;
        text += L"\r\n";
    }
    return text;
}

void launch_tool(HWND owner, const Tool& tool) {
    const fs::path path = g_root / tool.executable;
    if (!fs::is_regular_file(path)) {
        set_status(std::wstring(L"找不到 ") + tool.name + L"，请先运行 build.cmd。", true);
        MessageBoxW(owner, (std::wstring(L"工具尚未构建：\r\n") + path.wstring()).c_str(),
                    L"自用工具箱", MB_OK | MB_ICONWARNING);
        return;
    }

    std::wstring command_line = L"\"" + path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, g_root.c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        set_status(std::wstring(L"启动失败：") + tool.name + L"（错误 " +
                             std::to_wstring(error) + L"）", true);
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    set_status(std::wstring(L"已启动：") + tool.name);
}

void create_controls(HWND window) {
    g_body_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    g_title_font = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");

    HWND title = make_control(window, L"STATIC", L"自用工具箱", SS_LEFT,
                              28, 22, 664, 36);
    HWND subtitle = make_control(window, L"STATIC",
                                 L"集中启动显示、色彩、IYX 与键盘诊断工具。",
                                 SS_LEFT, 30, 62, 660, 26);
    HWND status_list = make_control(window, L"STATIC", tool_status().c_str(),
                                    SS_LEFT | SS_NOPREFIX, 30, 102, 660, 112);
    g_status = make_control(window, L"STATIC", L"就绪。", SS_LEFT,
                            30, 352, 660, 28);

    set_font(title, g_title_font);
    set_font(subtitle, g_body_font);
    set_font(status_list, g_body_font);
    set_font(g_status, g_body_font);

    const int button_width = 154;
    const int button_height = 44;
    const int gap = 12;
    const int start_x = 30;
    const int y = 238;
    for (int index = 0; index < 4; ++index) {
        const Tool& tool = kTools[index];
        HWND button = make_control(window, L"BUTTON", tool.name,
                                   BS_PUSHBUTTON | WS_TABSTOP,
                                   start_x + index * (button_width + gap), y,
                                   button_width, button_height, kIccButton + index);
        set_font(button, g_body_font);
    }

    HWND folder = make_control(window, L"BUTTON", L"打开工具目录",
                               BS_PUSHBUTTON | WS_TABSTOP, 30, 302, 154, 38,
                               kFolderButton);
    set_font(folder, g_body_font);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param,
                             LPARAM l_param) {
    switch (message) {
    case WM_CREATE:
        create_controls(window);
        return 0;
    case WM_COMMAND:
        if (HIWORD(w_param) == BN_CLICKED) {
            const int id = LOWORD(w_param);
            if (id >= kIccButton && id <= kKeyboardButton) {
                launch_tool(window, kTools[id - kIccButton]);
            } else if (id == kFolderButton) {
                ShellExecuteW(window, L"open", g_root.c_str(), nullptr,
                              g_root.c_str(), SW_SHOWNORMAL);
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(w_param);
        SetBkColor(dc, RGB(250, 250, 250));
        SetTextColor(dc, (reinterpret_cast<HWND>(l_param) == g_status && g_status_error)
                              ? RGB(178, 32, 42)
                              : RGB(35, 38, 42));
        static HBRUSH background = CreateSolidBrush(RGB(250, 250, 250));
        return reinterpret_cast<LRESULT>(background);
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(w_param), &rect,
                 reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        return 1;
    }
    case WM_DESTROY:
        DeleteObject(g_body_font);
        DeleteObject(g_title_font);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

int check_mode() {
    g_root = executable_directory();
    return all_tools_present() ? 0 : 1;
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

    SetProcessDPIAware();
    g_root = executable_directory();

    const wchar_t* class_name = L"PersonalToolboxWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    window_class.lpszClassName = class_name;
    if (!RegisterClassExW(&window_class)) {
        MessageBoxW(nullptr, L"无法注册工具箱窗口。", L"自用工具箱",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND window = CreateWindowExW(
        0, class_name, L"自用工具箱", WS_OVERLAPPED | WS_CAPTION |
        WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 730, 430,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"无法创建工具箱窗口。", L"自用工具箱",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
