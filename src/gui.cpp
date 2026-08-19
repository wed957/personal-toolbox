#include <windows.h>
#include <commdlg.h>

#include "resource.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kScopeSystem = 0;
constexpr int kScopeCurrentUser = 1;
constexpr int kProfileTypeIcc = 0;
constexpr int kProfileSubtypeNone = 4;

constexpr int kDisplayComboId = 1001;
constexpr int kProfileComboId = 1002;
constexpr int kImportButtonId = 1003;
constexpr int kRefreshButtonId = 1004;
constexpr int kApplyButtonId = 1005;
constexpr int kResetButtonId = 1006;
constexpr int kStatusLabelId = 1007;

using AddDisplayAssociationFn = HRESULT(WINAPI*)(int, PCWSTR, LUID, UINT32, BOOL, BOOL);
using SetDisplayDefaultFn = HRESULT(WINAPI*)(int, PCWSTR, int, int, LUID, UINT32);
using GetDisplayDefaultFn = HRESULT(WINAPI*)(int, LUID, UINT32, int, int, LPWSTR*);
using InstallColorProfileFn = BOOL(WINAPI*)(PCWSTR, PCWSTR);
using GetColorDirectoryFn = BOOL(WINAPI*)(PCWSTR, PWSTR, PDWORD);

class UiError final : public std::exception {
public:
    explicit UiError(std::wstring message) : message_(std::move(message)) {}
    const std::wstring& message() const { return message_; }

private:
    std::wstring message_;
};

std::wstring win32_message(DWORD error) {
    PWSTR buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, error, 0, reinterpret_cast<PWSTR>(&buffer), 0, nullptr);
    std::wstring message = size && buffer ? std::wstring(buffer, size) : L"未知错误";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::wstring hresult_message(HRESULT result) {
    wchar_t code[16]{};
    swprintf(code, 16, L"0x%08lX", static_cast<unsigned long>(result));
    return std::wstring(code) + L" - " + win32_message(static_cast<DWORD>(result));
}

struct ColorApi {
    HMODULE module = nullptr;
    AddDisplayAssociationFn add_display_association = nullptr;
    SetDisplayDefaultFn set_display_default = nullptr;
    GetDisplayDefaultFn get_display_default = nullptr;
    InstallColorProfileFn install_color_profile = nullptr;
    GetColorDirectoryFn get_color_directory = nullptr;

    ColorApi() {
        module = LoadLibraryW(L"mscms.dll");
        if (!module) {
            throw UiError(L"无法加载 Windows 色彩管理组件。\n" + win32_message(GetLastError()));
        }
        add_display_association = load<AddDisplayAssociationFn>("ColorProfileAddDisplayAssociation");
        set_display_default = load<SetDisplayDefaultFn>("ColorProfileSetDisplayDefaultAssociation");
        get_display_default = load<GetDisplayDefaultFn>("ColorProfileGetDisplayDefault");
        install_color_profile = load<InstallColorProfileFn>("InstallColorProfileW");
        get_color_directory = load<GetColorDirectoryFn>("GetColorDirectoryW");
        if (!add_display_association || !set_display_default || !get_display_default ||
            !install_color_profile) {
            throw UiError(L"当前 Windows 版本不支持所需的 ICC 接口。\n请使用 Windows 10 1809 或更高版本。");
        }
    }

    ~ColorApi() {
        if (module) {
            FreeLibrary(module);
        }
    }

    ColorApi(const ColorApi&) = delete;
    ColorApi& operator=(const ColorApi&) = delete;

private:
    template <typename T>
    T load(const char* name) const {
        return reinterpret_cast<T>(GetProcAddress(module, name));
    }
};

struct DisplayInfo {
    LUID adapter_id{};
    UINT32 source_id = 0;
    std::wstring gdi_name;
    std::wstring friendly_name;
    bool primary = false;
};

struct AppState {
    ColorApi color_api;
    HWND window = nullptr;
    HWND display_combo = nullptr;
    HWND profile_combo = nullptr;
    HWND current_label = nullptr;
    HWND status_label = nullptr;
    HFONT title_font = nullptr;
    HFONT body_font = nullptr;
    HFONT strong_font = nullptr;
    HBRUSH background_brush = nullptr;
    std::vector<DisplayInfo> displays;
    std::vector<fs::path> profiles;
};

