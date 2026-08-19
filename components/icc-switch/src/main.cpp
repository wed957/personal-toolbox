#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kScopeSystem = 0;
constexpr int kScopeCurrentUser = 1;
constexpr int kProfileTypeIcc = 0;
constexpr int kProfileSubtypeNone = 4;

using AddDisplayAssociationFn = HRESULT(WINAPI*)(int, PCWSTR, LUID, UINT32, BOOL, BOOL);
using RemoveDisplayAssociationFn = HRESULT(WINAPI*)(int, PCWSTR, LUID, UINT32, BOOL);
using SetDisplayDefaultFn = HRESULT(WINAPI*)(int, PCWSTR, int, int, LUID, UINT32);
using GetDisplayDefaultFn = HRESULT(WINAPI*)(int, LUID, UINT32, int, int, LPWSTR*);
using InstallColorProfileFn = BOOL(WINAPI*)(PCWSTR, PCWSTR);
using UninstallColorProfileFn = BOOL(WINAPI*)(PCWSTR, PCWSTR, BOOL);
using GetColorDirectoryFn = BOOL(WINAPI*)(PCWSTR, PWSTR, PDWORD);
using WcsAssociateFn = BOOL(WINAPI*)(int, PCWSTR, PCWSTR);
using WcsSetDefaultFn = BOOL(WINAPI*)(int, PCWSTR, int, int, DWORD, PCWSTR);
using WcsGetDefaultSizeFn = BOOL(WINAPI*)(int, PCWSTR, int, int, DWORD, PDWORD);
using WcsGetDefaultFn = BOOL(WINAPI*)(int, PCWSTR, int, int, DWORD, DWORD, PWSTR);

struct ColorApi {
    HMODULE module = nullptr;
    AddDisplayAssociationFn add_display_association = nullptr;
    RemoveDisplayAssociationFn remove_display_association = nullptr;
    SetDisplayDefaultFn set_display_default = nullptr;
    GetDisplayDefaultFn get_display_default = nullptr;
    InstallColorProfileFn install_color_profile = nullptr;
    UninstallColorProfileFn uninstall_color_profile = nullptr;
    GetColorDirectoryFn get_color_directory = nullptr;
    WcsAssociateFn wcs_associate = nullptr;
    WcsSetDefaultFn wcs_set_default = nullptr;
    WcsGetDefaultSizeFn wcs_get_default_size = nullptr;
    WcsGetDefaultFn wcs_get_default = nullptr;

    ColorApi() {
        module = LoadLibraryW(L"mscms.dll");
        if (!module) {
            throw std::runtime_error("无法加载 mscms.dll");
        }
        add_display_association = load<AddDisplayAssociationFn>("ColorProfileAddDisplayAssociation");
        remove_display_association = load<RemoveDisplayAssociationFn>("ColorProfileRemoveDisplayAssociation");
        set_display_default = load<SetDisplayDefaultFn>("ColorProfileSetDisplayDefaultAssociation");
        get_display_default = load<GetDisplayDefaultFn>("ColorProfileGetDisplayDefault");
        install_color_profile = load<InstallColorProfileFn>("InstallColorProfileW");
        uninstall_color_profile = load<UninstallColorProfileFn>("UninstallColorProfileW");
        get_color_directory = load<GetColorDirectoryFn>("GetColorDirectoryW");
        wcs_associate = load<WcsAssociateFn>("WcsAssociateColorProfileWithDevice");
        wcs_set_default = load<WcsSetDefaultFn>("WcsSetDefaultColorProfile");
        wcs_get_default_size = load<WcsGetDefaultSizeFn>("WcsGetDefaultColorProfileSize");
        wcs_get_default = load<WcsGetDefaultFn>("WcsGetDefaultColorProfile");
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

std::string utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring win32_message(DWORD error) {
    PWSTR buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, error, 0, reinterpret_cast<PWSTR>(&buffer), 0, nullptr);
    std::wstring message = size && buffer ? std::wstring(buffer, size) : L"未知错误";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::string last_error(const char* action) {
    const DWORD error = GetLastError();
    return std::string(action) + "失败 (" + std::to_string(error) + "): " + utf8(win32_message(error));
}

std::string hresult_error(const char* action, HRESULT result) {
    return std::string(action) + "失败 (0x" + [] (HRESULT value) {
        char text[16]{};
        std::snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(value));
        return std::string(text);
    }(result) + "): " + utf8(win32_message(static_cast<DWORD>(result)));
}

std::vector<DisplayInfo> enumerate_displays() {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (status != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(status));
        throw std::runtime_error(last_error("读取显示器数量"));
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                                &mode_count, modes.data(), nullptr);
    if (status != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(status));
        throw std::runtime_error(last_error("读取显示器"));
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

        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!display.gdi_name.empty() && EnumDisplayDevicesW(display.gdi_name.c_str(), 0, &device, 0)) {
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
        }
        displays.push_back(std::move(display));
    }

    std::stable_sort(displays.begin(), displays.end(), [](const DisplayInfo& left, const DisplayInfo& right) {
        return left.primary && !right.primary;
    });
    return displays;
}

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
        throw std::runtime_error(last_error("读取系统目录"));
    }
    return fs::path(windows) / L"System32" / L"spool" / L"drivers" / L"color";
}