std::unique_ptr<AppState> g_app;

fs::path color_directory(const ColorApi& api) {
    if (api.get_color_directory) {
        DWORD size = 0;
        api.get_color_directory(nullptr, nullptr, &size);
        if (size > 0) {
            std::wstring buffer(size, L'\0');
            if (api.get_color_directory(nullptr, buffer.data(), &size)) {
                buffer.resize(std::wcslen(buffer.c_str()));
                return fs::path(buffer);
            }
        }
    }

    wchar_t windows[MAX_PATH]{};
    const UINT size = GetWindowsDirectoryW(windows, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        throw UiError(L"无法读取 Windows 系统目录。");
    }
    return fs::path(windows) / L"System32" / L"spool" / L"drivers" / L"color";
}

bool is_color_profile(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    return extension == L".icc" || extension == L".icm";
}

std::vector<fs::path> load_profiles(const ColorApi& api) {
    std::vector<fs::path> profiles;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(color_directory(api), error)) {
        if (entry.is_regular_file(error) && is_color_profile(entry.path())) {
            profiles.push_back(entry.path());
        }
    }
    if (error) {
        throw UiError(L"无法读取系统 ICC 目录。");
    }
    std::sort(profiles.begin(), profiles.end(), [](const fs::path& left, const fs::path& right) {
        return _wcsicmp(left.filename().c_str(), right.filename().c_str()) < 0;
    });
    return profiles;
}

std::vector<DisplayInfo> load_displays() {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (status != ERROR_SUCCESS) {
        throw UiError(L"无法读取活动显示器。\n" + win32_message(static_cast<DWORD>(status)));
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                &mode_count, modes.data(), nullptr);
    if (status != ERROR_SUCCESS) {
        throw UiError(L"无法读取显示器配置。\n" + win32_message(static_cast<DWORD>(status)));
    }
    paths.resize(path_count);

    std::vector<DisplayInfo> displays;
    std::set<std::tuple<LONG, DWORD, UINT32>> seen;
    for (const auto& path : paths) {
        const auto key = std::make_tuple(path.targetInfo.adapterId.HighPart,
                                         path.targetInfo.adapterId.LowPart,
                                         path.sourceInfo.id);
        if (!seen.insert(key).second) {
            continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        DisplayConfigGetDeviceInfo(&source.header);

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        DisplayConfigGetDeviceInfo(&target.header);

        DisplayInfo display;
        display.adapter_id = path.targetInfo.adapterId;
        display.source_id = path.sourceInfo.id;
        display.gdi_name = source.viewGdiDeviceName;
        display.friendly_name = target.flags.friendlyNameFromEdid
                                    ? target.monitorFriendlyDeviceName
                                    : L"通用显示器";

        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &adapter, 0); ++index) {
            if (display.gdi_name == adapter.DeviceName) {
                display.primary = (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                break;
            }
            adapter = {};
            adapter.cb = sizeof(adapter);
        }
        displays.push_back(std::move(display));
    }

    std::stable_sort(displays.begin(), displays.end(), [](const DisplayInfo& left, const DisplayInfo& right) {
        return left.primary && !right.primary;
    });
    return displays;
}

std::wstring get_current_profile(const ColorApi& api, const DisplayInfo& display) {
    for (const int scope : {kScopeCurrentUser, kScopeSystem}) {
        LPWSTR profile = nullptr;
        const HRESULT result = api.get_display_default(scope, display.adapter_id, display.source_id,
                                                       kProfileTypeIcc, kProfileSubtypeNone, &profile);
        if (SUCCEEDED(result) && profile) {
            std::wstring value(profile);
            LocalFree(profile);
            return fs::path(value).filename().wstring();
        }
    }
    return {};
}

void apply_system_profile(const ColorApi& api, const DisplayInfo& display, const fs::path& profile) {
    const HRESULT result = api.add_display_association(kScopeCurrentUser,
                                                       profile.filename().c_str(),
                                                       display.adapter_id,
                                                       display.source_id,
                                                       TRUE,
                                                       FALSE);
    if (FAILED(result) && HRESULT_CODE(result) != ERROR_ALREADY_EXISTS &&
        HRESULT_CODE(result) != ERROR_FILE_EXISTS) {
        throw UiError(L"切换 ICC 失败。\n" + hresult_message(result));
    }
    const HRESULT default_result = api.set_display_default(
        kScopeCurrentUser, profile.filename().c_str(), kProfileTypeIcc, kProfileSubtypeNone,
        display.adapter_id, display.source_id);
    if (FAILED(default_result)) {
        throw UiError(L"设置默认 ICC 失败。\n" + hresult_message(default_result));
    }

    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Color"),
                        SMTO_ABORTIFHUNG, 1000, &ignored);
}

int scale_value(HWND window, int value) {
    const UINT dpi = GetDpiForWindow(window);
    return MulDiv(value, dpi ? static_cast<int>(dpi) : 96, 96);
}

void set_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND make_control(HWND parent, const wchar_t* class_name, const wchar_t* text,
                  DWORD style, int x, int y, int width, int height, int id = 0) {
    return CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
                           scale_value(parent, x), scale_value(parent, y),
                           scale_value(parent, width), scale_value(parent, height),
                           parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void set_status(const std::wstring& text, bool error = false) {
    if (!g_app || !g_app->status_label) {
        return;
    }
    SetWindowTextW(g_app->status_label, text.c_str());
    SetWindowLongPtrW(g_app->status_label, GWLP_USERDATA, error ? 1 : 0);
    InvalidateRect(g_app->status_label, nullptr, TRUE);
}

void update_current_profile() {
    const LRESULT display_index = SendMessageW(g_app->display_combo, CB_GETCURSEL, 0, 0);
    if (display_index == CB_ERR || static_cast<std::size_t>(display_index) >= g_app->displays.size()) {
        SetWindowTextW(g_app->current_label, L"当前配置：未检测到");
        return;
    }

    const std::wstring current = get_current_profile(
        g_app->color_api, g_app->displays[static_cast<std::size_t>(display_index)]);
    const std::wstring label = L"当前配置：" +
                               (current.empty() ? std::wstring(L"未设置") : current);
    SetWindowTextW(g_app->current_label, label.c_str());

    if (!current.empty()) {
        for (std::size_t index = 0; index < g_app->profiles.size(); ++index) {
            if (_wcsicmp(current.c_str(), g_app->profiles[index].filename().c_str()) == 0) {
                SendMessageW(g_app->profile_combo, CB_SETCURSEL, index, 0);
                break;
            }
        }
    }
}

void refresh_data() {
    const int previous_display = static_cast<int>(
        SendMessageW(g_app->display_combo, CB_GETCURSEL, 0, 0));
    g_app->displays = load_displays();
    g_app->profiles = load_profiles(g_app->color_api);

    SendMessageW(g_app->display_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& display : g_app->displays) {
        std::wstring label = display.friendly_name + L"  [" + display.gdi_name + L"]";
        if (display.primary) {
            label += L"  主屏";
        }
        SendMessageW(g_app->display_combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
    }

    SendMessageW(g_app->profile_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& profile : g_app->profiles) {
        SendMessageW(g_app->profile_combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(profile.filename().c_str()));
    }

    if (!g_app->displays.empty()) {
        const int selected = previous_display >= 0 &&
                                     previous_display < static_cast<int>(g_app->displays.size())
                                 ? previous_display
                                 : 0;
        SendMessageW(g_app->display_combo, CB_SETCURSEL, selected, 0);
    }
    if (!g_app->profiles.empty()) {
        SendMessageW(g_app->profile_combo, CB_SETCURSEL, 0, 0);
    }
    update_current_profile();
    set_status(L"已刷新。");
}

void import_profile() {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_app->window;
    dialog.lpstrFilter = L"ICC 配置文件 (*.icc;*.icm)\0*.icc;*.icm\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&dialog)) {
        return;
    }

    const fs::path source(file);
    if (!is_color_profile(source)) {
        throw UiError(L"请选择 .icc 或 .icm 文件。");
    }
    if (!g_app->color_api.install_color_profile(nullptr, source.c_str())) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            throw UiError(L"安装 ICC 失败。\n" + win32_message(error));
        }
    }

    refresh_data();
    for (std::size_t index = 0; index < g_app->profiles.size(); ++index) {
        if (_wcsicmp(g_app->profiles[index].filename().c_str(), source.filename().c_str()) == 0) {
            SendMessageW(g_app->profile_combo, CB_SETCURSEL, index, 0);
            break;
        }
    }
    set_status(L"ICC 已导入，请点击“应用”。");
}