bool is_profile(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    return extension == L".icc" || extension == L".icm";
}

std::vector<fs::path> installed_profiles(const ColorApi& api) {
    std::vector<fs::path> profiles;
    const fs::path directory = color_directory(api);
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (entry.is_regular_file(error) && is_profile(entry.path())) {
            profiles.push_back(entry.path());
        }
    }
    if (error) {
        throw std::runtime_error("无法读取系统 ICC 目录: " + error.message());
    }
    std::sort(profiles.begin(), profiles.end(), [](const fs::path& left, const fs::path& right) {
        return _wcsicmp(left.filename().c_str(), right.filename().c_str()) < 0;
    });
    return profiles;
}

std::wstring current_profile(const ColorApi& api, const DisplayInfo& display, std::string* scope_name = nullptr) {
    if (api.get_display_default) {
        for (const auto [scope, name] : {std::pair{kScopeCurrentUser, "用户"},
                                        std::pair{kScopeSystem, "系统"}}) {
            LPWSTR profile = nullptr;
            const HRESULT result = api.get_display_default(scope, display.adapter_id, display.source_id,
                                                           kProfileTypeIcc, kProfileSubtypeNone, &profile);
            if (SUCCEEDED(result) && profile) {
                std::wstring value(profile);
                LocalFree(profile);
                if (scope_name) {
                    *scope_name = name;
                }
                return value;
            }
        }
    }

    if (api.wcs_get_default_size && api.wcs_get_default && !display.gdi_name.empty()) {
        DWORD bytes = 0;
        if (api.wcs_get_default_size(kScopeCurrentUser, display.gdi_name.c_str(),
                                     kProfileTypeIcc, kProfileSubtypeNone, 0, &bytes) && bytes > 0) {
            std::wstring value(bytes / sizeof(wchar_t), L'\0');
            if (api.wcs_get_default(kScopeCurrentUser, display.gdi_name.c_str(), kProfileTypeIcc,
                                    kProfileSubtypeNone, 0, bytes, value.data())) {
                value.resize(std::wcslen(value.c_str()));
                if (scope_name) {
                    *scope_name = "用户";
                }
                return value;
            }
        }
    }
    return {};
}

fs::path resolve_profile(const ColorApi& api, const fs::path& input, bool install) {
    if (fs::exists(input)) {
        if (!fs::is_regular_file(input) || !is_profile(input)) {
            throw std::runtime_error("请选择 .icc 或 .icm 文件");
        }
        const fs::path absolute = fs::absolute(input);
        if (install) {
            if (!api.install_color_profile) {
                throw std::runtime_error("系统不支持安装 ICC 配置");
            }
            if (!api.install_color_profile(nullptr, absolute.c_str())) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                    throw std::runtime_error(last_error("安装 ICC"));
                }
            }
        }
        return color_directory(api) / absolute.filename();
    }

    const fs::path directory = color_directory(api);
    for (const auto& profile : installed_profiles(api)) {
        if (_wcsicmp(profile.filename().c_str(), input.filename().c_str()) == 0) {
            return directory / profile.filename();
        }
    }
    throw std::runtime_error("未找到 ICC: " + utf8(input.wstring()));
}

void broadcast_color_change() {
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Color"),
                        SMTO_ABORTIFHUNG, 1000, &ignored);
}

void set_profile(const ColorApi& api, const DisplayInfo& display, const fs::path& profile) {
    const std::wstring profile_name = profile.filename().wstring();
    if (api.add_display_association && api.set_display_default) {
        const HRESULT result = api.add_display_association(kScopeCurrentUser, profile_name.c_str(),
                                                          display.adapter_id, display.source_id,
                                                          TRUE, FALSE);
        if (FAILED(result) && HRESULT_CODE(result) != ERROR_ALREADY_EXISTS &&
            HRESULT_CODE(result) != ERROR_FILE_EXISTS) {
            throw std::runtime_error(hresult_error("切换 ICC", result));
        }
        const HRESULT default_result = api.set_display_default(
            kScopeCurrentUser, profile_name.c_str(), kProfileTypeIcc, kProfileSubtypeNone,
            display.adapter_id, display.source_id);
        if (FAILED(default_result)) {
            throw std::runtime_error(hresult_error("设置默认 ICC", default_result));
        }
        broadcast_color_change();
        return;
    }

    if (!api.wcs_associate || !api.wcs_set_default || display.gdi_name.empty()) {
        throw std::runtime_error("当前 Windows 版本不支持显示器 ICC 切换");
    }
    if (!api.wcs_associate(kScopeCurrentUser, profile_name.c_str(), display.gdi_name.c_str())) {
        throw std::runtime_error(last_error("关联 ICC"));
    }
    if (!api.wcs_set_default(kScopeCurrentUser, display.gdi_name.c_str(), kProfileTypeIcc,
                             kProfileSubtypeNone, 0, profile_name.c_str())) {
        throw std::runtime_error(last_error("设置默认 ICC"));
    }
    broadcast_color_change();
}