void apply_selected_profile() {
    const LRESULT display_index = SendMessageW(g_app->display_combo, CB_GETCURSEL, 0, 0);
    const LRESULT profile_index = SendMessageW(g_app->profile_combo, CB_GETCURSEL, 0, 0);
    if (display_index == CB_ERR || profile_index == CB_ERR) {
        throw UiError(L"请先选择显示器和 ICC 配置。");
    }

    set_status(L"正在应用...");
    apply_system_profile(g_app->color_api,
                         g_app->displays[static_cast<std::size_t>(display_index)],
                         g_app->profiles[static_cast<std::size_t>(profile_index)]);
    update_current_profile();
    set_status(L"已应用到当前用户。");
}

void reset_all_displays() {
    const auto profile = std::find_if(g_app->profiles.begin(), g_app->profiles.end(),
                                      [](const fs::path& item) {
        return _wcsicmp(item.filename().c_str(), L"sRGB Color Space Profile.icm") == 0;
    });
    if (profile == g_app->profiles.end()) {
        throw UiError(L"系统默认 sRGB 配置不存在。");
    }
    if (g_app->displays.empty()) {
        throw UiError(L"未检测到活动显示器。");
    }

    set_status(L"正在恢复默认...");
    for (const auto& display : g_app->displays) {
        apply_system_profile(g_app->color_api, display, *profile);
    }
    update_current_profile();
    set_status(L"全部显示器已恢复为系统 sRGB。");
}

template <typename Action>
void run_ui_action(Action action) {
    try {
        action();
    } catch (const UiError& error) {
        set_status(error.message(), true);
        MessageBoxW(g_app ? g_app->window : nullptr, error.message().c_str(),
                    L"ICC Switch", MB_OK | MB_ICONERROR);
    } catch (...) {
        const std::wstring message = L"发生未知错误。";
        set_status(message, true);
        MessageBoxW(g_app ? g_app->window : nullptr, message.c_str(),
                    L"ICC Switch", MB_OK | MB_ICONERROR);
    }
}

void create_controls(HWND window) {
    g_app->window = window;
    const UINT dpi = GetDpiForWindow(window);
    const int body_height = -MulDiv(10, dpi, 72);
    const int title_height = -MulDiv(20, dpi, 72);
    g_app->body_font = CreateFontW(body_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_app->strong_font = CreateFontW(body_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_app->title_font = CreateFontW(title_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    HWND title = make_control(window, L"STATIC", L"ICC Switch", SS_LEFT,
                              28, 22, 584, 38);
    HWND subtitle = make_control(window, L"STATIC", L"选择显示器与 ICC，然后应用。", SS_LEFT,
                                 28, 62, 584, 24);
    HWND display_label = make_control(window, L"STATIC", L"显示器", SS_LEFT,
                                      28, 104, 584, 22);
    g_app->display_combo = make_control(window, L"COMBOBOX", L"",
                                        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                        28, 128, 584, 240, kDisplayComboId);
    HWND profile_label = make_control(window, L"STATIC", L"ICC 配置", SS_LEFT,
                                      28, 180, 584, 22);
    g_app->profile_combo = make_control(window, L"COMBOBOX", L"",
                                        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                        28, 204, 584, 240, kProfileComboId);
    g_app->current_label = make_control(window, L"STATIC", L"当前配置：读取中...", SS_LEFT,
                                        28, 256, 584, 24);
    HWND import_button = make_control(window, L"BUTTON", L"导入 ICC...",
                                      BS_PUSHBUTTON | WS_TABSTOP,
                                      28, 306, 118, 38, kImportButtonId);
    HWND reset_button = make_control(window, L"BUTTON", L"恢复默认",
                                     BS_PUSHBUTTON | WS_TABSTOP,
                                     158, 306, 142, 38, kResetButtonId);
    HWND refresh_button = make_control(window, L"BUTTON", L"刷新",
                                       BS_PUSHBUTTON | WS_TABSTOP,
                                       366, 306, 108, 38, kRefreshButtonId);
    HWND apply_button = make_control(window, L"BUTTON", L"应用",
                                     BS_DEFPUSHBUTTON | WS_TABSTOP,
                                     486, 306, 126, 38, kApplyButtonId);
    g_app->status_label = make_control(window, L"STATIC", L"", SS_LEFT,
                                       28, 365, 584, 24, kStatusLabelId);

    set_font(title, g_app->title_font);
    set_font(subtitle, g_app->body_font);
    set_font(display_label, g_app->strong_font);
    set_font(profile_label, g_app->strong_font);
    set_font(g_app->display_combo, g_app->body_font);
    set_font(g_app->profile_combo, g_app->body_font);
    set_font(g_app->current_label, g_app->body_font);
    set_font(import_button, g_app->body_font);
    set_font(reset_button, g_app->body_font);
    set_font(refresh_button, g_app->body_font);
    set_font(apply_button, g_app->strong_font);
    set_font(g_app->status_label, g_app->body_font);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE:
        create_controls(window);
        run_ui_action(refresh_data);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kDisplayComboId:
            if (HIWORD(w_param) == CBN_SELCHANGE) {
                run_ui_action(update_current_profile);
            }
            break;
        case kImportButtonId:
            run_ui_action(import_profile);
            break;
        case kRefreshButtonId:
            run_ui_action(refresh_data);
            break;
        case kApplyButtonId:
            run_ui_action(apply_selected_profile);
            break;
        case kResetButtonId:
            run_ui_action(reset_all_displays);
            break;
        default:
            break;
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        const HDC dc = reinterpret_cast<HDC>(w_param);
        const HWND control = reinterpret_cast<HWND>(l_param);
        SetBkColor(dc, RGB(250, 250, 250));
        if (g_app && control == g_app->status_label &&
            GetWindowLongPtrW(control, GWLP_USERDATA) != 0) {
            SetTextColor(dc, RGB(178, 32, 42));
        } else if (g_app && control == g_app->status_label) {
            SetTextColor(dc, RGB(32, 112, 66));
        } else {
            SetTextColor(dc, RGB(35, 38, 42));
        }
        return reinterpret_cast<LRESULT>(g_app->background_brush);
    }

    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(w_param), &rect, g_app->background_brush);
        return 1;
    }

    case WM_DESTROY:
        DeleteObject(g_app->title_font);
        DeleteObject(g_app->body_font);
        DeleteObject(g_app->strong_font);
        DeleteObject(g_app->background_brush);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    SetProcessDPIAware();
    try {
        g_app = std::make_unique<AppState>();
        g_app->background_brush = CreateSolidBrush(RGB(250, 250, 250));

        const wchar_t* class_name = L"IccSwitchNativeWindow";
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ICC_SWITCH));
        window_class.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ICC_SWITCH));
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = g_app->background_brush;
        window_class.lpszClassName = class_name;
        if (!RegisterClassExW(&window_class)) {
            throw UiError(L"无法注册主窗口。\n" + win32_message(GetLastError()));
        }

        const UINT dpi = GetDpiForSystem();
        RECT rect{0, 0, MulDiv(640, dpi, 96), MulDiv(418, dpi, 96)};
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&rect, style, FALSE);
        HWND window = CreateWindowExW(0, class_name, L"ICC Switch", style,
                                      CW_USEDEFAULT, CW_USEDEFAULT,
                                      rect.right - rect.left, rect.bottom - rect.top,
                                      nullptr, nullptr, instance, nullptr);
        if (!window) {
            throw UiError(L"无法创建主窗口。\n" + win32_message(GetLastError()));
        }

        ShowWindow(window, show_command);
        UpdateWindow(window);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        g_app.reset();
        return static_cast<int>(message.wParam);
    } catch (const UiError& error) {
        MessageBoxW(nullptr, error.message().c_str(), L"ICC Switch",
                    MB_OK | MB_ICONERROR);
        return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"ICC Switch 启动失败。", L"ICC Switch",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