void remove_profile(const ColorApi& api, const std::vector<DisplayInfo>& displays,
                    const fs::path& profile) {
    if (!api.remove_display_association || !api.uninstall_color_profile) {
        throw std::runtime_error("当前 Windows 版本不支持卸载 ICC");
    }

    const std::wstring profile_name = profile.filename().wstring();
    for (const auto& display : displays) {
        const std::wstring current = current_profile(api, display);
        if (!current.empty() &&
            _wcsicmp(fs::path(current).filename().c_str(), profile_name.c_str()) == 0) {
            throw std::runtime_error("该 ICC 仍在使用，请先 reset 或切换到其他配置");
        }
    }

    for (const auto& display : displays) {
        api.remove_display_association(kScopeCurrentUser, profile_name.c_str(),
                                       display.adapter_id, display.source_id, FALSE);
    }
    if (!api.uninstall_color_profile(nullptr, profile.c_str(), TRUE)) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            throw std::runtime_error(last_error("卸载 ICC"));
        }
    }
    broadcast_color_change();
}

std::size_t parse_display_index(const wchar_t* text, std::size_t count) {
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (!text[0] || (end && *end) || value == 0 || value > count) {
        throw std::runtime_error("显示器序号无效");
    }
    return static_cast<std::size_t>(value - 1);
}

void print_usage() {
    std::cout
        << "ICC Switch - Windows ICC 切换工具\n\n"
        << "用法:\n"
        << "  icc-switch list\n"
        << "  icc-switch profiles\n"
        << "  icc-switch set <ICC 文件或配置名> [显示器序号]\n"
        << "  icc-switch install <ICC 文件>\n"
        << "  icc-switch reset\n"
        << "  icc-switch remove <已安装 ICC>\n";
}

int run(int argc, wchar_t** argv) {
    if (argc < 2 || std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h") {
        print_usage();
        return 0;
    }

    ColorApi api;
    const std::wstring command = argv[1];
    if (command == L"profiles") {
        const auto profiles = installed_profiles(api);
        std::cout << "已安装 ICC (" << profiles.size() << "):\n";
        for (std::size_t index = 0; index < profiles.size(); ++index) {
            std::cout << "  " << index + 1 << ". " << utf8(profiles[index].filename().wstring()) << "\n";
        }
        return 0;
    }

    if (command == L"install") {
        if (argc != 3) {
            throw std::runtime_error("install 需要一个 ICC 文件路径");
        }
        const fs::path profile = resolve_profile(api, fs::path(argv[2]), true);
        std::cout << "已安装: " << utf8(profile.filename().wstring()) << "\n";
        return 0;
    }

    const auto displays = enumerate_displays();
    if (displays.empty()) {
        throw std::runtime_error("未检测到活动显示器");
    }

    if (command == L"list") {
        std::cout << "活动显示器 (" << displays.size() << "):\n";
        for (std::size_t index = 0; index < displays.size(); ++index) {
            std::string scope;
            const std::wstring profile = current_profile(api, displays[index], &scope);
            std::cout << "  " << index + 1 << ". " << utf8(displays[index].friendly_name)
                      << " [" << utf8(displays[index].gdi_name) << "]"
                      << (displays[index].primary ? " 主屏" : "") << "\n"
                      << "     ICC: " << (profile.empty() ? "未设置" : utf8(fs::path(profile).filename().wstring()))
                      << (scope.empty() ? "" : " (" + scope + ")") << "\n";
        }
        return 0;
    }

    if (command == L"reset") {
        const fs::path profile = resolve_profile(api, fs::path(L"sRGB Color Space Profile.icm"), false);
        for (const auto& display : displays) {
            set_profile(api, display, profile);
        }
        std::cout << "已恢复全部显示器为系统 sRGB\n";
        return 0;
    }

    if (command == L"remove") {
        if (argc != 3) {
            throw std::runtime_error("remove 需要一个已安装 ICC 名称");
        }
        const fs::path profile = resolve_profile(api, fs::path(argv[2]), false);
        remove_profile(api, displays, profile);
        std::cout << "已卸载: " << utf8(profile.filename().wstring()) << "\n";
        return 0;
    }

    if (command == L"set") {
        if (argc < 3 || argc > 4) {
            throw std::runtime_error("set 需要 ICC 文件或配置名，可选显示器序号");
        }
        const std::size_t display_index = argc == 4 ? parse_display_index(argv[3], displays.size()) : 0;
        const fs::path profile = resolve_profile(api, fs::path(argv[2]), true);
        set_profile(api, displays[display_index], profile);
        std::cout << "已切换: 显示器 " << display_index + 1 << " -> "
                  << utf8(profile.filename().wstring()) << "\n";
        return 0;
    }

    throw std::runtime_error("未知命令: " + utf8(command));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "错误: " << error.what() << "\n";
        return 1;
    }
}
